#!/bin/bash
# ============================================================
#  demo.sh - 一键演示脚本（照着跑，每步输出都能直接截图交作业）
#
#  用法:  bash tests/demo.sh
#
#  演示内容对应作业的四个功能要求：
#     ① create  生成空洞文件：逻辑 128MiB，实际占用 0
#     ② write   定点写入：写 4KiB，只有那几块变实，其它全是空洞
#     ③ 续传    Ctrl-C 中断后继续，且续传结果校验一致
#     ④ status  打印逻辑大小 / 实际占用块数 / 空洞占比
# ============================================================
set -u

BIN="${BIN:-./holedl}"
DIR="${DIR:-/tmp/holedl-demo}"
SRV_PORT="${SRV_PORT:-8765}"

cd "$(dirname "$0")/.." || exit 1

if [ ! -x "$BIN" ]; then
  echo "找不到 $BIN，请先执行 make" >&2
  exit 1
fi

rm -rf "$DIR"
mkdir -p "$DIR"

banner() {
  echo
  echo "################################################################"
  echo "#  $*"
  echo "################################################################"
}

hr() { echo "----------------------------------------------------------------"; }

# ============================================================
banner "① create：生成逻辑 128MiB 的空洞文件"
# ============================================================
F="$DIR/hole.bin"
"$BIN" create "$F" 128M
hr
echo "用系统命令交叉验证（这一步最能说明问题）："
ls -l "$F" | awk '{printf "  ls -l  逻辑大小 = %s 字节\n", $5}'
echo "  du     实际占用 = $(du -h "$F" | cut -f1)"
echo "  stat   实际块数 = $(stat -c '%b' "$F") 个 512B 块"
hr
echo "结论：逻辑 128MiB，磁盘实际占用 0 —— 未写入区域不占任何数据块。"

# ============================================================
banner "② write：定点写入，不破坏其它空洞"
# ============================================================
"$BIN" write "$F" --offset 4M --len 4K --byte 0xAA
hr
echo "再在 96MiB 处写 4KiB："
"$BIN" write "$F" --offset 96M --len 4K --byte 0xBB
hr
echo "此时再看系统口径："
echo "  逻辑大小 = $(stat -c %s "$F") 字节 ($(du -h --apparent-size "$F" | cut -f1))"
echo "  实际占用 = $(du -h "$F" | cut -f1)   <- 只写了 8KiB，占了 2 个块"
hr
echo "读一下证明：0 处仍是空洞，4MiB 处变成了 0xAA"
echo -n "  偏移 0    首字节: "; dd if="$F" bs=1 skip=0 count=1 2>/dev/null | od -An -tx1
echo -n "  偏移 4MiB 首字节: "; dd if="$F" bs=1 skip=$((4*1024*1024)) count=1 2>/dev/null | od -An -tx1
echo -n "  偏移 96MiB首字节: "; dd if="$F" bs=1 skip=$((96*1024*1024)) count=1 2>/dev/null | od -An -tx1

# ============================================================
banner "④ status：打印逻辑大小 / 实际占用 / 空洞占比 / 占用图"
# ============================================================
"$BIN" status "$F" --map

# ============================================================
banner "③ 断点续传：限速下载 + Ctrl-C 中断 + 续传"
# ============================================================
SRC="$DIR/source.bin"
echo "造一个 32MiB 的源文件……"
head -c 33554432 /dev/urandom > "$SRC"
echo "  源文件大小: $(stat -c %s "$SRC") 字节  sha256: $(sha256sum "$SRC" | cut -c1-16)…"

if ! command -v python3 >/dev/null 2>&1; then
  echo "没有 python3，跳过 HTTP 断点续传演示（可用本地文件源代替）"
  exit 0
fi

# 限速 1MiB/s → 32MiB 大约需要 32 秒，留足按 Ctrl-C 的时间。
# 分块 1MiB → 共 32 块，输出不会太长。
THROTTLE=1048576
BLK=1M
python3 tests/range_server.py "$DIR" "$SRV_PORT" "$THROTTLE" >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.2

URL="http://127.0.0.1:$SRV_PORT/source.bin"
OUT="$DIR/download.bin"
rm -f "$OUT" "$OUT.holedl.state"

hr
echo "服务器限速 $THROTTLE B/s，分块 $BLK，总共约 32 秒。"
echo
echo "    >>>  下完 3～5 块之后按 Ctrl-C 中断（约 5 秒后开始按）  <<<"
echo
hr
# 给操作者留出准备时间
for i in 5 4 3 2 1; do printf "\r    倒计时 %d ..." "$i"; sleep 1; done
printf "\r                        \n"

# 前台运行，方便按 Ctrl-C；被中断后脚本继续往下走
"$BIN" download "$URL" "$OUT" --block "$BLK"

hr
echo "中断后的状态（注意：只下了几块，其它仍是空洞）："
ls -l "$OUT" 2>/dev/null | awk '{printf "  逻辑大小 = %s 字节\n", $5}'
echo "  实际占用 = $(du -h "$OUT" 2>/dev/null | cut -f1)"
echo "  状态文件内容（记录哪些块已完成）："
sed 's/^/      /' "$OUT.holedl.state" 2>/dev/null || echo "      (没有状态文件)"
echo
echo "  已下载部分的数据是正确的（前 1MiB 与源文件比对）："
if cmp -s <(head -c 1048576 "$SRC") <(head -c 1048576 "$OUT"); then
  echo "      ✔ 一致"
else
  echo "      ? 第 1 块尚未下完（中断得比较早），这是正常的"
fi

hr
echo "现在重新执行同一条命令 —— 注意它只补没下完的块："
hr
"$BIN" download "$URL" "$OUT" --block "$BLK"

hr
echo "续传完成后做完整性校验（逐字节比对源文件）："
"$BIN" verify "$SRC" "$OUT"
hr
echo "sha256 对比："
echo "  源  : $(sha256sum "$SRC" | cut -d' ' -f1)"
echo "  目标: $(sha256sum "$OUT" | cut -d' ' -f1)"

kill $SRV 2>/dev/null
trap - EXIT

# ============================================================
banner "完成"
# ============================================================
echo "演示结束。全部数据在 $DIR"
echo "对应作业四项要求：create 空洞 / write 定点写 / status 统计 / 断点续传"
