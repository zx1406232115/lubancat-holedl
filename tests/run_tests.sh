#!/bin/bash
# ============================================================
#  run_tests.sh - holedl 功能自测
#
#  逐条验证作业的四个功能要求：
#    ① create  空洞文件：逻辑 >=128KB、未写入区不占磁盘块
#    ② write   lseek+write 定点写，不破坏其它空洞
#    ③ 续传    中断后从断点继续
#    ④ status  打印逻辑大小/实际占用块数/空洞占比
#
#  用法: bash tests/run_tests.sh [holedl路径]
# ============================================================
set -u

BIN="${1:-./holedl}"
WORK="$(mktemp -d /tmp/holedl-test.XXXXXX)"
PASS=0
FAIL=0

if [ ! -x "$BIN" ]; then
  echo "找不到可执行文件: $BIN （先执行 make）" >&2
  exit 1
fi

ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
head1() { echo; echo "=== $1 ==="; }

# 取「实际占用字节」(st_blocks * 512)，这是判断空洞有没有被破坏的唯一硬指标
alloc_bytes() { stat -c '%b * 512' "$1" | bc; }
logic_bytes() { stat -c '%s' "$1"; }

# 判断文件在 off 处连续 len 字节是否全为 0（即是否仍为空洞）
is_zero_at() {
  local f="$1" off="$2" len="$3"
  dd if="$f" bs=1 skip="$off" count="$len" 2>/dev/null \
    | cmp -s - <(dd if=/dev/zero bs=1 count="$len" 2>/dev/null)
}

# 读单字节的十六进制值
hex_at() {
  dd if="$1" bs=1 skip="$2" count=1 2>/dev/null | od -An -tx1 | tr -d ' \n'
}

# 按 holedl 的状态文件格式生成进度位图（每 4 个块 = 1 个 hex 字符）
# 用法: make_bitmap <总块数> <已完成块数>
# 例:   make_bitmap 6 3   -> e0     make_bitmap 32 5 -> f800000000
make_bitmap() {
  local nblocks="$1" done="$2" nchars i v b bit out=""
  nchars=$(( (nblocks + 3) / 4 ))
  i=0
  while [ $i -lt $nchars ]; do
    v=0
    b=0
    while [ $b -lt 4 ]; do
      bit=$(( i * 4 + b ))
      if [ $bit -lt $nblocks ] && [ $bit -lt $done ]; then
        v=$(( v | (1 << (3 - b)) ))
      fi
      b=$(( b + 1 ))
    done
    out="$out$(printf '%x' $v)"
    i=$(( i + 1 ))
  done
  printf '%s' "$out"
}

echo "holedl 功能自测"
echo "工作目录: $WORK"
echo "可执行文件: $(readlink -f "$BIN")"
echo "内核: $(uname -r)  架构: $(uname -m)"

# ------------------------------------------------------------
head1 "测试 1: create 生成空洞文件，逻辑 64MiB 但实占 0"
# ------------------------------------------------------------
F1="$WORK/hole.bin"
"$BIN" create "$F1" 64M
L=$(logic_bytes "$F1"); A=$(alloc_bytes "$F1")
if [ "$L" -eq 67108864 ]; then ok "逻辑大小 = 64MiB ($L B)"; else bad "逻辑大小错误: $L"; fi
if [ "$A" -eq 0 ]; then ok "实际占用 = 0 字节（全是空洞）"; else bad "实际占用应为 0，实际 $A"; fi

# 逻辑大小也要满足 >=128KB
if [ "$L" -ge 131072 ]; then ok "逻辑大小 >= 128KiB"; else bad "逻辑大小 < 128KiB"; fi

# du 交叉验证：du 显示的是磁盘占用
DU=$(du -B1 "$F1" | cut -f1)
if [ "$DU" -eq 0 ]; then ok "du 报告 0 字节（与 st_blocks 一致）"; else bad "du 报告 $DU 字节"; fi

# 空洞读出来必须全 0。
# 注意：不要用 `od | tr -d '0'` 判断——那样会把 0x10、0x30 里的 0 也删掉，
#       造成假通过/假失败。正确做法是直接和等长的全零文件比。
dd if=/dev/zero of="$WORK/zeros.bin" bs=4096 count=1 2>/dev/null
if cmp -s <(dd if="$F1" bs=4096 count=1 2>/dev/null) "$WORK/zeros.bin"; then
  ok "读取空洞返回全 0"
else
  bad "空洞内容非 0"
fi

# ------------------------------------------------------------
head1 "测试 2: write 定点写入，不破坏其它空洞"
# ------------------------------------------------------------
# 在 1MiB 处写 4KiB
"$BIN" write "$F1" --offset 1M --len 4K --byte 0xAA >/dev/null
A2=$(alloc_bytes "$F1")
L2=$(logic_bytes "$F1")
if [ "$L2" -eq "$L" ]; then ok "写入后逻辑大小不变 ($L2)"; else bad "逻辑大小被改成了 $L2"; fi

# 只写了 4096 字节，实际占用应该很小（一个 4KiB 块左右），远小于逻辑大小
if [ "$A2" -gt 0 ] && [ "$A2" -lt 1048576 ]; then
  ok "实际占用仅 $A2 字节（写 4KiB 只占少量块，远小于 64MiB）"
else
  bad "实际占用异常: $A2"
fi

# 写入点之外必须还是空洞（用 is_zero_at 严格按字节比对，不靠删字符）
if is_zero_at "$F1" 0 64; then
  ok "偏移 0 处仍是空洞（全 0）"
else
  bad "偏移 0 处被写脏了"
fi

# 写入点本身必须真的是 0xAA
B=$(hex_at "$F1" 1048576)
if [ "$B" = "aa" ]; then ok "偏移 1MiB 处确实是 0xAA"; else bad "偏移 1MiB 处是 $B，应为 aa"; fi

# 空洞总量应该还是绝大部分
if [ "$A2" -lt $((L / 100)) ]; then ok "空洞仍占 99% 以上"; else bad "空洞比例过低: $A2/$L"; fi

# ------------------------------------------------------------
head1 "测试 3: 多次定点写入不同偏移，每次都不碰其它区域"
# ------------------------------------------------------------
"$BIN" write "$F1" --offset 32M --len 8K --byte 0xBB >/dev/null
A3=$(alloc_bytes "$F1")
B1=$(hex_at "$F1" 1048576)
B2=$(hex_at "$F1" $((32*1024*1024)))
B3=$(hex_at "$F1" $((16*1024*1024)))
if [ "$B1" = "aa" ]; then ok "1MiB 处旧数据未被覆盖 (0xAA)"; else bad "1MiB 处变成了 $B1"; fi
if [ "$B2" = "bb" ]; then ok "32MiB 处新数据正确 (0xBB)"; else bad "32MiB 处是 $B2"; fi
if [ "$B3" = "00" ]; then ok "16MiB 处仍是空洞 (0x00)"; else bad "16MiB 处是 $B3，应为 00"; fi
if [ "$A3" -lt 1048576 ]; then ok "两处写入合计实占仅 $A3 字节"; else bad "实占偏大: $A3"; fi

# ------------------------------------------------------------
head1 "测试 4: status 输出逻辑大小 / 实际占用块 / 空洞占比"
# ------------------------------------------------------------
OUT=$("$BIN" status "$F1" --map)
echo "$OUT" | sed 's/^/    /'
echo "$OUT" | grep -q "逻辑大小" && ok "打印了逻辑大小" || bad "未打印逻辑大小"
echo "$OUT" | grep -q "实际占用" && ok "打印了实际占用" || bad "未打印实际占用"
echo "$OUT" | grep -q "空洞占比" && ok "打印了空洞占比" || bad "未打印空洞占比"
echo "$OUT" | grep -q "占用图"  && ok "打印了占用图"   || bad "未打印占用图"

# ------------------------------------------------------------
head1 "测试 5: 本地文件源断点续传（含强制中断 + 续传后一致性）"
# ------------------------------------------------------------
SRC="$WORK/src.bin"
OUTF="$WORK/dl.bin"

# 造一个有辨识度的源文件：1.5MiB，内容按块区分
head -c 1572864 /dev/urandom > "$SRC"

# 5a) 正常下完
"$BIN" download "$SRC" "$OUTF" --block 256K
if "$BIN" verify "$SRC" "$OUTF" >/dev/null; then ok "本地源下载后内容与源一致"; else bad "内容不一致"; fi
if [ ! -f "$OUTF.holedl.state" ]; then ok "全部完成后状态文件被清理"; else bad "状态文件残留"; fi

# 5b) 模拟中断：删掉状态文件+目标文件，手工构造「下到一半」的局面
rm -f "$OUTF" "$OUTF.holedl.state"
SIZE=$(logic_bytes "$SRC")
"$BIN" create "$OUTF" "$SIZE" >/dev/null
# 只把前 3 块的内容写进去（模拟下到第 3 块时被 Ctrl-C）
dd if="$SRC" of="$OUTF" bs=256K count=3 conv=notrunc 2>/dev/null
# 造出对应的状态文件：前 3 块标记完成
NBLK=$(( (SIZE + 262144 - 1) / 262144 ))
HEX=$(make_bitmap "$NBLK" 3)
printf 'HOLEDL1\n%s\n%s\n262144\n%s\n' "$SRC" "$SIZE" "$HEX" > "$OUTF.holedl.state"

A_BEFORE=$(alloc_bytes "$OUTF")
RESUME_OUT=$("$BIN" download "$SRC" "$OUTF" --block 256K)
if echo "$RESUME_OUT" | grep -q "发现断点状态"; then
  ok "续传时正确识别出已有进度"
else
  bad "续传未识别断点状态（退化成全新下载了）"
fi
if "$BIN" verify "$SRC" "$OUTF" >/dev/null; then ok "断点续传后内容与源完全一致"; else bad "续传后内容不一致"; fi
if [ ! -f "$OUTF.holedl.state" ]; then ok "续传完成后状态文件被清理"; else bad "续传后状态文件残留"; fi

# ------------------------------------------------------------
head1 "测试 5c: 回归测试 — 块数 >16 时位图编解码必须一致"
# ------------------------------------------------------------
# 背景：曾经出现过「写入方按每字节 8 位、读取方按每半字节 4 位」的编码不一致，
#       块数 <=16 时两边长度碰巧相等所以测试全过，块数一多续传就静默失效。
#       这个用例专门覆盖大块数。
BIGSRC="$WORK/big-src.bin"
BIGOUT="$WORK/big-resume.bin"
head -c 3145728 /dev/urandom > "$BIGSRC"      # 3MiB, 64KiB 块 => 48 块
rm -f "$BIGOUT" "$BIGOUT.holedl.state"
BSIZE=$(logic_bytes "$BIGSRC")
"$BIN" create "$BIGOUT" "$BSIZE" >/dev/null
dd if="$BIGSRC" of="$BIGOUT" bs=65536 count=20 conv=notrunc 2>/dev/null   # 前 20 块
NB=$(( (BSIZE + 65536 - 1) / 65536 ))
HB=$(make_bitmap "$NB" 20)
printf 'HOLEDL1\n%s\n%s\n65536\n%s\n' "$BIGSRC" "$BSIZE" "$HB" > "$BIGOUT.holedl.state"
if [ "$NB" -le 16 ]; then
  bad "用例设计有误：块数应大于 16，实际 $NB"
else
  ok "用例覆盖大块数（共 $NB 块，已完成 20 块，位图 $HB）"
fi
RESUME_OUT2=$("$BIN" download "$BIGSRC" "$BIGOUT" --block 64K)
if echo "$RESUME_OUT2" | grep -q "已完成 20 块"; then
  ok "大块数下续传正确识别出「已完成 20 块」"
else
  bad "大块数下续传未识别断点状态"
fi
if "$BIN" verify "$BIGSRC" "$BIGOUT" >/dev/null; then
  ok "大块数续传后内容与源完全一致"
else
  bad "大块数续传后内容不一致"
fi

# ------------------------------------------------------------
head1 "测试 6: 未下载完时，文件是空洞（磁盘占用 < 逻辑大小）"
# ------------------------------------------------------------
O2="$WORK/partial.bin"
rm -f "$O2" "$O2.holedl.state"
BIG="$WORK/big.bin"
head -c 8388608 /dev/urandom > "$BIG"
"$BIN" create "$O2" 8388608 >/dev/null
dd if="$BIG" of="$O2" bs=1048576 count=1 conv=notrunc 2>/dev/null
A6=$(alloc_bytes "$O2"); L6=$(logic_bytes "$O2")
if [ "$A6" -lt "$L6" ]; then
  ok "部分下载的文件实占 $A6 < 逻辑 $L6（空洞机制生效）"
else
  bad "实占 $A6 不小于逻辑 $L6"
fi

# ------------------------------------------------------------
head1 "测试 7: HTTP 源下载 + 断点续传（需要 python3）"
# ------------------------------------------------------------
# 注意：不能用 `python3 -m http.server`，它不支持 Range（对 Range 请求回 200 整包），
#       而断点续传必须靠 206 Partial Content。所以用 tests/range_server.py。
if command -v python3 >/dev/null 2>&1 && [ -f "$(dirname "$0")/range_server.py" ]; then
  PORT=$(( 8900 + (RANDOM % 100) ))
  python3 "$(dirname "$0")/range_server.py" "$WORK" "$PORT" >/dev/null 2>&1 &
  SRV=$!
  sleep 1.2

  if ! kill -0 $SRV 2>/dev/null; then
    bad "Range 测试服务器启动失败"
  else
    URL="http://127.0.0.1:$PORT/src.bin"

    # 7a) 完整下载
    HOUT="$WORK/http.bin"
    rm -f "$HOUT" "$HOUT.holedl.state"
    if "$BIN" download "$URL" "$HOUT" --block 256K >/dev/null 2>&1; then
      if "$BIN" verify "$SRC" "$HOUT" >/dev/null; then
        ok "HTTP 完整下载后内容与源一致（验证了 Range 定点写）"
      else
        bad "HTTP 下载内容不一致"
      fi
    else
      bad "HTTP 下载失败"
    fi

    # 7b) HTTP 断点续传：伪造「已下 2 块」的状态，验证只补剩下的块
    HRES="$WORK/http-resume.bin"
    rm -f "$HRES" "$HRES.holedl.state"
    HSIZE=$(logic_bytes "$SRC")
    "$BIN" create "$HRES" "$HSIZE" >/dev/null
    dd if="$SRC" of="$HRES" bs=262144 count=2 conv=notrunc 2>/dev/null
    HN=$(( (HSIZE + 262144 - 1) / 262144 ))
    printf 'HOLEDL1\n%s\n%s\n262144\n%s\n' "$URL" "$HSIZE" "$(make_bitmap "$HN" 2)" > "$HRES.holedl.state"

    RESUME_OUT=$("$BIN" download "$URL" "$HRES" --block 256K)
    if echo "$RESUME_OUT" | grep -q "已完成 2 块"; then
      ok "HTTP 续传正确识别出「已完成 2 块」"
    else
      bad "HTTP 续传未识别断点状态"
    fi
    if "$BIN" verify "$SRC" "$HRES" >/dev/null; then
      ok "HTTP 断点续传后内容与源完全一致"
    else
      bad "HTTP 续传后内容不一致"
    fi

    # 7c) 服务器不支持 Range 时，应降级为顺序下载且结果仍然正确
    #     （用 python3 -m http.server 模拟「不支持 Range」的服务器）
    DUMBPORT=$(( PORT + 200 ))
    ( cd "$WORK" && python3 -m http.server $DUMBPORT --bind 127.0.0.1 >/dev/null 2>&1 ) &
    DUMB=$!
    sleep 1.2
    FB="$WORK/fallback.bin"
    rm -f "$FB" "$FB.holedl.state"
    if "$BIN" download "http://127.0.0.1:$DUMBPORT/src.bin" "$FB" --block 256K >/dev/null 2>&1; then
      if "$BIN" verify "$SRC" "$FB" >/dev/null; then
        ok "不支持 Range 的服务器：降级顺序下载后内容仍正确"
      else
        bad "降级顺序下载后内容不一致"
      fi
    else
      bad "降级顺序下载失败"
    fi
    kill $DUMB 2>/dev/null; wait $DUMB 2>/dev/null
  fi

  kill $SRV 2>/dev/null
  wait $SRV 2>/dev/null
else
  echo "  [SKIP] 未找到 python3 或 range_server.py，跳过 HTTP 测试"
fi

# ------------------------------------------------------------
echo
echo "==================== 结果 ===================="
echo "  通过: $PASS    失败: $FAIL"
echo "  测试数据保留在: $WORK"
if [ "$FAIL" -eq 0 ]; then
  echo "  ✔ 全部功能测试通过"
  exit 0
else
  echo "  ✘ 有失败项"
  exit 1
fi
