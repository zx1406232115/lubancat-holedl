# holedl — 基于文件 IO 的空洞文件「断点续传」下载器

编程作业（AI 协作版）提交物

**目标平台**：鲁班猫 1（`EmbedFire LubanCat-1` / RK3566 / aarch64 / Linux）
**实测环境**：Ubuntu 20.04.6 LTS，内核 4.19.232，板端 gcc 9.4.0

> 板端运行证据（真实输出，非模拟）见 [`docs/evidence/`](docs/evidence/README.md)

---

## 一、作业要求对照

| 要求 | 实现 | 板端验证 |
|---|---|---|
| **create** 生成空洞文件，逻辑 ≥128KB，未写入区不占磁盘块 | `holedl create <file> <size>` 内部只用 `ftruncate()`，从不 `fallocate()`/写零 | `ls -l` 显示逻辑 128MiB，`du` 显示 **0** |
| **write** `lseek`+`write` 定点写入，不破坏其它空洞 | `holedl write <file> --offset N`，用 `pwrite()` 定点写 | 写两处 4KiB 后逻辑大小不变，`du` 仅 **8KiB**，其余偏移读回全 `0x00` |
| **续传** 记录已完成偏移，Ctrl-C 后从断点继续 | 进度位图原子落盘到 `<out>.holedl.state`，续传走 HTTP `Range` | 真机杀进程后重跑，`发现断点状态：已完成 5 块`，从第 6 块继续，`sha256` 一致 |
| **status** 打印逻辑大小 / 实际占用块数 / 空洞占比 | `holedl status <file> [--map]` | 输出三项指标 + 占用图 |

---

## 二、核心原理：什么是「空洞」，怎么造、怎么保住

### 1. 空洞（hole）是文件系统层面的概念

一个文件有**两个大小**：

- **逻辑大小**（`st_size`）：程序读文件时看到的字节数，`ls -l` 显示的就是它。
- **实际占用**（`st_blocks × 512`）：这个文件真正在磁盘上吃掉的块，`du` 显示的就是它。

当 `st_blocks × 512 < st_size` 时，中间那段「没有数据块承载」的区间就是**空洞**。
读空洞不会报错，内核直接返回全 `0x00`。

### 2. 怎么造空洞：`ftruncate()`

```c
int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
ftruncate(fd, 128 * 1024 * 1024);   /* 只改 inode 的 i_size，不分配数据块 */
```

`ftruncate()` 只修改 inode 里的 `i_size` 字段，**内核不会为这段区间分配任何数据块**。
所以立刻 `stat` 就能看到：逻辑 128MiB，`st_blocks = 0`。

> **反例（很多同学会踩的坑）**：用 `fallocate()` / `posix_fallocate()` 预分配，或者
> 自己 `write()` 一堆 `0` 去「撑大」文件 —— 这两种做法都会**真的占满磁盘**，
> `st_blocks` 立刻等于逻辑大小，空洞就没了。本项目全程刻意避开这两个 API。

### 3. 怎么保住空洞：只写自己那一段

空洞一旦被写，那块就会变成实块（而且写 1 字节也要占用一整个文件系统块，通常是 4KiB）。
所以下载器的铁律是：**只对自己的 `offset` 段做定点写，其它区域连碰都不碰。**

```c
pwrite(fd, buf, len, offset);   /* 只影响 [offset, offset+len)，别的空洞原样保留 */
```

这里用 `pwrite()` 而不是 `lseek()` + `write()`：两者语义等价，但 `pwrite()` 是
「带偏移的写」，**不会改动文件偏移量这个共享状态**，原子且多线程安全，
也省掉一次系统调用。（作业要求里提到的 `lseek + write` 就是指这种「定点写」能力。）

### 4. 怎么续传：进度位图 + HTTP Range

```
① 探测源文件总大小  →  HEAD 请求读 Content-Length
② 造一个同样逻辑大小的空洞文件（此刻磁盘占用 = 0）
③ 以 256KiB 为一块，只请求还没下完的块：Range: bytes=start-end
④ 每块收到 206 响应后 pwrite 到对应偏移
⑤ 每块完成后立刻把进度位图 fsync 落盘，然后才处理下一块
⑥ 中断后重新执行同一条命令 → 读回位图 → 只补缺失的块
```

进度文件 `<out>.holedl.state` 是纯文本，方便肉眼检查：

```
HOLEDL1                              ← 格式标识
/tmp/demo/source.bin                 ← 数据源（不匹配则重新开始）
33554432                             ← 总大小
1048576                              ← 分块大小
f0000000                             ← 位图（十六进制），第 i 位 = 第 i 块已完成
```

**为什么每块完成就落盘**：Ctrl-C（`SIGINT`）会直接终止进程，
如果只在最后写一次进度，中断就白下了。逐个块 `state_save()` + `fdatasync()`
保证「已经下完的块」一定不会重下。

**为什么先写临时文件再 rename**：`state_save()` 写 `xxx.holedl.state.tmp`，
`fsync` 后 `rename()` 到正式名。`rename` 在同一文件系统内是原子的，
所以哪怕正好在写进度时被 Ctrl-C，也不会留下一个写坏的状态文件。

---

## 三、代码结构

```
linux-homework/
├── include/
│   └── holedl.h            公共接口：数据结构和函数声明
├── src/
│   ├── util.c              参数解析(64M/128K)、pread/pwrite 循环封装、错误打印
│   ├── sparse.c            空洞核心：create / write_at / stat / 占用图
│   ├── download.c          断点续传引擎：URL 解析、HTTP Range、状态位图
│   └── main.c              命令行入口（create/write/status/download/verify）
├── tests/
│   ├── run_tests.sh        31 项功能自测
│   ├── demo.sh             一键演示（专为录屏/截图设计）
│   └── range_server.py     支持 Range 的测试服务器（可限速）
├── scripts/
│   ├── deploy.bat          Windows 一键部署（传输+编译+自测）
│   ├── board-check.sh      板端环境探测（型号/存储/编译器/文件系统）
│   ├── board-build.sh      板端编译+自测+空洞验证（输出适合截图）
│   ├── board-evidence.sh   生成板端证据文件（01/03/04）
│   └── board-resume-demo.sh 真实中断+续传演示（生成证据 02）
├── tools/
│   └── render_evidence.py  把板端输出渲染成终端风格 PNG
├── docs/
│   ├── 01-板卡系统安装指南.md
│   ├── 02-AI协作日志.md
│   ├── 03-AI代码审查记录.md
│   └── evidence/           板端运行证据（.txt 原始输出 + .png 渲染图）
├── Makefile
└── README.md
```

**设计取舍**

- 无第三方依赖，只用 glibc + Linux 系统调用，板端 `make` 直接出可执行文件。
- 数据源同时支持 `http://` 和**本地文件路径**。本地源让「空洞 + 断点续传」
  的逻辑可以完全脱离网络验证，答辩时不怕断网。
- 块大小默认 256KiB：够小才有续传粒度，够大才不会让 HTTP 请求数爆炸。
- `verify` 子命令逐块流式比对（1MiB 缓冲），不把整个文件读进内存。

---

## 四、编译

### 板端原生编译（本项目采用）

把整个目录拷到板子上，直接 `make`：

```bash
cat@lubancat:~$ cd linux-homework
cat@lubancat:~/linux-homework$ make
gcc -D_FILE_OFFSET_BITS=64 -Iinclude -O2 -g -std=gnu11 -Wall -Wextra ... -c -o build/download.o src/download.c
...
  ✔ 已生成 holedl (aarch64)
```

生成的 `holedl` 就是 aarch64 可执行文件，不需要交叉工具链。

### 交叉编译（可选，如果在 PC 上编）

```bash
make CROSS_COMPILE=aarch64-linux-gnu-
```

### 其他

```bash
make test        # 跑 31 项功能自测
make clean       # 清理
```

---

## 五、使用示例

```bash
# ① 生成 128MiB 的空洞文件
./holedl create big.bin 128M
#   ✔ 已创建空洞文件 big.bin（逻辑大小 128.00 MiB (134217728 B)）
#     逻辑大小 : 128.00 MiB (134217728 B)
#     实际占用 : 0 B
#     空洞占比 : 100.00%

# 用系统命令验证「逻辑有、占用无」
ls -l big.bin     # -rw-r--r-- 1 cat cat 134217728 ...   ← 逻辑 128MiB
du -h big.bin     # 0        big.bin                     ← 实际占用 0

# ② 在 4MiB 处定点写 4KiB，其它区域必须还是空洞
./holedl write big.bin --offset 4M --len 4K --byte 0xAA
du -h big.bin     # 4.0K     big.bin   ← 写 4KiB 只占 1 个块

# ③ 查看状态与占用图
./holedl status big.bin --map
#   逻辑大小 (st_size)      : 128.00 MiB (134217728 B)
#   实际占用 (st_blocks*512): 4.00 KiB (4096 B)
#   其中空洞                : 128.00 MiB (134213632 B)
#   空洞占比                : 100.00%
#   占用图: [....#...................................................]

# ④ 断点续传
./holedl download http://example.com/big.iso out.bin --block 256K
#   ^C 中断后重新执行同一条命令，只补没下完的块

# ⑤ 完整性校验
./holedl verify ./source.bin ./out.bin
#   ✔ 校验通过：./source.bin 与 ./out.bin 完全一致（33554432 字节）
```

### 一键演示

```bash
bash tests/demo.sh
```

脚本会依次演示四项功能，并在下载到一半时提示你按 **Ctrl-C**，
然后再执行续传，最后做 `sha256` 比对。每一步输出都适合直接截图。

---

## 六、测试结果

在开发板（`EmbedFire LubanCat-1` / aarch64 / Ubuntu 20.04）上 `bash tests/run_tests.sh`，
共 **31 项断言全部通过**：

| 测试组 | 内容 | 结果 |
|---|---|---|
| 1 | create 生成 64MiB 空洞：逻辑正确、实占 0、`du` 为 0、读回全 0 | PASS |
| 2 | write 定点写 4KiB：逻辑不变、实占仅 4KiB、其它偏移仍为 0、写入点数据正确 | PASS |
| 3 | 多次写不同偏移：旧数据不被覆盖、新数据正确、未写区仍是空洞 | PASS |
| 4 | status 输出逻辑大小/实际占用/空洞占比/占用图 | PASS |
| 5 | 本地源断点续传：伪造「已下 3/6 块」后续传，内容完全一致 | PASS |
| **5c** | **回归测试：块数 > 16 时位图编解码必须一致**（48 块 / 已完成 20 块） | PASS |
| 6 | 未下载完的文件实占 < 逻辑大小（空洞机制生效） | PASS |
| 7 | HTTP 完整下载、HTTP 断点续传、**不支持 Range 的服务器降级顺序下载** | PASS |

```
==================== 结果 ====================
  通过: 31    失败: 0
  ✔ 全部功能测试通过
```

> **5c 的由来**：第一版在真机上曾经出现「断点状态写对了、续传却从头开始」。
> 根因是状态位图的**写入方按字节编码、读取方按半字节算长度**，
> 块数 ≤16 时两个长度公式恰好相等，所有用例都落在这个「幸运区间」里所以全绿；
> 块数一多就静默退化成全新下载。修复后专门补了这个覆盖大块数的回归用例。
> 完整分析见 [`docs/03-AI代码审查记录.md`](docs/03-AI代码审查记录.md) 缺陷 6。

### 关于「服务端不支持 Range」的降级处理

绝大多数静态文件服务器（nginx / Apache / python 的 `RangeHTTPServer`）都支持 Range，
但简易服务器（如 `python3 -m http.server`）会忽略 `Range` 头、直接返回 `200` + 整个文件。

这种情况下**分块定点写无从谈起**，本项目会：

1. 检测到 `Range` 请求返回了 `200` 而不是 `206`；
2. 打印提示并降级为**顺序下载**：先把目标文件 `ftruncate` 清回空洞，
   丢弃之前写入的数据（避免新旧数据混在一起），然后整包顺序下载；
3. 明确告知用户：该模式下中断需要重头再来。

这是一个诚实的设计取舍 —— 真正的断点续传能力客观上依赖 `Range`，
与其假装能续传（写出一个内容错误的文件），不如降级并说清楚。

---

## 七、在开发板上运行

**本项目实际使用的板卡**：`EmbedFire LubanCat-1`（RK3566 / aarch64 / Ubuntu 20.04.6 LTS / 内核 4.19.232）。

> 实测这台板子**出厂已带完整系统且 SSH 可用**，因此没有走烧录流程。
> 如果换一块空板，烧录步骤见
> [`docs/01-板卡系统安装指南.md`](docs/01-板卡系统安装指南.md)。

```bash
# 1. 把工程传到板子（PC 上执行）
scp -r linux-homework cat@<板子IP>:~/

# 2. 板子上编译（原生 gcc，不需要交叉工具链）
ssh cat@<板子IP>
cd ~/linux-homework && make
#   ✔ 已生成 holedl (aarch64)

# 3. 确认是 aarch64 原生编译产物（关键证据）
uname -m          # aarch64
file holedl       # ELF 64-bit LSB ... ARM aarch64 ... interpreter /lib/ld-linux-aarch64.so.1

# 4. 跑测试与证据生成
make test                              # 31 项功能自测
bash scripts/board-evidence.sh         # 生成空洞机制 / HTTP 下载 / 自测证据
bash scripts/board-resume-demo.sh      # 真实中断 + 续传演示（会真的杀进程）
bash tests/demo.sh                     # 交互式演示（提示按 Ctrl-C）
```

板子默认账号（野火官方文档）：普通用户 `cat` / 密码 `temppwd`，root / `root`。

### 板端运行证据

| 证据 | 内容 |
|---|---|
| [`docs/evidence/01-board-and-sparse.txt`](docs/evidence/01-board-and-sparse.txt) | 板卡信息、`file holedl`（aarch64）、空洞 create/write/status 全程 |
| [`docs/evidence/02-resume.txt`](docs/evidence/02-resume.txt) | 真实中断 + 断点续传 + `verify` + `sha256` |
| [`docs/evidence/03-http-download.txt`](docs/evidence/03-http-download.txt) | HTTP Range 端到端下载 |
| [`docs/evidence/04-selftest.log`](docs/evidence/04-selftest.log) | 31 项自测完整输出 |

同名 `.png` 是这些原始输出的终端风格渲染图（板卡未接显示器，无法屏幕截图），
详见 [`docs/evidence/README.md`](docs/evidence/README.md)。

---

## 八、AI 协作说明

本作业要求「AI 生成初版 + AI 协作日志 + AI 代码审查 + AI 辅助测试 + README 核对」。
相关记录见：

- [`docs/02-AI协作日志.md`](docs/02-AI协作日志.md) —— 完整 prompt 记录、采纳与拒绝的判断依据
- [`docs/03-AI代码审查记录.md`](docs/03-AI代码审查记录.md) —— ≥3 处 AI 生成代码的真实缺陷：
  现象 → 根因 → 验证方法

其中印象最深的一处：AI 生成的 HTTP 响应解析代码在解析头部时**就地修改了接收缓冲区**，
而响应体就紧跟在头部后面，导致 body 的第一个字节被 `strtok` 写入的 `'\0'` 覆盖。
这个 bug 只在「头部和 body 在同一次 `recv()` 里返回」时才触发 —— 本地小文件测试
很容易碰不到，换成真实网络环境就会偶发数据损坏。详见代码审查记录第 2 条。

---

## 九、提交物清单

- [x] 源码（`src/`、`include/`）
- [x] Makefile（板端原生编译，可选交叉编译）
- [x] README.md（本文件）
- [x] AI 协作日志（`docs/02-AI协作日志.md`）
- [x] 板端运行截图（见报告文档；用 `bash tests/demo.sh` 生成）
