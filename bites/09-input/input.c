/*
 * Bite 9 — Input Without Footguns
 *
 * Compile + run (from the repo root):
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/09-input/input.c -o /tmp/b09 && /tmp/b09
 *
 *   Test it with the bite's five inputs (one line each):
 *   printf '3 4\n3\nabc\n3 abc\n99999999999999999\n3 5\n' | /tmp/b09
 *
 *   Expected: "3 4" -> sum 6; "3" and "abc" and "3 abc" -> "invalid
 *   input", re-prompt; the huge number overflows int (sscanf %d gives
 *   an unspecified/garbage value — see the note below); finally "3 5"
 *   succeeds and the program exits 0.
 *
 * BREAK IT ON PURPOSE:
 *   Type (or pipe) 80 characters at this program. Nothing overflows:
 *   fgets reads AT MOST size-1 characters and always NUL-terminates —
 *   that truncation is the safety net. Now imagine the same program
 *   written with scanf("%s", buf): scanf keeps reading past your array
 *   and smashes the stack. That's a buffer overflow with a smiley face.
 *   Extra observation: with a 100-char pipe, the leftover text stays in
 *   stdin and gets picked up by the NEXT fgets — fgets truncates the
 *   line, it doesn't discard it.
 *
 * Learn tie-ins:
 *   - fgets keeps the '\n' (when the line fits) — deal with it.
 *   - sscanf's return value = number of items successfully converted.
 *     Checking it is your validation: "3 abc" converts only 1 item, so
 *     the line is rejected even though it starts with a number.
 *   - %d overflow (99999999999999999) is undefined behavior in sscanf —
 *     you can't fully "handle" it with %d. The professional fix is
 *     strtol with errno checking (more strings and pointers needed —
 *     revisit after Phase 2). Here we OBSERVE it, as the bite allows.
 */

#include <stdio.h>

#define BUF_SIZE 64

int main(void)
{
    char buf[BUF_SIZE];

    printf("Feed me two integers on one line (e.g. '3 4'). Ctrl-D to quit.\n");

    for (;;) {
        printf("> ");
        fflush(stdout);   /* make the prompt appear before we block on input */

        /* fgets: read at most BUF_SIZE-1 chars, always NUL-terminate.
         * NULL return means EOF or read error — the only clean way out. */
        if (fgets(buf, sizeof buf, stdin) == NULL) {
            printf("\nEOF — goodbye.\n");
            return 0;
        }

        /* Strip the trailing newline: fgets KEEPS it (if the line fit).
         * Do it the explicit way: walk to the end, find '\n', kill it. */
        int i = 0;
        while (buf[i] != '\0') {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                break;
            }
            i++;
        }

        /* Parse TWO integers from the line we already have. The & before
         * a and b means "the address of" — sscanf must know WHERE to
         * store results. Forgetting & is the classic crash; gcc's
         * -Wformat usually catches it. */
        int a, b;
        int converted = sscanf(buf, "%d %d", &a, &b);

        if (converted == 2) {
            printf("sum of %d and %d is %d\n", a, b, a + b);
            return 0;   /* success: one good line ends the program */
        }

        /* Anything else — 0 or 1 items converted — is invalid input.
         * Diagnose a little so the re-prompt teaches something. */
        if (converted == 1) {
            printf("invalid input: got one number but need two (line was '%s')\n",
                   buf);
        } else {
            printf("invalid input: no numbers found (line was '%s')\n", buf);
        }
        /* No crash, no exit: loop around and re-prompt. Garbage in,
         * re-prompt out. That's the "done when" of this bite. */
    }
}
