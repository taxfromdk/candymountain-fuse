/*
 * strbuf - a growable byte buffer for building text (JSON, HTTP headers).
 *
 * No hidden allocation beyond the buffer itself; every call that may grow
 * the buffer reports failure through sb->oom, which sticks, so a long
 * sequence of appends is checked once at the end.
 */
#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *data;   /* NUL-terminated at data[len] */
    size_t len;
    size_t cap;
    int    oom;    /* set when an allocation failed; the buffer is then
                      left as it was before the failing append */
} sb_t;

void sb_init(sb_t *sb);
void sb_free(sb_t *sb);

/* Append raw bytes / a string / printf-formatted text. */
int sb_append(sb_t *sb, const void *p, size_t n);
int sb_puts(sb_t *sb, const char *s);
int sb_printf(sb_t *sb, const char *fmt, ...);

/* Append s as a JSON string literal with quotes and escapes. */
int sb_json_str(sb_t *sb, const char *s);

/* Hand the buffer to the caller (malloc'd, NUL-terminated); sb is reset. */
char *sb_detach(sb_t *sb, size_t *len);

#endif /* STRBUF_H */
