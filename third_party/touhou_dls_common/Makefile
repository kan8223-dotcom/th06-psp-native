# Optional host-side tools; the PSP build compiles src/ directly.
CXX ?= c++
CPPFLAGS += -Iinclude
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CORE_SRC := src/sha256.cpp src/dls.cpp src/smf_scan.cpp src/subset.cpp src/manifest.cpp \
            src/blob.cpp src/synth_tables.cpp src/msgs_control.cpp src/synth.cpp src/zun_seq.cpp
CORE_OBJ := $(CORE_SRC:src/%.cpp=build/%.o)
HEADERS := $(wildcard include/tdls/*.hpp)

.PHONY: all
all: build/dls_render_tool build/dls_subset_tool

build/%.o: src/%.cpp $(HEADERS)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-exceptions -fno-rtti -c $< -o $@

build/dls_render_tool: tools/dls_render_tool.cpp $(CORE_OBJ) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(CORE_OBJ) -o $@

build/dls_subset_tool: tools/dls_subset_tool.cpp $(CORE_OBJ) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(CORE_OBJ) -o $@
