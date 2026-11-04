# Static musl when available -- it removes the dynamic loader from every
# invocation, which dominates the runtime of a process this short-lived.
MUSL := $(shell command -v musl-gcc 2>/dev/null)
ifneq ($(MUSL),)
CC := musl-gcc
LDFLAGS += -static
endif

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -fno-plt -ffunction-sections -fdata-sections
# Apple's linker spells section garbage collection and symbol stripping differently
ifeq ($(shell uname -s),Darwin)
LDFLAGS += -Wl,-dead_strip
else
LDFLAGS += -Wl,--gc-sections -s
endif
LDLIBS := -lm

# lint: the stricter warning set, as errors, without producing a binary
WARN := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral \
        -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wcast-qual -Wwrite-strings -Werror
SAN := -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all

PREFIX ?= $(HOME)/.claude/claude-statusline

statusline: src/statusline.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

test: tests
	./tests

tests: src/tests.c src/statusline.c
	$(CC) $(CFLAGS) -Wno-unused-function -DSL_TEST -o $@ src/tests.c $(LDFLAGS) $(LDLIBS)

lint:
	$(CC) $(WARN) -fsyntax-only src/statusline.c
	$(CC) $(WARN) -Wno-unused-function -DSL_TEST -fsyntax-only src/tests.c

# unit tests under AddressSanitizer + UBSan; musl ships no sanitizer runtime, so
# this uses the system compiler (SAN_CC=clang to switch)
SAN_CC ?= cc
sanitize: src/tests.c src/statusline.c
	$(SAN_CC) $(SAN) -Wno-unused-function -DSL_TEST -o tests-san src/tests.c -lm
	./tests-san

install: statusline
	mkdir -p $(PREFIX)
	cp statusline $(PREFIX)/statusline

clean:
	rm -f statusline tests tests-san

.PHONY: test lint sanitize install clean
