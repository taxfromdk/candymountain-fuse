# FUSE client

This repository (candymountain-fuse) is `ixmount`: a FUSE3 filesystem
that mounts a candymountain `ixserve` server read-only, so that any
program that reads files can browse the indexes, recordings and views and
read frames in every format the server produces. Its only link to
candymountain is HTTP; the tests need a built checkout for its `ixserve`
and test recordings (`CANDYMOUNTAIN`, default `../candymountain`). It is the client side of what the original project's
`ixfs` did in one process: there the filesystem decoded; here it only
fetches, and the server decodes. No decoding, no libav, no index parsing
on the client.

Dependencies: libfuse3 and libcurl (`pkg-config fuse3 libcurl`), the
`fusermount3` runtime. Tests also use ffmpeg/ffprobe, curl and python3.

# Using it

    ixmount <server-url> <mountpoint> [--cache-size 256M] [--ttl 60]
            [--timeout 120] [-f] [-s] [other fuse options]
    fusermount3 -u <mountpoint>

`-f` keeps it in the foreground; without it `ixmount` daemonises like any
FUSE filesystem. It refuses to mount a URL that does not answer `/api/`.

The tree:

    <mount>/
      stats                               the server's /api/stats JSON, then one
                                          line with the client's own counters
      <idx>/                              one folder per index, by its number
        info                              the index's absolute path and totals
        <recording>/
          info                            media, ix, loaded, views and counts
          camera_alignment_settings.json  the recording's calibration file, when
                                          it has one (the server's /calibration)
          L/ R/ P/                        the picture views (V_0x.. for more)
            jpg/ bmp/ yuv/ y4m/ h265/     the formats (P has no h265)
              0000017_324002975_17.jpg     <index>_<pts>_<keyoffset>.<fmt>
                                          P: <index>_<ptsL>_<ptsR>_<keyoffset>.<fmt>

Frame names are the ones `ixfs` used, seven-digit index first, so `ls` sorts
them and a name identifies the frame by PTS even if the recording is
re-indexed. Only the canonical name exists: `5.jpg` is ENOENT. Audio and
KLV are not there, as on the server.

File sizes: yuv, y4m and bmp are exact (from the view's geometry, which
the server reports once the recording is loaded); jpg and h265 are upper
bounds (the jpg is only known once encoded, a key frame's h265 carries the
prepended parameter sets), served with direct_io so a read returns the
real length and tools that read to EOF get the right bytes. `cp` and
`cmp` work on all of them; a program that trusts `st_size` for a jpg gets
a larger number than the file delivers.

# How it works

`src/json.c` is a small JSON parser (tree, lookups by key and position,
escapes, int64 and double numbers, depth-limited). Strings are unescaped
into one scratch buffer the size of the document and copied out at their
exact length: the parser once allocated "the rest of the document" for
every string, half a million 12 MB malloc/free pairs per frame listing
page, which glibc's main arena absorbed (a listing took a second) but a
thread arena - every FUSE worker is one - answered with a heap trim and
regrow per key, and a 190,000-frame folder took nine minutes to list and
the process was killed. `test_json` parses a 200,000-item listing in a
thread under a time bound so this cannot come back. `src/api.c` does HTTP
GET with libcurl, one easy handle per thread kept alive between calls,
mapping 404 to ENOENT, 503 to EAGAIN and the rest to EIO. `src/catalog.c`
holds what the server has, as the filesystem sees it, and fetches on
first use:

- the index list (`/api/?info=1`) and each index's recordings with their
  views and counts (`/api/<idx>?info=1`; a recording's `calibration_bytes`
  says whether, and how large, its calibration file is listed, exact size,
  fetched from `/api/<idx>/<rec>/calibration` on open) are cached for `--ttl` seconds,
  then refreshed on the next look (a refresh that fails keeps the old
  listing, so a server hiccup does not empty the mount);
- a view's frame table (pts, key offset, h265 payload size per frame, the
  geometry and parameter-set size) is fetched in pages of 100,000 from
  `/api/<idx>/<rec>/<view>` on the first listing or lookup of a frame
  folder, and kept for the mount's life under an entry budget (4 M
  frames; least recently used tables go first). This is what makes `ls`
  of a 750,000-frame folder a few requests, and a frame lookup a table
  index;
- frame bytes are fetched whole on open (`/api/<idx>/<rec>/<view>/<n>.<fmt>`)
  and kept in an in-memory LRU under `--cache-size` bytes, pinned while
  open, so re-reads and the many small reads of a tool come from memory
  and a frame is never fetched twice while it fits.

`tools/ixmount.c` is the FUSE layer: getattr and readdir ask the catalog,
open fetches (or pins) the frame or generates the info/stats text, read
copies from memory, release unpins. Entry and attribute timeouts are 60 s
so the kernel caches lookups; frames with exact sizes keep the page cache
(`keep_cache`), the others use direct_io. Multithreaded FUSE: listings
are fetched under the catalog lock (rare, and a listing must not be
fetched twice), frames outside any lock, so concurrent readers keep the
server's decoders busy.

Browsing thousands of recordings costs the server one index-directory read
per recording (its `?info=1`), the same as its own viewer page; nothing
heavy happens until a frame folder is listed or a frame read.

# Tests

`make test` runs `test_json` (natively and under valgrind; it ends with a
200,000-item listing parsed in a thread, which must take well under a
second) and
`tests/test_mount.sh`: a real `ixserve` (candymountain's, built) on a
free port over its test recordings as index 0 plus a one-recording second
index, mounted with `ixmount -f`; it checks a dead URL is refused before
mounting, the root, index and recording folders and info files, the
calibration file beside the view folders (byte-identical, exact size), the
format folders (no h265 under P), frame counts against the API, the frame
name form and its agreement with the API's pts and key offset, every
format's bytes and sizes (jpg bytes identical to the API's, h265 key
decodes with ffmpeg, yuv/y4m/bmp exact sizes and dimensions, P at 7680
wide, partial reads), ENOENT for wrong indexes, recordings, views, formats
and names, that writes fail, that a re-read hits the client cache, six
concurrent reads, and a clean `fusermount3 -u` with exit 0. It skips when
there is no `/dev/fuse` or `fusermount3`.
