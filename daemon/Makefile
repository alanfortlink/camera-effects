CXX ?= g++
CXXFLAGS ?= -O2 -g0 -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CXXFLAGS += -Isrc -Ithird_party $(shell pkg-config --cflags opencv5 libonnxruntime libpipewire-0.3)
LDFLAGS += $(shell pkg-config --libs libonnxruntime libpipewire-0.3) \
	-lopencv_videoio -lopencv_dnn -lopencv_objdetect -lopencv_imgcodecs -lopencv_imgproc -lopencv_geometry -lopencv_flann -lopencv_core -lpthread

SRC := $(wildcard src/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC))
BIN := build/irisd

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp src/*.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf build

.PHONY: all clean
