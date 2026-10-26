/*
 * Bite 8 — The Preprocessor & Multi-File Compile (INTERFACE)
 *
 * This header DESCRIBES what mathutil.c DEFINES. Headers are read by
 * every file that wants to use the functions; the .c file is compiled
 * once and linked in. "Headers describe; sources define."
 *
 * Learn tie-ins:
 *   - #include is literal text pasting by the preprocessor, before any
 *     real compilation happens.
 *   - <...> means "search the system include paths" (stdio.h);
 *     "..." means "search my own directories first" (mathutil.h).
 *   - The include guard below makes double-inclusion harmless. Note the
 *     nuance (verified by experiment): repeating prototypes is legal C,
 *     so a guard isn't strictly needed for THIS header — but a struct,
 *     enum, or variable in a header cannot repeat, and the guard is the
 *     habit that keeps every future header safe.
 *   - #define creates a compile-time constant — no memory, no type,
 *     pure text substitution before the compiler ever sees it.
 */

/* The guard: if MATHUTIL_H is not defined yet, define it and process
 * the rest of this file. If the file is included a second time, the
 * macro already exists, the #ifndef fails, and everything up to #endif
 * is skipped — so the prototypes are never declared twice. */
#ifndef MATHUTIL_H
#define MATHUTIL_H

/* Bite 8, Do-item 3: a #define constant in the header, shared by every
 * file that includes it. It has NO type and NO storage — the preprocessor
 * literally replaces the text TABLE_MAX with 10 everywhere it appears. */
#define TABLE_MAX 10

/* Prototypes only — no bodies here. The parameter names are documentation
 * for the reader; the compiler matches on types alone. */
int max3(int a, int b, int c);
int clamp(int v, int lo, int hi);
int sum_digits(int n);

#endif /* MATHUTIL_H */
