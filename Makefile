# inkaur — no third-party libraries: only make + gcc/clang + libc (C99 + POSIX).
CC      ?= cc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

ARCH := $(shell uname -m)

STD     = -std=c99 -pedantic -D_XOPEN_SOURCE=700
WARN    = -Wall -Wextra -Wformat=2 -Wformat-security -Wshadow -Wpointer-arith \
          -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wundef \
          -Wimplicit-fallthrough -Wnull-dereference \
          -Werror=implicit-function-declaration -Werror=format-security -Werror=vla
HARDEN  = -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 \
          -fstack-protector-strong -fstack-clash-protection \
          -fPIE -fno-strict-overflow -fno-strict-aliasing \
          -fno-delete-null-pointer-checks -ftrivial-auto-var-init=zero
ifeq ($(ARCH),x86_64)
HARDEN += -fcf-protection=full
endif
ifeq ($(ARCH),aarch64)
HARDEN += -mbranch-protection=standard
endif
LDHARDEN = -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,separate-code \
           -Wl,--as-needed

CFLAGS_ALL  = $(STD) $(WARN) $(HARDEN) $(EXTRA_CFLAGS)
LDFLAGS_ALL = $(LDHARDEN) $(EXTRA_LDFLAGS)

SRC  = alloc.c buf.c validate.c proc.c fsutil.c json.c net.c rpc.c srcinfo.c \
       vercmp.c pacman.c git.c audit.c review.c tty.c install.c search.c
OBJ  = $(SRC:.c=.o)

TEST_SRC = tests/main.c tests/unit/test_validate.c tests/unit/test_json.c \
           tests/unit/test_vercmp.c tests/unit/test_srcinfo.c \
           tests/unit/test_audit.c tests/unit/test_buf.c \
           tests/functional/test_cli.c
SAN      = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
           -fno-sanitize-recover=all
TEST_CFLAGS = $(STD) $(WARN) $(SAN) -DTESTING -I.
TEST_OBJ = $(addprefix build-test/,$(SRC:.c=.o) $(TEST_SRC:.c=.o))

all: inkaur

inkaur: $(OBJ) inkaur.o
	$(CC) $(CFLAGS_ALL) $(OBJ) inkaur.o $(LDFLAGS_ALL) -o $@

%.o: %.c
	$(CC) $(CFLAGS_ALL) -MMD -MP -c $< -o $@

build-test/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -MMD -MP -c $< -o $@

tests/run_tests: $(TEST_OBJ)
	$(CC) $(TEST_CFLAGS) $(TEST_OBJ) -o $@

# Unit tests under ASan/UBSan + functional tests against the hardened binary.
check: inkaur tests/run_tests
	./tests/run_tests

test: check

# Shows the hardening actually present in the binary (needs binutils' readelf).
hardening-report: inkaur
	@LC_ALL=C readelf -h inkaur | grep 'Type:' || true
	@LC_ALL=C readelf -lW inkaur | grep -E 'GNU_RELRO|GNU_STACK' || true
	@LC_ALL=C readelf -dW inkaur | grep -E 'BIND_NOW|FLAGS|NEEDED' || true
	@LC_ALL=C readelf -sW --dyn-syms inkaur | grep -cE '__stack_chk_fail|_chk@' | sed 's/^/fortified\/canary symbols: /' || true

# install never compiles: build as your user, then `sudo make install`.
install:
	@test -x inkaur || { echo "run 'make' as a normal user first"; exit 1; }
	install -Dm755 inkaur $(DESTDIR)$(BINDIR)/inkaur

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/inkaur

clean:
	rm -rf *.o *.d inkaur build-test tests/run_tests

-include $(OBJ:.o=.d) inkaur.d $(TEST_OBJ:.o=.d)

.PHONY: all check test install uninstall clean hardening-report
