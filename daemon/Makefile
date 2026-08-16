CXX ?= g++
CXXFLAGS ?= -O2 -g0 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
# Omarchy ships opencv 5; fall back to the opencv4 .pc on a not-yet-updated system.
OPENCV_PKG := $(shell pkg-config --exists opencv5 && echo opencv5 || echo opencv4)
CXXFLAGS += -Isrc -Ithird_party $(shell pkg-config --cflags $(OPENCV_PKG) libonnxruntime libpipewire-0.3)
LDFLAGS += $(shell pkg-config --libs libonnxruntime libpipewire-0.3) \
	-lopencv_videoio -lopencv_dnn -lopencv_objdetect -lopencv_imgcodecs -lopencv_imgproc -lopencv_geometry -lopencv_flann -lopencv_core -lpthread

# Build directory (may be absolute). install.sh builds outside the checkout:
# omarchy-shell watches ~/.config/omarchy/plugins and reloads plugins on every write.
BUILD ?= build
SRC := $(wildcard src/*.cpp)
OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRC))
BIN := $(BUILD)/camesd

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.cpp src/*.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)

.PHONY: all clean
