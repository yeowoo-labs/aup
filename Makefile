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
# The version aup --version says: the VERSION file's line.
VERSION := $(shell cat VERSION)
$(BUILD)/aup: compiler/main.c compiler/compile.c compiler/aup.h VERSION | $(BUILD)
	$(CC) $(CPPFLAGS) -DAUP_VERSION='"$(VERSION)"' $(CFLAGS) compiler/main.c compiler/compile.c $(LDFLAGS) -lm $(LDLIBS) -o $@

check: $(BUILD)/aup
	$(MAKE) -C $(PURE) build/pure-run build/pure-serve
	python3 tests/check.py --aup $(BUILD)/aup --pure-run $(PURE)/build/pure-run --pure $(PURE)
