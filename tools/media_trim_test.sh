#!/bin/sh
# Host test for scripts/apertar-media-trim: argument-validation paths only.
# Run: sh tools/media_trim_test.sh   (the success path needs the Pi)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
HELPER="$HERE/../scripts/apertar-media-trim"
FAILS=0

expect2() {
    # expect2 <label> <args...>
    label=$1; shift
    out=$(sh "$HELPER" "$@" 2>&1 >/dev/null)
    rc=$?
    if [ "$rc" -eq 2 ]; then
        echo "[ok]   $label"
    else
        echo "[FAIL] $label: exit $rc, expected 2 ($out)"
        FAILS=$((FAILS + 1))
    fi
}

NL='
'
expect2 "no args"
expect2 "two args" /media/a /media/b
expect2 "/" /
expect2 "/media" /media
expect2 "/media/" /media/
expect2 "/media/../etc" /media/../etc
expect2 "/etc/passwd" /etc/passwd
expect2 "/media/RAW; ls" "/media/RAW; ls"
expect2 "path with space" "/media/my drive"
expect2 "empty string" ""
expect2 "newline" "/media/RAW${NL}/etc"
expect2 "/media/NOPE (not a mount point / tool missing)" /media/NOPE

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS failed"
    exit 1
fi
echo "all ok"
