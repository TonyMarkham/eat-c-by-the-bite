/*
 * Bite 40 — Error handling conventions: the one logging channel.
 * Convention + build/run commands in contact.h and main.c.
 *
 * log_error/log_info are this project's single error channel (rule 4
 * of the convention): they exist so the TOP layer can report without
 * each module inventing its own fprintf format. Low-level modules
 * never call these - they return codes.
 */

#ifndef LOG_H
#define LOG_H

/* "error: <formatted message>\n" on stderr. */
void log_error(const char *fmt, ...);

/* "info: <formatted message>\n" on stdout. */
void log_info(const char *fmt, ...);

#endif /* LOG_H */
