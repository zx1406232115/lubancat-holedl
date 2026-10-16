# Makefile - holedl 空洞文件断点续传下载器
#
# 板端原生编译（作业采用这种方式：板子是 aarch64，直接用它自己的 gcc）：
#     make
# 也可以交叉编译（如果在 PC 上装了交叉工具链）：
#     make CROSS_COMPILE=aarch64-linux-gnu-
#
# 目标：无第三方依赖，只用 glibc + Linux 系统调用。

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc

BIN     := holedl
SRCDIR  := src
OBJDIR  := build

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

# 64 位文件偏移 + GNU 扩展（pread/pwrite/ftruncate/SEEK_DATA/fdatasync 等需要）
# 注意：_GNU_SOURCE 由各 .c 文件自己 #define，这里不重复定义以免 warning
CPPFLAGS += -D_FILE_OFFSET_BITS=64 -Iinclude

# 大文件空洞演示要处理 >2GB 的偏移，显式要求 64 位 off_t
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wshadow -Wformat=2 -Wconversion \
           -Wno-sign-conversion -Wpointer-arith -Wcast-qual

# 依赖 pthread？不需要——全程单线程，pwrite 定点写就够了
LDFLAGS +=
LDLIBS  +=

.PHONY: all clean test run-demo help

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "  ✔ 已生成 $@ ($(shell uname -m))"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	@mkdir -p $(OBJDIR)

# 自动依赖：头文件改了也会重编
-include $(DEPS)

# 一键功能演示（生成日志，方便截图/写报告）
test: $(BIN)
	@bash tests/run_tests.sh

clean:
	@rm -rf $(OBJDIR) $(BIN)
	@echo "  ✔ 已清理"

help:
	@echo "make            编译出 ./$(BIN)"
	@echo "make test       跑全部功能测试并输出日志"
	@echo "make clean      清理"
