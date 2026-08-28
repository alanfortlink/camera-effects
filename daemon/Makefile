CXX ?= g++
CXXFLAGS ?= -O2 -g0 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
# Omarchy ships opencv 5; fall back to the opencv4 .pc on a not-yet-updated system.
OPENCV_PKG := $(shell pkg-config --exists opencv5 && echo opencv5 || echo opencv4)
CXXFLAGS += -Isrc -Ithird_party $(shell pkg-config --cflags $(OPENCV_PKG) libonnxruntime libpipewire-0.3)
# No -lonnxruntime: src/ortloader.cpp resolves its one entry point with dlopen so
# that an onnxruntime upgrade cannot leave an unloadable binary behind. Its .pc is
# still used for the headers above.
LDFLAGS += $(shell pkg-config --libs libpipewire-0.3) -ldl \
	-lopencv_videoio -lopencv_dnn -lopencv_objdetect -lopencv_imgcodecs -lopencv_imgproc -lopencv_geometry -lopencv_flann -lopencv_core -lpthread

# Build directory (may be absolute). install.sh builds outside the checkout:
# omarchy-shell watches ~/.config/omarchy/plugins and reloads plugins on every write.
BUILD ?= build
SRC := $(wildcard src/*.cpp)
OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC))
BIN := $(BUILD)/camera-effects-server

# A system update can change the ABI under a build dir that make still considers
# up to date: onnxruntime renames its symbol version node on every release
# (VERS_1.28.0 -> VERS_1.29.0, no old node kept) and opencv bumps its soname.
# Nothing would be rebuilt, so `./install.sh` reinstalled the same unloadable
# binary. Depend on the versions themselves and rebuild when they move.
STAMP := $(BUILD)/deps.stamp
DEPS_VERSION := $(shell $(CXX) -dumpversion; pkg-config --modversion $(OPENCV_PKG) libonnxruntime libpipewire-0.3 2>/dev/null)

all: $(BIN)

$(STAMP): FORCE
	@mkdir -p $(BUILD)
	@printf '%s\n' '$(DEPS_VERSION)' | cmp -s - $@ || printf '%s\n' '$(DEPS_VERSION)' > $@

$(BIN): $(OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp src/*.hpp $(STAMP)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)

.PHONY: all clean FORCE
FORCE:
