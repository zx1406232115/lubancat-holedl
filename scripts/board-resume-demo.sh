#!/bin/bash
# ============================================================
#  在开发板上演示真实的中断 + 断点续传
#
#  做法：
#    1. 起一个限速的 HTTP 服务器（支持 Range）
#    2. 开始下载，下到一半时真的把进程杀掉（模拟 Ctrl-C）
#    3. 查看中断现场：文件仍是空洞、状态文件记录已完成的块
#    4. 重新执行同一条命令 → 只补没下完的块
#    5. verify 逐字节比对 + sha256 对比
# ============================================================
set -u

cd ~/linux-homework || exit 1
BIN=./holedl
W=~/resume-demo
PORT=8799
THROTTLE=1048576
BLK=1M
INTERRUPT_AFTER=6

rm -rf "$W"; mkdir -p "$W"
mkdir -p ~/evidence
L=~/evidence/02-resume.txt

# ---- 造 32MiB 源文件 ----
# 用 dd 生成，尺寸精确可控。verify 是逐字节比对，源内容本身不需要有规律。
dd if=/dev/urandom of="$W/source.bin" bs=1M count=32 2>/dev/null
EXPECT=$(( 32 * 1048576 ))
GOT=$(stat -c %s "$W/source.bin")
if [ "$GOT" != "$EXPECT" ]; then
  echo "ERROR: source size $GOT != expected $EXPECT"
  exit 1
fi

python3 tests/range_server.py "$W" "$PORT" "$THROTTLE" >/dev/null 2>&1 &
SRV=$!
sleep 1.5

URL="http://127.0.0.1:$PORT/source.bin"
OUT="$W/download.bin"
rm -f "$OUT" "$OUT.holedl.state"

{
echo "================================================================"
echo " Real interrupt + resume test   (on the board)"
echo "================================================================"
echo
echo "source     : $URL   (server throttled to $THROTTLE B/s)"
echo "block size : $BLK"
echo "sha256(src): $(sha256sum "$W/source.bin" | cut -d' ' -f1)"
echo
echo "----------------------------------------------------------------"
echo " step 1: start download, interrupt it after ${INTERRUPT_AFTER}s"
echo "         (the process is really killed = same as Ctrl-C)"
echo "----------------------------------------------------------------"
} 2>&1 | tee "$L"

$BIN download "$URL" "$OUT" --block "$BLK" >>"$L" 2>&1 &
DL=$!
sleep "$INTERRUPT_AFTER"
if kill -0 $DL 2>/dev/null; then
  kill -INT $DL 2>/dev/null
  sleep 0.5
  kill -9 $DL 2>/dev/null
  KILLED=yes
else
  KILLED=no
fi
wait $DL 2>/dev/null

{
echo
echo "process interrupted? : $KILLED"
echo
echo "----------------------------------------------------------------"
echo " step 2: state right after the interrupt"
echo "----------------------------------------------------------------"
if [ -f "$OUT" ]; then
  printf '  logical size (ls -l) : %s bytes\n' "$(stat -c %s "$OUT")"
  printf '  allocated (du -h)    : %s\n' "$(du -h "$OUT" | cut -f1)"
  printf '  st_blocks            : %s blocks of 512B\n' "$(stat -c %b "$OUT")"
else
  echo "  (target file not created yet)"
fi
echo
echo "  progress state file (records which blocks finished):"
if [ -f "$OUT.holedl.state" ]; then
  sed 's/^/      /' "$OUT.holedl.state"
else
  echo "      (no state file)"
fi
echo
echo "  => the file is still mostly a HOLE: only fetched blocks use disk"
} 2>&1 | tee -a "$L"

{
echo
echo "----------------------------------------------------------------"
echo " step 3: re-run the SAME command -> resumes only missing blocks"
echo "----------------------------------------------------------------"
} 2>&1 | tee -a "$L"

$BIN download "$URL" "$OUT" --block "$BLK" >>"$L" 2>&1
RC=$?

{
echo
echo "----------------------------------------------------------------"
echo " step 4: verify integrity"
echo "----------------------------------------------------------------"
} 2>&1 | tee -a "$L"

$BIN verify "$W/source.bin" "$OUT" >>"$L" 2>&1
VRC=$?
echo "  resume exit code : $RC" >>"$L"
echo "  verify exit code : $VRC" >>"$L"

{
echo
echo "  sha256(source) : $(sha256sum "$W/source.bin" | cut -d' ' -f1)"
echo "  sha256(target) : $(sha256sum "$OUT" | cut -d' ' -f1)"
echo
echo "  final file:"
echo "    logical : $(stat -c %s "$OUT") bytes"
echo "    on disk : $(du -h "$OUT" | cut -f1)"
echo
echo "================================================================"
echo " RESULT: resume exit=$RC  verify exit=$VRC  (0 = success)"
echo "================================================================"
} 2>&1 | tee -a "$L"

kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
echo
echo "evidence written to $L"
