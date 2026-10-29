# Builds the library and its tests on macOS (or any machine with clang).
#
#   make            build/lib/libdeflate_cpp.a
#   make test       builds and runs every test
#   BUILD=build/x   builds somewhere else, so two builds can run at once

CXX := clang++
BUILD ?= build
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wshadow -Werror -MMD -MP

LIBRARY := $(BUILD)/lib/libdeflate_cpp.a
SOURCES := $(shell find Deflate -name "*.cpp" | sort)
OBJECTS := $(SOURCES:%.cpp=$(BUILD)/obj/%.o)
TEST_SOURCES := $(wildcard Tests/*.cpp)
TEST_OBJECTS := $(TEST_SOURCES:%.cpp=$(BUILD)/obj/%.o)

all: $(LIBRARY)

$(LIBRARY): $(OBJECTS)
	@mkdir -p $(dir $@)
	rm -f $@
	ar rcs $@ $^

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests: $(TEST_OBJECTS) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: all $(BUILD)/tests
	./$(BUILD)/tests

clean:
	rm -rf build

.PHONY: all test clean

-include $(shell find $(BUILD)/obj -name "*.d" 2>/dev/null)
