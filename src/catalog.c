#include "catalog.h"

#include "api.h"
#include "json.h"
#include "strbuf.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NAME_WIDTH   7             /* frame names: %07zu_... as ixfs did */
#define PAGE         100000        /* frames per listing request */
#define MAX_VIEWS    8
#define Y4M_HDR_MAX  64

/* ---- data model ---------------------------------------------------------- */

typedef struct {
    char     name[32];
    int      pid;              /* -1 for P */
    uint32_t frames;
    int      is_p;
    int      has_h265;         /* every video view except P */
    double   duration_s;
    /* frame table (loaded on demand) */
    uint64_t *pts;             /* [frames] (P: pts of L) */
    uint64_t *pts_r;           /* P only: pts of R */
    uint16_t *ko;              /* key offset */
    uint32_t *psize;           /* payload bytes (h265), 0 for P */
    int       table_loaded;
    int       width, height;   /* decoded geometry (0 until known) */
    uint32_t  params_bytes;    /* prepended to h265 keys */
    uint64_t  last_use;
} view_t;

typedef struct {
    char    name[256];
    char    media[1024];
    char    ix[1024];
    int     loaded;            /* server-side state at listing time */
    int64_t cal_bytes;         /* size of camera_alignment_settings.json, -1: none */
    view_t  views[MAX_VIEWS];
    size_t  nviews;
    int     unreadable;
} rec_t;

typedef struct {
    size_t  id;
    char    path[1024];
    size_t  recordings;
    uint64_t l, r, p;
    double   duration_s;
    rec_t  *recs;              /* fetched on demand */
    size_t  nrecs;
    time_t  recs_time;
} index_t;

struct frame_blob {
    char      key[512];
    uint8_t  *data;
    size_t    len;
    unsigned  pins;
    uint64_t  last_use;
    struct frame_blob *prev, *next;   /* LRU list: head = most recent */
};

struct catalog {
    char      base[1024];
    int       ttl;
    uint64_t  frame_budget, entry_budget;

    pthread_mutex_t lock;             /* metadata */
    index_t  *idx;
    size_t    nidx;
    time_t    idx_time;
    uint64_t  clock;
    uint64_t  table_entries;

    pthread_mutex_t clock_lock;
    frame_blob_t *head, *tail;
    uint64_t  cache_bytes;
    uint32_t  cache_entries;
    uint64_t  frames_fetched, bytes_fetched, cache_hits;
};

/* ---- helpers --------------------------------------------------------------- */

static void copy_str(char *dst, size_t cap, const char *s)
{
    size_t l = s ? strlen(s) : 0;

    if (l > cap - 1)
        l = cap - 1;
    memcpy(dst, s, l);
    dst[l] = '\0';
}

/* Percent-encode a path component for a URL. */
static void url_enc(sb_t *sb, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            sb_append(sb, &c, 1);
        else
            sb_printf(sb, "%%%c%c", hex[c >> 4], hex[c & 15]);
    }
}

static int fmt_ok(const char *fmt, const view_t *v)
{
    if (strcmp(fmt, "jpg") == 0 || strcmp(fmt, "bmp") == 0 ||
        strcmp(fmt, "yuv") == 0 || strcmp(fmt, "y4m") == 0)
        return 1;
    return strcmp(fmt, "h265") == 0 && v->has_h265;
}

static const char *const FMTS[] = { "jpg", "bmp", "yuv", "y4m", "h265" };
static const char CAL_NAME[] = "camera_alignment_settings.json";   /* beside the view folders */

/* ---- fetching ---------------------------------------------------------------- */

/* The index list: /api/?info=1 */
static int load_indexes(catalog_t *c)
{
    json_t *j;
    const json_t *items;
    int rc;

    if (c->idx != NULL && time(NULL) - c->idx_time < c->ttl)
        return 0;
    rc = api_get_json(c->base, "/api/?info=1", &j);
    if (rc != 0)
        return c->idx != NULL ? 0 : rc;      /* keep a stale list over none */
    items = json_get(j, "items");
    {
        size_t n = json_len(items);
        index_t *arr = calloc(n ? n : 1, sizeof(index_t));

        if (arr == NULL) {
            json_free(j);
            return -ENOMEM;
        }
        /* carry the already fetched recordings over by id */
        for (size_t i = 0; i < n; i++) {
            const json_t *it = json_at(items, i);
            const json_t *fr = json_get(it, "frames");
            index_t *x = &arr[i];

            x->id = (size_t)json_int(json_get(it, "id"), (int64_t)i);
            copy_str(x->path, sizeof(x->path), json_str(json_get(it, "path"), "?"));
            x->recordings = (size_t)json_int(json_get(it, "recordings"), 0);
            x->l = (uint64_t)json_int(json_get(fr, "L"), 0);
            x->r = (uint64_t)json_int(json_get(fr, "R"), 0);
            x->p = (uint64_t)json_int(json_get(fr, "P"), 0);
            x->duration_s = json_num(json_get(it, "duration_s"), 0);
            for (size_t k = 0; k < c->nidx; k++)
                if (c->idx[k].id == x->id) {
                    x->recs = c->idx[k].recs;
                    x->nrecs = c->idx[k].nrecs;
                    x->recs_time = c->idx[k].recs_time;
                    c->idx[k].recs = NULL;
                }
        }
        for (size_t k = 0; k < c->nidx; k++) {
            for (size_t r = 0; r < c->idx[k].nrecs; r++)
                for (size_t v = 0; v < c->idx[k].recs[r].nviews; v++) {
                    view_t *vw = &c->idx[k].recs[r].views[v];

                    free(vw->pts); free(vw->pts_r); free(vw->ko); free(vw->psize);
                }
            free(c->idx[k].recs);
        }
        free(c->idx);
        c->idx = arr;
        c->nidx = n;
        c->idx_time = time(NULL);
    }
    json_free(j);
    return 0;
}

static index_t *find_index(catalog_t *c, size_t id)
{
    for (size_t i = 0; i < c->nidx; i++)
        if (c->idx[i].id == id)
            return &c->idx[i];
    return NULL;
}

static void free_recs(rec_t *recs, size_t n, catalog_t *c)
{
    for (size_t r = 0; r < n; r++)
        for (size_t v = 0; v < recs[r].nviews; v++) {
            view_t *vw = &recs[r].views[v];

            if (vw->table_loaded)
                c->table_entries -= vw->frames;
            free(vw->pts); free(vw->pts_r); free(vw->ko); free(vw->psize);
        }
    free(recs);
}

/* One index's recordings with their views: /api/<idx>?info=1 */
static int load_recs(catalog_t *c, index_t *x)
{
    char path[64];
    json_t *j;
    const json_t *items;
    rec_t *recs;
    size_t n;
    int rc;

    if (x->recs != NULL && time(NULL) - x->recs_time < c->ttl)
        return 0;
    snprintf(path, sizeof(path), "/api/%zu?info=1", x->id);
    rc = api_get_json(c->base, path, &j);
    if (rc != 0)
        return x->recs != NULL ? 0 : rc;
    items = json_get(j, "items");
    n = json_len(items);
    recs = calloc(n ? n : 1, sizeof(rec_t));
    if (recs == NULL) {
        json_free(j);
        return -ENOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        const json_t *it = json_at(items, i);
        const json_t *views = json_get(it, "views");
        rec_t *r = &recs[i];

        copy_str(r->name, sizeof(r->name), json_str(json_get(it, "name"), "?"));
        copy_str(r->media, sizeof(r->media), json_str(json_get(it, "media"), ""));
        copy_str(r->ix, sizeof(r->ix), json_str(json_get(it, "ix"), ""));
        r->loaded = json_bool(json_get(it, "loaded"), 0);
        r->cal_bytes = json_int(json_get(it, "calibration_bytes"), -1);
        r->unreadable = views == NULL;
        for (size_t k = 0; k < json_len(views) && r->nviews < MAX_VIEWS; k++) {
            const json_t *vj = json_at(views, k);
            view_t *v = &r->views[r->nviews];
            const char *media = json_str(json_get(vj, "media"), "video");

            copy_str(v->name, sizeof(v->name), json_str(json_get(vj, "name"), "?"));
            v->pid = (int)json_int(json_get(vj, "pid"), -1);
            v->frames = (uint32_t)json_int(json_get(vj, "frames"), 0);
            v->is_p = strcmp(media, "stereo") == 0;
            v->has_h265 = !v->is_p;
            v->duration_s = json_num(json_get(vj, "duration_s"), 0);
            /* carry over a frame table fetched under the previous listing */
            for (size_t o = 0; x->recs && o < x->nrecs; o++)
                if (strcmp(x->recs[o].name, r->name) == 0)
                    for (size_t ov = 0; ov < x->recs[o].nviews; ov++) {
                        view_t *old = &x->recs[o].views[ov];

                        if (strcmp(old->name, v->name) == 0 && old->table_loaded &&
                            old->frames == v->frames) {
                            v->pts = old->pts; v->pts_r = old->pts_r;
                            v->ko = old->ko; v->psize = old->psize;
                            v->table_loaded = 1;
                            v->width = old->width; v->height = old->height;
                            v->params_bytes = old->params_bytes;
                            old->pts = old->pts_r = NULL; old->ko = NULL; old->psize = NULL;
                            old->table_loaded = 0;
                            c->table_entries += v->frames;   /* free_recs takes it back */
                        }
                    }
            r->nviews++;
        }
    }
    json_free(j);
    if (x->recs != NULL)
        free_recs(x->recs, x->nrecs, c);
    x->recs = recs;
    x->nrecs = n;
    x->recs_time = time(NULL);
    return 0;
}

static rec_t *find_rec(index_t *x, const char *name)
{
    for (size_t i = 0; i < x->nrecs; i++)
        if (strcmp(x->recs[i].name, name) == 0)
            return &x->recs[i];
    return NULL;
}

static view_t *find_view(rec_t *r, const char *name)
{
    for (size_t i = 0; i < r->nviews; i++)
        if (strcmp(r->views[i].name, name) == 0)
            return &r->views[i];
    return NULL;
}

/* Drop the least recently used frame tables until within the budget. */
static void trim_tables(catalog_t *c, const view_t *keep)
{
    while (c->table_entries > c->entry_budget) {
        view_t *victim = NULL;

        for (size_t i = 0; i < c->nidx; i++)
            for (size_t r = 0; r < c->idx[i].nrecs; r++)
                for (size_t v = 0; v < c->idx[i].recs[r].nviews; v++) {
                    view_t *vw = &c->idx[i].recs[r].views[v];

                    if (vw->table_loaded && vw != keep &&
                        (victim == NULL || vw->last_use < victim->last_use))
                        victim = vw;
                }
        if (victim == NULL)
            return;
        free(victim->pts); free(victim->pts_r); free(victim->ko); free(victim->psize);
        victim->pts = victim->pts_r = NULL; victim->ko = NULL; victim->psize = NULL;
        victim->table_loaded = 0;
        c->table_entries -= victim->frames;
    }
}

/* A view's frame table: /api/<idx>/<rec>/<view>?from=&count= in pages. */
static int load_table(catalog_t *c, index_t *x, rec_t *r, view_t *v)
{
    size_t got = 0;

    v->last_use = ++c->clock;
    if (v->table_loaded)
        return 0;
    v->pts = calloc(v->frames ? v->frames : 1, sizeof(uint64_t));
    v->ko = calloc(v->frames ? v->frames : 1, sizeof(uint16_t));
    v->pts_r = v->is_p ? calloc(v->frames ? v->frames : 1, sizeof(uint64_t)) : NULL;
    v->psize = v->is_p ? NULL : calloc(v->frames ? v->frames : 1, sizeof(uint32_t));
    if (v->pts == NULL || v->ko == NULL || (v->is_p && v->pts_r == NULL) ||
        (!v->is_p && v->psize == NULL))
        goto nomem;
    do {
        sb_t sb;
        json_t *j;
        const json_t *items;
        size_t n;
        int rc;

        sb_init(&sb);
        sb_printf(&sb, "/api/%zu/", x->id);
        url_enc(&sb, r->name);
        sb_puts(&sb, "/");
        url_enc(&sb, v->name);
        sb_printf(&sb, "?from=%zu&count=%d", got, PAGE);
        if (sb.oom) {
            sb_free(&sb);
            goto nomem;
        }
        rc = api_get_json(c->base, sb.data, &j);
        sb_free(&sb);
        if (rc != 0)
            goto fail_rc;
        v->width = (int)json_int(json_get(j, "width"), v->width);
        v->height = (int)json_int(json_get(j, "height"), v->height);
        v->params_bytes = (uint32_t)json_int(json_get(j, "params_bytes"), 0);
        {
            uint32_t total = (uint32_t)json_int(json_get(j, "frames"), v->frames);

            if (total != v->frames) {        /* the recording changed: resize */
                uint64_t *p = realloc(v->pts, (total ? total : 1) * sizeof(uint64_t));
                uint16_t *k = realloc(v->ko, (total ? total : 1) * sizeof(uint16_t));

                if (p == NULL || k == NULL) { free(p ? p : v->pts); json_free(j); goto nomem; }
                v->pts = p; v->ko = k;
                if (v->is_p) {
                    uint64_t *pr = realloc(v->pts_r, (total ? total : 1) * sizeof(uint64_t));
                    if (pr == NULL) { json_free(j); goto nomem; }
                    v->pts_r = pr;
                } else {
                    uint32_t *ps = realloc(v->psize, (total ? total : 1) * sizeof(uint32_t));
                    if (ps == NULL) { json_free(j); goto nomem; }
                    v->psize = ps;
                }
                v->frames = total;
            }
        }
        items = json_get(j, "items");
        n = json_len(items);
        for (size_t i = 0; i < n && got + i < v->frames; i++) {
            const json_t *it = json_at(items, i);
            size_t at = got + i;

            if (v->is_p) {
                v->pts[at] = (uint64_t)json_int(json_get(it, "pts_l"), 0);
                v->pts_r[at] = (uint64_t)json_int(json_get(it, "pts_r"), 0);
            } else {
                v->pts[at] = (uint64_t)json_int(json_get(it, "pts"), 0);
                v->psize[at] = (uint32_t)json_int(json_get(it, "size"), 0);
            }
            v->ko[at] = (uint16_t)json_int(json_get(it, "key_offset"), 0);
        }
        json_free(j);
        if (n == 0)
            break;
        got += n;
        continue;
fail_rc:
        free(v->pts); free(v->pts_r); free(v->ko); free(v->psize);
        v->pts = v->pts_r = NULL; v->ko = NULL; v->psize = NULL;
        return rc;
    } while (got < v->frames);
    v->table_loaded = 1;
    c->table_entries += v->frames;
    trim_tables(c, v);
    return 0;
nomem:
    free(v->pts); free(v->pts_r); free(v->ko); free(v->psize);
    v->pts = v->pts_r = NULL; v->ko = NULL; v->psize = NULL;
    return -ENOMEM;
}

/* ---- names and sizes ----------------------------------------------------------- */

static void frame_name(const view_t *v, size_t n, const char *fmt, char *buf,
                       size_t cap)
{
    if (v->is_p)
        snprintf(buf, cap, "%0*zu_%" PRIu64 "_%" PRIu64 "_%u.%s", NAME_WIDTH, n,
                 v->pts[n], v->pts_r[n], v->ko[n], fmt);
    else
        snprintf(buf, cap, "%0*zu_%" PRIu64 "_%u.%s", NAME_WIDTH, n, v->pts[n],
                 v->ko[n], fmt);
}

static size_t y4m_hdr_len(int w, int h)
{
    char hdr[Y4M_HDR_MAX];

    return (size_t)snprintf(hdr, sizeof(hdr), "YUV4MPEG2 W%d H%d F30:1 Ip A1:1 C420\n",
                            w, h) + 6;       /* + "FRAME\n" */
}

/* File size of a frame: exact for yuv/y4m/bmp, an upper bound for jpg and
 * h265 (served with direct_io, so a shorter read is fine). */
static void frame_size(const view_t *v, size_t n, const char *fmt, off_t *size,
                       int *exact)
{
    /* the server reports the P view's width already stacked (2W) */
    int w = v->width, h = v->height;
    size_t yuv = (size_t)w * h * 3 / 2;

    *exact = 1;
    if (strcmp(fmt, "yuv") == 0)
        *size = (off_t)yuv;
    else if (strcmp(fmt, "y4m") == 0)
        *size = (off_t)(y4m_hdr_len(w, h) + yuv);
    else if (strcmp(fmt, "bmp") == 0)
        *size = (off_t)(54 + (size_t)w * h * 3);
    else if (strcmp(fmt, "h265") == 0) {
        *size = (off_t)v->psize[n] + (v->ko[n] == 0 ? (off_t)v->params_bytes : 0);
        *exact = 0;
    } else {                                   /* jpg: unknown until encoded */
        *size = (off_t)w * h * 3;
        *exact = 0;
    }
}

/* ---- open / close ----------------------------------------------------------------- */

catalog_t *catalog_open(const catalog_config_t *cfg)
{
    catalog_t *c = calloc(1, sizeof(*c));

    if (c == NULL)
        return NULL;
    copy_str(c->base, sizeof(c->base), cfg->base);
    while (c->base[0] && c->base[strlen(c->base) - 1] == '/')
        c->base[strlen(c->base) - 1] = '\0';
    c->ttl = cfg->ttl_s > 0 ? cfg->ttl_s : 60;
    c->frame_budget = cfg->frame_cache ? cfg->frame_cache : 256ull << 20;
    c->entry_budget = cfg->frame_entries ? cfg->frame_entries : 4000000;
    pthread_mutex_init(&c->lock, NULL);
    pthread_mutex_init(&c->clock_lock, NULL);
    return c;
}

void catalog_close(catalog_t *c)
{
    frame_blob_t *b, *next;

    if (c == NULL)
        return;
    for (size_t i = 0; i < c->nidx; i++)
        free_recs(c->idx[i].recs, c->idx[i].nrecs, c);
    free(c->idx);
    for (b = c->head; b != NULL; b = next) {
        next = b->next;
        free(b->data);
        free(b);
    }
    pthread_mutex_destroy(&c->lock);
    pthread_mutex_destroy(&c->clock_lock);
    free(c);
}

/* ---- lookup / list ----------------------------------------------------------------- */

static int split(const char *path, char parts[6][256], int *n)
{
    const char *p = path;

    *n = 0;
    while (*p == '/')
        p++;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);

        if (*n == 6 || l >= 256)
            return -1;
        memcpy(parts[*n], p, l);
        parts[*n][l] = '\0';
        (*n)++;
        p += l;
        while (*p == '/')
            p++;
    }
    return 0;
}

static int parse_index(const char *s, size_t *out)
{
    char *end;
    unsigned long long v;

    if (s[0] < '0' || s[0] > '9')
        return -1;
    v = strtoull(s, &end, 10);
    if (*end)
        return -1;
    *out = (size_t)v;
    return 0;
}

int catalog_lookup(catalog_t *c, const char *path, node_info_t *out)
{
    char parts[6][256];
    int n, rc = 0;
    index_t *x;
    rec_t *r;
    view_t *v;

    memset(out, 0, sizeof(*out));
    if (split(path, parts, &n) != 0)
        return 0;
    if (n == 0) {
        out->kind = NODE_ROOT;
        return 0;
    }
    if (n == 1 && strcmp(parts[0], "stats") == 0) {
        out->kind = NODE_STATS;
        out->size = 65536;
        return 0;
    }
    if (parse_index(parts[0], &out->idx) != 0)
        return 0;

    pthread_mutex_lock(&c->lock);
    rc = load_indexes(c);
    if (rc != 0)
        goto done;
    x = find_index(c, out->idx);
    if (x == NULL)
        goto done;                             /* NODE_NONE */
    if (n == 1) {
        out->kind = NODE_INDEX;
        goto done;
    }
    if (n == 2 && strcmp(parts[1], "info") == 0) {
        out->kind = NODE_INDEX_INFO;
        out->size = 4096;
        goto done;
    }
    rc = load_recs(c, x);
    if (rc != 0)
        goto done;
    r = find_rec(x, parts[1]);
    if (r == NULL)
        goto done;
    copy_str(out->rec, sizeof(out->rec), r->name);
    if (n == 2) {
        out->kind = NODE_REC;
        goto done;
    }
    if (n == 3 && strcmp(parts[2], "info") == 0) {
        out->kind = NODE_REC_INFO;
        out->size = 8192;
        goto done;
    }
    if (n == 3 && r->cal_bytes >= 0 && strcmp(parts[2], CAL_NAME) == 0) {
        out->kind = NODE_REC_CAL;
        out->size = (off_t)r->cal_bytes;
        out->exact = 1;
        goto done;
    }
    v = find_view(r, parts[2]);
    if (v == NULL)
        goto done;
    copy_str(out->view, sizeof(out->view), v->name);
    if (n == 3) {
        out->kind = NODE_VIEW;
        goto done;
    }
    if (!fmt_ok(parts[3], v))
        goto done;
    copy_str(out->fmt, sizeof(out->fmt), parts[3]);
    if (n == 4) {
        out->kind = NODE_FMT;
        goto done;
    }
    if (n == 5) {
        char *end, canon[128];
        unsigned long long fr;

        if (parts[4][0] < '0' || parts[4][0] > '9')
            goto done;
        fr = strtoull(parts[4], &end, 10);
        rc = load_table(c, x, r, v);
        if (rc != 0)
            goto done;
        if (fr >= v->frames)
            goto done;
        frame_name(v, (size_t)fr, out->fmt, canon, sizeof(canon));
        if (strcmp(parts[4], canon) != 0)
            goto done;                         /* only the canonical name exists */
        out->kind = NODE_FRAME;
        out->frame = (size_t)fr;
        frame_size(v, (size_t)fr, out->fmt, &out->size, &out->exact);
    }
done:
    pthread_mutex_unlock(&c->lock);
    if (rc == -ENOENT) {                       /* the server says: gone */
        out->kind = NODE_NONE;
        rc = 0;
    }
    if (rc != 0)
        out->kind = NODE_NONE;
    return rc;
}

int catalog_list(catalog_t *c, const node_info_t *n,
                 void (*add)(void *ud, const char *name), void *ud)
{
    int rc = 0;
    index_t *x = NULL;
    rec_t *r = NULL;
    view_t *v = NULL;

    pthread_mutex_lock(&c->lock);
    if (n->kind == NODE_ROOT) {
        rc = load_indexes(c);
        if (rc == 0) {
            char name[32];

            add(ud, "stats");
            for (size_t i = 0; i < c->nidx; i++) {
                snprintf(name, sizeof(name), "%zu", c->idx[i].id);
                add(ud, name);
            }
        }
        goto done;
    }
    rc = load_indexes(c);
    if (rc != 0)
        goto done;
    x = find_index(c, n->idx);
    if (x == NULL) {
        rc = -ENOENT;
        goto done;
    }
    rc = load_recs(c, x);
    if (rc != 0)
        goto done;
    if (n->kind == NODE_INDEX) {
        add(ud, "info");
        for (size_t i = 0; i < x->nrecs; i++)
            add(ud, x->recs[i].name);
        goto done;
    }
    r = find_rec(x, n->rec);
    if (r == NULL) {
        rc = -ENOENT;
        goto done;
    }
    if (n->kind == NODE_REC) {
        add(ud, "info");
        if (r->cal_bytes >= 0)
            add(ud, CAL_NAME);
        for (size_t i = 0; i < r->nviews; i++)
            add(ud, r->views[i].name);
        goto done;
    }
    v = find_view(r, n->view);
    if (v == NULL) {
        rc = -ENOENT;
        goto done;
    }
    if (n->kind == NODE_VIEW) {
        for (size_t i = 0; i < sizeof(FMTS) / sizeof(FMTS[0]); i++)
            if (fmt_ok(FMTS[i], v))
                add(ud, FMTS[i]);
        goto done;
    }
    if (n->kind == NODE_FMT) {
        char name[128];

        rc = load_table(c, x, r, v);
        if (rc != 0)
            goto done;
        for (size_t i = 0; i < v->frames; i++) {
            frame_name(v, i, n->fmt, name, sizeof(name));
            add(ud, name);
        }
        goto done;
    }
    rc = -ENOTDIR;
done:
    pthread_mutex_unlock(&c->lock);
    return rc;
}

/* ---- info / stats text ------------------------------------------------------------- */

int catalog_text(catalog_t *c, const node_info_t *n, char **text, size_t *len)
{
    sb_t sb;
    int rc = 0;

    sb_init(&sb);
    if (n->kind == NODE_REC_CAL) {         /* the server's copy of the file, as it is */
        char path[600];
        uint8_t *body;
        size_t bl;

        snprintf(path, sizeof(path), "/api/%zu/%s/calibration", n->idx, n->rec);
        rc = api_get(c->base, path, &body, &bl, NULL);
        if (rc != 0)
            return rc;
        *text = (char *)body;
        *len = bl;
        return 0;
    }
    if (n->kind == NODE_STATS) {
        uint8_t *body;
        size_t bl;
        catalog_stats_t st;

        rc = api_get(c->base, "/api/stats", &body, &bl, NULL);
        if (rc != 0)
            return rc;
        catalog_get_stats(c, &st);
        sb_append(&sb, body, bl);
        free(body);
        if (sb.len && sb.data[sb.len - 1] != '\n')
            sb_puts(&sb, "\n");
        sb_printf(&sb, "{\"client\":{\"frames_fetched\":%" PRIu64
                  ",\"bytes_fetched\":%" PRIu64 ",\"cache_hits\":%" PRIu64
                  ",\"cache_bytes\":%" PRIu64 ",\"cache_entries\":%u,"
                  "\"table_entries\":%" PRIu64 "}}\n",
                  st.frames_fetched, st.bytes_fetched, st.cache_hits,
                  st.cache_bytes, st.cache_entries, st.table_entries);
    } else {
        index_t *x;

        pthread_mutex_lock(&c->lock);
        rc = load_indexes(c);
        x = rc == 0 ? find_index(c, n->idx) : NULL;
        if (x == NULL) {
            rc = rc ? rc : -ENOENT;
        } else if (n->kind == NODE_INDEX_INFO) {
            sb_printf(&sb, "index %zu\npath %s\nserver %s\nrecordings %zu\n"
                      "frames L %" PRIu64 " R %" PRIu64 " P %" PRIu64 "\n"
                      "duration %.3f\n", x->id, x->path, c->base, x->recordings,
                      x->l, x->r, x->p, x->duration_s);
        } else {
            rec_t *r;

            rc = load_recs(c, x);
            r = rc == 0 ? find_rec(x, n->rec) : NULL;
            if (r == NULL) {
                rc = rc ? rc : -ENOENT;
            } else {
                sb_printf(&sb, "name %s\nindex %zu\nmedia %s\nix %s\nloaded %s\n",
                          r->name, x->id, r->media, r->ix, r->loaded ? "yes" : "no");
                for (size_t i = 0; i < r->nviews; i++) {
                    const view_t *v = &r->views[i];

                    if (v->is_p)
                        sb_printf(&sb, "view P pid - media stereo frames %u duration %.3f\n",
                                  v->frames, v->duration_s);
                    else
                        sb_printf(&sb, "view %s pid 0x%x media video frames %u\n",
                                  v->name, v->pid, v->frames);
                    if (v->width)
                        sb_printf(&sb, "size %s %dx%d\n", v->name, v->width,
                                  v->height);
                }
            }
        }
        pthread_mutex_unlock(&c->lock);
    }
    if (rc != 0 || sb.oom) {
        sb_free(&sb);
        return rc ? rc : -ENOMEM;
    }
    *text = sb_detach(&sb, len);
    return 0;
}

/* ---- frame cache ------------------------------------------------------------------- */

static void lru_unlink(catalog_t *c, frame_blob_t *b)
{
    if (b->prev) b->prev->next = b->next; else c->head = b->next;
    if (b->next) b->next->prev = b->prev; else c->tail = b->prev;
    b->prev = b->next = NULL;
}

static void lru_push_front(catalog_t *c, frame_blob_t *b)
{
    b->prev = NULL;
    b->next = c->head;
    if (c->head) c->head->prev = b; else c->tail = b;
    c->head = b;
}

static void evict(catalog_t *c)
{
    frame_blob_t *b = c->tail;

    while (b != NULL && c->cache_bytes > c->frame_budget) {
        frame_blob_t *prev = b->prev;

        if (b->pins == 0) {
            lru_unlink(c, b);
            c->cache_bytes -= b->len;
            c->cache_entries--;
            free(b->data);
            free(b);
        }
        b = prev;
    }
}

int catalog_frame_get(catalog_t *c, const node_info_t *n, frame_blob_t **blob,
                      const uint8_t **data, size_t *len)
{
    sb_t sb;
    char key[512];
    frame_blob_t *b;
    uint8_t *body;
    size_t bl;
    int rc;

    sb_init(&sb);
    sb_printf(&sb, "/api/%zu/", n->idx);
    url_enc(&sb, n->rec);
    sb_puts(&sb, "/");
    url_enc(&sb, n->view);
    sb_printf(&sb, "/%zu.%s", n->frame, n->fmt);
    if (sb.oom) {
        sb_free(&sb);
        return -ENOMEM;
    }
    copy_str(key, sizeof(key), sb.data);

    pthread_mutex_lock(&c->clock_lock);
    for (b = c->head; b != NULL; b = b->next)
        if (strcmp(b->key, key) == 0)
            break;
    if (b != NULL) {
        lru_unlink(c, b);
        lru_push_front(c, b);
        b->pins++;
        c->cache_hits++;
        pthread_mutex_unlock(&c->clock_lock);
        sb_free(&sb);
        *blob = b;
        *data = b->data;
        *len = b->len;
        return 0;
    }
    pthread_mutex_unlock(&c->clock_lock);

    rc = api_get(c->base, sb.data, &body, &bl, NULL);
    sb_free(&sb);
    if (rc != 0)
        return rc;
    b = calloc(1, sizeof(*b));
    if (b == NULL) {
        free(body);
        return -ENOMEM;
    }
    copy_str(b->key, sizeof(b->key), key);
    b->data = body;
    b->len = bl;
    b->pins = 1;
    pthread_mutex_lock(&c->clock_lock);
    lru_push_front(c, b);
    c->cache_bytes += bl;
    c->cache_entries++;
    c->frames_fetched++;
    c->bytes_fetched += bl;
    evict(c);
    pthread_mutex_unlock(&c->clock_lock);
    *blob = b;
    *data = b->data;
    *len = b->len;
    return 0;
}

void catalog_frame_release(catalog_t *c, frame_blob_t *b)
{
    if (b == NULL)
        return;
    pthread_mutex_lock(&c->clock_lock);
    if (b->pins > 0)
        b->pins--;
    if (c->cache_bytes > c->frame_budget)
        evict(c);
    pthread_mutex_unlock(&c->clock_lock);
}

void catalog_get_stats(catalog_t *c, catalog_stats_t *out)
{
    pthread_mutex_lock(&c->clock_lock);
    out->frames_fetched = c->frames_fetched;
    out->bytes_fetched = c->bytes_fetched;
    out->cache_hits = c->cache_hits;
    out->cache_bytes = c->cache_bytes;
    out->cache_entries = c->cache_entries;
    pthread_mutex_unlock(&c->clock_lock);
    pthread_mutex_lock(&c->lock);
    out->table_entries = c->table_entries;
    pthread_mutex_unlock(&c->lock);
}
