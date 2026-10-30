/*
 * catalog - what the server has, as the filesystem sees it.
 *
 * The mount's tree:
 *
 *   /                                  the indexes, by number: 0, 1, ...; stats
 *   /stats                             the server's /api/stats JSON
 *   /<idx>                             info + the index's recordings
 *   /<idx>/info                        the index's absolute path and totals
 *   /<idx>/<rec>                       info + the recording's views: L, R, P, ...
 *   /<idx>/<rec>/info                  media, ix, views and frame counts
 *   /<idx>/<rec>/camera_alignment_settings.json
 *                                      the recording's calibration file, when it has one
 *   /<idx>/<rec>/<view>                the formats: jpg bmp yuv y4m h265 (P: no h265)
 *   /<idx>/<rec>/<view>/<fmt>          the frames
 *   /<idx>/<rec>/<view>/<fmt>/<name>   <index>_<pts>_<keyoffset>.<fmt>
 *                                      (P: <index>_<ptsL>_<ptsR>_<keyoffset>.<fmt>)
 *
 * Everything is fetched from the server on first use and cached: the index
 * list and each index's recordings (with their views and counts) for a
 * while (ttl), a view's frame table (pts and key offset per frame, the
 * geometry) for the mount's life under an entry budget, and frame bytes
 * in an LRU under a byte budget. Nothing is decoded here: the server
 * produces every format.
 *
 * Thread-safe: one lock for the metadata, one for the frame cache; HTTP
 * fetches of listings happen under the metadata lock (they are rare and
 * a listing must not be fetched twice), frame fetches outside any lock.
 */
#ifndef CATALOG_H
#define CATALOG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct catalog catalog_t;

typedef struct {
    const char *base;          /* server URL, e.g. http://host:8080 */
    int         ttl_s;         /* listing cache lifetime (0 = 60) */
    uint64_t    frame_cache;   /* frame bytes kept in memory (0 = 256 MiB) */
    uint64_t    frame_entries; /* frame-table entries kept (0 = 4 M) */
} catalog_config_t;

catalog_t *catalog_open(const catalog_config_t *cfg);
void catalog_close(catalog_t *c);

/* What a path is. */
typedef enum {
    NODE_NONE = 0,     /* no such path (-ENOENT) */
    NODE_ROOT,
    NODE_STATS,
    NODE_INDEX,
    NODE_INDEX_INFO,
    NODE_REC,
    NODE_REC_INFO,
    NODE_REC_CAL,      /* camera_alignment_settings.json */
    NODE_VIEW,
    NODE_FMT,
    NODE_FRAME
} node_t;

typedef struct {
    node_t   kind;
    size_t   idx;              /* index number */
    char     rec[256];
    char     view[32];
    char     fmt[8];
    size_t   frame;            /* NODE_FRAME */
    off_t    size;             /* file size for frames / info (0 for dirs) */
    int      exact;            /* 1: size is exact; 0: an upper bound */
} node_info_t;

/* Classify a path. Returns 0, or a negative errno when something had to be
 * fetched and failed (-ENOENT is NODE_NONE, reported as 0 with kind NONE
 * when the path simply does not exist). */
int catalog_lookup(catalog_t *c, const char *path, node_info_t *out);

/* Directory listing: calls add(ud, name) for each entry (no . and ..).
 * Returns 0 or a negative errno. */
int catalog_list(catalog_t *c, const node_info_t *n,
                 void (*add)(void *ud, const char *name), void *ud);

/* The text of an info, stats or calibration file (malloc'd; caller frees). */
int catalog_text(catalog_t *c, const node_info_t *n, char **text, size_t *len);

/* The bytes of a frame file (from the cache or the server). The returned
 * buffer is owned by the cache and pinned until catalog_frame_release. */
typedef struct frame_blob frame_blob_t;
int catalog_frame_get(catalog_t *c, const node_info_t *n, frame_blob_t **blob,
                      const uint8_t **data, size_t *len);
void catalog_frame_release(catalog_t *c, frame_blob_t *blob);

/* Counters for the stats file. */
typedef struct {
    uint64_t frames_fetched, bytes_fetched, cache_hits, cache_bytes;
    uint32_t cache_entries;
    uint64_t table_entries;
} catalog_stats_t;
void catalog_get_stats(catalog_t *c, catalog_stats_t *out);

#endif /* CATALOG_H */
