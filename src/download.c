/*
 * download.c - 断点续传下载引擎
 *
 * 设计要点（对应作业的「断点续传」要求）：
 *
 *   A. 目标文件一开始就是空洞文件
 *      先 dl_probe_size() 问出源文件总大小，然后 sparse_create() 造出
 *      同样逻辑大小的空洞文件，磁盘占用 0。之后每下完一块才定点写入。
 *
 *   B. 进度落盘
 *      进度写在 <输出文件>.holedl.state 里，格式是纯文本：
 *        HOLEDL1 / src / total / block_size / hex位图
 *      位图第 i 位为 1 表示第 i 块已经真正写进文件了。
 *      每下完一块就 rewrite 一次状态文件并 fsync —— Ctrl-C 之后
 *      已经落盘的数据不会白下，重启后只补没下完的块。
 *
 *   C. 续传靠 HTTP Range
 *      每块单独发一个 `Range: bytes=start-end` 请求，服务端回 206，
 *      我们 pwrite 到对应偏移。不支持 Range 的服务器会回 200，
 *      这时只能从头顺序下（代码里会警告并退化成整文件顺序写）。
 *
 *   D. 数据源可以是本地文件
 *      为了在没有网络/没有服务器时也能演示和验收，src 支持普通路径，
 *      走 pread 读源文件、pwrite 写目标文件，空洞与续传逻辑完全一样。
 */
#define _GNU_SOURCE
#include "holedl.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_BLOCK   (256ULL * 1024)      /* 默认分块 256KiB */
#define IO_TIMEOUT_SEC  15
#define STATE_MAGIC     "HOLEDL1"

/* ---------------- URL 解析 ---------------- */

struct url {
    char host[256];
    char port[16];
    char path[2048];
};

static int url_parse(const char *url, struct url *u)
{
    const char *p = url;
    memset(u, 0, sizeof(*u));
    snprintf(u->port, sizeof(u->port), "80");

    if (strncasecmp(p, "http://", 7) != 0) {
        errno = EPROTONOSUPPORT;
        return log_err("只支持 http:// （当前: %s）", url);
    }
    p += 7;

    /* host[:port] */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon != NULL && (slash == NULL || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl == 0 || hl >= sizeof(u->host)) {
            errno = EINVAL;
            return log_err("非法主机名");
        }
        memcpy(u->host, p, hl);
        u->host[hl] = '\0';

        const char *ps = colon + 1;
        size_t pl = slash ? (size_t)(slash - ps) : strlen(ps);
        if (pl == 0 || pl >= sizeof(u->port)) {
            errno = EINVAL;
            return log_err("非法端口");
        }
        memcpy(u->port, ps, pl);
        u->port[pl] = '\0';
    } else {
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl == 0 || hl >= sizeof(u->host)) {
            errno = EINVAL;
            return log_err("非法主机名");
        }
        memcpy(u->host, p, hl);
        u->host[hl] = '\0';
    }

    /* path（缺省 "/"） */
    if (slash == NULL || *slash == '\0')
        snprintf(u->path, sizeof(u->path), "/");
    else
        snprintf(u->path, sizeof(u->path), "%s", slash);

    return 0;
}

static bool is_http_url(const char *src)
{
    return strncasecmp(src, "http://", 7) == 0;
}

/* ---------------- 建连 ---------------- */

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        errno = 0;
        return log_err("域名解析失败 %s:%s (%s)", host, port, gai_strerror(rc));
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        struct timeval tv = { IO_TIMEOUT_SEC, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;                       /* 连上了 */

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        errno = 0;
        return log_err("连接 %s:%s 失败", host, port);
    }
    return fd;
}

/* 完整发出去，处理短写 */
static int send_full(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/*
 * 发一个 HTTP 请求并解析状态行 + 头部。
 * on_body 回调把响应体一段段喂出去；返回 0 表示继续。
 */
typedef int (*body_cb)(void *ctx, const char *data, size_t len);

struct http_resp {
    int      status;          /* 200 / 206 / 404 ... */
    uint64_t content_length;  /* Content-Length 的值 */
    uint64_t range_start;     /* 206 时 Content-Range 的起点 */
    uint64_t range_total;     /* 206 时 Content-Range 的总长 */
    bool     has_range;
    bool     has_clen;
};

/*
 * method: "GET" 或 "HEAD"。
 * HEAD 只用来读 Content-Length，不读 body —— 这样探测大文件大小
 * 就不用把整个文件拉下来（用 GET+Range 探测会白下 1 字节，而且
 * 万一服务端不支持 Range，就会把整个文件都传过来，非常浪费）。
 */
static int http_request(const char *method, const char *url,
                        const char *range_hdr,
                        body_cb cb, void *ctx, struct http_resp *out)
{
    struct url u;
    if (url_parse(url, &u) != 0)
        return -1;

    memset(out, 0, sizeof(*out));
    out->content_length = 0;
    out->range_total    = 0;

    int fd = tcp_connect(u.host, u.port);
    if (fd < 0)
        return -1;

    char req[3072];
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%s\r\n"
                     "User-Agent: holedl/1.0\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     method, u.path, u.host, u.port,
                     range_hdr ? range_hdr : "");
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        close(fd);
        errno = EINVAL;
        return log_err("请求头过长");
    }
    if (send_full(fd, req, (size_t)n) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return log_err("发送请求失败");
    }

    /* ---- 读头部，直到 \r\n\r\n ---- */
    char  hdr[8192];
    size_t hlen = 0;
    size_t body_off = 0;
    bool   hdr_done = false;

    while (hlen < sizeof(hdr) - 1) {
        ssize_t r = recv(fd, hdr + hlen, sizeof(hdr) - 1 - hlen, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            errno = e;
            return log_err("接收响应失败");
        }
        if (r == 0)
            break;
        hlen += (size_t)r;
        hdr[hlen] = '\0';

        char *p = strstr(hdr, "\r\n\r\n");
        if (p != NULL) {
            hdr_done = true;
            body_off = (size_t)(p - hdr) + 4;
            break;
        }
    }

    if (!hdr_done) {
        close(fd);
        errno = 0;
        return log_err("响应头部不完整或被截断");
    }

    /* ---- 解析状态行 ---- */
    if (strncmp(hdr, "HTTP/", 5) != 0) {
        close(fd);
        errno = 0;
        return log_err("不是合法的 HTTP 响应");
    }
    {
        char *sp = strchr(hdr, ' ');
        out->status = sp ? atoi(sp + 1) : 0;
    }

    /*
     * ---- 逐行解析头部 ----
     * 注意：body 就紧跟在 hdr[body_off] 之后，绝不能就地改写 hdr ——
     * 改写分隔符会把已经收到的那部分 body 破坏掉（strtok 会在 token
     * 末尾写 '\0'，正好会踩掉 body 的第一个字节）。
     * 所以先把「头部区域」整体拷进独立的 scratch 缓冲区再切分。
     */
    if (body_off > 0) {
        size_t hregion = body_off - 4;       /* 去掉结尾的 \r\n\r\n */
        while (hregion > 0 &&
               (hdr[hregion - 1] == '\r' || hdr[hregion - 1] == '\n'))
            hregion--;

        char scratch[8192];
        if (hregion >= sizeof(scratch))
            hregion = sizeof(scratch) - 1;
        memcpy(scratch, hdr, hregion);
        scratch[hregion] = '\0';

        char *save = NULL;
        char *tok  = strtok_r(scratch, "\r\n", &save);
        if (tok != NULL)
            tok = strtok_r(NULL, "\r\n", &save);   /* 跳过状态行 */

        for (; tok != NULL; tok = strtok_r(NULL, "\r\n", &save)) {
            if (strncasecmp(tok, "Content-Length:", 15) == 0) {
                out->content_length = strtoull(tok + 15, NULL, 10);
                out->has_clen = true;
            } else if (strncasecmp(tok, "Content-Range:", 14) == 0) {
                /* Content-Range: bytes 100-199/1000 */
                const char *sl   = strchr(tok + 14, '/');
                const char *dash = strchr(tok + 14, '-');
                if (dash != NULL)
                    out->range_start = strtoull(dash + 1, NULL, 10);
                if (sl != NULL)
                    out->range_total = strtoull(sl + 1, NULL, 10);
                out->has_range = true;
            }
        }
    }

    /* ---- 把头部之后已经读到的 body 先喂给回调 ---- */
    size_t have = hlen - body_off;
    if (have > 0 && cb != NULL) {
        if (cb(ctx, hdr + body_off, have) != 0) {
            close(fd);
            return -1;
        }
    }

    /* ---- 继续读剩余 body ---- */
    char buf[64 * 1024];
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            errno = e;
            return log_err("读取响应体失败");
        }
        if (r == 0)
            break;                       /* 服务端关闭，结束 */
        if (cb != NULL && cb(ctx, buf, (size_t)r) != 0) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

/* ---- 用于探测大小的空回调 ---- */
static int cb_discard(void *ctx, const char *data, size_t len)
{
    (void)ctx; (void)data; (void)len;
    return 0;
}

/* ---- 把 body 写到文件指定偏移的回调上下文 ---- */
struct sink {
    int      fd;
    uint64_t offset;      /* 目标文件里的写偏移 */
    uint64_t written;     /* 已写字节 */
    uint64_t expect;      /* 期望收到的字节数（用于校验） */
};

static int cb_to_file(void *ctx, const char *data, size_t len)
{
    struct sink *s = (struct sink *)ctx;
    if (len == 0)
        return 0;
    /* 超出该块范围的字节直接丢弃（防止服务端多给） */
    if (s->expect != 0 && s->written + len > s->expect)
        len = (size_t)(s->expect - s->written);

    if (len > 0) {
        if (sparse_write_at(s->fd, data, len, (off_t)(s->offset + s->written)) < 0)
            return -1;
        s->written += len;
    }
    return 0;
}

/* ---------------- 大小探测 ---------------- */

static int probe_local(const char *path, uint64_t *out)
{
    struct stat sb;
    if (stat(path, &sb) != 0)
        return log_err("stat(%s) 失败", path);
    if (!S_ISREG(sb.st_mode)) {
        errno = EINVAL;
        return log_err("%s 不是普通文件", path);
    }
    *out = (uint64_t)sb.st_size;
    return 0;
}

int dl_probe_size(const char *src, uint64_t *out_size)
{
    if (!is_http_url(src))
        return probe_local(src, out_size);

    struct http_resp r;

    /*
     * 优先用 HEAD：只读 Content-Length，不传 body，对大文件最省流量。
     * 有些简易服务器不给 HEAD 的 Content-Length，所以失败再退回 Range。
     */
    if (http_request("HEAD", src, NULL, NULL, NULL, &r) == 0 &&
        r.status == 200 && r.has_clen) {
        *out_size = r.content_length;
        return 0;
    }

    /* 退回 Range: bytes=0-0 —— 支持 Range 的服务端会回 206 + Content-Range */
    if (http_request("GET", src, "Range: bytes=0-0\r\n",
                     cb_discard, NULL, &r) != 0)
        return -1;

    if (r.status == 206 && r.has_range && r.range_total > 0) {
        *out_size = r.range_total;
        return 0;
    }
    if (r.status == 200 && r.has_clen) {
        /* 服务端不支持 Range，但至少给了长度 */
        *out_size = r.content_length;
        return 0;
    }
    errno = 0;
    return log_err("无法探测大小 (HTTP %d，可能不支持 Range)", r.status);
}

/*
 * 服务端不支持 Range（对 Range 请求回了 200 整包）时的兜底：
 * 顺序下载整个文件，每次 recv 的数据按 seq 偏移 pwrite 到正确位置。
 *
 * 代价说明：这种方式一次请求必须下完，中途断了只能从头再来，
 * 所以它只是「能跑通」的降级路径，真正的断点续传依然依赖 Range。
 */
static int dl_sequential_fallback(const char *src, int outfd, uint64_t total)
{
    struct sink s = { .fd = outfd, .offset = 0, .written = 0, .expect = total };
    struct http_resp r;

    if (http_request("GET", src, NULL, cb_to_file, &s, &r) != 0)
        return -1;
    if (r.status != 200 && r.status != 206) {
        errno = 0;
        return log_err("顺序下载失败 (HTTP %d)", r.status);
    }
    if (s.written != total) {
        errno = 0;
        return log_err("顺序下载数据不完整：期望 %" PRIu64 " 字节，实收 %" PRIu64 " 字节",
                       total, s.written);
    }
    return 0;
}

/* ---------------- 状态文件 ---------------- */

static void state_path(const char *out, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s.holedl.state", out);
}

/*
 * 位图编解码。
 *
 * 这里必须严格保证「写」和「读」用同一套规则，否则状态文件会读不回来。
 * 规则：每 4 个块压成 1 个 hex 字符，共 (nblocks+3)/4 个字符；
 *       字符内高位对应编号小的块（即块 i 落在字符 i/4 的位 (3 - i%4)）。
 *
 * 曾经踩过的坑：写入方按「每字节 8 位」输出、读取方按「每半字节 4 位」
 * 计算长度，块数 >16 时两边的长度不一致，读取方认为文件被截断，
 * 于是每次续传都静默退化成「全新下载」。因此加了一条位图往返自检。
 */
static int hexval(char c);

static uint64_t bitmap_hexlen(uint64_t nblocks)
{
    return (nblocks + 3) / 4;
}

/* 把 done[] 编码成 hex 字符串，返回写入的字符数 */
static uint64_t bitmap_encode(const uint8_t *done, uint64_t nblocks, char *out)
{
    uint64_t n = bitmap_hexlen(nblocks);
    for (uint64_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (unsigned b = 0; b < 4; b++) {
            uint64_t bit = i * 4 + b;
            if (bit < nblocks && done[bit])
                v |= 1u << (3 - b);
        }
        out[i] = "0123456789abcdef"[v];
    }
    out[n] = '\0';
    return n;
}

/* 把 hex 字符串解码进 done[]；字符数不足返回 -1 */
static int bitmap_decode(const char *hex, uint64_t nblocks, uint8_t *done)
{
    uint64_t n = bitmap_hexlen(nblocks);
    for (uint64_t i = 0; i < n; i++) {
        int v = hexval(hex[i]);
        if (v < 0)
            return -1;
        for (unsigned b = 0; b < 4; b++) {
            uint64_t bit = i * 4 + b;
            if (bit < nblocks)
                done[bit] = (uint8_t)((v >> (3 - b)) & 1);
        }
    }
    return 0;
}

static int state_save(const struct dl_state *st)
{
    char sp[4200];
    state_path(st->out_path, sp, sizeof(sp));

    /* 先写临时文件再 rename，避免 Ctrl-C 时写坏状态文件 */
    char tmp[4300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", sp);

    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return log_err("无法写状态文件 %s", tmp);

    if (fprintf(f, "%s\n%s\n%" PRIu64 "\n%" PRIu64 "\n",
                STATE_MAGIC, st->src, st->total, st->block_size) < 0) {
        fclose(f);
        return log_err("写状态文件失败");
    }

    char *hex = malloc((size_t)bitmap_hexlen(st->nblocks) + 1);
    if (hex == NULL) {
        fclose(f);
        errno = ENOMEM;
        return log_err("内存不足");
    }
    bitmap_encode(st->done, st->nblocks, hex);

    /*
     * 位图往返自检：写出去的必须能一模一样读回来。
     * 就是为了防止「写入方和读取方编码规则不一致」这种 bug 再次溜过去
     * ——它会让续传静默失效，而且不容易在块数少的时候暴露。
     */
    {
        uint8_t *check = calloc((size_t)st->nblocks, 1);
        if (check == NULL) {
            free(hex);
            fclose(f);
            errno = ENOMEM;
            return log_err("内存不足");
        }
        int bad = (bitmap_decode(hex, st->nblocks, check) != 0) ||
                  (memcmp(check, st->done, (size_t)st->nblocks) != 0);
        free(check);
        if (bad) {
            free(hex);
            fclose(f);
            errno = 0;
            return log_err("内部错误：进度位图往返自检失败，已放弃保存");
        }
    }

    if (fprintf(f, "%s\n", hex) < 0) {
        free(hex);
        fclose(f);
        return log_err("写状态位图失败");
    }
    free(hex);

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        return log_err("同步状态文件失败");
    }
    if (fclose(f) != 0)
        return log_err("关闭状态文件失败");

    if (rename(tmp, sp) != 0)
        return log_err("rename 状态文件失败");
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * 载入状态。返回 0 表示成功恢复，1 表示没有可用状态（全新开始），-1 出错。
 * force_restart 为 true 时直接全新开始（--restart）。
 */
static int state_load(struct dl_state *st, bool force_restart)
{
    if (force_restart)
        return 1;

    char sp[4200];
    state_path(st->out_path, sp, sizeof(sp));

    FILE *f = fopen(sp, "r");
    if (f == NULL)
        return 1;                        /* 没有状态文件 → 全新开始 */

    char magic[32] = {0}, src[1024] = {0};
    unsigned long long total = 0, blk = 0;
    if (fscanf(f, "%31s\n%1023s\n%llu\n%llu\n", magic, src, &total, &blk) != 4) {
        fclose(f);
        return 1;
    }
    if (strcmp(magic, STATE_MAGIC) != 0 || blk == 0) {
        fclose(f);
        return 1;
    }
    if (strcmp(src, st->src) != 0 || (uint64_t)total != st->total) {
        fprintf(stderr, "holedl: 状态文件与当前任务不匹配（源或大小变了），重新开始\n");
        fclose(f);
        return 1;
    }
    if ((uint64_t)blk != st->block_size) {
        fprintf(stderr, "holedl: 分块大小变了(%llu→%" PRIu64 ")，重新开始\n",
                blk, st->block_size);
        fclose(f);
        return 1;
    }

    uint64_t want = bitmap_hexlen(st->nblocks);   /* hex 字符数 */
    char *hex = malloc((size_t)want + 2);
    if (hex == NULL) {
        fclose(f);
        errno = ENOMEM;
        return -1;
    }
    size_t got = fread(hex, 1, (size_t)want, f);
    fclose(f);
    hex[got] = '\0';
    if (got < want) {
        /* 状态文件比预期短：旧版本格式或写到一半被杀，只能重新开始 */
        fprintf(stderr, "holedl: 状态文件不完整（位图 %zu/%" PRIu64
                        " 字符），重新开始\n", got, want);
        free(hex);
        return 1;
    }

    if (bitmap_decode(hex, st->nblocks, st->done) != 0) {
        fprintf(stderr, "holedl: 状态文件位图格式非法，重新开始\n");
        free(hex);
        return 1;
    }
    free(hex);
    return 0;
}

/* ---------------- 主流程 ---------------- */

/*
 * 下载第 block_index 块。
 * range_unsupported 不为 NULL 时，若服务端对 Range 请求回了 200（整包），
 * 就把 *range_unsupported 置 true 并返回 0，让调用方决定是否走顺序兜底。
 */
static int download_block(const char *src, int outfd, uint64_t block_index,
                          uint64_t block_size, uint64_t total,
                          bool *range_unsupported)
{
    uint64_t start = block_index * block_size;
    uint64_t end   = start + block_size;
    if (end > total)
        end = total;
    uint64_t want = end - start;
    if (want == 0)
        return 0;

    if (is_http_url(src)) {
        struct http_resp r;
        char range[96];
        snprintf(range, sizeof(range), "Range: bytes=%" PRIu64 "-%" PRIu64 "\r\n",
                 start, end - 1);

        struct sink s = { .fd = outfd, .offset = start, .written = 0, .expect = want };
        if (http_request("GET", src, range, cb_to_file, &s, &r) != 0)
            return -1;

        if (r.status == 200) {
            /*
             * 服务端忽略了 Range，直接把整个文件传回来了。
             * 这批数据是按 seq 顺序流式写进去的，已经污染了 [0, written)，
             * 不能当成功处理 —— 交给调用方走「清空重下」的顺序兜底。
             */
            if (range_unsupported != NULL) {
                *range_unsupported = true;
                return 0;
            }
            errno = 0;
            return log_err("服务端不支持 Range（返回 200），无法定点续传");
        }
        if (r.status != 206) {
            errno = 0;
            return log_err("第 %" PRIu64 " 块请求失败 (HTTP %d)",
                           block_index, r.status);
        }
        if (s.written != want) {
            errno = 0;
            return log_err("第 %" PRIu64 " 块数据不完整：期望 %" PRIu64
                           " 字节，实收 %" PRIu64 " 字节",
                           block_index, want, s.written);
        }
        return 0;
    }

    /* 本地文件源：直接从源文件 pread，再定点 pwrite 到目标 */
    int infd = open(src, O_RDONLY);
    if (infd < 0)
        return log_err("打不开源文件 %s", src);

    char *buf = malloc((size_t)block_size);
    if (buf == NULL) {
        close(infd);
        errno = ENOMEM;
        return log_err("内存不足，块大小 %" PRIu64 " 太大", block_size);
    }

    ssize_t n = pread_full(infd, buf, (size_t)want, (off_t)start);
    close(infd);
    if (n < 0 || (uint64_t)n != want) {
        free(buf);
        errno = 0;
        return log_err("读源文件第 %" PRIu64 " 块失败", block_index);
    }

    ssize_t w = sparse_write_at(outfd, buf, (size_t)want, (off_t)start);
    free(buf);
    if (w < 0 || (uint64_t)w != want)
        return log_err("写目标文件第 %" PRIu64 " 块失败", block_index);

    return 0;
}

int download_run(const char *src, const char *out_path, uint64_t block_size)
{
    if (block_size == 0)
        block_size = DEFAULT_BLOCK;

    /* 1) 探测源大小 */
    uint64_t total = 0;
    if (dl_probe_size(src, &total) != 0)
        return -1;

    char hbuf[64];
    format_size(total, hbuf, sizeof(hbuf));
    printf("── 数据源   : %s\n", src);
    printf("── 总大小   : %s\n", hbuf);
    printf("── 输出文件 : %s\n", out_path);
    printf("── 分块大小 : %" PRIu64 " B\n", block_size);
    fflush(stdout);

    /* 2) 组装状态 */
    struct dl_state st;
    memset(&st, 0, sizeof(st));
    snprintf(st.src, sizeof(st.src), "%s", src);
    snprintf(st.out_path, sizeof(st.out_path), "%s", out_path);
    st.total      = total;
    st.block_size = block_size;
    st.nblocks    = total ? (total + block_size - 1) / block_size : 1;
    st.done       = calloc(st.nblocks, 1);
    if (st.done == NULL) {
        errno = ENOMEM;
        return log_err("内存不足");
    }

    /* 3) 目标文件：不存在就造空洞文件；存在就检查尺寸 */
    struct stat sb;
    bool out_exists = (stat(out_path, &sb) == 0);
    if (out_exists) {
        if ((uint64_t)sb.st_size != total) {
            fprintf(stderr, "holedl: 已存在文件大小不匹配(%" PRIu64 " != %" PRIu64
                            ")，重建空洞文件\n", (uint64_t)sb.st_size, total);
            out_exists = false;
        }
    }
    if (!out_exists) {
        if (sparse_create(out_path, total) != 0) {
            free(st.done);
            return -1;
        }
        printf("── 已创建空洞文件（逻辑 %s，实际占用 0）\n", hbuf);
    }

    /* 4) 恢复进度 */
    int rc = state_load(&st, false);
    if (rc < 0) {
        free(st.done);
        return -1;
    }

    uint64_t todo = 0;
    for (uint64_t i = 0; i < st.nblocks; i++)
        if (!st.done[i]) todo++;

    if (rc == 0) {
        printf("── 发现断点状态：共 %" PRIu64 " 块，已完成 %" PRIu64
               " 块，待续传 %" PRIu64 " 块\n",
               st.nblocks, st.nblocks - todo, todo);
    } else {
        printf("── 全新下载：共 %" PRIu64 " 块\n", st.nblocks);
    }

    if (todo == 0) {
        printf("── 所有块均已完成，无需下载 ✔\n");
        free(st.done);
        return 0;
    }

    /* 5) 打开目标文件（O_RDWR，不截断！） */
    int outfd = sparse_open_rw(out_path);
    if (outfd < 0) {
        free(st.done);
        return -1;
    }

    /* 6) 逐块下载 —— 每块完成立刻落盘状态，保证 Ctrl-C 安全 */
    int  ret      = 0;
    bool all_done = true;   /* 是否所有块都真正下完了 */

    for (uint64_t i = 0; i < st.nblocks; i++) {
        if (st.done[i])
            continue;

        printf("   [%3" PRIu64 "/%" PRIu64 "] 下载块……", i + 1, st.nblocks);
        fflush(stdout);

        bool range_unsupported = false;
        if (download_block(src, outfd, i, st.block_size, st.total,
                           &range_unsupported) != 0) {
            printf(" 失败\n");
            ret = -1;
            all_done = false;
            break;                        /* 保留已完成进度，可续传 */
        }

        if (range_unsupported) {
            /*
             * 服务端不支持 Range：放弃分块，改成「清空后整包顺序下载」。
             * 既然要重下整份，就必须把之前已写入的数据全部丢弃，
             * 否则新旧数据混在一起会得到错误的文件。
             */
            printf(" 服务端不支持 Range\n");
            printf("── 降级为顺序下载：清空目标文件重新整包下载"
                   "（该模式下中断需重头再来）\n");

            if (ftruncate(outfd, 0) != 0 ||
                ftruncate(outfd, (off_t)st.total) != 0) {
                ret = log_err("重置目标文件失败");
                all_done = false;
                break;
            }
            /* 之前记下的进度全部作废 */
            memset(st.done, 0, st.nblocks);
            {
                char sp[4200];
                state_path(out_path, sp, sizeof(sp));
                unlink(sp);
            }

            if (dl_sequential_fallback(src, outfd, st.total) != 0) {
                printf("── 顺序下载失败\n");
                ret = -1;
                all_done = false;
                break;
            }

            printf("── 顺序下载完成 ✔\n");
            all_done = true;
            ret = 0;
            break;                        /* 整份已经下完，直接结束 */
        }

        st.done[i] = 1;
        if (state_save(&st) != 0) {
            printf(" 状态保存失败\n");
            ret = -1;
            all_done = false;
            break;
        }

        /* 让元数据尽快可见，同时不影响空洞 */
        if (fdatasync(outfd) != 0) {
            printf(" 落盘失败\n");
            ret = -1;
            all_done = false;
            break;
        }
        printf(" ok\n");
    }

    close(outfd);

    if (ret == 0 && all_done) {
        printf("── 下载完成 ✔\n");
        char sp[4200];
        state_path(out_path, sp, sizeof(sp));
        unlink(sp);                       /* 全部完成，清掉状态文件 */
    } else if (ret != 0) {
        printf("── 中断，进度已保存，重新执行同一条命令即可续传\n");
    }

    free(st.done);
    return ret;
}
