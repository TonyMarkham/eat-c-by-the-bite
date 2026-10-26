/*
 * Bite 10 — Checkpoint: Number Cruncher (interface)
 *
 * Every Phase 1 skill except the file split converges here. This header
 * describes the math operations; calc.c defines them; main.c runs the
 * menu loop and owns all input handling.
 *
 * Compile + run (from bites/10-checkpoint-calculator/):
 *   gcc -Wall -Wextra -Werror -std=c17 -g calc.c main.c -o calc && ./calc
 *
 * BREAK IT ON PURPOSE (run these against the binary; NONE may crash):
 *   1. Type 'q' in the middle of a calculation (after the first operand).
 *   2. Press Ctrl-D at any prompt (menu or operands) — EOF path.
 *   3. Feed garbage operands: 'abc', '3 xyz', an empty line.
 *   If the program crashes, the checkpoint isn't done.
 *
 * Phase-1 constraint note: no pointers as out-parameters (that's bite
 * 14). Division by zero is therefore reported INSIDE idiv with an error
 * message and a 0 return — returning a status code is a Phase 4 habit.
 */

#ifndef CALC_H
#define CALC_H

/* All arithmetic lives on long (64-bit on this platform) so the menu
 * math has a bit more headroom than plain int. Overflow is still
 * possible and still undefined — documented, not hidden. */

long add(long a, long b);
long sub(long a, long b);
long mul(long a, long b);

/* Integer division. b == 0 is rejected with an error message and a
 * return of 0 — never a crash (hardware divide-by-zero would abort). */
long idiv(long a, long b);

/* Integer power by LOOP, not pow(): base multiplied by itself exp times.
 * exp < 0 is rejected (integer powers of negative exponent aren't
 * integers); exp == 0 returns 1 by the standard convention. */
long ipow(long base, long exp);

#endif /* CALC_H */
