# luci-app-easytier C 重写版构建
#
# 宿主/交叉通用：CC / CFLAGS / LIBS 均可外部覆盖。
#   make objs  —— 只编译不链接（增量验证用，缺实现不会报链接错误）
#   make link  —— 链接 easytierd 与 easytierctl
#   make clean
#
# 交叉编译由 OpenWrt 包 Makefile（easytierd/Makefile）调用 link 目标。

CC ?= gcc
BUILD_DIR ?= build
STAGING ?=
LIBS ?= -lubus -lubox -lblobmsg_json -luci -ljson-c

WARN := -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -D_GNU_SOURCE -Iinclude $(WARN)
LDFLAGS ?=
ifneq ($(STAGING),)
CFLAGS += -I$(STAGING)/include
LDFLAGS += -L$(STAGING)/lib
endif
LDFLAGS += -Wl,-rpath-link,$(STAGING)/lib

LIB_SRCS := $(wildcard lib/*.c) $(wildcard api/*.c)
DAEMON_SRCS := $(wildcard daemon/*.c)
CTL_SRCS := $(wildcard ctl/*.c)

LIB_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
DAEMON_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(DAEMON_SRCS))
CTL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CTL_SRCS))
ALL_OBJS := $(LIB_OBJS) $(DAEMON_OBJS) $(CTL_OBJS)

# 入口 main() 只属于 easytierd；easytierctl 需要 daemon 的 service 实现（api_service 依赖），
# 但不能带上 easytierd 的 main，故单独剔除。
ENTRY_OBJ := $(BUILD_DIR)/daemon/etd_entry.o

.PHONY: all objs link clean check-sym

all: objs

objs: $(ALL_OBJS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

link: $(BUILD_DIR)/easytierd $(BUILD_DIR)/easytierctl

$(BUILD_DIR)/easytierd: $(LIB_OBJS) $(DAEMON_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD_DIR)/easytierctl: $(LIB_OBJS) $(CTL_OBJS) $(filter-out $(ENTRY_OBJ),$(DAEMON_OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -rf $(BUILD_DIR)
