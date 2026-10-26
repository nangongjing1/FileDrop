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

INCLUDE  := include

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
  CXXFLAGS := /nologo /std:c++17 /EHsc /MT /Os /W3 /utf-8 /I$(INCLUDE) /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS /Gy /Gw
  LDFLAGS  := /OPT:REF /OPT:ICF /INCREMENTAL:NO
  LDLIBS   := ws2_32.lib iphlpapi.lib shell32.lib ole32.lib oleaut32.lib
  COMPILE   = $(CXX) $(CXXFLAGS) /c $< /Fo:$@
  COMPILE_RES = $(RC) /nologo /fo $(RES) $<
  LINK      = $(CXX) $(CXXFLAGS) /Fe:$@ $(OBJ) $(RES) /link $(LDFLAGS) $(LDLIBS)
else ifeq ($(OS),Windows_NT)
  EXE      := filedrop.exe
  OBJ      := $(SRC:.cpp=.o)
  DEPS     := $(OBJ:.o=.d)
  RES      := src/filedrop.res
  RC       ?= windres
  CXX      ?= g++
  CXXFLAGS ?= -std=c++17 -Os -Wall -Wextra
  CXXFLAGS += -I$(INCLUDE) -D_WIN32_WINNT=0x0601 -pthread -MMD -MP -ffunction-sections -fdata-sections
  LDFLAGS  := -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s
  LDLIBS   := -lws2_32 -liphlpapi -lshell32 -lole32 -loleaut32
  COMPILE   = $(CXX) $(CXXFLAGS) -c $< -o $@
  COMPILE_RES = $(RC) -O coff -o $@ $<
  LINK      = $(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(RES) $(LDLIBS)
else
  EXE      := filedrop
  OBJ      := $(SRC:.cpp=.o)
  DEPS     := $(OBJ:.o=.d)
  RES      :=
  CXX      ?= g++
  CXXFLAGS ?= -std=c++17 -Os -Wall -Wextra
  CXXFLAGS += -I$(INCLUDE) -pthread -MMD -MP -ffunction-sections -fdata-sections
  ifeq ($(shell uname -s),Darwin)
    LDFLAGS  := -Wl,-dead_strip -s
  else
    LDFLAGS  := -Wl,--gc-sections -s
  endif
  LDLIBS   := -pthread
  COMPILE   = $(CXX) $(CXXFLAGS) -c $< -o $@
  LINK      = $(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)
endif

PYTHON ?= python

ifneq (,$(findstring sh,$(notdir $(SHELL))))
  POSIX_SHELL := 1
endif

ifeq ($(V),1)
  Q    :=
  STEP  =
else
  Q    := @
  ifeq ($(POSIX_SHELL),1)
    STEP = @echo "  $(1)"
  else
    STEP = @echo   $(1)
  endif
endif

ifeq ($(POSIX_SHELL),1)
  NOW_SEC = date +%s
else
  NOW_SEC = powershell -NoProfile -Command "[int64]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())"
endif

BUILD_START := $(shell $(NOW_SEC))

ifeq ($(POSIX_SHELL),1)
  REPORT = now=$$(date +%s); \
    size=$$(wc -c < '$(EXE)' | awk '{ print $$1 }'); \
    secs=$$((now - $(BUILD_START))); \
    printf '\n  Build succeeded\n'; \
    printf '  Artifact   %s\n' '$(EXE)'; \
    printf '  Location   %s\n' '$(abspath $(EXE))'; \
    awk -v b="$$size" 'BEGIN { if (b >= 1048576) printf "  Size       %.2f MB (%d bytes)\n", b / 1048576, b; else printf "  Size       %.1f KB (%d bytes)\n", b / 1024, b }'; \
    if [ "$$secs" -lt 1 ]; then printf '  Elapsed    <1 s\n'; else printf '  Elapsed    %s s\n' "$$secs"; fi
else
  REPORT = powershell -NoProfile -Command "$$f = Get-Item -LiteralPath '$(abspath $(EXE))'; $$s = [int64]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) - $(BUILD_START); if ($$f.Length -ge 1MB) { $$size = '{0:N2} MB' -f ($$f.Length / 1MB) } else { $$size = '{0:N1} KB' -f ($$f.Length / 1KB) }; Write-Host ''; Write-Host '  Build succeeded'; Write-Host ('  Artifact   ' + $$f.Name); Write-Host ('  Location   ' + $$f.FullName); Write-Host ('  Size       ' + $$size + ' (' + $$f.Length + ' bytes)'); if ($$s -lt 1) { Write-Host '  Elapsed    <1 s' } else { Write-Host ('  Elapsed    ' + $$s + ' s') }"
endif

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
	$(call STEP,LD   $@)
	$(Q)$(LINK)
	@$(REPORT)

src/%.o: src/%.cpp
	$(call STEP,CXX  $<)
	$(Q)$(COMPILE)

src/%.obj: src/%.cpp
	$(call STEP,CXX  $<)
	$(Q)$(COMPILE)

src/gen_web_assets.cpp:
	$(call STEP,WEB  $@)
	$(Q)$(PYTHON) tools/embed_web.py web src/gen_web_assets.cpp

src/filedrop.ico:
	$(call STEP,ICON $@)
	$(Q)$(PYTHON) tools/make_icon.py web/favicon.svg src/filedrop.ico

src/filedrop.res: src/filedrop.rc src/filedrop.ico
	$(call STEP,RC   $<)
	$(Q)$(COMPILE_RES)

assets:
	$(call STEP,WEB  src/gen_web_assets.cpp)
	$(Q)$(PYTHON) tools/embed_web.py web src/gen_web_assets.cpp
ifeq ($(OS),Windows_NT)
	-@cmd /c "del /Q /F src\gen_web_assets.o src\gen_web_assets.obj 2>NUL"
else
	-@rm -f src/gen_web_assets.o
endif

icon:
	$(call STEP,ICON src/filedrop.ico)
	$(Q)$(PYTHON) tools/make_icon.py web/favicon.svg src/filedrop.ico
ifeq ($(OS),Windows_NT)
	-@cmd /c "del /Q /F src\filedrop.res 2>NUL"
else
	-@rm -f src/filedrop.res
endif

run: $(EXE)
	$(call STEP,RUN  $(EXE))
ifeq ($(OS),Windows_NT)
	$(Q)cmd /c $(EXE)
else
	$(Q)./$(EXE)
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
	$(call STEP,CXX  tools/dns_test.cpp)
	$(Q)$(CXX) /nologo /std:c++17 /EHsc /Os $(MSVC_TEST_FLAGS) /I$(INCLUDE) /Fe:$(TEST_EXE) tools/dns_test.cpp src/dns_protocol.cpp
else
	$(call STEP,CXX  tools/dns_test.cpp)
	$(Q)$(CXX) -std=c++17 -Os -I$(INCLUDE) $(TEST_STATIC) -o $(TEST_EXE) tools/dns_test.cpp src/dns_protocol.cpp $(TEST_LIBS)
endif
	$(call STEP,RUN  $(TEST_EXE))
	$(Q)$(RUN_TEST)

test-http:
ifeq ($(MSVC),1)
	$(call STEP,CXX  tools/http_test.cpp)
	$(Q)$(CXX) /nologo /std:c++17 /EHsc /Os $(MSVC_TEST_FLAGS) /I$(INCLUDE) /D_WIN32_WINNT=0x0601 /Fe:$(HTTP_TEST_EXE) tools/http_test.cpp src/http_server.cpp src/util.cpp src/applog.cpp ws2_32.lib iphlpapi.lib shell32.lib
else
	$(call STEP,CXX  tools/http_test.cpp)
	$(Q)$(CXX) -std=c++17 -Os -I$(INCLUDE) -D_WIN32_WINNT=0x0601 -pthread $(TEST_STATIC) -o $(HTTP_TEST_EXE) tools/http_test.cpp src/http_server.cpp src/util.cpp src/applog.cpp $(TEST_LIBS)
endif
	$(call STEP,RUN  $(HTTP_TEST_EXE))
	$(Q)$(RUN_HTTP_TEST)

help:
	@echo "make          构建 $(EXE)"
	@echo "make MSVC=1   使用 MSVC 构建(需先运行 vcvars64.bat)"
	@echo "make assets   重新嵌入 web/ 静态资源"
	@echo "make icon     由 web/favicon.svg 重新生成应用图标"
	@echo "make test     运行 DNS / HTTP 协议单元测试"
	@echo "make run      构建并运行"
	@echo "make clean    删除构建产物"
	@echo "make V=1      显示完整构建日志"

-include $(DEPS)
