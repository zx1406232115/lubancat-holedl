/*
 * holedl.h - 空洞文件下载器 公共接口
 *
 * 课程作业：基于文件 IO 的空洞文件「断点续传」下载器
 * 目标平台：鲁班猫 1 (LubanCat-1) / RK3566 / aarch64 / Linux
 *
 * 核心思想：
 *   1) 用 ftruncate() 一次性把文件「逻辑大小」拉到目标尺寸，
 *      内核只在 inode 里记录 i_size，不分配任何数据块 —— 这就是空洞(hole)。
 *   2) 只在真正拿到数据的偏移处 pwrite()，被写的块才从空洞变成实块，
 *      其余部分始终保持空洞，磁盘占用 = 0。
 *   3) 每下完一块就把进度写进状态文件，Ctrl-C 之后读回来接着下。
 */
#ifndef HOLEDL_H
#define HOLEDL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* ---------------- 通用工具 (util.c) ---------------- */

/*
 * 把 "64M" / "128K" / "1048576" 这类字符串解析成字节数。
 * 支持后缀：K/KiB、M/MiB、G/GiB（大小写不敏感，按 1024 进制）。
 * 成功返回 0 并写入 *out，失败返回 -1。
 */
int parse_size(const char *s, uint64_t *out);

/* 把字节数格式化成人类可读字符串，写入 buf（如 "64.00 MiB (67108864 B)"） */
void format_size(uint64_t bytes, char *buf, size_t buflen);

/*
 * 从文件里完整读取 [offset, offset+len) 这段数据。
 * 返回实际读到的字节数，-1 表示出错。
 */
ssize_t pread_full(int fd, void *buf, size_t len, off_t offset);

/*
 * 把缓冲区完整写入文件 [offset, offset+len) 这段位置。
 * 内部循环处理短写(partial write)。返回写入字节数，-1 表示出错。
 */
ssize_t pwrite_full(int fd, const void *buf, size_t len, off_t offset);

/* 打印带 errno 的错误信息，返回 -1，方便 `return log_err(...)` 这种写法 */
int log_err(const char *fmt, ...);

/* ---------------- 空洞文件核心 (sparse.c) ---------------- */

/* 一个文件的空洞占用率统计 */
struct sparse_stat {
    uint64_t logical_size;   /* 逻辑大小：st_size，看起来有多大 */
    uint64_t allocated;      /* 实际占用：st_blocks * 512，真正吃了多少盘 */
    uint64_t hole_bytes;     /* 空洞大小：logical_size - allocated */
    uint64_t block_size;     /* 文件系统的块大小(st_blksize)，用于换算展示 */
    double   hole_ratio;     /* 空洞占比 = hole_bytes / logical_size */
};

/*
 * create: 生成一个逻辑大小为 size 的空洞文件。
 *
 * 关键：全程只用 ftruncate()，绝不用 fallocate()/posix_fallocate()/写零，
 * 所以文件逻辑大小 = size，而实际占块 = 0。
 * 返回 0 成功，-1 失败。
 */
int sparse_create(const char *path, uint64_t size);

/*
 * write: 在文件的 offset 处写入数据，不破坏其它空洞。
 *
 * 用 pwrite() 而不是 lseek()+write()：
 *   - pwrite 是「带偏移的写」，原子且线程安全，不依赖文件偏移这个共享状态；
 *   - 语义上和 lseek+write 完全等价，但少一次系统调用，也不会因为
 *     多线程共享 fd 而互相踩掉偏移。
 * 这个函数内部保证只写这一段，其它区域连碰都不碰，空洞自然保持。
 * 返回写入字节数，-1 失败。
 */
ssize_t sparse_write_at(int fd, const void *buf, size_t len, off_t offset);

/* 打开(必要时创建)目标文件，O_RDWR。返回 fd，-1 失败 */
int sparse_open_rw(const char *path);

/* 取文件空洞统计，返回 0 成功 / -1 失败 */
int sparse_get_stat(const char *path, struct sparse_stat *out);

/* 判断 [offset, offset+len) 这段区间是否落在空洞里（用 SEEK_DATA/SEEK_HOLE）*/
bool sparse_range_is_hole(int fd, off_t offset, off_t len);

/* 打印 status 报告：逻辑大小 / 实际占块 / 空洞占比 */
void sparse_print_status(const char *path, const struct sparse_stat *st);

/*
 * 打印空洞占比（自动选择小数位数）。
 * 直接用 "%.2f%%" 会有显示矛盾：写了 4KiB 后真实占比是 99.9969%，
 * 会被四舍五入成 "100.00%"，和「实占 4096 B」看起来互相打架。
 */
void sparse_print_hole_ratio(double ratio);

/*
 * 逐块扫描并打印占用图，让「哪些块是真数据、哪些是空洞」一目了然。
 * map_block: 每格代表多少字节（0 表示自动取 1MiB）
 */
void sparse_print_map(const char *path, uint64_t map_block);

/* ---------------- 断点续传 (download.c) ---------------- */

/* 断点续传状态，全部落在磁盘上，进程死了也不丢 */
struct dl_state {
    char     src[1024];   /* 数据源：URL 或本地路径 */
    uint64_t total;       /* 源文件总大小 */
    uint64_t block_size;  /* 分块大小 */
    uint64_t nblocks;     /* 总块数 */
    uint8_t *done;        /* done[i] != 0 表示第 i 块已经落盘 */
    char     out_path[4096];
};

/*
 * download: 把 src 下载/复制到 out_path，支持断点续传。
 *
 * 流程：
 *   1) 探测 src 总大小；
 *   2) 若 out_path 不存在 → 先 sparse_create() 造一个空洞文件；
 *      若已存在 → 校验大小，读回 .holedl.state 恢复进度；
 *   3) 只下载「未完成」的块，每块用 sparse_write_at() 定点写入；
 *   4) 每块完成后立刻把状态写回磁盘，Ctrl-C 后可直接续跑。
 *
 * src 支持：http://host/path（用 HTTP Range 请求）
 * 返回 0 成功，-1 失败。
 */
int download_run(const char *src, const char *out_path, uint64_t block_size);

/* 只探测远端/本地数据源的大小，返回 0 成功 / -1 失败 */
int dl_probe_size(const char *src, uint64_t *out_size);

#endif /* HOLEDL_H */
