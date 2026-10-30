#include "api.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static long api_timeout = 120;
static pthread_key_t handle_key;
static int key_made;

static void handle_destroy(void *p)
{
    if (p != NULL)
        curl_easy_cleanup(p);
}

int api_global_init(void)
{
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
        return -1;
    if (pthread_key_create(&handle_key, handle_destroy) != 0)
        return -1;
    key_made = 1;
    return 0;
}

void api_global_cleanup(void)
{
    if (key_made) {
        /* the calling thread's handle; other threads' are freed on exit */
        handle_destroy(pthread_getspecific(handle_key));
        pthread_setspecific(handle_key, NULL);
        pthread_key_delete(handle_key);
        key_made = 0;
    }
    curl_global_cleanup();
}

void api_set_timeout(long seconds)
{
    api_timeout = seconds > 0 ? seconds : 120;
}

static CURL *thread_handle(void)
{
    CURL *h = pthread_getspecific(handle_key);

    if (h == NULL) {
        h = curl_easy_init();
        if (h != NULL)
            pthread_setspecific(handle_key, h);
    }
    return h;
}

typedef struct {
    uint8_t *data;
    size_t   len, cap;
    int      oom;
} buf_t;

static size_t on_data(char *p, size_t sz, size_t n, void *ud)
{
    buf_t *b = ud;
    size_t add = sz * n;

    if (b->len + add + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        uint8_t *d;

        while (cap < b->len + add + 1)
            cap *= 2;
        d = realloc(b->data, cap);
        if (d == NULL) {
            b->oom = 1;
            return 0;                        /* aborts the transfer */
        }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, add);
    b->len += add;
    b->data[b->len] = '\0';
    return add;
}

int api_get(const char *base, const char *path, uint8_t **body, size_t *len,
            long *status)
{
    CURL *h = thread_handle();
    char *url;
    size_t bl = strlen(base), pl = strlen(path);
    buf_t b = { NULL, 0, 0, 0 };
    CURLcode cc;
    long st = 0;
    int rc;

    *body = NULL;
    *len = 0;
    if (status != NULL)
        *status = 0;
    if (h == NULL)
        return -ENOMEM;
    url = malloc(bl + pl + 1);
    if (url == NULL)
        return -ENOMEM;
    memcpy(url, base, bl);
    memcpy(url + bl, path, pl + 1);

    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, api_timeout);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "ixmount/1");
    cc = curl_easy_perform(h);
    free(url);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &st);
    if (status != NULL)
        *status = st;
    if (b.oom) {
        free(b.data);
        return -ENOMEM;
    }
    if (cc != CURLE_OK) {
        free(b.data);
        return -EIO;
    }
    if (st == 200) {
        if (b.data == NULL) {                /* empty body: still valid */
            b.data = calloc(1, 1);
            if (b.data == NULL)
                return -ENOMEM;
        }
        *body = b.data;
        *len = b.len;
        return 0;
    }
    free(b.data);
    rc = st == 404 ? -ENOENT : st == 503 ? -EAGAIN : -EIO;
    return rc;
}

int api_get_json(const char *base, const char *path, json_t **out)
{
    uint8_t *body;
    size_t len;
    int rc = api_get(base, path, &body, &len, NULL);

    *out = NULL;
    if (rc != 0)
        return rc;
    *out = json_parse((const char *)body, len, NULL);
    free(body);
    return *out ? 0 : -EIO;
}
