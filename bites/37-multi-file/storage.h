/*
 * Bite 37 — Multi-file projects: storage module (INTERFACE).
 *
 * The point of a second module: main.c now knows the file format only
 * through this interface, and contact.c knows nothing about files at
 * all. Three responsibilities, three files, no entanglement.
 *
 * Format: one contact per line, "name;phone\n" (same as bite 35).
 *
 * BREAK IT ON PURPOSE: covered in main.c (linker and header-guard
 * experiments). For this file specifically: if you delete a prototype
 * here that main.c calls, you get an "undefined reference" LINK error,
 * not a compile error - the contract between modules is checked late.
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "contact.h"

/* Save the whole book, one "name;phone\n" line per contact.
 * Returns 0 ok, -1 could not open/write the file. */
int storage_save(const contactbook_t *b, const char *path);

/* Load contacts from path, ADDING them to the book (which should be
 * empty or you may get duplicates).
 * Malformed lines are warned about on stderr and skipped - the load
 * survives a dirty file. Returns 0 ok, -1 could not open the file. */
int storage_load(contactbook_t *b, const char *path);

#endif /* STORAGE_H */
