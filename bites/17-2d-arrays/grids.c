/*
 * Bite 17 - 2D Arrays and Arrays of Strings
 * ==========================================
 * Compile & run (from the repo root inside WSL):
 *
 *     gcc -Wall -Wextra -Werror -std=c17 -g bites/17-2d-arrays/grids.c -o /tmp/b17 && /tmp/b17
 *
 * The "Learn" list, translated into code below:
 *   - int grid[4][5] is CONTIGUOUS memory, row-major: row 0's five ints,
 *     then row 1's five, ... grid[i][j] is *(*(grid + i) + j): step i
 *     rows (i * 5 ints), then j elements.
 *   - Passing a 2D array to a function REQUIRES the column count:
 *     void f(int g[][5], size_t rows). Without the column count the
 *     compiler cannot compute where row i starts.
 *   - char names[10][32] is 10 strings of up to 31 chars + '\0' each --
 *     fixed-size, simple, and exactly what the contact book uses.
 *
 * BREAK IT ON PURPOSE:
 *   Access grid[4][5] on an int grid[4][5] -- one past BOTH dimensions
 *   (that is element #25 of a 20-element block). 2D arrays make it easy
 *   to wander off the end; ASan still catches it:
 *
 *     gcc -DBREAK_IT=1 -Wall -Wextra -Werror -std=c17 -g \
 *         bites/17-2d-arrays/grids.c -o /tmp/b17break && /tmp/b17break
 *
 *   (Verified: the break build compiles under -Werror and ASan reports
 *   "stack-buffer-overflow" on the run.)
 */

#include <stdio.h>
#include <string.h>

enum { ROWS = 9, COLS = 9 };

/*
 * 2D array parameter: rows may vary, columns may NOT. The compiler
 * computes the address of g[i][j] as base + i*COLS*sizeof(int) +
 * j*sizeof(int) -- without the column count it cannot do the i*COLS
 * part. This is the 2D version of the "arrays decay to pointers" lie
 * from bite 11.
 */
static void print_grid(int g[][COLS], size_t rows)
{
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < COLS; c++)
            printf("%4d", g[r][c]);
        printf("\n");
    }
}

/*
 * Returns the INDEX of the longest string. Note the parameter type:
 * `const char lines[][32]` -- the 32 is mandatory, and const promises
 * we only read (bite 21 paying forward).
 */
static size_t longest_line(const char lines[][32], size_t n)
{
    size_t best = 0, bestlen = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(lines[i]);
        if (len > bestlen) {
            bestlen = len;
            best = i;
        }
    }
    return best;
}

int main(void)
{
    /* 1. Multiplication table: fill FIRST, print from the array after.
     * The array is the data; printing is a separate concern. */
    int table[ROWS][COLS];
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            table[r][c] = (r + 1) * (c + 1);

    printf("multiplication table (stored in int table[%d][%d]):\n", ROWS, COLS);
    print_grid(table, ROWS);

    /* grid[i][j] is *(*(grid+i)+j): step i rows, then j elements. */
    printf("\ntable[2][3] = %d and *(*(table+2)+3) = %d -- same thing\n\n",
           table[2][3], *(*(table + 2) + 3));

    /* 2. Five cities in a fixed grid of strings. */
    char names[5][32] = { "Lisbon", "Tokyo", "Nairobi", "Oslo", "Reykjavik" };

    printf("%d cities in char names[5][32] (%zu bytes total: 5 x 32):\n",
           5, sizeof names);
    for (size_t i = 0; i < 5; i++)
        printf("  %zu. %-12s (strlen %zu)\n", i + 1, names[i], strlen(names[i]));

    /* 3. longest_line does the work; main just prints the result. */
    size_t longest = longest_line(names, 5);
    printf("longest: \"%s\" (index %zu, %zu chars)\n",
           names[longest], longest, strlen(names[longest]));

#ifndef BREAK_IT
#define BREAK_IT 0   /* set to 1 (or -DBREAK_IT=1) to commit the crime */
#endif
#if BREAK_IT
    /* THE CRIME: int grid[4][5] has 20 elements. grid[4][5] is element
     * #25 (4*5 + 5): five ints past the end. With 2D arrays it is easy
     * to convince yourself "4 and 5 are the sizes, so they must be
     * valid indexes" -- they are not; both are one too many. */
    int grid[4][5] = { { 0 } };
    printf("\n--- breaking: grid[4][5] on an int[4][5] ---\n");
    grid[4][5] = 99;
    printf("read back %d (the write AND this read are both off the end)\n",
           grid[4][5]);
#endif

    return 0;
}
