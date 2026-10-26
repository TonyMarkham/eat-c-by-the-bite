/*
 * Bite 8 — The Preprocessor & Multi-File Compile (USER OF THE LIBRARY)
 *
 * Compile + run — the real build, three explicit steps
 * (from bites/08-preprocessor/):
 *   gcc -Wall -Wextra -Werror -std=c17 -g -c mathutil.c
 *   gcc -Wall -Wextra -Werror -std=c17 -g -c main.c
 *   gcc main.o mathutil.o -o app && ./app
 *
 * Or in one shot, which does the same three steps internally:
 *   gcc -Wall -Wextra -Werror -std=c17 -g mathutil.c main.c -o app && ./app
 *
 * BREAK IT ON PURPOSE:
 *   Remove the #ifndef/#define/#endif guard from mathutil.h, then
 *   include it TWICE in this file (there's already a duplicate #include
 *   below — with the guard removed, the header's contents get pasted in
 *   twice). Surprise: this header still compiles! Function prototypes
 *   may legally repeat, and redefining a macro to the *identical* value
 *   is benign. Try it and see for yourself.
 *   So why guards? Because the moment the header grows anything that
 *   cannot repeat — a struct definition (redefinition of 'struct foo'
 *   is a hard error, verified), an enum, a file-scope variable — the
 *   unguarded double inclusion explodes. The guard is cheap insurance
 *   you add before the explosion, not after. Bug immunized forever.
 *
 * Learn tie-ins:
 *   - main.c only ever needed the PROTOTYPES to compile: it calls
 *     max3/clamp/sum_digits without knowing their bodies. The linker
 *     later stitches main.o's "please call max3" notes to mathutil.o's
 *     actual code. A missing definition would be a LINK error, not a
 *     compile error — that's "what the linker does with a missing
 *     function" from bite 6, seen from the multi-file side.
 */

#include <stdio.h>
#include "mathutil.h"   /* our header: quotes, not angle brackets */

/* BREAK IT ON PURPOSE: with the guard removed from mathutil.h, this
 * duplicate include pastes the header a second time. (See the header
 * comment for why that only bites once your header holds structs,
 * enums, or variables — and why you add the guard anyway.) */
#include "mathutil.h"   /* harmless NOW, because of the include guard */

int main(void)
{
    printf("--- calling mathutil functions from another file ---\n");
    printf("max3(3, 9, 5)      = %d\n", max3(3, 9, 5));
    printf("clamp(42, 0, 10)   = %d\n", clamp(42, 0, 10));
    printf("clamp(-5, 0, 10)   = %d\n", clamp(-5, 0, 10));
    printf("sum_digits(98765)  = %d\n", sum_digits(98765));

    /* --- TABLE_MAX, defined in the header. At compile time this line
     * literally becomes `for (int row = 1; row <= 10; row++)`. */
    printf("--- table %dx%d, sized by TABLE_MAX from mathutil.h ---\n",
           TABLE_MAX, TABLE_MAX);
    for (int row = 1; row <= TABLE_MAX; row++) {
        for (int col = 1; col <= TABLE_MAX; col++) {
            printf("%4d", row * col);
        }
        printf("\n");
    }

    return 0;
}
