#include "utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *br_read_file(const char *path, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (out_size) {
        *out_size = n;
    }
    return buf;
}

noreturn void br_fatal_at(const char *path, int line, int col, const char *fmt, ...)
{
    fprintf(stderr, "%s:%d:%d: erro: ", path ? path : "<?>", line, col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

noreturn void br_fatal(const char *fmt, ...)
{
    fprintf(stderr, "brc: erro: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *br_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        br_fatal("memoria insuficiente (malloc %zu)", n);
    }
    return p;
}

void *br_xcalloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p) {
        br_fatal("memoria insuficiente (calloc %zu x %zu)", n, size);
    }
    return p;
}

void *br_xrealloc(void *p, size_t n)
{
    void *np = realloc(p, n);
    if (!np) {
        br_fatal("memoria insuficiente (realloc %zu)", n);
    }
    return np;
}

char *br_xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *r = (char *)br_xmalloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

char *br_xstrndup(const char *s, size_t n)
{
    char *r = (char *)br_xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}
