#!/bin/bash
# Generate board-side evidence files (run ON the board)
set -u
cd ~/linux-homework || exit 1
mkdir -p ~/evidence

# 用绝对路径：下面会 cd 到工作目录，相对路径会失效
BIN="$HOME/linux-homework/holedl"
test -x "$BIN" || { echo "ERROR: $BIN not found or not executable"; exit 1; }
W=~/evidence-work
rm -rf "$W"; mkdir -p "$W"
cd "$W" || exit 1

# ============================================================
# 01: board identity + build artifact + sparse mechanism
# ============================================================
{
echo "================================================================"
echo " holedl - board / build / sparse-file evidence"
echo "================================================================"
echo
echo "-- board identity --"
echo "model      : $(tr -d '\0' < /proc/device-tree/model)"
echo "hostname   : $(hostname)"
echo "kernel     : $(uname -r)"
echo "arch       : $(uname -m)"
echo "os         : $(. /etc/os-release; echo "$PRETTY_NAME")"
echo "gcc        : $(gcc --version | head -1)"
echo
echo "-- executable (built natively on the board with its own gcc) --"
file ~/linux-homework/holedl
echo
echo "-- filesystem of the working directory --"
df -h . | tail -1
echo "fs type    : $(stat -f -c %T .)"
echo
echo "================================================================"
echo " (1) create : logical 128MiB sparse file"
echo "================================================================"
$BIN create demo.bin 128M
echo
echo "cross-check with system tools:"
printf '  ls -l   logical size : %s bytes\n' "$(stat -c %s demo.bin)"
printf '  du -h   disk usage   : %s\n' "$(du -h demo.bin | cut -f1)"
printf '  st_blocks            : %s blocks of 512B\n' "$(stat -c %b demo.bin)"
echo "  => logical 128MiB, but ZERO blocks on disk (all hole)"
echo
echo "reading the hole returns all zeros:"
printf '  first byte @ 0       : '; dd if=demo.bin bs=1 skip=0 count=1 2>/dev/null | od -An -tx1
echo
echo "================================================================"
echo " (2) write : 4KiB at offset 4MiB, then 4KiB at offset 96MiB"
echo "================================================================"
$BIN write demo.bin --offset 4M --len 4K --byte 0xAA
echo
$BIN write demo.bin --offset 96M --len 4K --byte 0xBB
echo
echo "system tools after the two 4KiB writes:"
printf '  ls -l   logical size : %s bytes   (unchanged)\n' "$(stat -c %s demo.bin)"
printf '  du -h   disk usage   : %s   <= only 8KiB was written\n' "$(du -h demo.bin | cut -f1)"
echo
echo "read back to prove every untouched region is still a hole:"
printf '  first byte @ 0       : '; dd if=demo.bin bs=1 skip=0 count=1 2>/dev/null | od -An -tx1
printf '  first byte @ 4MiB    : '; dd if=demo.bin bs=1 skip=4194304 count=1 2>/dev/null | od -An -tx1
printf '  first byte @ 96MiB   : '; dd if=demo.bin bs=1 skip=100663296 count=1 2>/dev/null | od -An -tx1
printf '  first byte @ 64MiB   : '; dd if=demo.bin bs=1 skip=67108864 count=1 2>/dev/null | od -An -tx1
echo
echo "================================================================"
echo " (3) status : logical size / allocated blocks / hole ratio"
echo "================================================================"
$BIN status demo.bin --map
} > ~/evidence/01-board-and-sparse.txt 2>&1

# ============================================================
# 03: end-to-end HTTP download (Range) + integrity
# ============================================================
PORT=8877
dd if=/dev/urandom of="$W/dl-src.bin" bs=1M count=12 2>/dev/null
python3 ~/linux-homework/tests/range_server.py "$W" "$PORT" >/dev/null 2>&1 &
SRV=$!
sleep 1.5

{
echo "================================================================"
echo " holedl - end-to-end HTTP download evidence"
echo "================================================================"
echo
echo "server : python3 tests/range_server.py   (supports Range / 206)"
echo "source : $W/dl-src.bin"
printf 'sha256(source) : %s\n' "$(sha256sum "$W/dl-src.bin" | cut -d' ' -f1)"
echo
echo "----------------------------------------------------------------"
echo " download (block = 1M)"
echo "----------------------------------------------------------------"
$BIN download "http://127.0.0.1:$PORT/dl-src.bin" "$W/dl-out.bin" --block 1M
echo
echo "----------------------------------------------------------------"
echo " result"
echo "----------------------------------------------------------------"
printf '  sha256(target) : %s\n' "$(sha256sum "$W/dl-out.bin" | cut -d' ' -f1)"
$BIN verify "$W/dl-src.bin" "$W/dl-out.bin"
echo
echo "  final file:"
printf '    logical size : %s bytes\n' "$(stat -c %s "$W/dl-out.bin")"
printf '    on disk      : %s\n' "$(du -h "$W/dl-out.bin" | cut -f1)"
} > ~/evidence/03-http-download.txt 2>&1

kill $SRV 2>/dev/null; wait $SRV 2>/dev/null

cd ~ || exit 1
rm -rf "$W"

echo "evidence regenerated:"
ls -la ~/evidence/
