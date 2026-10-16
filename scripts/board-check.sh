# 开发板环境探测（一次性拿到所有关键信息）
#
# 用法 —— 三选一：
#   A) 在 Windows PowerShell 里：
#        ssh cat@192.168.1.101 'bash -s' < scripts\board-check.sh
#   B) 先 ssh 进板子，再粘贴整个文件内容
#   C) 已在板子上：bash board-check.sh
#
# 密码：temppwd

echo "########## 1. 板卡身份 ##########"
echo "主机名    : $(hostname)"
echo "架构      : $(uname -m)"
echo "内核      : $(uname -r)"
echo "设备树型号: $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo '读不到')"
echo "发行版    : $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"

echo
echo "########## 2. 存储与内存 ##########"
free -h 2>/dev/null | head -2
echo "--- 磁盘 ---"
df -h / /home /tmp 2>/dev/null
echo "--- 块设备（看有没有 eMMC / SD 卡）---"
lsblk 2>/dev/null | head -15

echo
echo "########## 3. 编译工具 ##########"
for t in gcc make cc git python3 uname; do
  printf '  %-10s ' "$t"
  if command -v "$t" >/dev/null 2>&1; then "$t" --version 2>/dev/null | head -1; else echo "缺失"; fi
done

echo
echo "########## 4. 包管理器 ##########"
if command -v apt >/dev/null 2>&1; then
  echo "  apt 可用"
  echo "  软件源:"
  grep -rhE '^deb ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null | head -4 | sed 's/^/    /'
elif command -v opkg >/dev/null 2>&1; then
  echo "  opkg 可用（Buildroot 类系统，可能没有 gcc）"
elif command -v dnf >/dev/null 2>&1; then
  echo "  dnf 可用"
else
  echo "  没有认识包管理器"
fi

echo
echo "########## 5. 网络 ##########"
ip -brief addr 2>/dev/null | sed 's/^/  /'
echo "  路由:"
ip route 2>/dev/null | sed 's/^/    /'

echo
echo "########## 6. 服务 ##########"
echo "  sshd: $(systemctl is-active ssh 2>/dev/null || systemctl is-active sshd 2>/dev/null || echo '未知')"

echo
echo "########## 7. 临时目录的文件系统（决定能不能演示空洞）##########"
for d in / /home /tmp /root; do
  [ -d "$d" ] || continue
  fstype=$(stat -f -c %T "$d" 2>/dev/null)
  echo "  $d -> $fstype"
done
echo "--- 实测能否建空洞文件（关键！必须看到 blocks=0）---"
T=$(mktemp /tmp/hole-test.XXXXXX 2>/dev/null) || T=/tmp/hole-test.$$
truncate -s 16M "$T" 2>/dev/null && {
  echo "  逻辑大小 = $(stat -c %s "$T") 字节"
  echo "  实际占用 = $(stat -c %b "$T") 个 512B 块  <- 必须是 0"
}
rm -f "$T"

echo
echo "########## 8. 是否支持 SEEK_DATA / SEEK_HOLE ##########"
python3 - <<'PY' 2>/dev/null || echo "  (需要 python3 才能测，可跳过)"
import os, tempfile
p = tempfile.mktemp(dir='/tmp')
with open(p,'wb') as f:
    f.truncate(8*1024*1024)
fd = os.open(p, os.O_RDONLY)
print("  SEEK_DATA 支持:", hasattr(os,'SEEK_DATA'))
print("  SEEK_HOLE 支持:", hasattr(os,'SEEK_HOLE'))
os.close(fd); os.remove(p)
PY

echo
echo "########## 探测完成 ##########"
echo "请把以上全部输出复制回来。"
