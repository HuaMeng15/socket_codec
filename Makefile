TOP_DIR = .
BUILD_DIR = $(TOP_DIR)/build
CXX=g++

# VVENC flag: set to 1 to enable h266 (VVC) codec support
# Usage: make VVENC=1
VVENC ?= 0

CXXFLAGS = -pthread -fPIC -std=c++23 -g -ggdb -pedantic -Wall -Wextra -Wno-missing-field-initializers -DDEBUG

# Add VVENC define if flag is set
ifeq ($(VVENC),1)
  CXXFLAGS += -DVVENC
endif

# FFmpeg paths (must be built in third_party/ffmpeg before compiling)
FFMPEG_DIR = third_party/ffmpeg
FFMPEG_BUILD_DIR = $(FFMPEG_DIR)/build
FFMPEG_INCLUDE = -I$(FFMPEG_DIR) -I$(FFMPEG_BUILD_DIR)

INCLUDES = -I. -Icodec -Itransmission -Ilog_system -Itools -I./include \
           $(FFMPEG_INCLUDE)

LDFLAGS = -L./lib -L$(FFMPEG_BUILD_DIR)/libavcodec -L$(FFMPEG_BUILD_DIR)/libavutil
LDLIBS = -lx264 -lavcodec -lavutil -llzma -ldrm

# Conditionally include vvenc/vvdec libraries if VVENC is enabled
ifeq ($(VVENC),1)
  LDLIBS += -lvvenc -lvvdec
endif

# Source files - conditionally include h266 based on VVENC flag
SRCS = $(wildcard codec/*.cc \
				 codec/h264/*.cc \
				 transmission/*.cc \
				 log_system/*.cc \
				 tools/*.cc \
				 config/config.cc \
				 socket_codec.cc \
				 video_capture_and_send.cc)

# Conditionally add h266 sources
ifeq ($(VVENC),1)
  SRCS += $(wildcard codec/h266/*.cc)
endif

OBJS = $(patsubst %.cc,$(BUILD_DIR)/%.o,$(SRCS))

TARGET = $(BUILD_DIR)/socket_codec
TEST_TARGET = $(BUILD_DIR)/test_decoder

all: $(BUILD_DIR) $(TARGET)

test: $(BUILD_DIR) $(TEST_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Test decoder target - only includes necessary objects
TEST_OBJS = $(BUILD_DIR)/log_system/log_system.o \
            $(BUILD_DIR)/test_decoder.o

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: %.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f compile_commands.json

# Generate compile_commands.json for language servers (requires 'bear' or 'compiledb')
# Install bear: brew install bear (macOS) or apt-get install bear (Linux)
# Install compiledb: pip install compiledb
compile_commands.json:
	@if command -v bear > /dev/null; then \
		bear -- make clean all; \
	elif command -v compiledb > /dev/null; then \
		compiledb make clean all; \
	else \
		echo "Error: Neither 'bear' nor 'compiledb' is installed."; \
		echo "Install one of them to generate compile_commands.json:"; \
		echo "  macOS: brew install bear"; \
		echo "  Linux: apt-get install bear  OR  pip install compiledb"; \
		exit 1; \
	fi

.PHONY: all test clean compile_commands.json
