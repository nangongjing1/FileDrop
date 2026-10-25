# Filedrop · 局域网文件传输
#
# 默认使用 GCC / Clang(MinGW-w64、Linux、macOS)：
#   make
#
# Windows 上若使用 Microsoft Visual C++(在 vcvars64 环境中)：
#   make MSVC=1
#
# 修改 web/ 下的页面后：
#   make assets && make
#
# 修改 web/favicon.svg(应用图标)后：
#   make icon && make

SRC := src/main.cpp \
       src/app.cpp \
       src/http_server.cpp \
       src/devices.cpp \
       src/store.cpp \
       src/sha256.cpp \
       src/util.cpp \
       src/dns_server.cpp \
       src/dns_protocol.cpp \
       src/firewall.cpp \
       src/applog.cpp \
       src/gen_web_assets.cpp

ifeq ($(MSVC),1)
  EXE      := filedrop.exe
  OBJ      := $(SRC:.cpp=.obj)
  DEPS     :=
  RES      := src/filedrop.res
  RC       := rc
  CXX      := cl
  CXXFLAGS := /nologo /std:c++17 /EHsc /MT /O2 /W3 /utf-8 /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS
  LDLIBS   := ws2_32.lib iphlpapi.lib shell32.lib ole32.lib oleaut32.lib
  COMPILE   = $(CXX) $(CXXFLAGS) /c $< /Fo:$@
  COMPILE_RES = $(RC) /nologo /fo $(RES) $<
  LINK      = $(CXX) $(CXXFLAGS) /Fe:$@ $(OBJ) $(RES) $(LDLIBS)
else ifeq ($(OS),Windows_NT)
  EXE      := filedrop.exe
  OBJ      := $(SRC:.cpp=.o)
  DEPS     := $(OBJ:.o=.d)
  RES      := src/filedrop.res
  RC       ?= windres
  CXX      ?= g++
  CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
  CXXFLAGS += -D_WIN32_WINNT=0x0601 -pthread -MMD -MP
  LDLIBS   := -static -static-libgcc -static-libstdc++ -lws2_32 -liphlpapi -lshell32 -lole32 -loleaut32
  COMPILE   = $(CXX) $(CXXFLAGS) -c $< -o $@
  COMPILE_RES = $(RC) -O coff -o $@ $<
  LINK      = $(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(RES) $(LDLIBS)
else
  EXE      := filedrop
  OBJ      := $(SRC:.cpp=.o)
  DEPS     := $(OBJ:.o=.d)
  RES      :=
  CXX      ?= g++
  CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
  CXXFLAGS += -pthread -MMD -MP
  LDLIBS   := -pthread
  COMPILE   = $(CXX) $(CXXFLAGS) -c $< -o $@
  LINK      = $(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)
endif

PYTHON ?= python

ifeq ($(OS),Windows_NT)
  TEST_EXE := dns_test.exe
  RUN_TEST := cmd /c $(TEST_EXE)
  HTTP_TEST_EXE := http_test.exe
  RUN_HTTP_TEST := cmd /c $(HTTP_TEST_EXE)
  TEST_STATIC := -static -static-libgcc -static-libstdc++
  TEST_LIBS := -lws2_32 -liphlpapi -lshell32 -lole32 -loleaut32
  MSVC_TEST_FLAGS := /MT
else
  TEST_EXE := dns_test
  RUN_TEST := ./$(TEST_EXE)
  HTTP_TEST_EXE := http_test
  RUN_HTTP_TEST := ./$(HTTP_TEST_EXE)
  TEST_STATIC :=
  TEST_LIBS := -pthread
  MSVC_TEST_FLAGS :=
endif

.PHONY: all assets icon run clean help test test-dns test-http

all: $(EXE)

$(EXE): $(OBJ) $(RES)
	$(LINK)

src/%.o: src/%.cpp
	$(COMPILE)

src/%.obj: src/%.cpp
	$(COMPILE)

src/gen_web_assets.cpp:
	$(PYTHON) tools/embed_web.py web src/gen_web_assets.cpp

src/filedrop.ico:
	$(PYTHON) tools/make_icon.py web/favicon.svg src/filedrop.ico

src/filedrop.res: src/filedrop.rc src/filedrop.ico
	$(COMPILE_RES)

assets:
	$(PYTHON) tools/embed_web.py web src/gen_web_assets.cpp
ifeq ($(OS),Windows_NT)
	-@cmd /c "del /Q /F src\gen_web_assets.o src\gen_web_assets.obj 2>NUL"
else
	-@rm -f src/gen_web_assets.o
endif

icon:
	$(PYTHON) tools/make_icon.py web/favicon.svg src/filedrop.ico
ifeq ($(OS),Windows_NT)
	-@cmd /c "del /Q /F src\filedrop.res 2>NUL"
else
	-@rm -f src/filedrop.res
endif

run: $(EXE)
ifeq ($(OS),Windows_NT)
	cmd /c $(EXE)
else
	./$(EXE)
endif

clean:
ifeq ($(OS),Windows_NT)
	-@cmd /c "del /Q /F $(subst /,\,$(OBJ)) $(subst /,\,$(DEPS)) $(subst /,\,$(RES)) 2>NUL"
	-@cmd /c "del /Q /F $(EXE) $(TEST_EXE) $(HTTP_TEST_EXE) 2>NUL"
else
	-@rm -f $(OBJ) $(DEPS) $(RES) $(EXE) $(TEST_EXE) $(HTTP_TEST_EXE)
endif

test: test-dns test-http

test-dns:
ifeq ($(MSVC),1)
	$(CXX) /nologo /std:c++17 /EHsc /O2 $(MSVC_TEST_FLAGS) /Isrc /Fe:$(TEST_EXE) tools/dns_test.cpp src/dns_protocol.cpp
else
	$(CXX) -std=c++17 -O2 -Isrc $(TEST_STATIC) -o $(TEST_EXE) tools/dns_test.cpp src/dns_protocol.cpp $(TEST_LIBS)
endif
	$(RUN_TEST)

test-http:
ifeq ($(MSVC),1)
	$(CXX) /nologo /std:c++17 /EHsc /O2 $(MSVC_TEST_FLAGS) /Isrc /D_WIN32_WINNT=0x0601 /Fe:$(HTTP_TEST_EXE) tools/http_test.cpp src/http_server.cpp src/util.cpp src/applog.cpp ws2_32.lib iphlpapi.lib shell32.lib
else
	$(CXX) -std=c++17 -O2 -Isrc -D_WIN32_WINNT=0x0601 -pthread $(TEST_STATIC) -o $(HTTP_TEST_EXE) tools/http_test.cpp src/http_server.cpp src/util.cpp src/applog.cpp $(TEST_LIBS)
endif
	$(RUN_HTTP_TEST)

help:
	@echo "make          构建 $(EXE)"
	@echo "make MSVC=1   使用 MSVC 构建(需先运行 vcvars64.bat)"
	@echo "make assets   重新嵌入 web/ 静态资源"
	@echo "make icon     由 web/favicon.svg 重新生成应用图标"
	@echo "make test     运行 DNS / HTTP 协议单元测试"
	@echo "make run      构建并运行"
	@echo "make clean    删除构建产物"

-include $(DEPS)
