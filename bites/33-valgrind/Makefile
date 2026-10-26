# Bite 33 — valgrind discipline, automated: the `check:` target
#
# From bite 38 this pattern gets formalized in every project's Makefile.
# Usage (inside WSL, in this folder):
#   make          build buggy + fixed
#   make check    run BOTH under valgrind — buggy must produce reports,
#                 fixed must be silent
#   make asan     build + run the fixed program under ASan/UBSan too
#   make clean
#
# NOTE: valgrind does NOT set a nonzero exit code on findings by default —
# its stderr output IS the report. If you want a failing exit code (for CI):
#   valgrind --error-exitcode=9 ...

CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -std=c17 -g
VG      = valgrind --leak-check=full --show-leak-kinds=all

all: buggy fixed

buggy: buggy.c
	$(CC) $(CFLAGS) buggy.c -o buggy

fixed: fixed.c
	$(CC) $(CFLAGS) fixed.c -o fixed

# The habit this bite installs: BOTH runs, in sequence, every time.
check: all
	@echo "===== buggy: expect a wall of findings ====="
	$(VG) ./buggy
	@echo "===== fixed: expect silence ====="
	$(VG) ./fixed

asan:
	$(CC) $(CFLAGS) -fsanitize=address,undefined fixed.c -o fixed-asan
	./fixed-asan

clean:
	rm -f buggy fixed fixed-asan

.PHONY: all check asan clean
