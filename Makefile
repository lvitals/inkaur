CC ?= gcc
BASE_CFLAGS = -std=c99 -g -Wall -Wextra -pedantic
CFLAGS = $(BASE_CFLAGS)
LIBS = -lcurl -lalpm

OBJ = alloc.o json.o requests.o output.o install.o search.o util.o rpc.o pacman.o
TEST_OBJ = tests/main.o tests/unit/test_json.o tests/unit/test_util.o tests/unit/test_alloc.o tests/unit/test_rpc.o tests/unit/test_install.o tests/functional/test_cli.o

# Flags for coverage (only used in test target)
COVERAGE_FLAGS = --coverage

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: inkaur

inkaur: $(OBJ) inkaur.o
	$(CC) $(CFLAGS) $(OBJ) inkaur.o $(LIBS) -o inkaur

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: all
	install -Dm755 inkaur $(DESTDIR)$(BINDIR)/inkaur

clean:
	rm -f *.o inkaur tests/main.o tests/unit/*.o tests/functional/*.o tests/run_tests
	rm -f *.gcno *.gcda *.gcna *.gcov tests/*.gcno tests/unit/*.gcno tests/functional/*.gcno tests/*.gcda tests/unit/*.gcda tests/functional/*.gcda tests/*.gcna tests/unit/*.gcna tests/functional/*.gcna tests/*.gcov tests/unit/*.gcov tests/functional/*.gcov

uninstall:
	rm /usr/local/bin/inkaur

# Special rule to build with coverage and testing flags
test: CFLAGS += $(COVERAGE_FLAGS) -DTESTING
test: LDFLAGS += $(COVERAGE_FLAGS)
test: clean inkaur tests/run_tests
	@./tests/run_tests
	@printf "\n--- Coverage Statistics ---\n"
	@gcov $(OBJ) | awk ' \
		/File/ {file=$$2; gsub(/'\''/, "", file)} \
		/Lines executed:/ { \
			if (file !~ /^\//) { \
				split($$0, a, ":"); \
				split(a[2], b, "%"); \
				printf("%-20s | %6s%%\n", file, b[1]); \
			} \
		}' | sort -r -k 3
	@echo "---------------------------"

coverage-report: test
	gcov $(OBJ)

check-leaks:
	@./check_leaks.sh

tests/run_tests: $(TEST_OBJ) $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)


.PHONY: all install clean uninstall test coverage-report check-leaks
