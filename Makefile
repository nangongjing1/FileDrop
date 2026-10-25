# Filedrop · 局域网文件传输
#
# 默认使用 GCC / Clang（MinGW-w64、Linux、macOS）：
#   make
#
# Windows 上若使用 Microsoft Visual C++（在 vcvars64 环境中）：
#   make MSVC=1
#
# 修改 web/ 下的页面后：
#   make assets && make

SRC := src/main.cpp \
       src/app.cpp \
       src/http_server.cpp \
       src/devices.cpp \
       src/store.cpp \
       src/sha256.cpp \
       src/util.cpp \
       src/dns_server.cpp \
       src/dns_protocol.cpp \
       src/gen_web_assets.cpp

ifeq ($(MSVC),1)
  EXE      := filedrop.exe
  OBJ      := $(SRC:.cpp=.obj)
  DEPS     :=
  CXX      := cl
  CXXFLAGS := /nologo /std:c++17 /EHsc /MT /O2 /W3 /utf-8 /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS
  LDLIBS   := ws2_32.lib iphlpapi.lib shell32.lib
  COMPILE   = $(CXX) $(CXXFLAGS) /c $< /Fo:$@
  LINK      = $(CXX) $(CXXFLAGS) /Fe:$@ $(OBJ) $(LDLIBS)
else ifeq ($(OS),Windows_NT)
  EXE      := filedrop.exe
  OBJ      := $(SRC:.cpp=.o)
  DEPS     := $(OBJ:.o=.d)
  CXX      ?= g++
  CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
  CXXFLAGS += -D_WIN32_WINNT=0x0601 -pthread -MMD -MP
  LDLIBS   := -static -static-libgcc -static-libstdc++ -lws2_32 -liphlpapi -lshell32
  COMPILE   = $(CXX) $(CXXFLAGS) -c $< -o $@
  LINK      = $(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)
else
  EXE      := filedrop
  OBJ      := $(SRC:.cpp=.o)
  DEPS     := $(OBJ:.o=.d)
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
  TEST_LDFLAGS := -static -static-libgcc -static-libstdc++
  MSVC_TEST_FLAGS := /MT
else
  TEST_EXE := dns_test
  RUN_TEST := ./$(TEST_EXE)
  TEST_LDFLAGS :=
  MSVC_TEST_FLAGS :=
endif

.PHONY: all assets run clean help test-dns

all: $(EXE)

$(EXE): $(OBJ)
	$(LINK)

src/%.o: src/%.cpp
	$(COMPILE)

src/%.obj: src/%.cpp
	$(COMPILE)

src/gen_web_assets.cpp:
	$(PYTHON) tools/embed_web.py web src/gen_web_assets.cpp

assets:
	$(PYTHON) tools/embed_web.py web src/gen_web_assets.cpp
ifeq ($(OS),Windows_NT)
	-@cmd /c "del /Q /F src\gen_web_assets.o src\gen_web_assets.obj 2>NUL"
else
	-@rm -f src/gen_web_assets.o
endif

run: $(EXE)
ifeq ($(OS),Windows_NT)
	cmd /c $(EXE)
else
	./$(EXE)
endif

clean:
ifeq ($(OS),Windows_NT)
	-@cmd /c "del /Q /F $(subst /,\,$(OBJ)) $(subst /,\,$(DEPS)) 2>NUL"
	-@cmd /c "del /Q /F $(EXE) $(TEST_EXE) 2>NUL"
else
	-@rm -f $(OBJ) $(DEPS) $(EXE) $(TEST_EXE)
endif

test-dns:
ifeq ($(MSVC),1)
	$(CXX) /nologo /std:c++17 /EHsc /O2 $(MSVC_TEST_FLAGS) /Isrc /Fe:$(TEST_EXE) tools/dns_test.cpp src/dns_protocol.cpp
else
	$(CXX) -std=c++17 -O2 -Isrc $(TEST_LDFLAGS) -o $(TEST_EXE) tools/dns_test.cpp src/dns_protocol.cpp
endif
	$(RUN_TEST)

help:
	@echo "make          构建 $(EXE)"
	@echo "make MSVC=1   使用 MSVC 构建（需先运行 vcvars64.bat）"
	@echo "make assets   重新嵌入 web/ 静态资源"
	@echo "make test-dns 运行 DNS 协议单元测试"
	@echo "make run      构建并运行"
	@echo "make clean    删除构建产物"

-include $(DEPS)
