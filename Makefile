# Static musl when available -- it removes the dynamic loader from every
# invocation, which dominates the runtime of a process this short-lived.
MUSL := $(shell command -v musl-gcc 2>/dev/null)
ifneq ($(MUSL),)
CC := musl-gcc
LDFLAGS += -static
endif

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -fno-plt -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections -s
LDLIBS := -lm

PREFIX ?= $(HOME)/.claude/claude-statusline

statusline: src/statusline.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

test: tests
	./tests

tests: src/tests.c src/statusline.c
	$(CC) $(CFLAGS) -Wno-unused-function -DSL_TEST -o $@ src/tests.c $(LDFLAGS) $(LDLIBS)

install: statusline
	mkdir -p $(PREFIX)
	cp statusline $(PREFIX)/statusline

clean:
	rm -f statusline tests

.PHONY: test install clean
