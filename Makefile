CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build
# The Pure checkout that owns the module format; tests load every module with it.
PURE ?= ../pure

.PHONY: all check
all: $(BUILD)/aup

$(BUILD):
	mkdir -p $@

# One part, the compiler (compiler/); tests/check.py checks it end to end on Pure.
$(BUILD)/aup: compiler/main.c compiler/compile.c compiler/aup.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) compiler/main.c compiler/compile.c $(LDFLAGS) $(LDLIBS) -o $@

check: $(BUILD)/aup
	$(MAKE) -C $(PURE) build/pure-run build/pure-serve
	python3 tests/check.py --aup $(BUILD)/aup --pure-run $(PURE)/build/pure-run --pure $(PURE)
