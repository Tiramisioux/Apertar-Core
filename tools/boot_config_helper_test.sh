#!/bin/sh
# Host test for scripts/apertar-boot-config, run on a scratch copy whose DEST points into a temp folder.
#   sh tools/boot_config_helper_test.sh
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/firmware"
sed -e "s#^DEST=.*#DEST=$WORK/firmware/config.txt#" -e "s#^BACKUP=.*#BACKUP=$WORK/firmware/config.txt.apertar.bak#" \
    "$HERE/scripts/apertar-boot-config" >"$WORK/helper"
FAILED=0
check() { if [ "$1" = 0 ]; then echo "[ok] $2"; else echo "[FAIL] $2"; FAILED=$((FAILED + 1)); fi; }
run() { sh "$WORK/helper" "$@" 2>"$WORK/err"; }

printf '[all]\ndtparam=audio=on\n' >"$WORK/new"
printf '[pi5]\ndtparam=i2c_arm=on\n[all]\n' >"$WORK/firmware/config.txt"

run <"$WORK/new"; rc=$?
[ $rc -eq 0 ] && cmp -s "$WORK/new" "$WORK/firmware/config.txt"; check $? "replaces config.txt with stdin"
printf '[pi5]\ndtparam=i2c_arm=on\n[all]\n' | cmp -s - "$WORK/firmware/config.txt.apertar.bak"; check $? "keeps the previous file as config.txt.apertar.bak"
[ -z "$(ls -A "$WORK/firmware" | grep '^\.config')" ]; check $? "leaves no temp file"

run x <"$WORK/new"; [ $? -eq 2 ]; check $? "an argument is refused (2)"
: | run; [ $? -eq 2 ]; check $? "empty input is refused (2)"
printf 'dtparam=audio=on\n' | run; [ $? -eq 2 ]; check $? "text without a [pi5]/[pi4]/[all] section is refused (2)"
printf '[all]\n\000x\n' | run; [ $? -eq 2 ]; check $? "NUL bytes are refused (2)"
head -c 70000 /dev/zero | tr '\000' 'a' | { printf '[all]\n'; cat; } | run; [ $? -eq 2 ]; check $? "more than 64 KiB is refused (2)"
cmp -s "$WORK/new" "$WORK/firmware/config.txt"; check $? "a refused input leaves config.txt as it was"
rm "$WORK/firmware/config.txt"
run <"$WORK/new"; [ $? -eq 3 ]; check $? "no config.txt: exit 3"

if [ "$FAILED" -eq 0 ]; then echo "PASSED (0 failed)"; else echo "FAILED ($FAILED failed)"; fi
[ "$FAILED" -eq 0 ]
