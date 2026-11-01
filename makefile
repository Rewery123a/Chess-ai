SRC    := engine
FATHOM := $(SRC)/fathom

CXXFLAGS := -O3 -std=c++17 -pthread -mpopcnt -flto -MMD -MP -I$(FATHOM)
CFLAGS   := -O3 -std=gnu11 -pthread -mpopcnt -flto -MMD -MP -I$(FATHOM)

CXXSRC := $(wildcard $(SRC)/*.cpp)

# ---- Linux ----
LCXX := g++
LCC  := gcc
LOBJ := $(patsubst $(SRC)/%.cpp,build/linux/%.o,$(CXXSRC)) build/linux/tbprobe.o

# ---- Windows (cross-compile) ----
WCXX := x86_64-w64-mingw32-g++-posix
WCC  := x86_64-w64-mingw32-gcc-posix
WOBJ := $(patsubst $(SRC)/%.cpp,build/win/%.o,$(CXXSRC)) build/win/tbprobe.o

all: engine engine.exe
linux: engine
windows: engine.exe

engine: $(LOBJ)
	$(LCXX) $(CXXFLAGS) -o $@ $^

engine.exe: $(WOBJ)
	$(WCXX) $(CXXFLAGS) -static -o $@ $^

build/linux/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(LCXX) $(CXXFLAGS) -c $< -o $@

build/linux/tbprobe.o: $(FATHOM)/tbprobe.c
	@mkdir -p $(dir $@)
	$(LCC) $(CFLAGS) -c $< -o $@

build/win/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(WCXX) $(CXXFLAGS) -c $< -o $@

build/win/tbprobe.o: $(FATHOM)/tbprobe.c
	@mkdir -p $(dir $@)
	$(WCC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build engine engine.exe

-include $(wildcard build/*/*.d)
.PHONY: all linux windows clean