/*
 * api - HTTP GET against an ixserve server, with libcurl.
 *
 * One libcurl easy handle per thread, kept between calls so connections
 * stay alive (FUSE serves requests on several threads; each gets its own).
 * Answers come back whole in memory: a frame is a few hundred kilobytes to
 * 25 MB, a listing page a few megabytes.
 */
#ifndef API_H
#define API_H

#include "json.h"

#include <stddef.h>
#include <stdint.h>

/* Once per process, before any other call; and once at the end. */
int  api_global_init(void);
void api_global_cleanup(void);

/*
 * GET base + path. On success (HTTP 200) returns 0 and hands over the
 * body (malloc'd, NUL-terminated beyond len; caller frees). Otherwise a
 * negative errno: -ENOENT for 404, -EAGAIN for 503, -EIO for other
 * statuses and transport errors, -ENOMEM. *status gets the HTTP status
 * (0 when the request never got an answer).
 */
int api_get(const char *base, const char *path, uint8_t **body, size_t *len,
            long *status);

/* GET and parse as JSON; -EIO when the body is not JSON. */
int api_get_json(const char *base, const char *path, json_t **out);

/* The request timeout in seconds (default 120; a cold P frame can take a
 * few seconds on a busy server). */
void api_set_timeout(long seconds);

#endif /* API_H */
