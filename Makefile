# buddhabox-tui: build every example, or one.
#
#   make               build every example into build/
#   make acid          build one (then run ./build/acid)
#   make clean         remove build/
#
# The DSP modules live in src/buddhabox/: a copy of buddhabox-framework's
# DSP library (everything but its Arduino-only io/ and engine/), taken
# at framework commit 7c5c012 and free to change here.
#
# Needs: a C++17 compiler. On Linux also the ALSA headers
# (Debian, Ubuntu, Raspberry Pi OS: sudo apt install libasound2-dev).

# -O2: the DSP needs the optimiser.
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Isrc

ifeq ($(shell uname),Darwin)
  LDLIBS = -framework AudioToolbox -framework CoreMIDI \
           -framework CoreFoundation
else
  LDLIBS = -lasound -lpthread
endif

LIB = $(wildcard src/bbtui/*.cpp)
HEADERS = $(wildcard src/*/*.h src/*/*/*.h)
EXAMPLES = $(notdir $(wildcard examples/*))

all: $(EXAMPLES)

# "make acid" means "make build/acid".
$(EXAMPLES): %: build/%

# Each program is its main.cpp plus the library's three .cpp files, in
# one compiler call. A few seconds each; not worth separate objects.
build/%: examples/%/main.cpp $(LIB) $(HEADERS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $< $(LIB) -o $@ $(LDLIBS)

clean:
	rm -rf build

.PHONY: all clean $(EXAMPLES)
