/*
 * Bite 43 — CMake + tests: intvec module (interface) - bite 25's
 * dynamic int array, now a real module under test.
 * Build/run commands in CMakeLists.txt's header comment.
 */

#ifndef INTVEC_H
#define INTVEC_H

#include <stddef.h>

typedef struct {
    int *data;
    size_t len;
    size_t cap;
} intvec;

void iv_init(intvec *v);
int  iv_push(intvec *v, int value);     /* 0 ok, -1 OOM */
int  iv_get(const intvec *v, size_t i); /* value at i, or -1 if out of range */
void iv_free(intvec *v);

#endif /* INTVEC_H */
