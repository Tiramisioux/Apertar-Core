#!/bin/sh
# Host test: the direct payload write (default) and the scratch-copy path (APERTAR_DNG_PAYLOAD_COPY=1)
# must produce byte-identical DNGs, for every layout; APERTAR_DNG_WRITE_ROWS must not change the bytes.
#   sh tools/dng_payload_write_test.sh
set -eu
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/apertar-dng-payload-test
rm -rf "$OUT"; mkdir -p "$OUT/direct" "$OUT/copy" "$OUT/rows512"
g++ -std=c++20 -O2 -Wall -Wextra -I src -I include tools/dng_payload_write_test.cpp \
    src/cdng/CdngEncoder.cpp src/cdng/LosslessJpegEncoder.cpp src/color/ColorProfileConfig.cpp \
    -o "$OUT/test" -lyaml -lpthread
"$OUT/test" "$OUT/direct"
APERTAR_DNG_PAYLOAD_COPY=1 "$OUT/test" "$OUT/copy"
APERTAR_DNG_PAYLOAD_COPY=1 APERTAR_DNG_WRITE_ROWS=512 "$OUT/test" "$OUT/rows512"
status=0
for f in "$OUT"/direct/*.dng; do
    n=$(basename "$f")
    if cmp -s "$f" "$OUT/copy/$n" && cmp -s "$f" "$OUT/rows512/$n"; then
        echo "identical $n"
    else
        echo "DIFFERENT $n"; status=1
    fi
done
if command -v strace >/dev/null 2>&1; then
    echo "write() calls, direct path:"; strace -f -c -e trace=write,writev "$OUT/test" "$OUT/direct" 2>&1 | grep -E 'write|total' || true
    echo "write() calls, copy path:";  APERTAR_DNG_PAYLOAD_COPY=1 strace -f -c -e trace=write,writev "$OUT/test" "$OUT/copy" 2>&1 | grep -E 'write|total' || true
fi
rm -rf "$OUT"
[ "$status" -eq 0 ] && echo "PASS" || echo "FAIL"
exit $status
