# Bite 38 — Make: hand-written build for the 4-file contact book.
#
# Quick reference:
#   make            build ./prog
#   ./prog --selftest   non-interactive round-trip check
#   make check      ASan/UBSan build + valgrind run
#   make clean      remove prog, all .o files, the ASan binary
#
# The grammar: a rule is
#     target: dependencies
# <TAB>	recipe
# The recipe line MUST start with a real TAB character - not spaces.
# (This file uses literal tabs on the recipe lines; a space there is
# the #1 beginner Makefile error: "missing separator".)
#
# What make computes: a target is rebuilt if it is missing OR if any
# dependency is NEWER (timestamps). Everything else is skipped. That is
# the entire magic - you could do it with stat() and a shell script.
#
# BREAK IT ON PURPOSE: delete `contact.h` from the main.o dependency
# line below, then `touch contact.h && make`. Nothing rebuilds and the
# stale main.o is silently linked in - the classic stale-header trap.
# Re-add the dep, touch the header again, watch make fix it.

CC     := gcc
CFLAGS := -Wall -Wextra -Werror -std=c17 -g

OBJS := contact.o storage.o main.o

# Final link: prog depends on all objects; one recipe relinks.
prog: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# ---- dependency tracking: the part people get wrong ----------------
# Every .o depends on its .c AND every header it includes. Missing
# header deps = the "changed the .h, nothing rebuilt" trap. Read each
# file's #include list and mirror it here.
contact.o: contact.c contact.h
storage.o: storage.c storage.h contact.h
main.o:    main.c    contact.h storage.h

# ---- pattern rule: how to build ANY .o from its .c -----------------
# $< = the first dependency (the .c), $@ = the target (the .o).
# The explicit dep lines above supply the extra header dependencies;
# this one rule supplies the recipe for all of them.
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ---- phony targets: not files, just commands -----------------------
# .PHONY tells make these names are not real files, so a stray file
# called "clean" can't shadow them.
.PHONY: check clean

# check: bite 33's workflow, formalized.
#   1. sanitizer build + selftest run   (catches UB / bad frees)
#   2. valgrind on the normal build     (catches leaks + memory errors)
check: prog
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o prog_asan contact.c storage.c main.c
	./prog_asan --selftest
	valgrind --leak-check=full --show-leak-kinds=all ./prog --selftest
	rm -f prog_asan

clean:
	rm -f prog prog_asan $(OBJS)
