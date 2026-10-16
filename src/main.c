/*
 * main.c - holedl 命令行入口
 *
 * 子命令：
 *   create   <file> <size>                     造空洞文件
 *   write    <file> --offset N [...]           定点写入，不破坏其它空洞
 *   status   <file> [--map]                    打印逻辑大小/占块/空洞占比
 *   download <src> <out> [--block N]           断点续传下载
 *   verify   <src> <out>                       逐字节比对源与目标
 */
#define _GNU_SOURCE
#include "holedl.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
    printf(
"holedl - 基于文件 IO 的空洞文件「断点续传」下载器\n"
"\n"
"用法:\n"
"  %s create   <file> <size>              生成逻辑大小为 size 的空洞文件\n"
"                                         size 支持 64M / 128K / 1048576 写法\n"
"  %s write    <file> --offset <N> [...]  在指定偏移写入，不破坏其它空洞\n"
"        --offset N        写入起始偏移（必填）\n"
"        --len N           写入长度，默认 4096\n"
"        --byte 0xNN       用固定字节填充（默认 0xAA）\n"
"        --from <src>      从 src 文件拷贝这段数据过来\n"
"  %s status   <file> [--map]             打印空洞统计；--map 画占用图\n"
"  %s download <src> <out> [--block <N>]  断点续传下载（src 可为 http:// 或本地文件）\n"
"        --block N         分块大小，默认 256K\n"
"        --restart         忽略已有进度，从头开始\n"
"  %s verify   <src> <out>                逐块比对两个文件是否一致\n"
"\n"
"典型流程:\n"
"  %s create out.bin 64M                    # 造空洞，du 显示 0\n"
"  %s status out.bin                        # 空洞占比 100%%\n"
"  %s write  out.bin --offset 1M --len 4K   # 只写 4K，其它仍是空洞\n"
"  %s status out.bin --map                  # 直观看到哪块被填实\n"
"  %s download ./src.bin out.bin --block 256K   # 断点续传\n"
"  %s verify ./src.bin out.bin\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* 把 0xNN 或十进制解析成字节 */
static int parse_byte(const char *s, unsigned *out)
{
    if (s == NULL || *s == '\0')
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);   /* base 0 → 自动识别 0x 前缀 */
    if (errno != 0 || end == s || *end != '\0' || v > 0xFF)
        return -1;
    *out = (unsigned)v;
    return 0;
}

/* ---------------- create ---------------- */

static int cmd_create(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: holedl create <file> <size>\n");
        return 2;
    }
    const char *path = argv[0];
    uint64_t size = 0;
    if (parse_size(argv[1], &size) != 0) {
        fprintf(stderr, "holedl: 无法解析尺寸 '%s'（试试 64M / 128K / 1048576）\n",
                argv[1]);
        return 2;
    }
    if (size == 0) {
        fprintf(stderr, "holedl: 尺寸必须大于 0\n");
        return 2;
    }
    if (sparse_create(path, size) != 0)
        return 1;

    char hbuf[64];
    format_size(size, hbuf, sizeof(hbuf));
    printf("✔ 已创建空洞文件 %s（逻辑大小 %s）\n", path, hbuf);

    /* 立刻回读一次，用数据证明「逻辑大小有、实际占用无」 */
    struct sparse_stat st;
    if (sparse_get_stat(path, &st) == 0) {
        char abuf[64];
        format_size(st.allocated, abuf, sizeof(abuf));
        printf("  逻辑大小 : %s\n", hbuf);
        printf("  实际占用 : %s\n", abuf);
        printf("  空洞占比 : ");
        sparse_print_hole_ratio(st.hole_ratio);
        printf("\n");
        if (st.allocated != 0)
            fprintf(stderr,
                    "holedl: 警告——实际占用不为 0，该文件系统可能不支持空洞\n");
    }
    return 0;
}

/* ---------------- write ---------------- */

static int cmd_write(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "用法: holedl write <file> --offset N [--len N] "
                        "[--byte 0xNN | --from <src>]\n");
        return 2;
    }
    const char *path = argv[0];
    const char *from = NULL;
    uint64_t offset = 0, len = 4096;
    bool have_offset = false;
    unsigned fill = 0xAA;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &offset) != 0) {
                fprintf(stderr, "holedl: 非法 offset '%s'\n", argv[i]);
                return 2;
            }
            have_offset = true;
        } else if (strcmp(argv[i], "--len") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &len) != 0) {
                fprintf(stderr, "holedl: 非法 len '%s'\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--byte") == 0 && i + 1 < argc) {
            if (parse_byte(argv[++i], &fill) != 0) {
                fprintf(stderr, "holedl: 非法 byte '%s'\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            from = argv[++i];
        } else {
            fprintf(stderr, "holedl: 未知参数 '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!have_offset) {
        fprintf(stderr, "holedl: write 必须指定 --offset\n");
        return 2;
    }
    if (len == 0) {
        fprintf(stderr, "holedl: len 必须大于 0\n");
        return 2;
    }
    if (len > SIZE_MAX) {
        fprintf(stderr, "holedl: len 过大\n");
        return 2;
    }

    int fd = sparse_open_rw(path);
    if (fd < 0)
        return 1;

    /* 先确认目标区间存在（防止 offset 超出逻辑大小，那样会把文件撑大，
     * 虽然也是空洞，但和「定点填洞」的演示意图不符，所以直接拒绝） */
    struct sparse_stat before;
    if (sparse_get_stat(path, &before) != 0) {
        close(fd);
        return 1;
    }
    if (offset + len > before.logical_size) {
        fprintf(stderr,
                "holedl: 写入区间 [%" PRIu64 ", %" PRIu64 ") 超出文件逻辑大小 %" PRIu64 "\n",
                offset, offset + len, before.logical_size);
        close(fd);
        return 2;
    }

    /* 准备待写数据 */
    unsigned char *buf = malloc((size_t)len);
    if (buf == NULL) {
        close(fd);
        errno = ENOMEM;
        return log_err("内存不足");
    }

    if (from != NULL) {
        int sfd = open(from, O_RDONLY);
        if (sfd < 0) {
            free(buf);
            close(fd);
            return log_err("打不开源文件 %s", from);
        }
        ssize_t n = pread_full(sfd, buf, (size_t)len, 0);
        close(sfd);
        if (n < 0 || (uint64_t)n != len) {
            free(buf);
            close(fd);
            errno = 0;
            return log_err("从 %s 读取 %" PRIu64 " 字节失败", from, len);
        }
    } else {
        memset(buf, (int)fill, (size_t)len);
    }

    printf("── 写入前: 逻辑 %" PRIu64 " B / 实占 %" PRIu64 " B / 空洞 ",
           before.logical_size, before.allocated);
    sparse_print_hole_ratio(before.hole_ratio);
    printf("\n");

    /* 关键：定点写，只影响 [offset, offset+len) */
    ssize_t w = sparse_write_at(fd, buf, (size_t)len, (off_t)offset);
    free(buf);
    if (w < 0 || (uint64_t)w != len) {
        close(fd);
        return log_err("写入失败");
    }

    if (fsync(fd) != 0) {
        close(fd);
        return log_err("fsync 失败");
    }
    close(fd);

    struct sparse_stat after;
    if (sparse_get_stat(path, &after) != 0)
        return 1;

    printf("✔ 已在偏移 %" PRIu64 " 写入 %" PRIu64 " 字节（%s）\n",
           offset, len,
           from ? from : "固定字节填充");
    printf("── 写入后: 逻辑 %" PRIu64 " B / 实占 %" PRIu64 " B / 空洞 ",
           after.logical_size, after.allocated);
    sparse_print_hole_ratio(after.hole_ratio);
    printf("\n");
    printf("   实占增量: %" PRId64 " B（写 1 字节也要占一个文件系统块，故通常大于写入量）\n",
           (int64_t)after.allocated - (int64_t)before.allocated);
    printf("   逻辑大小未变: %s（空洞没有被「撑开」）\n",
           after.logical_size == before.logical_size ? "是" : "否！异常");
    return 0;
}

/* ---------------- status ---------------- */

static int cmd_status(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "用法: holedl status <file> [--map]\n");
        return 2;
    }
    const char *path = argv[0];
    bool want_map = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--map") == 0)
            want_map = true;
        else {
            fprintf(stderr, "holedl: 未知参数 '%s'\n", argv[i]);
            return 2;
        }
    }

    struct sparse_stat st;
    if (sparse_get_stat(path, &st) != 0)
        return 1;
    sparse_print_status(path, &st);
    if (want_map)
        sparse_print_map(path, 0);
    return 0;
}

/* ---------------- download ---------------- */

static int cmd_download(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: holedl download <src> <out> [--block N] [--restart]\n");
        return 2;
    }
    const char *src = argv[0];
    const char *out = argv[1];
    uint64_t block = 256ULL * 1024;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--block") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &block) != 0 || block == 0) {
                fprintf(stderr, "holedl: 非法 block '%s'\n", argv[i]);
                return 2;
            }
        } else {
            fprintf(stderr, "holedl: 未知参数 '%s'\n", argv[i]);
            return 2;
        }
    }

    int rc = download_run(src, out, block);
    if (rc != 0)
        return 1;

    /* 下载完顺手给出空洞统计，方便截图 */
    struct sparse_stat st;
    if (sparse_get_stat(out, &st) == 0)
        sparse_print_status(out, &st);
    return 0;
}

/* ---------------- verify ---------------- */

static int cmd_verify(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: holedl verify <src> <out>\n");
        return 2;
    }
    const char *src = argv[0];
    const char *out = argv[1];

    int a = open(src, O_RDONLY);
    if (a < 0)
        return log_err("打不开 %s", src);
    int b = open(out, O_RDONLY);
    if (b < 0) {
        close(a);
        return log_err("打不开 %s", out);
    }

    struct sparse_stat sa, sb;
    int rc = 0;
    if (sparse_get_stat(src, &sa) != 0 || sparse_get_stat(out, &sb) != 0) {
        rc = 1;
        goto done;
    }
    if (sa.logical_size != sb.logical_size) {
        printf("✘ 大小不一致: 源 %" PRIu64 " B, 目标 %" PRIu64 " B\n",
               sa.logical_size, sb.logical_size);
        rc = 1;
        goto done;
    }

    /* 逐块流式比对，避免把大文件整个读进内存 */
    const size_t CH = 1u << 20;
    unsigned char *ba = malloc(CH), *bb = malloc(CH);
    if (ba == NULL || bb == NULL) {
        free(ba); free(bb);
        errno = ENOMEM;
        rc = log_err("内存不足");
        goto done;
    }

    uint64_t off = 0, diff_bytes = 0, first_diff = 0;
    bool have_first = false;
    while (off < sa.logical_size) {
        size_t want = CH;
        if (sa.logical_size - off < want)
            want = (size_t)(sa.logical_size - off);

        ssize_t na = pread_full(a, ba, want, (off_t)off);
        ssize_t nb = pread_full(b, bb, want, (off_t)off);
        if (na < 0 || nb < 0 || na != nb) {
            printf("✘ 偏移 %" PRIu64 " 读取出错\n", off);
            rc = 1;
            break;
        }
        if (memcmp(ba, bb, (size_t)na) != 0) {
            for (size_t i = 0; i < (size_t)na; i++) {
                if (ba[i] != bb[i]) {
                    diff_bytes++;
                    if (!have_first) {
                        first_diff = off + i;
                        have_first = true;
                    }
                }
            }
        }
        off += (uint64_t)na;
    }
    free(ba);
    free(bb);

    if (rc == 0) {
        if (diff_bytes == 0) {
            printf("✔ 校验通过：%s 与 %s 完全一致（%" PRIu64 " 字节）\n",
                   src, out, sa.logical_size);
        } else {
            printf("✘ 校验失败：%s 与 %s 有 %" PRIu64 " 字节不同，"
                   "首个不同在偏移 %" PRIu64 "\n",
                   src, out, diff_bytes, first_diff);
            rc = 1;
        }
    }

done:
    close(a);
    close(b);
    return rc;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    const char *cmd = argv[1];
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (strcmp(cmd, "create") == 0)
        return cmd_create(sub_argc, sub_argv);
    if (strcmp(cmd, "write") == 0)
        return cmd_write(sub_argc, sub_argv);
    if (strcmp(cmd, "status") == 0)
        return cmd_status(sub_argc, sub_argv);
    if (strcmp(cmd, "download") == 0)
        return cmd_download(sub_argc, sub_argv);
    if (strcmp(cmd, "verify") == 0)
        return cmd_verify(sub_argc, sub_argv);

    fprintf(stderr, "holedl: 未知子命令 '%s'\n\n", cmd);
    usage(argv[0]);
    return 2;
}
