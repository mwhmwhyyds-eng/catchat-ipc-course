CC ?= cc
CPPFLAGS += -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
SOURCES = src/main.c src/ipc.c src/ui.c src/text.c
BUILD_DIR ?= build
OBJECTS = $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean check sanitize
all: $(BUILD_DIR)/catchat

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c src/ipc.h src/ui.h src/text.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/catchat: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) $(LDLIBS) -o $@.tmp
	mv -f -- $@.tmp $@

$(BUILD_DIR)/ui-behavior: tests/ui_behavior.c $(filter-out $(BUILD_DIR)/main.o,$(OBJECTS))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@.tmp
	mv -f -- $@.tmp $@

check: all $(BUILD_DIR)/ui-behavior
	./$(BUILD_DIR)/ui-behavior
	python3 tests/integration.py ./$(BUILD_DIR)/catchat

sanitize:
	$(MAKE) BUILD_DIR=build-sanitize CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' check

clean:
	rm -rf build build-sanitize
