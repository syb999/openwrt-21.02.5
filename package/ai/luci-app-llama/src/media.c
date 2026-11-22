/* strcasestr 在 musl 里要 _GNU_SOURCE 才有声明，否则返回类型被当成 int，
 * 64 位指针会被截断成 32 位 —— 表现是带 Range 的请求直接空回复/段错误。 */
#define _GNU_SOURCE
/* mediad.c — 极简视频文件服务（唯一目的：支持 HTTP Range）
 *
 * 为什么不用 uhttpd：
 *   ① uhttpd 21.02 不把 Range 头传给 CGI（实测 CGI 环境里只有 ACCEPT /
 *      HOST / USER_AGENT，没有 HTTP_RANGE）
 *   ② 它自己也不给静态文件做 Range（OpenWrt 的已知限制）
 * 而 <video> 拖进度条、Safari/iOS 播放 mp4 都要求服务器能回 206。
 *
 * 所以另起一个 8091 上的小服务：
 *   GET /f/<文件名>        → 在几个白名单目录里按文件名找，支持 ?range
 *   GET /health            → {"ok":1}
 * 只读、只服务视频/图片/音频扩展名，不执行任何东西。
 *
 * 编译：aarch64-openwrt-linux-musl-gcc -O2 -o mediad media.c（静态）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8091

static const char *DIRS[] = {
    "/opt/llama/out",
    "/tmp/llama_agent/gen",
    "/tmp/llama_agent/downloads",
    "/tmp/llama_agent/shots",
    "/tmp/llama_agent",
    "/opt/mp4",
    NULL
};

static const char *mime_of(const char *n) {
    const char *e = strrchr(n, '.');
    if (!e) return "application/octet-stream";
    if (!strcasecmp(e, ".mp4") || !strcasecmp(e, ".m4v")) return "video/mp4";
    if (!strcasecmp(e, ".webm")) return "video/webm";
    if (!strcasecmp(e, ".mov"))  return "video/quicktime";
    if (!strcasecmp(e, ".mkv"))  return "video/x-matroska";
    if (!strcasecmp(e, ".mp3"))  return "audio/mpeg";
    if (!strcasecmp(e, ".wav"))  return "audio/wav";
    if (!strcasecmp(e, ".png"))  return "image/png";
    if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(e, ".gif"))  return "image/gif";
    return "application/octet-stream";
}

/* 文件名只允许这些字符，杜绝 ../ 之类的穿越 */
static int name_ok(const char *n) {
    if (!n || !n[0] || strlen(n) > 200) return 0;
    for (const char *p = n; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_' && *p != '+')
            return 0;
    return 1;
}

static void send_err(int fd, int code, const char *msg) {
    char b[256];
    int n = snprintf(b, sizeof(b),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s", code, msg, strlen(msg), msg);
    write(fd, b, (size_t)n);
}

static void handle(int fd) {
    char req[2048];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) return;
    req[n] = 0;

    char method[16] = {0}, url[1024] = {0};
    if (sscanf(req, "%15s %1023s", method, url) != 2) { send_err(fd, 400, "bad request"); return; }

    if (!strcmp(url, "/health")) {
        const char *ok = "{\"ok\":1}";
        char b[160];
        int m = snprintf(b, sizeof(b),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(ok), ok);
        write(fd, b, (size_t)m);
        return;
    }

    /* 解析 Range 头 */
    long long rstart = -1, rend = -1;
    const char *r = strcasestr(req, "\r\nRange:");
    if (r) {
        const char *b = strstr(r, "bytes=");
        if (b) {
            b += 6;
            if (*b == '-') {                    /* 后缀 bytes=-N */
                rstart = -2;
                rend = atoll(b + 1);
            } else {
                rstart = atoll(b);
                const char *d = strchr(b, '-');
                if (d && d[1] >= '0' && d[1] <= '9') rend = atoll(d + 1);
                else rend = -1;                 /* 到结尾 */
            }
        }
    }

    /* /f/<name> */
    if (strncmp(url, "/f/", 3) != 0) { send_err(fd, 404, "use /f/<file>"); return; }
    char name[256];
    snprintf(name, sizeof(name), "%s", url + 3);
    char *q = strchr(name, '?');
    if (q) *q = 0;
    if (!name_ok(name)) { send_err(fd, 400, "bad name"); return; }

    char hit[1024] = {0};
    for (int i = 0; DIRS[i]; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", DIRS[i], name);
        if (access(p, R_OK) == 0) { snprintf(hit, sizeof(hit), "%s", p); break; }
    }
    if (!hit[0]) { send_err(fd, 404, "file not found"); return; }

    struct stat st;
    if (stat(hit, &st) != 0) { send_err(fd, 404, "stat failed"); return; }

    long long start = 0, end = st.st_size - 1;
    int partial = 0;
    if (rstart != -1) {
        if (rstart == -2) {                     /* 最后 N 字节 */
            if (rend > 0) {
                start = st.st_size - rend;
                if (start < 0) start = 0;
                end = st.st_size - 1;
                partial = 1;
            }
        } else if (rstart >= 0 && rstart < st.st_size) {
            start = rstart;
            end = (rend >= rstart && rend < st.st_size) ? rend : st.st_size - 1;
            partial = 1;
        }
    }

    char hdr[600];
    int hn;
    if (partial)
        hn = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
            "Accept-Ranges: bytes\r\nContent-Range: bytes %lld-%lld/%lld\r\n"
            "Content-Length: %lld\r\nConnection: close\r\n"
            "Cache-Control: no-store\r\n\r\n",
            mime_of(hit), start, end, (long long)st.st_size, end - start + 1);
    else
        hn = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
            "Accept-Ranges: bytes\r\nContent-Length: %lld\r\n"
            "Connection: close\r\nCache-Control: no-store\r\n\r\n",
            mime_of(hit), (long long)st.st_size);
    if (write(fd, hdr, (size_t)hn) < 0) return;

    int f = open(hit, O_RDONLY);
    if (f < 0) return;
    lseek(f, (off_t)start, SEEK_SET);
    char buf[32768];
    long long left = end - start + 1;
    while (left > 0) {
        size_t want = left > (long long)sizeof(buf) ? sizeof(buf) : (size_t)left;
        ssize_t got = read(f, buf, want);
        if (got <= 0) break;
        if (write(fd, buf, (size_t)got) < 0) break;
        left -= got;
    }
    close(f);
}

int main(int argc, char **argv) {
    int port = PORT;
    if (argc > 1) port = atoi(argv[1]);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);          /* 子进程自动回收 */

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("bind"); return 1; }
    if (listen(s, 16) != 0) { perror("listen"); return 1; }

    fprintf(stderr, "mediad listening on :%d\n", port);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        pid_t pid = fork();
        if (pid == 0) {
            close(s);
            handle(c);
            close(c);
            _exit(0);
        }
        close(c);
    }
    return 0;
}
