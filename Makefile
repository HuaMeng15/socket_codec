TOP_DIR = .
BUILD_DIR = $(TOP_DIR)/build
CXX=g++
CXXFLAGS = -pthread -fPIC -std=c++23 -g -ggdb -pedantic -Wall -Wextra -Wno-missing-field-initializers -DDEBUG -mmacosx-version-min=14.6

INCLUDES = -I. -Icodec -Itransmission -Ilog_system -Itools -I./include

LDFLAGS = -L./lib
LDLIBS = -lvvenc -lvvdec

SRCS = $(wildcard codec/*.cc \
				 transmission/*.cc \
				 log_system/*.cc \
				 tools/*.cc \
				 socket_codec.cc)

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