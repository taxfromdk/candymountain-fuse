/*
 * json - a small JSON parser for the server's answers.
 *
 * Parses a complete document into a tree; values are looked up by key or
 * position. Numbers keep both a double and, when they are integral and fit,
 * an int64. Strings are unescaped (\n, \", \uXXXX to UTF-8, ...). No
 * streaming, no writing: the answers are at most a few megabytes (a frame
 * listing page) and are parsed once.
 */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} json_type_t;

typedef struct json json_t;

/* Parse len bytes; NULL on a syntax error (errpos, if given, says where). */
json_t *json_parse(const char *text, size_t len, size_t *errpos);
void json_free(json_t *j);

json_type_t json_type(const json_t *j);

/* Object member by key (NULL if absent or not an object). */
const json_t *json_get(const json_t *obj, const char *key);
/* Array element / object member by position; count of either. */
const json_t *json_at(const json_t *j, size_t i);
const char   *json_key_at(const json_t *obj, size_t i);
size_t        json_len(const json_t *j);

/* Value accessors; the defaults are returned for a missing or mistyped
 * value, so chains like json_int(json_get(o, "frames"), 0) are safe. */
const char *json_str(const json_t *j, const char *dflt);
double      json_num(const json_t *j, double dflt);
int64_t     json_int(const json_t *j, int64_t dflt);
int         json_bool(const json_t *j, int dflt);

#endif /* JSON_H */
