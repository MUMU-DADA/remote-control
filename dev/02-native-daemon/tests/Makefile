# autod 的验证测试
#
# 所有测试都在开发机上**真实运行**，走完整的系统路径，不是 mock：
#   - 触控：真的创建虚拟触摸屏、注入事件、从 /dev/input/eventN 读回
#   - 截图：真的 fork/exec 一个进程、通过管道读像素
#
#   make              编译全部
#   sudo make run     跑全部（需要 root：/dev/uinput 默认 0600）
#   sudo ./test_integration      单独跑
#   make clean
#
# 需要已加载的 uinput 内核模块（modprobe uinput）。

CC       ?= gcc
CXX      ?= g++
CFLAGS   ?= -O1 -g -Wall -Wextra
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter

# 测试源文件用 #include "appops.h" 这种写法（相对**包含者**所在目录解析），
# 所以必须显式给出 daemon 的 include 路径。
#
# ⚠️ 这个变量曾经是未定义的：规则里写了 $(INCLUDES) 但没人定义它，
#    而当时 test_appops 二进制是手工编出来的，make 一直报"已是最新"，
#    把这个错误整整掩盖了一轮。
INCLUDES = -I$(DAEMON) -I.
DAEMON    = ../daemon

# log_buffer 现在是所有用到 ALOG* 的模块的依赖（日志同时进环形缓冲），
# 所以放进公共源，而不是每个测试各加一遍。
COMMON_SRCS = $(DAEMON)/inject.cpp $(DAEMON)/inject_uinput.cpp \
              $(DAEMON)/log_buffer.cpp
COMMON_HDRS = $(DAEMON)/inject.h $(DAEMON)/inject_backend.h \
              $(DAEMON)/autod_log.h $(DAEMON)/autod_platform.h \
              $(DAEMON)/protocol.h test_util.h

# 内置的 libwebp（vendor/webp）。dispatch 现在会调到 ImageEncoder，
# 而 WebP 那条路是完整内置的源码 —— 测试要链进来。
#
# JPEG 那边不用：它是 dlopen 系统库，没有链接期依赖。
WEBP_DIR   = $(DAEMON)/vendor/webp
WEBP_SRCS  = $(wildcard $(WEBP_DIR)/src/enc/*.c) \
             $(wildcard $(WEBP_DIR)/src/dsp/*.c) \
             $(wildcard $(WEBP_DIR)/src/utils/*.c)
WEBP_OBJS  = $(patsubst $(WEBP_DIR)/src/%.c,$(WEBP_OBJ)/%.o,$(WEBP_SRCS))
WEBP_OBJ   = build/webp
WEBP_CFLAGS = -O2 -DANDROID -DWEBP_SWAP_16BIT_CSP -DWEBP_USE_THREAD \
              -I$(WEBP_DIR)

# [1] 单元测试：只测注入后端
UNIT_SRCS = test_inject_uinput.cpp $(COMMON_SRCS)

# [2] 集成测试：真实 socket 服务端 + Dispatcher + 协议 + 注入 + 桩截图
# dispatch 现在会用到 AppOps / FileOps / HttpClient，集成测试要把它们一起链进来
INTEG_SRCS = test_integration.cpp $(COMMON_SRCS) \
             $(DAEMON)/socket_server.cpp \
             $(DAEMON)/dispatch.cpp \
             $(DAEMON)/capture_stub.cpp \
             $(DAEMON)/appops.cpp \
             $(DAEMON)/subprocess.cpp \
             $(DAEMON)/fileops.cpp \
             $(DAEMON)/http_client.cpp \
             $(DAEMON)/service_state.cpp \
             $(DAEMON)/selftest.cpp \
             $(DAEMON)/keyboard.cpp \
             $(DAEMON)/clipops.cpp \
             $(DAEMON)/png_encoder.cpp \
             $(DAEMON)/json_parser.cpp \
             $(DAEMON)/webui.cpp \
             $(DAEMON)/websocket.cpp \
             $(DAEMON)/config_file.cpp \
             $(DAEMON)/image_encoder.cpp \
             $(DAEMON)/frame_hub.cpp \
             $(DAEMON)/jpeg_encoder.cpp \
             $(DAEMON)/webp_encoder.cpp

# [3] screencap 截图后端：配 fake_screencap 冒充设备命令
SCREENCAP_SRCS = test_capture_screencap.cpp $(DAEMON)/capture_screencap.cpp \
                 $(DAEMON)/log_buffer.cpp
APPOPS_SRCS    = test_appops.cpp $(DAEMON)/appops.cpp $(DAEMON)/subprocess.cpp \
                 $(DAEMON)/log_buffer.cpp

BINS = test_inject_uinput test_integration test_capture_screencap test_appops test_json test_keyboard test_websocket

all: $(BINS) fake_screencap

test_inject_uinput: $(UNIT_SRCS) $(COMMON_HDRS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(UNIT_SRCS)

$(WEBP_OBJ)/%.o: $(WEBP_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

test_integration: $(INTEG_SRCS) $(COMMON_HDRS) $(WEBP_OBJS) \
                  $(DAEMON)/socket_server.h $(DAEMON)/dispatch.h $(DAEMON)/capture.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(WEBP_DIR) -pthread -o $@ \
	    $(INTEG_SRCS) $(WEBP_OBJS) -lm

JSON_SRCS = test_json.cpp $(DAEMON)/json_parser.cpp $(DAEMON)/json_writer.h

WS_SRCS = test_websocket.cpp $(DAEMON)/websocket.cpp $(DAEMON)/log_buffer.cpp

test_websocket: $(WS_SRCS) test_util.h $(DAEMON)/websocket.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(WS_SRCS)

KB_SRCS = test_keyboard.cpp $(DAEMON)/keyboard.cpp $(DAEMON)/log_buffer.cpp

test_keyboard: $(KB_SRCS) test_util.h $(DAEMON)/keyboard.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(KB_SRCS)

test_json: $(JSON_SRCS) test_util.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(JSON_SRCS)

test_appops: $(APPOPS_SRCS) test_util.h $(DAEMON)/appops.h $(DAEMON)/subprocess.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^

test_capture_screencap: $(SCREENCAP_SRCS) test_util.h $(DAEMON)/capture.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SCREENCAP_SRCS)

fake_screencap: fake_screencap.c
	$(CC) $(CFLAGS) -o $@ $<

run: all
	@if [ "$$(id -u)" -ne 0 ]; then \
		echo "需要 root，改用 sudo"; \
		sudo $(MAKE) --no-print-directory _run; \
	else \
		$(MAKE) --no-print-directory _run; \
	fi

_run: all
	@./test_inject_uinput;                       u=$$?; echo; \
	 ./test_integration;                         i=$$?; echo; \
	 ./test_capture_screencap ./fake_screencap;  c=$$?; echo; \
	 ./test_appops;                              a=$$?; echo; \
	 ./test_json;                                j=$$?; echo; \
	 ./test_keyboard;                            k=$$?; echo; \
	 ./test_websocket;                           w=$$?; echo; \
	 echo "───────────────────────────────"; \
	 printf "  %-34s %s\n" "注入后端单元测试"   "$$([ $$u -eq 0 ] && echo 通过 || echo 失败)"; \
	 printf "  %-34s %s\n" "端到端集成测试"     "$$([ $$i -eq 0 ] && echo 通过 || echo 失败)"; \
	 printf "  %-34s %s\n" "screencap 截图后端" "$$([ $$c -eq 0 ] && echo 通过 || echo 失败)"; \
	 printf "  %-34s %s\n" "应用管理后端"       "$$([ $$a -eq 0 ] && echo 通过 || echo 失败)"; \
	 printf "  %-34s %s\n" "JSON 解析/输出"      "$$([ $$j -eq 0 ] && echo 通过 || echo 失败)"; \
	 printf "  %-34s %s\n" "按键注入"            "$$([ $$k -eq 0 ] && echo 通过 || echo 失败)"; \
	 printf "  %-34s %s\n" "WebSocket 握手"       "$$([ $$w -eq 0 ] && echo 通过 || echo 失败)"; \
	 exit $$(( u + i + c + a + j + k + w ))

clean:
	rm -rf $(WEBP_OBJ)
	rm -f $(BINS) fake_screencap

.PHONY: all run _run clean
