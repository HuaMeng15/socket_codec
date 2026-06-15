TOP_DIR = .
BUILD_DIR = $(TOP_DIR)/build
CXX=g++

# VVENC flag: set to 1 to enable h266 (VVC) codec support
# Usage: make VVENC=1
VVENC ?= 0

CXXFLAGS = -pthread -fPIC -std=c++2b -g -ggdb -pedantic -Wall -Wextra -Wno-missing-field-initializers -DDEBUG

# Add VVENC define if flag is set
ifeq ($(VVENC),1)
  CXXFLAGS += -DVVENC
endif

# x264 paths (built from source in third_party/x264)
X264_DIR = third_party/x264

# FFmpeg paths (must be built in third_party/ffmpeg before compiling)
FFMPEG_DIR = third_party/ffmpeg
FFMPEG_BUILD_DIR = $(FFMPEG_DIR)/build
FFMPEG_INCLUDE = -I$(FFMPEG_DIR) -I$(FFMPEG_BUILD_DIR)

INCLUDES = -I. -Icodec -Itransmission -Ilog_system -Itools -Ithird_party \
           $(FFMPEG_INCLUDE)

LDFLAGS = -L$(X264_DIR) -L$(FFMPEG_BUILD_DIR)/libavcodec -L$(FFMPEG_BUILD_DIR)/libavutil
LDLIBS = -lx264 -lavcodec -lavutil -llzma

# Linux-only libraries
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDLIBS += -ldrm
endif

# Conditionally include vvenc/vvdec libraries if VVENC is enabled
ifeq ($(VVENC),1)
  LDFLAGS += -L./lib
  LDLIBS += -lvvenc -lvvdec
endif

# Source files - conditionally include h266 based on VVENC flag
SRCS = $(wildcard codec/*.cc \
				 codec/h264/*.cc \
				 codec/mock_codec/*.cc \
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
UNIT_TEST_TARGET = $(BUILD_DIR)/unit_tests

all: $(BUILD_DIR) $(TARGET)

test: $(BUILD_DIR) $(TEST_TARGET)

unit_test: $(BUILD_DIR) $(UNIT_TEST_TARGET)
	./$(UNIT_TEST_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Test decoder target - only includes necessary objects
TEST_OBJS = $(BUILD_DIR)/log_system/log_system.o \
            $(BUILD_DIR)/test_decoder.o

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# Unit tests (gtest) — detect include/lib paths
GTEST_INCLUDE ?= $(shell pkg-config --cflags gtest 2>/dev/null || \
                  ([ -d /opt/homebrew/include/gtest ] && echo "-I/opt/homebrew/include") || \
                  echo "-I/usr/include")
GTEST_LDFLAGS ?= $(shell pkg-config --libs-only-L gtest 2>/dev/null || \
                  ([ -d /opt/homebrew/lib ] && echo "-L/opt/homebrew/lib") || \
                  echo "")
GTEST_LDLIBS = -lgtest -lgtest_main

UNIT_TEST_SRCS = $(wildcard tests/*_test.cc)
# Source files needed by tests (no main, no socket_codec.cc, no video_capture_and_send.cc)
TESTABLE_SRCS = $(wildcard tools/*.cc log_system/*.cc) \
                transmission/network_simulator.cc \
                transmission/network_sender.cc \
                transmission/feedback_handler.cc \
                transmission/feedback_collector.cc \
                transmission/gcc_controller.cc \
                transmission/bandwidth_prober.cc
UNIT_TEST_OBJS = $(patsubst %.cc,$(BUILD_DIR)/%.o,$(UNIT_TEST_SRCS)) \
                 $(patsubst %.cc,$(BUILD_DIR)/%.o,$(TESTABLE_SRCS))

$(UNIT_TEST_TARGET): $(UNIT_TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(GTEST_INCLUDE) -o $@ $^ $(GTEST_LDFLAGS) $(GTEST_LDLIBS)

# Pattern rules: test sources need gtest includes, placed before generic rule
$(BUILD_DIR)/tests/%.o: tests/%.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(GTEST_INCLUDE) -c $< -o $@

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

.PHONY: all test unit_test clean compile_commands.json
