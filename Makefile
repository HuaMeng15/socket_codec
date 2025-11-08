TOP_DIR = .
BUILD_DIR = $(TOP_DIR)/build
CXX=g++
CXXFLAGS = -pthread -fPIC -std=c++23 -g -ggdb -pedantic -Wall -Wextra -Wno-missing-field-initializers -DDEBUG -mmacosx-version-min=14.6

INCLUDES = -Icodec -Itransmission -Ilog_system -Itools -I./include

LDFLAGS = -L./lib
LDLIBS = -lvvenc

SRCS = $(wildcard codec/*.cc \
				 transmission/*.cc \
				 log_system/*.cc \
				 tools/*.cc \
				 socket_codec.cc)

OBJS = $(patsubst %.cc,$(BUILD_DIR)/%.o,$(SRCS))

TARGET = $(BUILD_DIR)/socket_codec

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: %.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean