# 在开发板上编译、自测、演示（复制到板子上执行）
#
# 用法：在板子上工程目录里执行
#   bash scripts/board-build.sh           # 安装依赖 + 编译 + 跑 31 项自测
#   bash scripts/board-build.sh --demo    # 额外跑交互式演示（会提示按 Ctrl-C）

set -u

echo "########## 0. 板卡信息 ##########"
echo "  架构: $(uname -m)   主机名: $(hostname)"
echo "  发行版: $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"

if [ "$(uname -m)" != "aarch64" ]; then
  echo "  ⚠️  注意：当前架构是 $(uname -m)，不是 aarch64"
fi

echo
echo "########## 1. 检查编译工具 ##########"
if ! command -v gcc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
  echo "  缺少 gcc 或 make，尝试安装……"
  if command -v apt >/dev/null 2>&1; then
    sudo apt update && sudo apt install -y build-essential
  elif command -v opkg >/dev/null 2>&1; then
    sudo opkg update && sudo opkg install gcc make libc-dev
  else
    echo "  ✘ 没有可用的包管理器，请手动安装 gcc 和 make" >&2
    exit 1
  fi
fi
echo "  gcc : $(gcc --version | head -1)"
echo "  make: $(make --version | head -1)"

echo
echo "########## 2. 确认在工程根目录 ##########"
if [ ! -f Makefile ] || [ ! -d src ]; then
  echo "  ✘ 当前目录不是工程根目录（找不到 Makefile / src）" >&2
  echo "     请先 cd 到 linux-homework 目录再执行" >&2
  exit 1
fi
echo "  当前目录: $(pwd)"

echo
echo "########## 3. 编译 ##########"
make clean >/dev/null 2>&1
if ! make; then
  echo "  ✘ 编译失败" >&2
  exit 1
fi
echo
echo "  --- 产物架构确认（这是板端原生编译的证据）---"
echo "  uname -m  : $(uname -m)"
file holedl

echo
echo "########## 4. 功能自测（31 项）##########"
if bash tests/run_tests.sh ./holedl; then
  TEST_RC=0
else
  TEST_RC=$?
fi

echo
echo "########## 5. 空洞机制快速验证（可直接截图）##########"
WORK=$(mktemp -d /tmp/blk.XXXXXX)
cd "$WORK" || exit 1
BIN="$(cd - >/dev/null && pwd)/holedl"

"$BIN" create demo.bin 128M
echo
echo "  ls -l 逻辑大小 : $(stat -c %s demo.bin) 字节"
echo "  du    实际占用 : $(du -h demo.bin | cut -f1)"
echo "  st_blocks      : $(stat -c %b demo.bin) 个 512B 块  <- 必须是 0"
echo
echo "  在 4MiB 处写 4KiB："
"$BIN" write demo.bin --offset 4M --len 4K --byte 0xAA
echo
echo "  du 现在显示    : $(du -h demo.bin | cut -f1)   <- 只占 4KiB"
echo
"$BIN" status demo.bin --map

cd - >/dev/null
rm -rf "$WORK"

echo
if [ "${1:-}" = "--demo" ]; then
  echo "########## 6. 交互式演示（Ctrl-C 断点续传）##########"
  bash tests/demo.sh
fi

echo
echo "########## 完成 ##########"
if [ "$TEST_RC" -eq 0 ]; then
  echo "  ✔ 编译通过、31 项自测全过"
else
  echo "  ✘ 自测有失败项，请把上面输出发回来"
fi
exit "$TEST_RC"
