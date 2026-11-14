# ---------------------------------------------------------------------------
# Zero-dependency Linux / macOS build.  No CMake required:
#
#     make                 # build-linux/mset   (AVX2 on)
#     make AVX2=0          # portable build for CPUs without AVX2
#     make CXX=clang++     # use Clang
#     make check           # run `mset info` and the 1e38 oracle comparison
#     make clean
#
# Equivalent one-liner (what build-linux.sh runs):
#     g++ -O3 -std=c++20 -mavx2 -pthread -Isrc \
#         src/{bigfloat,gmp_bridge,dive,reference,deep,render,palette,image,workspace}.cpp \
#         src/port/registry.cpp src/main.cpp -o build-linux/mset -ldl -lpthread
# ---------------------------------------------------------------------------
CXX      ?= g++
AVX2     ?= 1
CXXFLAGS ?= -O3 -std=c++20 -Wall -Wextra -pthread
LDLIBS   := -ldl -lpthread
OUTDIR   := build-linux
OUT      := $(OUTDIR)/mset

ifeq ($(AVX2),1)
CXXFLAGS += -mavx2 -mfma
endif

CORE := \
  src/bigfloat.cpp \
  src/gmp_bridge.cpp \
  src/dive.cpp \
  src/reference.cpp \
  src/deep.cpp \
  src/render.cpp \
  src/palette.cpp \
  src/image.cpp \
  src/workspace.cpp \
  src/port/registry.cpp

SOURCES := $(CORE) src/main.cpp

.PHONY: all check clean
all: $(OUT)

$(OUT): $(SOURCES)
	@mkdir -p $(OUTDIR)
	$(CXX) $(CXXFLAGS) -Isrc $(SOURCES) -o $@ $(LDLIBS)
	@ echo "Built $@  (AVX2=$(AVX2))"

check: $(OUT)
	$(OUT) info
	$(OUT) cmp 38 3000 5

clean:
	rm -rf $(OUTDIR)
