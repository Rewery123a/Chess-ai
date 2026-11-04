# Makefile -> builds chess-ai.exe (Windows only)
#
#   On Windows (MSYS2 UCRT64 / MINGW64 terminal):   make -j8
#   On Linux / GitHub Codespaces (cross-compile):   sudo apt install g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix
#                                                   make -j8
#
#   make ARCH=-march=native    tune for the CPU you build on (only if you run the exe on that same PC)
#   make ARCH=-mavx2           default when cross-compiling: runs on any AVX2 CPU
#   make verify                slow debug build that checks every incremental NNUE update -> chess-ai-verify.exe
#   make bench [NET=net.bin]   run "bench" (on Linux this needs wine)
#   make clean

EXE    ?= chess-ai.exe
STATIC ?= 1
MODE   ?= release
NET    ?=

ifeq ($(OS),Windows_NT)
  # native build inside MSYS2
  ARCH ?= -march=native
  RUN  ?=
else
  # Linux: cross-compile with mingw-w64 (the -posix variant is required: std::thread needs POSIX threads)
  ifeq ($(origin CXX),default)
    CXX := x86_64-w64-mingw32-g++-posix
  endif
  ifeq ($(origin CC),default)
    CC := x86_64-w64-mingw32-gcc-posix
  endif
  ARCH ?= -mavx2
  RUN  ?= wine
endif

ifneq ($(MAKECMDGOALS),clean)
  ifeq ($(shell command -v $(CXX) 2>/dev/null),)
    $(error Compiler '$(CXX)' not found. In Codespaces run: sudo apt install g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix)
  endif
endif

# Folder that holds main.cpp. Found automatically (here, or one folder down); override with  make SRCDIR=somefolder
SRCDIR ?= $(patsubst %/,%,$(dir $(firstword $(wildcard main.cpp */main.cpp))))
ifeq ($(SRCDIR),)
  SRCDIR := .
endif
vpath %.cpp $(SRCDIR)

# every C++ file of the engine (add new ones here)
SRCS := main.cpp uci.cpp search.cpp eval.cpp nnue.cpp book.cpp play.cpp tb.cpp

# Fathom (Syzygy probing): tbprobe.c includes tbchess.c itself, so only tbprobe.c is compiled
FATHOM := $(firstword $(wildcard $(SRCDIR)/fathom/src/tbprobe.c $(SRCDIR)/fathom/tbprobe.c))

ifeq ($(MODE),verify)
  OPT := -O2 -DNNUE_VERIFY
  EXE := $(basename $(EXE))-verify.exe
else
  OPT := -O3 -DNDEBUG
endif

CXXFLAGS += -std=c++17 $(OPT) $(ARCH) -Wall -pthread -I$(SRCDIR) -I$(SRCDIR)/fathom -I$(SRCDIR)/fathom/src
CFLAGS   += -std=gnu11 $(OPT) $(ARCH) -w -I$(SRCDIR) -I$(SRCDIR)/fathom -I$(SRCDIR)/fathom/src
LDFLAGS  += -pthread
ifeq ($(STATIC),1)
  LDFLAGS += -static
endif

# own folder, so objects from an older (Linux) build are never reused
BUILD := build/windows-$(MODE)
OBJS  := $(SRCS:%.cpp=$(BUILD)/%.o)
ifneq ($(FATHOM),)
  OBJS += $(BUILD)/fathom_tbprobe.o
endif

.PHONY: all verify bench clean
all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)
	@echo "built $@"

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/fathom_tbprobe.o: $(FATHOM)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

verify:
	@$(MAKE) MODE=verify

bench: $(EXE)
	@if [ -n "$(NET)" ]; then printf 'setoption name EvalFile value $(NET)\nbench 9\nquit\n' | $(RUN) ./$(EXE); \
	else printf 'bench 9\nquit\n' | $(RUN) ./$(EXE); fi

clean:
	rm -rf build chess-ai.exe chess-ai-verify.exe

-include $(OBJS:.o=.d)