/* Bounded local credentials. Never put secret contents in a channel event. */
#ifndef ES_CREDENTIALS_H
#define ES_CREDENTIALS_H
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define ES_SECRET_MAX 1024
static inline void es_secret_wipe(void *p, size_t n) {
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) *q++ = 0;
}
static inline int es_secret_copy(char out[ES_SECRET_MAX + 1], const char *value) {
    size_t n, i;
    memset(out, 0, ES_SECRET_MAX + 1);
    if (!value || !(n = strlen(value)) || n > ES_SECRET_MAX) return -1;
    for (i = 0; i < n; i++)
        if ((unsigned char)value[i] < 33 || (unsigned char)value[i] > 126) return -1;
    memcpy(out, value, n);
    return 0;
}
/* Only regular files, no symlinks, no group/world write or world read permissions.
 * The FS user must have read access (e.g. root:callcenter 0640). */
static inline int es_secret_read(char out[ES_SECRET_MAX + 1], const char *path) {
    char buf[ES_SECRET_MAX + 3];
    struct stat info;
    ssize_t n, part;
    int fd, result = -1;
    memset(out, 0, ES_SECRET_MAX + 1);
    if (!path || path[0] != '/') return -1;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || (info.st_mode & 0027)) goto done;
    n = 0;
    while (n < (ssize_t)sizeof(buf)) {
        part = read(fd, buf + n, sizeof(buf) - (size_t)n);
        if (part < 0) goto done;
        if (!part) break;
        n += part;
    }
    if (!n || n == (ssize_t)sizeof(buf)) goto done;
    if (buf[n - 1] == '\n') n--;
    if (n && buf[n - 1] == '\r') n--;
    if (!n || n > ES_SECRET_MAX || memchr(buf, '\0', (size_t)n)) goto done;
    buf[n] = 0;
    result = es_secret_copy(out, buf);
done:
    es_secret_wipe(buf, sizeof(buf));
    close(fd);
    return result;
}
#endif
