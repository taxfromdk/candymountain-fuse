#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(sb_t *sb)
{
    sb->data = NULL;
    sb->len = sb->cap = 0;
    sb->oom = 0;
}

void sb_free(sb_t *sb)
{
    free(sb->data);
    sb_init(sb);
}

static int sb_reserve(sb_t *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    size_t cap;
    char *p;

    if (need <= sb->cap)
        return 0;
    cap = sb->cap ? sb->cap : 256;
    while (cap < need)
        cap *= 2;
    p = realloc(sb->data, cap);
    if (p == NULL) {
        sb->oom = 1;
        return -1;
    }
    sb->data = p;
    sb->cap = cap;
    return 0;
}

int sb_append(sb_t *sb, const void *p, size_t n)
{
    if (sb_reserve(sb, n) != 0)
        return -1;
    memcpy(sb->data + sb->len, p, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_puts(sb_t *sb, const char *s)
{
    return sb_append(sb, s, strlen(s));
}

int sb_printf(sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || sb_reserve(sb, (size_t)n) != 0)
        return -1;
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
    return 0;
}

int sb_json_str(sb_t *sb, const char *s)
{
    if (sb_append(sb, "\"", 1) != 0)
        return -1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char esc[8];

        switch (c) {
        case '"':  if (sb_append(sb, "\\\"", 2)) return -1; break;
        case '\\': if (sb_append(sb, "\\\\", 2)) return -1; break;
        case '\n': if (sb_append(sb, "\\n", 2))  return -1; break;
        case '\r': if (sb_append(sb, "\\r", 2))  return -1; break;
        case '\t': if (sb_append(sb, "\\t", 2))  return -1; break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                if (sb_append(sb, esc, 6))
                    return -1;
            } else if (sb_append(sb, &c, 1)) {
                return -1;
            }
        }
    }
    return sb_append(sb, "\"", 1);
}

char *sb_detach(sb_t *sb, size_t *len)
{
    char *p = sb->data;

    if (p == NULL) {          /* nothing was ever appended */
        p = malloc(1);
        if (p != NULL)
            p[0] = '\0';
    }
    if (len != NULL)
        *len = sb->len;
    sb_init(sb);
    return p;
}
