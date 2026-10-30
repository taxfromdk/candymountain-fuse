#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct json {
    json_type_t type;
    union {
        int     b;
        struct { double d; int64_t i; int is_int; } n;
        char   *s;
        struct { json_t **items; char **keys; size_t n, cap; } c; /* array/object */
    } u;
};

typedef struct {
    const char *p, *end;
    int depth;
} parser_t;

#define MAX_DEPTH 64

static json_t *parse_value(parser_t *ps);

static json_t *node(json_type_t t)
{
    json_t *j = calloc(1, sizeof(*j));

    if (j != NULL)
        j->type = t;
    return j;
}

void json_free(json_t *j)
{
    if (j == NULL)
        return;
    switch (j->type) {
    case JSON_STRING:
        free(j->u.s);
        break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        for (size_t i = 0; i < j->u.c.n; i++) {
            json_free(j->u.c.items[i]);
            if (j->u.c.keys != NULL)
                free(j->u.c.keys[i]);
        }
        free(j->u.c.items);
        free(j->u.c.keys);
        break;
    default:
        break;
    }
    free(j);
}

static void skip_ws(parser_t *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int push(json_t *c, const char *key, json_t *v)
{
    if (c->u.c.n == c->u.c.cap) {
        size_t cap = c->u.c.cap ? c->u.c.cap * 2 : 8;
        json_t **items = realloc(c->u.c.items, cap * sizeof(*items));

        if (items == NULL)
            return -1;
        c->u.c.items = items;
        if (c->type == JSON_OBJECT) {
            char **keys = realloc(c->u.c.keys, cap * sizeof(*keys));

            if (keys == NULL)
                return -1;
            c->u.c.keys = keys;
        }
        c->u.c.cap = cap;
    }
    c->u.c.items[c->u.c.n] = v;
    if (c->type == JSON_OBJECT) {
        c->u.c.keys[c->u.c.n] = strdup(key);
        if (c->u.c.keys[c->u.c.n] == NULL)
            return -1;
    }
    c->u.c.n++;
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t utf8_put(char *out, uint32_t cp)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Parse a string starting at the opening quote; returns malloc'd text. */
static char *parse_string(parser_t *ps)
{
    char *out;
    size_t o = 0, cap;

    if (ps->p >= ps->end || *ps->p != '"')
        return NULL;
    ps->p++;
    cap = (size_t)(ps->end - ps->p) + 1;   /* unescaping never grows */
    out = malloc(cap);
    if (out == NULL)
        return NULL;
    while (ps->p < ps->end && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p++;

        if (c < 0x20)
            goto bad;
        if (c != '\\') {
            out[o++] = (char)c;
            continue;
        }
        if (ps->p >= ps->end)
            goto bad;
        c = (unsigned char)*ps->p++;
        switch (c) {
        case '"': case '\\': case '/': out[o++] = (char)c; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            uint32_t cp = 0;

            if (ps->end - ps->p < 4)
                goto bad;
            for (int k = 0; k < 4; k++) {
                int h = hexval(ps->p[k]);

                if (h < 0)
                    goto bad;
                cp = cp * 16 + (uint32_t)h;
            }
            ps->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {   /* surrogate pair */
                uint32_t lo = 0;

                if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u')
                    goto bad;
                for (int k = 0; k < 4; k++) {
                    int h = hexval(ps->p[2 + k]);

                    if (h < 0)
                        goto bad;
                    lo = lo * 16 + (uint32_t)h;
                }
                if (lo < 0xDC00 || lo > 0xDFFF)
                    goto bad;
                ps->p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            o += utf8_put(out + o, cp);
            break;
        }
        default:
            goto bad;
        }
    }
    if (ps->p >= ps->end)
        goto bad;
    ps->p++;                                 /* closing quote */
    out[o] = '\0';
    return out;
bad:
    free(out);
    return NULL;
}

static json_t *parse_number(parser_t *ps)
{
    const char *start = ps->p;
    char buf[64];
    size_t n;
    json_t *j;
    char *end;

    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;
    while (ps->p < ps->end &&
           ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' || *ps->p == 'e' ||
            *ps->p == 'E' || *ps->p == '+' || *ps->p == '-'))
        ps->p++;
    n = (size_t)(ps->p - start);
    if (n == 0 || n >= sizeof(buf))
        return NULL;
    memcpy(buf, start, n);
    buf[n] = '\0';
    j = node(JSON_NUMBER);
    if (j == NULL)
        return NULL;
    errno = 0;
    j->u.n.d = strtod(buf, &end);
    if (end != buf + n) {
        free(j);
        return NULL;
    }
    if (strpbrk(buf, ".eE") == NULL) {
        errno = 0;
        j->u.n.i = strtoll(buf, &end, 10);
        j->u.n.is_int = errno == 0;
    } else if (j->u.n.d == floor(j->u.n.d) && fabs(j->u.n.d) < 9.0e18) {
        j->u.n.i = (int64_t)j->u.n.d;
        j->u.n.is_int = 1;
    }
    return j;
}

static json_t *parse_container(parser_t *ps, json_type_t type)
{
    char close = type == JSON_ARRAY ? ']' : '}';
    json_t *c;

    if (++ps->depth > MAX_DEPTH)
        return NULL;
    c = node(type);
    if (c == NULL)
        return NULL;
    ps->p++;                                 /* [ or { */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return c;
    }
    for (;;) {
        char *key = NULL;
        json_t *v;

        skip_ws(ps);
        if (type == JSON_OBJECT) {
            key = parse_string(ps);
            if (key == NULL)
                goto bad;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                goto bad;
            }
            ps->p++;
            skip_ws(ps);
        }
        v = parse_value(ps);
        if (v == NULL || push(c, key, v) != 0) {
            free(key);
            json_free(v);
            goto bad;
        }
        free(key);
        skip_ws(ps);
        if (ps->p >= ps->end)
            goto bad;
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == close) {
            ps->p++;
            ps->depth--;
            return c;
        }
        goto bad;
    }
bad:
    json_free(c);
    return NULL;
}

static int match(parser_t *ps, const char *lit)
{
    size_t n = strlen(lit);

    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, lit, n) == 0) {
        ps->p += n;
        return 1;
    }
    return 0;
}

static json_t *parse_value(parser_t *ps)
{
    json_t *j;

    skip_ws(ps);
    if (ps->p >= ps->end)
        return NULL;
    switch (*ps->p) {
    case '{': return parse_container(ps, JSON_OBJECT);
    case '[': return parse_container(ps, JSON_ARRAY);
    case '"': {
        char *s = parse_string(ps);

        if (s == NULL)
            return NULL;
        j = node(JSON_STRING);
        if (j == NULL) {
            free(s);
            return NULL;
        }
        j->u.s = s;
        return j;
    }
    case 't':
        if (!match(ps, "true")) return NULL;
        j = node(JSON_BOOL);
        if (j) j->u.b = 1;
        return j;
    case 'f':
        if (!match(ps, "false")) return NULL;
        return node(JSON_BOOL);
    case 'n':
        if (!match(ps, "null")) return NULL;
        return node(JSON_NULL);
    default:
        return parse_number(ps);
    }
}

json_t *json_parse(const char *text, size_t len, size_t *errpos)
{
    parser_t ps = { text, text + len, 0 };
    json_t *j = parse_value(&ps);

    if (j != NULL) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            json_free(j);
            j = NULL;
        }
    }
    if (j == NULL && errpos != NULL)
        *errpos = (size_t)(ps.p - text);
    return j;
}

json_type_t json_type(const json_t *j)
{
    return j ? j->type : JSON_NULL;
}

const json_t *json_get(const json_t *obj, const char *key)
{
    if (obj == NULL || obj->type != JSON_OBJECT || key == NULL)
        return NULL;
    for (size_t i = 0; i < obj->u.c.n; i++)
        if (strcmp(obj->u.c.keys[i], key) == 0)
            return obj->u.c.items[i];
    return NULL;
}

const json_t *json_at(const json_t *j, size_t i)
{
    if (j == NULL || (j->type != JSON_ARRAY && j->type != JSON_OBJECT) ||
        i >= j->u.c.n)
        return NULL;
    return j->u.c.items[i];
}

const char *json_key_at(const json_t *obj, size_t i)
{
    if (obj == NULL || obj->type != JSON_OBJECT || i >= obj->u.c.n)
        return NULL;
    return obj->u.c.keys[i];
}

size_t json_len(const json_t *j)
{
    if (j == NULL)
        return 0;
    if (j->type == JSON_ARRAY || j->type == JSON_OBJECT)
        return j->u.c.n;
    if (j->type == JSON_STRING)
        return strlen(j->u.s);
    return 0;
}

const char *json_str(const json_t *j, const char *dflt)
{
    return j && j->type == JSON_STRING ? j->u.s : dflt;
}

double json_num(const json_t *j, double dflt)
{
    return j && j->type == JSON_NUMBER ? j->u.n.d : dflt;
}

int64_t json_int(const json_t *j, int64_t dflt)
{
    if (j == NULL || j->type != JSON_NUMBER)
        return dflt;
    return j->u.n.is_int ? j->u.n.i : (int64_t)j->u.n.d;
}

int json_bool(const json_t *j, int dflt)
{
    return j && j->type == JSON_BOOL ? j->u.b : dflt;
}
