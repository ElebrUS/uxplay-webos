#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int send_all(int fd, const void *ptr, size_t remaining) {
    const char *bytes = ptr;
    while (remaining) {
        ssize_t sent = send(fd, bytes, remaining, 0);
        if (sent <= 0) return -1;
        bytes += sent;
        remaining -= (size_t)sent;
    }
    return 0;
}

static void serve(int fd) {
    char req[8192] = {0}, method[12], path[256], range[128] = {0}, header[512];
    size_t received = 0;
    struct timeval timeout = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (!strstr(req, "\r\n\r\n") && received < sizeof(req) - 1) {
        ssize_t len = recv(fd, req + received, sizeof(req) - received - 1, 0);
        if (len <= 0) return;
        received += (size_t)len;
    }
    char *end_headers = strstr(req, "\r\n\r\n");
    if (!end_headers || sscanf(req, "%11s %255s", method, path) != 2) return;
    *end_headers = '\0';
    const int head = strcmp(method, "HEAD") == 0;
    if (!head && strcmp(method, "GET")) return;
    const char *file = NULL, *mime = NULL;
    if (!strcmp(path, "/")) {
        file = "index.html";
        mime = "text/html; charset=utf-8";
    } else if (!strcmp(path, "/sample.mp4")) {
        file = "sample.mp4";
        mime = "video/mp4";
    }
    int src = file ? open(file, O_RDONLY) : -1;
    if (src < 0) {
        static const char missing[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, missing, sizeof(missing) - 1);
        return;
    }
    struct stat st;
    if (fstat(src, &st) || st.st_size <= 0) { close(src); return; }
    long first = 0, last = (long)st.st_size - 1;
    for (char *line = strstr(req, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        if (!strncasecmp(line + 2, "Range:", 6)) {
            sscanf(line + 8, "%127s", range);
            break;
        }
    }
    int partial = 0;
    if (!strncmp(range, "bytes=", 6)) {
        char *split;
        long start = strtol(range + 6, &split, 10);
        if (*split == '-' && split != range + 6 && start >= 0 && start <= last) {
            first = start;
            if (isdigit((unsigned char)split[1])) {
                long stop = strtol(split + 1, NULL, 10);
                if (stop >= first && stop < last) last = stop;
            }
            partial = 1;
        }
    }
    const long length = last - first + 1;
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Accept-Ranges: bytes\r\nConnection: close\r\n",
                     partial ? "206 Partial Content" : "200 OK", mime, length);
    if (partial) n += snprintf(header + n, sizeof(header) - (size_t)n,
                               "Content-Range: bytes %ld-%ld/%ld\r\n", first, last, (long)st.st_size);
    n += snprintf(header + n, sizeof(header) - (size_t)n, "\r\n");
    fprintf(stderr, "%s %s -> %d\n", method, path, partial ? 206 : 200);
    if (send_all(fd, header, (size_t)n) == 0 && !head && lseek(src, first, SEEK_SET) >= 0) {
        char chunk[32768];
        long remaining = length;
        while (remaining > 0) {
            ssize_t size = read(src, chunk, remaining > (long)sizeof(chunk) ? sizeof(chunk) : (size_t)remaining);
            if (size <= 0 || send_all(fd, chunk, (size_t)size)) break;
            remaining -= size;
        }
    }
    close(src);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    signal(SIGPIPE, SIG_IGN);
    int listener = socket(AF_INET, SOCK_STREAM, 0), reuse = 1;
    if (listener < 0) return 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(8872);
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 8)) {
        perror("fixture server");
        close(listener);
        return 1;
    }
    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0 && errno == EINTR) continue;
        if (client < 0) break;
        serve(client);
        close(client);
    }
    close(listener);
    return 0;
}
