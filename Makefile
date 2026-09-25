CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build
# The Pure checkout that owns module format v1; tests load every module with it.
PURE ?= ../pure/prototypes/connections

.PHONY: all check
all: $(BUILD)/aup

$(BUILD):
	mkdir -p $@

$(BUILD)/aup: src/main.c src/compile.c src/aup.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c src/compile.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(BUILD)/aup
	$(MAKE) -C $(PURE) BUILD=build build/pure-run
	python3 tests/check.py --aup $(BUILD)/aup --pure-run $(PURE)/build/pure-run
