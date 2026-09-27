# 主机侧构建（不需要 AOSP）
#
# 平台相关的部分已经隔离：
#   - ProcessState 用 #ifdef __ANDROID__ 包住
#   - 截图后端可替换，主机用 capture_stub.cpp（合成图像）
#   - 日志走 autod_log.h（主机回退到 stderr）
#
# 于是可以在开发机上编译并运行**真正的 autod**，用真正的客户端连它，
# 验证除了 SurfaceFlinger / SELinux 之外的整条链路。
#
#   make            编译 autod-host
#   sudo make run   以 root 跑起来（需要 /dev/uinput）
#   make test       跑全部单元 + 集成测试
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter
DAEMON    = daemon

# 主机版：桩截图 + uinput 触控
HOST_SRCS = $(DAEMON)/main.cpp \
            $(DAEMON)/socket_server.cpp \
            $(DAEMON)/dispatch.cpp \
            $(DAEMON)/selftest.cpp \
            $(DAEMON)/capture_stub.cpp \
            $(DAEMON)/inject.cpp \
            $(DAEMON)/inject_uinput.cpp \
            $(DAEMON)/appops.cpp \
            $(DAEMON)/subprocess.cpp \
            $(DAEMON)/fileops.cpp \
            $(DAEMON)/http_client.cpp \
            $(DAEMON)/service_state.cpp \
            $(DAEMON)/log_buffer.cpp

HOST_HDRS = $(wildcard $(DAEMON)/*.h)

BIN       = autod-host
CTL       = autodctl-host
SOCKET    = /tmp/autod-host.sock

all: $(BIN) $(CTL)

$(BIN): $(HOST_SRCS) $(HOST_HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(HOST_SRCS)
	@echo "编译完成: ./$(BIN)"

# 设备端客户端的自检版本。
# 主机上没有 AndroidBitmap_compress，capture 会退回到写 PPM —— 只为让这份
# 从未在设备上跑过的代码在开发机上先过一遍编译和基础逻辑。
$(CTL): client/autodctl.cpp daemon/protocol.h
	$(CXX) $(CXXFLAGS) -o $@ client/autodctl.cpp
	@echo "编译完成: ./$(CTL)"

# 前台跑起来，方便另开终端用客户端连
run: $(BIN)
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "需要 root（/dev/uinput 默认 0600），改用 sudo"; \
		sudo ./$(BIN) --socket $(SOCKET) --foreground --verbose; \
	else \
		./$(BIN) --socket $(SOCKET) --foreground --verbose; \
	fi

# 后台起 + 冒烟测试 + 收尾，一条命令跑完
smoke: $(BIN)
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "需要 root，改用 sudo"; \
		sudo $(MAKE) --no-print-directory _smoke; \
	else \
		$(MAKE) --no-print-directory _smoke; \
	fi

_smoke: $(BIN)
	@rm -f $(SOCKET)
	@./$(BIN) --socket $(SOCKET) --foreground > /tmp/autod-host.log 2>&1 & \
	 pid=$$!; \
	 sleep 1; \
	 echo "── Python 客户端 ──"; \
	 python3 client/autod_client.py --socket $(SOCKET) info; \
	 python3 client/autod_client.py --socket $(SOCKET) capture -o /tmp/autod-host-shot.png; \
	 python3 client/autod_client.py --socket $(SOCKET) tap 540 960; \
	 python3 client/autod_client.py --socket $(SOCKET) swipe 200 1600 800 400 --ms 150; \
	 echo; echo "── C++ 客户端 (autodctl) ──"; \
	 ./$(CTL) --socket $(SOCKET) info; \
	 ./$(CTL) --socket $(SOCKET) capture -o /tmp/autodctl-shot.ppm; \
	 ./$(CTL) --socket $(SOCKET) tap 100 200; \
	 ./$(CTL) --socket $(SOCKET) swipe 100 1700 900 200 --ms 200; \
	 kill $$pid 2>/dev/null; wait $$pid 2>/dev/null; \
	 rm -f $(SOCKET); \
	 echo; echo "── 服务端日志 ──"; cat /tmp/autod-host.log

test:
	@$(MAKE) -C tests run

clean:
	rm -f $(BIN) $(CTL)
	@$(MAKE) -C tests clean

.PHONY: all run smoke _smoke test clean
