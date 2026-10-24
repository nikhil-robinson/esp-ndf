#!/bin/sh
# fattool round trip at FAT12 / FAT16 / FAT32 sizes, checked by dosfstools fsck.fat and by extracting again.
set -e
T=$(mktemp -d); trap 'rm -rf $T' EXIT
mkdir -p "$T/src/sub dir"; echo hello > "$T/src/HELLO.TXT"; head -c 70000 /dev/urandom > "$T/src/sub dir/long random name.bin"
for sz in 1048576 16777216 40001536; do
    python3 tools/fattool.py mkfat "$T/src" "$T/i.bin" $sz --sector 512 > /dev/null
    fsck.fat -n "$T/i.bin" > "$T/fsck.txt" 2>&1 || { cat "$T/fsck.txt"; exit 1; }
    rm -rf "$T/x"; python3 tools/fattool.py parse "$T/i.bin" -x "$T/x" | head -1
    diff -r "$T/src" "$T/x"
done
echo "fattool: FAT12 / FAT16 / FAT32 images pass fsck.fat and round-trip"
