#!/bin/bash
# ixmount end to end: a real ixserve on a free port over the testdata
# recordings (a scratch tree of symlinks, indexes built there if missing),
# mounted with ixmount in the foreground; the tree is walked with ls / stat /
# cat and the frames are checked with ffprobe / ffmpeg: every level and
# name, every format of L and P, exact and upper-bound sizes, the info and
# stats files, ENOENT for wrong names, read-only, the in-memory frame cache,
# concurrent reads, and a clean unmount.
#
#   tests/test_mount.sh <build-dir> <server-build-dir> <testdata-dir>
set -u
export LC_ALL=C            # ls order: uppercase before lowercase, as the checks expect
CANDYMOUNTAIN=${CANDYMOUNTAIN:-../candymountain}   # a built checkout: ixserve, ixtool, testdata
BUILD=${1:-build}
SBUILD=${2:-$CANDYMOUNTAIN/server/build}
TESTDATA=${3:-$CANDYMOUNTAIN/testdata}
IXMOUNT=$(readlink -f "$BUILD/bin/ixmount")
IXSERVE=$(readlink -f "$SBUILD/bin/ixserve")
IXTOOL=$(readlink -f "${IXTOOL:-$CANDYMOUNTAIN/indexer/build/bin/ixtool}")
WORK=$(mktemp -d /tmp/ixmount.XXXXXX)
MNT="$WORK/mnt"
SPID=""; MPID=""
cleanup() {
    mountpoint -q "$MNT" 2>/dev/null && fusermount3 -u "$MNT" 2>/dev/null
    [ -n "$MPID" ] && kill "$MPID" 2>/dev/null
    [ -n "$SPID" ] && kill "$SPID" 2>/dev/null
    sleep 0.2
    rm -rf "$WORK"
}
trap cleanup EXIT
fail() { echo "test_mount: FAIL: $*" >&2; [ -f "$WORK/mount.log" ] && tail -5 "$WORK/mount.log" >&2; [ -f "$WORK/server.log" ] && tail -3 "$WORK/server.log" >&2; exit 1; }
dims() { ffprobe -v error -show_entries stream=width,height -of csv=p=0 "$1" 2>/dev/null | head -1; }

command -v fusermount3 > /dev/null || { echo "test_mount: SKIP (no fusermount3)"; exit 0; }
[ -e /dev/fuse ] || { echo "test_mount: SKIP (no /dev/fuse)"; exit 0; }
[ -x "$IXSERVE" ] || fail "no ixserve at $IXSERVE (build ../server first)"

# ---- scratch library + server ---------------------------------------------------
n=0
for ts in "$TESTDATA"/*/video.ts; do
    [ -f "$ts" ] || continue
    u=$(basename "$(dirname "$ts")")
    mkdir -p "$WORK/tree/$u"
    ln -s "$(readlink -f "$ts")" "$WORK/tree/$u/video.ts"
    [ -f "$(dirname "$ts")/camera_alignment_settings.json" ] && ln -s "$(readlink -f "$(dirname "$ts")/camera_alignment_settings.json")" "$WORK/tree/$u/camera_alignment_settings.json"
    if [ -f "$ts.ix" ]; then ln -s "$(readlink -f "$ts.ix")" "$WORK/tree/$u/video.ts.ix"
    else "$IXTOOL" index "$ts" -o "$WORK/tree/$u/video.ts.ix" > /dev/null || fail "index $u"; fi
    echo "$u" >> "$WORK/tree/index.txt"
    n=$((n + 1))
done
[ "$n" -ge 2 ] || fail "need at least two recordings under $TESTDATA"
A=$(sed -n 1p "$WORK/tree/index.txt"); B=$(sed -n 2p "$WORK/tree/index.txt")
mkdir -p "$WORK/second"; printf '%s\n' "../tree/$B" > "$WORK/second/index.txt"

"$IXSERVE" "$WORK/tree/index.txt" "$WORK/second/index.txt" --port 0 --bind 127.0.0.1 \
    --cache "$WORK/cache" --workers 2 > "$WORK/server.log" 2>&1 &
SPID=$!
for i in $(seq 1 100); do
    port=$(sed -n 's|.*http://127.0.0.1:\([0-9]*\)/.*|\1|p' "$WORK/server.log"); [ -n "$port" ] && break
    kill -0 "$SPID" 2>/dev/null || fail "server exited: $(cat "$WORK/server.log")"; sleep 0.1
done
[ -n "$port" ] || fail "server did not report its port"
URL="http://127.0.0.1:$port"

# ---- a wrong url fails before mounting ---------------------------------------------
mkdir -p "$MNT"
"$IXMOUNT" "http://127.0.0.1:1" "$MNT" -f > "$WORK/bad.log" 2>&1 && fail "mounting a dead server must fail"
grep -q 'does not answer' "$WORK/bad.log" || fail "dead server message: $(cat "$WORK/bad.log")"

# ---- mount -----------------------------------------------------------------------------
"$IXMOUNT" "$URL" "$MNT" -f --cache-size 64M > "$WORK/mount.log" 2>&1 &
MPID=$!
for i in $(seq 1 50); do mountpoint -q "$MNT" && break; sleep 0.1; done
mountpoint -q "$MNT" || fail "not mounted: $(cat "$WORK/mount.log")"
grep -q '2 index(es)' "$WORK/mount.log" || fail "mount banner: $(cat "$WORK/mount.log")"

# ---- the tree ---------------------------------------------------------------------------
[ "$(ls "$MNT" | tr '\n' ' ')" = "0 1 stats " ] || fail "root: $(ls "$MNT")"
[ -d "$MNT/0" ] && [ -f "$MNT/stats" ] || fail "root types"
grep -q "^path $(readlink -f "$WORK/tree/index.txt")$" "$MNT/0/info" || fail "index 0 info path: $(cat "$MNT/0/info")"
grep -q "^recordings $n$" "$MNT/0/info" || fail "index 0 info recordings"
grep -q "^recordings 1$" "$MNT/1/info" || fail "index 1 info recordings"
[ "$(ls "$MNT/0" | grep -vc '^info$')" = "$n" ] || fail "index 0 lists $n recordings: $(ls "$MNT/0" | wc -l)"
[ "$(ls "$MNT/1" | grep -v '^info$')" = "$B" ] || fail "index 1 lists $B"
[ "$(ls "$MNT/0/$A" | tr '\n' ' ')" = "L P R camera_alignment_settings.json info " ] || fail "recording folder: $(ls "$MNT/0/$A")"
cmp -s "$MNT/0/$A/camera_alignment_settings.json" "$TESTDATA/$A/camera_alignment_settings.json" || fail "the calibration file differs from the one beside the media"
[ "$(stat -c %s "$MNT/0/$A/camera_alignment_settings.json")" = "$(stat -c %s "$TESTDATA/$A/camera_alignment_settings.json")" ] || fail "calibration file size"
grep -q "^media .*/$A/video.ts$" "$MNT/0/$A/info" || fail "recording info media"
grep -q '^view P pid - media stereo frames' "$MNT/0/$A/info" || fail "recording info P line"
[ "$(ls "$MNT/0/$A/L" | tr '\n' ' ')" = "bmp h265 jpg y4m yuv " ] || fail "L formats: $(ls "$MNT/0/$A/L")"
[ "$(ls "$MNT/0/$A/P" | tr '\n' ' ')" = "bmp jpg y4m yuv " ] || fail "P formats (no h265): $(ls "$MNT/0/$A/P")"
lcount=$(curl -s "$URL/api/0/$A" | python3 -c "import json,sys; print([v for v in json.load(sys.stdin)['views'] if v['name']=='L'][0]['frames'])")
pcount=$(curl -s "$URL/api/0/$A" | python3 -c "import json,sys; print([v for v in json.load(sys.stdin)['views'] if v['name']=='P'][0]['frames'])")
[ "$(ls "$MNT/0/$A/L/jpg" | wc -l)" = "$lcount" ] || fail "L/jpg lists $lcount frames"
[ "$(ls "$MNT/0/$A/P/y4m" | wc -l)" = "$pcount" ] || fail "P/y4m lists $pcount frames"
first=$(ls "$MNT/0/$A/L/jpg" | head -1)
echo "$first" | grep -qE '^0000000_[0-9]+_0\.jpg$' || fail "frame name form: $first"
pfirst=$(ls "$MNT/0/$A/P/jpg" | head -1)
echo "$pfirst" | grep -qE '^0000000_[0-9]+_[0-9]+_[0-9]+\.jpg$' || fail "P frame name form: $pfirst"
# the names agree with the API's pts and key offset
pts=$(curl -s "$URL/api/0/$A/L?from=5&count=1" | python3 -c "import json,sys; d=json.load(sys.stdin)['items'][0]; print(f\"{d['pts']}_{d['key_offset']}\")")
[ -f "$MNT/0/$A/L/jpg/0000005_$pts.jpg" ] || fail "frame 5 name 0000005_$pts.jpg not found"
echo "test_mount: tree ok ($n + 1 recordings in 2 indexes, $A: L=$lcount P=$pcount)"

# ---- frames ------------------------------------------------------------------------------
f5="$MNT/0/$A/L/jpg/0000005_$pts.jpg"
cp "$f5" "$WORK/5.jpg" || fail "cp jpg"
[ "$(dims "$WORK/5.jpg")" = "3840,2160" ] || fail "jpg dims"
cmp -s "$WORK/5.jpg" <(curl -s "$URL/api/0/$A/L/5.jpg") || fail "jpg bytes differ from the API"
[ "$(stat -c %s "$f5")" -ge "$(stat -c %s "$WORK/5.jpg")" ] || fail "jpg stat size must be an upper bound"
k0=$(ls "$MNT/0/$A/L/h265" | head -1)
cp "$MNT/0/$A/L/h265/$k0" "$WORK/k.hevc" || fail "cp h265"
ffmpeg -nostdin -v error -y -f hevc -i "$WORK/k.hevc" -frames:v 1 "$WORK/k.png" || fail "h265 key does not decode"
[ "$(dims "$WORK/k.png")" = "3840,2160" ] || fail "h265 key dims"
y2=$(ls "$MNT/0/$A/L/yuv" | sed -n 3p)
[ "$(stat -c %s "$MNT/0/$A/L/yuv/$y2")" -eq $((3840 * 2160 * 3 / 2)) ] || fail "yuv exact size"
cp "$MNT/0/$A/L/yuv/$y2" "$WORK/2.yuv" && [ "$(stat -c %s "$WORK/2.yuv")" -eq $((3840 * 2160 * 3 / 2)) ] || fail "yuv copy size"
y4=$(ls "$MNT/0/$A/L/y4m" | sed -n 3p)
cp "$MNT/0/$A/L/y4m/$y4" "$WORK/2.y4m" || fail "cp y4m"
[ "$(stat -c %s "$MNT/0/$A/L/y4m/$y4")" -eq "$(stat -c %s "$WORK/2.y4m")" ] || fail "y4m exact size"
[ "$(dims "$WORK/2.y4m")" = "3840,2160" ] || fail "y4m dims"
b1=$(ls "$MNT/0/$A/R/bmp" | sed -n 2p)
[ "$(stat -c %s "$MNT/0/$A/R/bmp/$b1")" -eq $((54 + 3840 * 3 * 2160)) ] || fail "bmp exact size"
[ "$(dims "$MNT/0/$A/R/bmp/$b1")" = "3840,2160" ] || fail "bmp dims read in place"
pj=$(ls "$MNT/0/$A/P/jpg" | sed -n 1p)
[ "$(dims "$MNT/0/$A/P/jpg/$pj")" = "7680,2160" ] || fail "P jpg dims"
py=$(ls "$MNT/0/$A/P/yuv" | sed -n 2p)
[ "$(stat -c %s "$MNT/0/$A/P/yuv/$py")" -eq $((7680 * 2160 * 3 / 2)) ] || fail "P yuv size"
# partial reads: head of a yuv file without reading it all
[ "$(head -c 100 "$MNT/0/$A/L/yuv/$y2" | wc -c)" = 100 ] || fail "partial read"
# the second index serves its recording too
[ "$(dims "$MNT/1/$B/L/jpg/$(ls "$MNT/1/$B/L/jpg" | head -1)")" = "3840,2160" ] || fail "index 1 frame"
echo "test_mount: frames ok (jpg/h265/yuv/y4m/bmp L/R, P 7680x2160, sizes, partial reads, both indexes)"

# ---- errors and read-only ----------------------------------------------------------------
[ ! -e "$MNT/7" ] || fail "index 7 must not exist"
[ ! -e "$MNT/0/nope" ] || fail "unknown recording"
[ ! -e "$MNT/0/$A/aac" ] || fail "no audio view"
[ ! -e "$MNT/0/$A/L/gif" ] || fail "unknown format"
[ ! -e "$MNT/0/$A/P/h265" ] || fail "P has no h265"
[ ! -e "$MNT/0/$A/L/jpg/0000005_1_1.jpg" ] || fail "wrong frame name must not exist"
[ ! -e "$MNT/0/$A/L/jpg/5.jpg" ] || fail "bare index name must not exist (only canonical names)"
touch "$MNT/0/x" 2>/dev/null && fail "read-only: touch succeeded"
( echo x > "$f5" ) 2>/dev/null && fail "read-only: write succeeded"
rm "$f5" 2>/dev/null && fail "read-only: rm succeeded"
echo "test_mount: errors and read-only ok"

# ---- cache and concurrency ----------------------------------------------------------------
hits0=$(python3 -c "import json,sys; print(json.loads(open(sys.argv[1]).read().splitlines()[-1])['client']['cache_hits'])" "$MNT/stats")
cp "$f5" "$WORK/5b.jpg" && cmp -s "$WORK/5.jpg" "$WORK/5b.jpg" || fail "re-read jpg"
hits1=$(python3 -c "import json,sys; print(json.loads(open(sys.argv[1]).read().splitlines()[-1])['client']['cache_hits'])" "$MNT/stats")
[ "$hits1" -gt "$hits0" ] || fail "second read should hit the client cache ($hits0 -> $hits1)"
# the stats file: the server's (pretty-printed) JSON, then one line of client counters
python3 -c "import json,sys; L=open(sys.argv[1]).read().splitlines(); d=json.loads('\n'.join(L[:-1])); assert d['frames_ok'] > 5, d" "$MNT/stats" || fail "stats body is the server's"
pids=""
for i in 2 3 4 5 6 7; do
    ( f=$(ls "$MNT/0/$A/L/jpg" | sed -n "$((i * 9))p"); [ "$(dims "$MNT/0/$A/L/jpg/$f")" = "3840,2160" ] && touch "$WORK/c$i.ok" ) & pids="$pids $!"
done
wait $pids
for i in 2 3 4 5 6 7; do [ -f "$WORK/c$i.ok" ] || fail "concurrent read $i"; done
echo "test_mount: cache and 6 concurrent reads ok"

# ---- unmount -------------------------------------------------------------------------------
fusermount3 -u "$MNT" || fail "unmount"
wait "$MPID"; rc=$?; MPID=""
[ "$rc" -eq 0 ] || fail "ixmount exit code $rc"
kill -INT "$SPID"; wait "$SPID"; SPID=""
echo "test_mount: PASS"
