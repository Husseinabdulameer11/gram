# Works with GNU make on Linux, macOS and Windows (MSYS2: `make` from any
# MSYS2 shell, or `mingw32-make`).
CFLAGS  ?= -O2
WARN     = -std=c89 -pedantic -Wall -Wextra -Wshadow -Wconversion -Werror

ifeq ($(OS),Windows_NT)
  EXE := .exe
  # mingw32-make defaults CC to `cc`, which MSYS2 does not always provide
  ifeq ($(origin CC),default)
    CC := gcc
  endif
  # MinGW gcc ships no sanitizer runtime (the CLANG64 toolchain does:
  # make test SAN="-fsanitize=address,undefined")
  SAN ?=
else
  EXE :=
  SAN ?= -fsanitize=address,undefined
endif

GRAMGEN = build/gramgen$(EXE)
LIBGRAM = build/libgram.a
TEST    = build/test$(EXE)

# `make install` puts gram where other projects (such as Crib) find it:
#   $(PREFIX)/bin/gramgen  $(PREFIX)/include/gram.h  $(PREFIX)/lib/libgram.a
# PREFIX=~/.local installs for one user without sudo.
PREFIX ?= /usr/local
AR     ?= ar

all: $(GRAMGEN) $(LIBGRAM)

build:
	mkdir -p build

# The library itself: strict C89, freestanding (no libc headers available).
build/gram.o: gram.c gram.h | build
	$(CC) $(CFLAGS) $(WARN) -ffreestanding -c gram.c -o $@

$(LIBGRAM): build/gram.o
	$(AR) rcs $@ build/gram.o

$(GRAMGEN): tools/gramgen.c build/gram.o
	$(CC) $(CFLAGS) -Wall -Wextra tools/gramgen.c build/gram.o -o $@

# The tests check precompiled tables against the compiled grammar, using the
# small fixture grammar tests/tiny.gram run through gramgen.
build/tiny_grammar.h build/tiny_grammar.c: tests/tiny.gram $(GRAMGEN)
	$(GRAMGEN) tests/tiny.gram tiny build

TEST_SRC = tests/test.c gram.c build/tiny_grammar.c
TEST_INC = -I. -Ibuild

test: build/tiny_grammar.c
	$(CC) $(CFLAGS) -Wall -Wextra $(SAN) -g $(TEST_INC) $(TEST_SRC) -o $(TEST)
	$(TEST)
	$(CC) $(CFLAGS) -Wall -Wextra $(SAN) -g $(TEST_INC) -DGRAM_MAX_STEPS=20000 $(TEST_SRC) -o $(TEST)
	$(TEST)

# Run the test suite on other CPUs under QEMU user-mode emulation.
# Needs qemu-user and the gcc cross compilers (Debian/Ubuntu:
# apt install qemu-user gcc-riscv64-linux-gnu gcc-arm-linux-gnueabihf
#             gcc-aarch64-linux-gnu gcc-s390x-linux-gnu).
# s390x is there because it is big-endian.
QEMU_TARGETS = riscv64-linux-gnu:qemu-riscv64 arm-linux-gnueabihf:qemu-arm \
               aarch64-linux-gnu:qemu-aarch64 s390x-linux-gnu:qemu-s390x
qemu: build/tiny_grammar.c
	@for t in $(QEMU_TARGETS); do \
	    cc=$${t%%:*}-gcc; q=$${t##*:}; \
	    if ! command -v $$cc >/dev/null || ! command -v $$q >/dev/null; then \
	        echo "$$t: skipped ($$cc or $$q not installed)"; continue; fi; \
	    $$cc -O2 -static $(TEST_INC) -DGRAM_MAX_STEPS=20000 $(TEST_SRC) -o build/test-$${t%%:*} || exit 1; \
	    printf "%s: " $${t%%:*}; $$q build/test-$${t%%:*} || exit 1; \
	done

# Fuzz the grammar compiler and the parser (needs clang with libFuzzer).
FUZZ_TIME ?= 60
fuzz: | build
	clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -DGRAM_MAX_STEPS=100000 tests/fuzz.c gram.c -o build/fuzz
	mkdir -p build/corpus
	./build/fuzz -max_len=600 -timeout=10 -max_total_time=$(FUZZ_TIME) build/corpus tests/corpus

# Compile the core for other CPUs with clang and verify that the object
# needs nothing from a C library. CPUs without a multiply instruction may
# reference the compiler's own arithmetic helpers (__mul...), nothing else.
TARGETS = x86_64-unknown-none i386-unknown-none aarch64-unknown-none \
          armv7m-none-eabi armv6m-none-eabi riscv64-unknown-elf \
          riscv32-unknown-elf@-march=rv32im riscv32-unknown-elf@-march=rv32i \
          riscv32-unknown-elf@-march=rv32e msp430-none-elf avr-none@-mmcu=atmega328p
cross: | build
	@for t in $(TARGETS); do \
	    f=`echo $$t | tr '@' ' '`; \
	    clang --target=$$f -Os $(WARN) -ffreestanding -c gram.c -o build/gram-$$t.o || exit 1; \
	    u=`llvm-nm -u build/gram-$$t.o 2>/dev/null || nm -u build/gram-$$t.o`; \
	    s=`llvm-size build/gram-$$t.o 2>/dev/null | tail -1 | awk '{print $$1+$$2}'`; \
	    u=`echo "$$u" | awk '{print $$2}' | tr '\n' ' ' | sed 's| *$$||'`; \
	    for x in $$u; do case $$x in __*) ;; *) echo "$$t: needs libc symbol $$x"; exit 1;; esac; done; \
	    if [ -n "$$u" ]; then echo "$$t: ok, $$s bytes, compiler runtime helpers only: $$u"; \
	    else echo "$$t: ok, $$s bytes, no external symbols"; fi; \
	done

install: $(GRAMGEN) $(LIBGRAM)
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	cp $(GRAMGEN) $(DESTDIR)$(PREFIX)/bin/
	cp gram.h $(DESTDIR)$(PREFIX)/include/
	cp $(LIBGRAM) $(DESTDIR)$(PREFIX)/lib/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/gramgen$(EXE) $(DESTDIR)$(PREFIX)/include/gram.h \
	      $(DESTDIR)$(PREFIX)/lib/libgram.a

clean:
	rm -rf build

.PHONY: all test cross qemu fuzz install uninstall clean
