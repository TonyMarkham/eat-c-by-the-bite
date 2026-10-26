/*
 * Bite 10 — Checkpoint: Number Cruncher (menu + input loop)
 *
 * Compile + run (from bites/10-checkpoint-calculator/):
 *   gcc -Wall -Wextra -Werror -std=c17 -g calc.c main.c -o calc && ./calc
 *
 *   Sample session (piped):
 *     printf 'add\n3\n4\nq\n' | ./calc
 *   Abuse session (all of it must survive):
 *     printf 'x\nadd\nabc\n3\nsub\n10\nq\n' | ./calc
 *     printf 'div\n5\n0\npow\n2\n10\nq\n'   | ./calc
 *     printf 'mul\n7\n'                      | ./calc   (EOF mid-calculation)
 *
 * BREAK IT ON PURPOSE:
 *   See the header comment in calc.h — q mid-calculation, Ctrl-D
 *   anywhere, garbage operands. Every path below ends in a message and
 *   a clean exit, never a crash.
 *
 * Everything from bites 1-9 is in play:
 *   do/while menu (bite 5) | switch dispatch (bite 4) | functions with
 *   prototypes in a header (bites 6+8) | fgets+sscanf validation (bite 9)
 *   | a static flag with file scope (bite 7).
 */

#include <stdio.h>
#include "calc.h"

/* File-scope statics: private to main.c (bite 7's module privacy).
 * input_aborted is our "the user hit EOF" flag. The honest Phase 1 way
 * to get two results out of a function — a return value AND a signal —
 * is a flag like this; bite 14's out-parameters will replace it. */
static int input_aborted = 0;

/* Read one line into buf with fgets. Returns 1 on success, 0 on EOF. */
static int read_line(char *buf, int size)
{
    printf("> ");
    fflush(stdout);

    if (fgets(buf, size, stdin) == NULL) {
        printf("\nEOF detected — shutting down cleanly.\n");
        input_aborted = 1;
        return 0;
    }

    /* Strip the trailing '\n' (bite 9): find it, overwrite it. */
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
            break;
        }
    }
    return 1;
}

/* Validated number input (bite 9's pattern, wrapped). Keeps re-prompting
 * until the line holds exactly one integer, or EOF ends the program.
 * Note the (unchecked, but honest) overflow caveat from bite 9: %d on a
 * huge number is UB; surviving garbage, not parsing it, is the goal. */
static long read_long(const char *prompt)
{
    char buf[64];

    for (;;) {
        printf("%s", prompt);
        if (!read_line(buf, sizeof buf)) {
            return 0;   /* EOF: value irrelevant, flag says to bail out */
        }

        long v;
        if (sscanf(buf, "%ld", &v) == 1) {
            return v;
        }
        printf("invalid input: '%s' is not an integer — try again\n", buf);
    }
}

/* Map a menu line to a command letter. Dispatching on the FIRST letter
 * (below, in the switch) is what makes both 'a' and 'add' work: any
 * word starting with the right letter selects its command. */
static char command_letter(const char *line)
{
    if (line[0] == '\0') {
        return '?';   /* empty line: no command at all */
    }
    return line[0];
}

static void print_menu(void)
{
    printf("\n[a]dd [s]ub [m]ul [d]iv [p]ow [q]uit\n");
}

int main(void)
{
    char buf[64];
    int running = 1;

    printf("Number Cruncher — checkpoint 1. Type the letter or the full word.\n");

    /* do/while: the menu prints at least once (bite 5's textbook use).
     * `running` is the real off switch; EOF or 'q' clears it. */
    do {
        print_menu();
        if (!read_line(buf, sizeof buf)) {
            break;   /* EOF at the menu: read_line already said goodbye */
        }

        switch (command_letter(buf)) {
        case 'a': {
            long x = read_long("first number: ");
            if (input_aborted) {   /* 'q' or Ctrl-D mid-calculation */
                break;
            }
            long y = read_long("second number: ");
            if (input_aborted) {
                break;
            }
            printf("%ld + %ld = %ld\n", x, y, add(x, y));
            break;
        }
        case 's': {
            long x = read_long("first number: ");
            if (input_aborted) {
                break;
            }
            long y = read_long("second number: ");
            if (input_aborted) {
                break;
            }
            printf("%ld - %ld = %ld\n", x, y, sub(x, y));
            break;
        }
        case 'm': {
            long x = read_long("first number: ");
            if (input_aborted) {
                break;
            }
            long y = read_long("second number: ");
            if (input_aborted) {
                break;
            }
            printf("%ld * %ld = %ld\n", x, y, mul(x, y));
            break;
        }
        case 'd': {
            long x = read_long("first number: ");
            if (input_aborted) {
                break;
            }
            long y = read_long("second number: ");
            if (input_aborted) {
                break;
            }
            /* idiv handles b == 0 internally — no crash, just a message. */
            printf("%ld / %ld = %ld\n", x, y, idiv(x, y));
            break;
        }
        case 'p': {
            long base = read_long("base: ");
            if (input_aborted) {
                break;
            }
            long exp = read_long("exponent (>= 0): ");
            if (input_aborted) {
                break;
            }
            printf("%ld ^ %ld = %ld\n", base, exp, ipow(base, exp));
            break;
        }
        case 'q':
            running = 0;
            printf("bye!\n");
            break;
        default:
            printf("unknown command '%s' — pick from the menu\n", buf);
            break;
        }
    } while (running);

    return 0;
}
