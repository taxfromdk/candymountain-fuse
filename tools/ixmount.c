/*
 * ixmount - mount an ixserve server as a read-only filesystem.
 *
 *   ixmount <server-url> <mountpoint> [options] [fuse options]
 *
 * The tree mirrors the server's API (see src/catalog.h): the indexes by
 * number, their recordings, the views L / R / P, the formats, and one file
 * per frame named <index>_<pts>_<keyoffset>.<fmt>. Reading a frame file
 * fetches it from the server; nothing is decoded here.
 */
#define FUSE_USE_VERSION 31

#include "api.h"
#include "catalog.h"

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    catalog_t *cat;
    time_t     start;
} app_t;

static app_t APP;

/* An open file: a frame pinned in the cache, or a generated text. */
typedef struct {
    node_info_t    node;
    frame_blob_t  *blob;
    const uint8_t *data;
    size_t         len;
    char          *text;       /* owned, for info/stats */
} handle_t;

static int ix_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    node_info_t n;
    int rc;

    (void)fi;
    memset(st, 0, sizeof(*st));
    rc = catalog_lookup(APP.cat, path, &n);
    if (rc != 0)
        return rc;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = st->st_mtime = st->st_ctime = APP.start;
    switch (n.kind) {
    case NODE_NONE:
        return -ENOENT;
    case NODE_STATS:
    case NODE_INDEX_INFO:
    case NODE_REC_INFO:
    case NODE_REC_CAL:
    case NODE_FRAME:
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_size = n.size;
        return 0;
    default:
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }
}

typedef struct {
    void            *buf;
    fuse_fill_dir_t  fill;
} dir_ctx_t;

static void add_entry(void *ud, const char *name)
{
    dir_ctx_t *d = ud;

    d->fill(d->buf, name, NULL, 0, 0);
}

static int ix_readdir(const char *path, void *buf, fuse_fill_dir_t fill,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags)
{
    node_info_t n;
    dir_ctx_t d = { buf, fill };
    int rc;

    (void)offset; (void)fi; (void)flags;
    rc = catalog_lookup(APP.cat, path, &n);
    if (rc != 0)
        return rc;
    if (n.kind == NODE_NONE)
        return -ENOENT;
    if (n.kind == NODE_STATS || n.kind == NODE_INDEX_INFO ||
        n.kind == NODE_REC_INFO || n.kind == NODE_REC_CAL || n.kind == NODE_FRAME)
        return -ENOTDIR;
    fill(buf, ".", NULL, 0, 0);
    fill(buf, "..", NULL, 0, 0);
    return catalog_list(APP.cat, &n, add_entry, &d);
}

static int ix_open(const char *path, struct fuse_file_info *fi)
{
    node_info_t n;
    handle_t *h;
    int rc;

    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        return -EACCES;
    rc = catalog_lookup(APP.cat, path, &n);
    if (rc != 0)
        return rc;
    if (n.kind == NODE_NONE)
        return -ENOENT;
    h = calloc(1, sizeof(*h));
    if (h == NULL)
        return -ENOMEM;
    h->node = n;
    switch (n.kind) {
    case NODE_STATS:
    case NODE_INDEX_INFO:
    case NODE_REC_INFO:
    case NODE_REC_CAL: {
        size_t len;

        rc = catalog_text(APP.cat, &n, &h->text, &len);
        if (rc != 0) {
            free(h);
            return rc;
        }
        h->data = (const uint8_t *)h->text;
        h->len = len;
        if (!n.exact)
            fi->direct_io = 1;          /* the size in getattr is a guess (the calibration's is exact) */
        break;
    }
    case NODE_FRAME:
        rc = catalog_frame_get(APP.cat, &n, &h->blob, &h->data, &h->len);
        if (rc != 0) {
            free(h);
            return rc;
        }
        if (!n.exact)
            fi->direct_io = 1;          /* jpg/h265: the real size is h->len */
        else
            fi->keep_cache = 1;         /* a frame never changes */
        break;
    default:
        free(h);
        return -EISDIR;
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int ix_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    size_t avail;

    (void)path;
    if (h == NULL)
        return -EIO;
    if (offset < 0 || (size_t)offset >= h->len)
        return 0;
    avail = h->len - (size_t)offset;
    if (size > avail)
        size = avail;
    memcpy(buf, h->data + offset, size);
    return (int)size;
}

static int ix_release(const char *path, struct fuse_file_info *fi)
{
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;

    (void)path;
    if (h == NULL)
        return 0;
    if (h->blob != NULL)
        catalog_frame_release(APP.cat, h->blob);
    free(h->text);
    free(h);
    return 0;
}

static void *ix_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->entry_timeout = 60.0;       /* listings may change on the server */
    cfg->attr_timeout = 60.0;
    cfg->negative_timeout = 1.0;
    return NULL;
}

static const struct fuse_operations ops = {
    .init    = ix_init,
    .getattr = ix_getattr,
    .readdir = ix_readdir,
    .open    = ix_open,
    .read    = ix_read,
    .release = ix_release,
};

static uint64_t parse_size(const char *s)
{
    char *end;
    double v = strtod(s, &end);

    switch (*end) {
    case 'k': case 'K': return (uint64_t)(v * 1024);
    case 'm': case 'M': return (uint64_t)(v * 1024 * 1024);
    case 'g': case 'G': return (uint64_t)(v * 1024 * 1024 * 1024);
    default:            return (uint64_t)v;
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: ixmount <server-url> <mountpoint> [options] [fuse options]\n"
        "\n"
        "  --cache-size <n>   frame bytes kept in memory (default 256M)\n"
        "  --ttl <s>          how long listings are trusted (default 60)\n"
        "  --timeout <s>      per-request timeout (default 120)\n"
        "  -f                 stay in the foreground (fuse)\n"
        "  -s                 single-threaded (fuse)\n"
        "\n"
        "Mounts http://host:port/ (an ixserve) read-only:\n"
        "  <mount>/<index>/<recording>/<L|R|P>/<jpg|bmp|yuv|y4m|h265>/<n>_<pts>_<ko>.<fmt>\n"
        "with an info file in every index and recording folder and stats at the root.\n"
        "Unmount with: fusermount3 -u <mountpoint>\n");
}

int main(int argc, char **argv)
{
    catalog_config_t cfg;
    const char *url = NULL, *mnt = NULL;
    char *fargv[64];
    int fargc = 0, rc;

    memset(&cfg, 0, sizeof(cfg));
    fargv[fargc++] = argv[0];
    for (int i = 1; i < argc && fargc < 60; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(a, "--cache-size") == 0 && v) { cfg.frame_cache = parse_size(v); i++; }
        else if (strcmp(a, "--ttl") == 0 && v)   { cfg.ttl_s = atoi(v); i++; }
        else if (strcmp(a, "--timeout") == 0 && v) { api_set_timeout(atol(v)); i++; }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
        else if (a[0] != '-' && url == NULL)      url = a;
        else if (a[0] != '-' && mnt == NULL)      mnt = a;
        else                                      fargv[fargc++] = argv[i];
    }
    if (url == NULL || mnt == NULL) {
        usage();
        return 2;
    }
    if (strncmp(url, "http://", 7) != 0) {
        fprintf(stderr, "ixmount: the server url must start with http://\n");
        return 2;
    }
    if (api_global_init() != 0) {
        fprintf(stderr, "ixmount: cannot initialise libcurl\n");
        return 1;
    }
    cfg.base = url;
    APP.cat = catalog_open(&cfg);
    APP.start = time(NULL);
    if (APP.cat == NULL) {
        fprintf(stderr, "ixmount: out of memory\n");
        return 1;
    }
    /* Prove the server answers before mounting, so a typo fails here. */
    {
        node_info_t n;
        json_t *j;

        rc = api_get_json(url, "/api/", &j);
        if (rc != 0) {
            fprintf(stderr, "ixmount: %s does not answer /api/ (%s)\n", url, strerror(-rc));
            catalog_close(APP.cat);
            api_global_cleanup();
            return 1;
        }
        fprintf(stderr, "ixmount: %s: %lld index(es), %lld recording(s); mounting on %s\n",
                url, (long long)json_int(json_get(j, "indexes"), 0),
                (long long)json_int(json_get(j, "recordings"), 0), mnt);
        json_free(j);
        (void)n;
    }
    fargv[fargc++] = (char *)mnt;
    fargv[fargc] = NULL;
    rc = fuse_main(fargc, fargv, &ops, NULL);
    catalog_close(APP.cat);
    api_global_cleanup();
    return rc;
}
