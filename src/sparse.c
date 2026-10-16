/*
 * sparse.c - 空洞文件核心：创建 / 定点写 / 统计
 *
 * 这个文件是整个作业的「空洞机制」所在，三个要点：
 *
 *   1. 造空洞 = ftruncate()
 *      ftruncate(fd, N) 只修改 inode 的 i_size，内核不会为这段区间分配
 *      任何数据块。所以 `ls -l` 看到 N 字节，`du` 看到 0。
 *      对比：ftruncate 之后如果不写数据，读出来全是 '\0'（内核对空洞读返回 0）。
 *
 *   2. 破坏空洞 = 写
 *      只有真正 pwrite() 过的那些块才会被分配。写 1 字节，内核也要为它
 *      分配一整个文件系统块(通常 4KiB)并按需补零，所以「写一点点、占用一大块」。
 *
 *   3. 保住空洞 = 绝不整文件重写
 *      下载器最容易犯的错是「先造一个大文件再从头覆盖写」或者「用
 *      fallocate 预分配」。前者会把空洞全填实，后者直接就把空间占满了。
 *      我们只对自己的 offset 段做 pwrite，别的区域连碰都不碰。
 */
#define _GNU_SOURCE
#include "holedl.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 文件系统报告的 st_blocks 单位固定是 512 字节（POSIX 规定），
 * 注意它和 st_blksize（最优 IO 大小，通常 4096）不是一回事。 */
#define ST_BLOCK_UNIT 512ULL

int sparse_create(const char *path, uint64_t size)
{
    if (path == NULL) {
        errno = EINVAL;
        return log_err("sparse_create: path 为空");
    }
    if (size > (uint64_t)INT64_MAX) {
        errno = EFBIG;
        return log_err("sparse_create: 尺寸过大 %" PRIu64, size);
    }

    /* O_CREAT|O_TRUNC：文件不存在就建，存在就清空重来 */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return log_err("打不开文件 %s", path);

    /*
     * 关键调用：只设置逻辑大小，不分配数据块。
     * 这里刻意不使用 posix_fallocate()/fallocate()/ftruncate+写零，
     * 那三种做法都会让 st_blocks 立刻变大，空洞就没了。
     */
    if (ftruncate(fd, (off_t)size) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return log_err("ftruncate(%s, %" PRIu64 ") 失败", path, size);
    }

    /* ftruncate 之后把元数据刷到磁盘，保证立刻 stat 就能看到正确大小 */
    if (fsync(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return log_err("fsync(%s) 失败", path);
    }
    if (close(fd) != 0)
        return log_err("close(%s) 失败", path);

    return 0;
}

int sparse_open_rw(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0)
        log_err("打不开文件 %s", path);
    return fd;
}

ssize_t sparse_write_at(int fd, const void *buf, size_t len, off_t offset)
{
    if (len == 0)
        return 0;
    if (offset < 0) {
        errno = EINVAL;
        return log_err("写入偏移不能为负");
    }

    /*
     * pwrite = 「写到指定偏移」，等价于 lseek(fd,off,SEEK_SET) + write(fd,...)
     * 但不会改动文件偏移量这个共享状态，多线程共用一个 fd 时也不会串。
     * 关键点：它只影响 [offset, offset+len) 这一段，其它空洞原样保留。
     */
    ssize_t n = pwrite_full(fd, buf, len, offset);
    if (n < 0)
        return log_err("pwrite(fd=%d, off=%jd, len=%zu) 失败",
                       fd, (intmax_t)offset, len);
    return n;
}

int sparse_get_stat(const char *path, struct sparse_stat *out)
{
    struct stat sb;
    if (stat(path, &sb) != 0)
        return log_err("stat(%s) 失败", path);
    if (!S_ISREG(sb.st_mode)) {
        errno = EINVAL;
        return log_err("%s 不是普通文件", path);
    }

    memset(out, 0, sizeof(*out));
    out->logical_size = (uint64_t)sb.st_size;
    out->allocated    = (uint64_t)sb.st_blocks * ST_BLOCK_UNIT;
    out->hole_bytes   = out->logical_size > out->allocated
                      ? out->logical_size - out->allocated : 0;
    out->block_size   = (uint64_t)(sb.st_blksize > 0 ? sb.st_blksize : 4096);
    out->hole_ratio   = out->logical_size
                      ? (double)out->hole_bytes / (double)out->logical_size
                      : 0.0;
    return 0;
}

bool sparse_range_is_hole(int fd, off_t offset, off_t len)
{
    if (len <= 0)
        return true;

    /*
     * SEEK_DATA：从 offset 开始往后找第一段「有数据」的位置；
     * SEEK_HOLE：从 offset 开始往后找第一段「空洞」的位置。
     * 找不到就返回 ENXIO。
     */
    off_t d = lseek(fd, offset, SEEK_DATA);
    if (d < 0) {
        if (errno == ENXIO)
            return true;             /* offset 之后全是空洞 */
        return false;                /* 文件系统不支持，保守当作有数据 */
    }
    if (d >= offset + len)
        return true;                 /* 第一段数据在区间之外 → 区间是空洞 */
    return false;
}

/*
 * 打印空洞占比。
 * 注意：不能直接用 "%.2f%%"。写了 4KiB 之后空洞占比其实是 99.9969%，
 * 用两位小数会显示成 "100.00%"，和上一行的「实占 4096 B」自相矛盾。
 * 所以当比例接近但不等于 100% 时，自动提高小数位数。
 */
void sparse_print_hole_ratio(double ratio)
{
    if (ratio >= 1.0) {
        printf("100.00%%");
    } else if (ratio > 0.9999) {
        printf("%.3f%%", ratio * 100.0);   /* 如 99.997% */
    } else {
        printf("%.2f%%", ratio * 100.0);
    }
}

void sparse_print_status(const char *path, const struct sparse_stat *st)
{
    char buf[64];

    printf("── status: %s\n", path);
    format_size(st->logical_size, buf, sizeof(buf));
    printf("   逻辑大小 (st_size)      : %s\n", buf);

    format_size(st->allocated, buf, sizeof(buf));
    printf("   实际占用 (st_blocks*512): %s\n", buf);

    format_size(st->hole_bytes, buf, sizeof(buf));
    printf("   其中空洞                : %s\n", buf);

    printf("   空洞占比                : ");
    sparse_print_hole_ratio(st->hole_ratio);
    printf("\n");

    /* 换算成「块」更贴近 df/du 的心智模型 */
    if (st->block_size > 0) {
        printf("   按 %" PRIu64 " B 块换算      : 逻辑 %" PRIu64 " 块 / 实占 %" PRIu64 " 块\n",
               st->block_size,
               (st->logical_size + st->block_size - 1) / st->block_size,
               (st->allocated + st->block_size - 1) / st->block_size);
    }

    /* 顺带对比一下 du 的口径，方便和老师解释两者关系 */
    printf("   提示: du 显示的就是「实际占用」，ls -l 显示的是「逻辑大小」\n");
}

void sparse_print_map(const char *path, uint64_t map_block)
{
    struct sparse_stat st;
    if (sparse_get_stat(path, &st) != 0)
        return;
    if (map_block == 0)
        map_block = 1024ULL * 1024;   /* 默认一格 1MiB */

    int fd = sparse_open_rw(path);
    if (fd < 0)
        return;

    uint64_t ncell = (st.logical_size + map_block - 1) / map_block;
    printf("── 占用图: %s  (一格 = %" PRIu64 " B, 共 %" PRIu64 " 格)\n",
           path, map_block, ncell);
    printf("   '#' = 已分配数据块   '.' = 空洞\n   [");

    for (uint64_t i = 0; i < ncell; i++) {
        off_t off = (off_t)(i * map_block);
        off_t len = (off_t)((i + 1) * map_block <= st.logical_size
                            ? map_block
                            : st.logical_size - i * map_block);
        bool hole = sparse_range_is_hole(fd, off, len);
        putchar(hole ? '.' : '#');
        /* 每 64 格换行一次，方便看 */
        if ((i + 1) % 64 == 0 && i + 1 < ncell)
            printf("\n    ");
    }
    printf("]\n");

    close(fd);
}
