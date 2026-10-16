/*
 * util.c - 通用小工具：参数解析、带偏移读写、错误打印
 */
#define _GNU_SOURCE
#include "holedl.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("holedl: ", stderr);
    vfprintf(stderr, fmt, ap);
    if (errno != 0)
        fprintf(stderr, ": %s", strerror(errno));
    fputc('\n', stderr);
    va_end(ap);
    return -1;
}

int parse_size(const char *s, uint64_t *out)
{
    if (s == NULL || *s == '\0')
        return -1;

    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s)
        return -1;

    uint64_t mult = 1;
    if (*end != '\0') {
        /* 只允许一个后缀字符，后面可以再跟 "B"/"iB" */
        char c = (char)toupper((unsigned char)*end);
        const char *rest = end + 1;

        if (rest[0] == 'i' || rest[0] == 'I')
            rest++;
        if (rest[0] == 'b' || rest[0] == 'B')
            rest++;
        if (*rest != '\0')
            return -1;

        switch (c) {
        case 'K': mult = 1024ULL; break;
        case 'M': mult = 1024ULL * 1024; break;
        case 'G': mult = 1024ULL * 1024 * 1024; break;
        default:  return -1;
        }
    }

    /* 溢出检查 */
    if (v != 0 && (uint64_t)v > UINT64_MAX / mult)
        return -1;

    *out = (uint64_t)v * mult;
    return 0;
}

void format_size(uint64_t bytes, char *buf, size_t buflen)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(buf, buflen, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, buflen, "%.2f %s (%llu B)", v, unit[i],
                 (unsigned long long)bytes);
}

ssize_t pread_full(int fd, void *buf, size_t len, off_t offset)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, (char *)buf + got, len - got, offset + (off_t)got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;   /* 提前 EOF */
        got += (size_t)n;
    }
    return (ssize_t)got;
}

ssize_t pwrite_full(int fd, const void *buf, size_t len, off_t offset)
{
    size_t put = 0;
    while (put < len) {
        ssize_t n = pwrite(fd, (const char *)buf + put, len - put,
                           offset + (off_t)put);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = ENOSPC;
            return -1;
        }
        put += (size_t)n;
    }
    return (ssize_t)put;
}
