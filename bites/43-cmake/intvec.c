/*
 * Bite 43 — CMake + tests: intvec implementation.
 * Doubling growth (bite 25), minimal surface. Note iv_get returning
 * -1 conflates "stored -1" with "out of range" - a real API would
 * return a status and write through a pointer. Documented trade-off,
 * kept here because the tests only probe in-range indices.
 */
#include "intvec.h"

#include <stdlib.h>

void iv_init(intvec *v)
{
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

int iv_push(intvec *v, int value)
{
    if (v == NULL)
        return -1;
    if (v->len == v->cap) {
        size_t newcap = (v->cap == 0) ? 4 : v->cap * 2;
        int *p = realloc(v->data, newcap * sizeof *p);
        if (p == NULL)
            return -1;
        v->data = p;
        v->cap = newcap;
    }
    v->data[v->len++] = value;
    return 0;
}

int iv_get(const intvec *v, size_t i)
{
    if (v == NULL || i >= v->len)
        return -1;
    return v->data[i];
}

void iv_free(intvec *v)
{
    if (v == NULL)
        return;
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}
