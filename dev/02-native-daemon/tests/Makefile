# autod 的验证测试
#
# 所有测试都在开发机上**真实运行**：创建虚拟触摸屏 → 注入事件 →
# 从 /dev/input/eventN 读回校验。走完整的内核 input 子系统路径，不是 mock。
#
#   make              编译全部
#   sudo make run     跑全部
#   make test_util    只编某个
#   sudo ./test_integration   单独跑
#   make clean
#
# 需要 root（/dev/uinput 默认 0600）和已加载的 uinput 内核模块
# （modprobe uinput）。

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter
DAEMON    = ../daemon

COMMON_SRCS = $(DAEMON)/inject.cpp $(DAEMON)/inject_uinput.cpp
COMMON_HDRS = $(DAEMON)/inject.h $(DAEMON)/inject_backend.h \
              $(DAEMON)/autod_log.h $(DAEMON)/protocol.h test_util.h

# 单元测试：只测注入后端
UNIT_SRCS = test_inject_uinput.cpp $(COMMON_SRCS)

# 集成测试：socket 服务端 + 真实 Dispatcher + 协议 + 注入 + 桩截图
# 截图后端用 capture_stub.cpp —— 生成合成图像，不需要 AOSP 环境
INTEG_SRCS = test_integration.cpp $(COMMON_SRCS) \
             $(DAEMON)/socket_server.cpp \
             $(DAEMON)/dispatch.cpp \
             $(DAEMON)/capture_stub.cpp

BINS = test_inject_uinput test_integration

INTEG_HDRS = $(DAEMON)/socket_server.h $(DAEMON)/dispatch.h $(DAEMON)/capture.h

all: $(BINS)

test_inject_uinput: $(UNIT_SRCS) $(COMMON_HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(UNIT_SRCS)

test_integration: $(INTEG_SRCS) $(COMMON_HDRS) $(INTEG_HDRS)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $(INTEG_SRCS)

run: $(BINS)
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "需要 root，改用 sudo"; \
		sudo $(MAKE) --no-print-directory _run; \
	else \
		$(MAKE) --no-print-directory _run; \
	fi

_run: $(BINS)
	@./test_inject_uinput; u=$$?; \
	 echo; \
	 ./test_integration; i=$$?; \
	 echo; \
	 echo "───────────────"; \
	 echo "单元测试: $$([ $$u -eq 0 ] && echo 通过 || echo 失败)"; \
	 echo "集成测试: $$([ $$i -eq 0 ] && echo 通过 || echo 失败)"; \
	 exit $$(( u + i ))

clean:
	rm -f $(BINS)

.PHONY: all run _run clean
