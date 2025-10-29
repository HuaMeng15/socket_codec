TOP_DIR = .
TRANSMISSION_DIR = $(TOP_DIR)/Transmission
BUILD_DIR = $(TOP_DIR)/build
CXX=g++
CXXFLAGS = -pthread -fPIC -std=c++23 -g -ggdb -pedantic -Wall -Wextra -Wno-missing-field-initializers -DDEBUG

INCLUDES = -Icodec -Itransmission -IlogSystem -Itools

SRCS = $(wildcard codec/*.cc \
				 transmission/*.cc \
				 logSystem/*.cc \
				 tools/*.cc \
				 socket_codec.cc)

OBJS = $(patsubst %.cc,$(BUILD_DIR)/%.o,$(SRCS))

TARGET = $(BUILD_DIR)/socket_codec

all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^

$(BUILD_DIR)/%.o: %.cc
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean