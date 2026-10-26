/*
 * Bite 45 — Processes: fork/exec/wait
 *
 * Compile & run:
 *   gcc -Wall -Wextra -Werror -std=c17 -g bites/45-processes/mini_shell.c -o /tmp/b45
 *   /tmp/b45                  (interactive mini shell; "exit" or Ctrl-D to quit)
 *   /tmp/b45 --zombie-demo    (run the "break it on purpose" zombie exercise)
 *
 * Inside the shell, try:  ls -l   |   grep root /etc/passwd   |   false   |   zombies
 * ("zombies" runs the deliberate-bug exercise from inside the shell.)
 *
 * WHAT THIS TEACHES (the "Learn" bullets, in code):
 *  - fork() returns TWICE from one call: 0 in the child, the child's PID in the
 *    parent. There is no other C function like it. Same code image, diverging
 *    paths — after the `if (pid == 0)` the two processes are strangers who
 *    happen to share a past.
 *  - exec (execlp et al.) REPLACES the current program image entirely. If it
 *    succeeds, nothing after it in your code ever runs — not the next line,
 *    not the return. That's why exec failure is the only case we handle.
 *  - wait/waitpid collects the child's exit status. A finished child nobody
 *    waits for stays behind as a ZOMBIE: a process-table entry holding only
 *    its exit status. Servers that fork per client must reap (see bite 49).
 *
 * BREAK IT ON PURPOSE:
 *   Fork 5 children that are never waited on; check `ps` for zombies
 *   (run: /tmp/b45 --zombie-demo, then in another terminal:
 *       ps -o pid,ppid,stat,cmd | grep -E 'PID|Z'
 *    you will see 5 entries with stat "Z" (zombie) / "<defunct>").
 *   The demo then waits for them all — run ps again: zombies gone.
 *   This is exactly why Phase 5 servers need signal(SIGCHLD, SIG_IGN) or a
 *   waitpid loop: every accepted connection that is forked and forgotten
 *   lingers in the process table forever.
 *
 * Done when: the mini shell runs `ls -l` and reports its exit code correctly.
 */

/* -std=c17 is STRICT ISO C: glibc hides POSIX APIs (fork, sigaction,
 * getaddrinfo, poll...) unless the source asks for them. This line is
 * the "give me POSIX 2008" switch — you will put it at the top of every
 * systems program compiled with -std=cNN. Order matters: it must come
 * before ANY #include. */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 64
#define MAX_LINE 256

/* ------------------------------------------------------------------ */
/* Demo 1: fork() returns twice                                        */
/* ------------------------------------------------------------------ */
static void demo_fork(void)
{
    pid_t pid;

    fflush(stdout); /* One stdio buffer is about to become two. Flush first,
                       or both processes may print the same pending output. */

    pid = fork();
    if (pid < 0) {
        perror("fork"); /* errno tells you why (EAGAIN, ENOMEM, ...) */
        exit(1);
    }

    if (pid == 0) {
        /* ---- THE CHILD ---- fork() returned 0 here. */
        printf("  child : pid=%d parent=%d (I am the copy)\n",
               (int)getpid(), (int)getppid());
        _exit(0); /* _exit: no atexit/stdio flush games inside a forked child.
                     exit() would run duplicate cleanup of the copied world. */
    }

    /* ---- THE PARENT ---- fork() returned the child's PID here. */
    {
        int status = 0;
        printf("  parent: pid=%d child=%d\n", (int)getpid(), (int)pid);

        if (waitpid(pid, &status, 0) < 0) { /* blocks until this child exits */
            perror("waitpid");
            return;
        }
        /* The status is a bitfield, not a plain exit code — decode it: */
        if (WIFEXITED(status))
            printf("  parent: child exited with code %d\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf("  parent: child was KILLED by signal %d\n", WTERMSIG(status));
    }
}

/* ------------------------------------------------------------------ */
/* Demo 2 ("break it on purpose" made observable): zombies             */
/* ------------------------------------------------------------------ */
static void demo_zombies(int n)
{
    int i;
    printf("forking %d children that exit immediately and are NOT waited on...\n", n);

    for (i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0)
            _exit(0); /* child dies at once -> becomes a zombie until reaped */
    }

    printf("Now (within ~3s) run in another terminal:\n"
           "    ps -o pid,ppid,stat,cmd | grep -E 'PID|Z'\n"
           "You should see %d zombie entries (STAT = Z, cmd = <defunct>).\n"
           "A zombie holds a PID + exit status: leaked kernel resources.\n",
           n);
    sleep(3);

    /* THE FIX: reap everything. waitpid(-1, ..., WNOHANG) collects any
     * finished child without blocking — the loop servers in bites 49+ use. */
    printf("Reaping with waitpid(-1, &st, WNOHANG) in a loop...\n");
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG); /* -1 = any child */
        if (pid == 0)
            break;      /* children exist but none has finished */
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;      /* ECHILD: no children left — all reaped */
        }
        printf("  reaped child %d\n", (int)pid);
    }
    printf("Run the ps line again: zombies are gone.\n"
           "(Server shortcut: signal(SIGCHLD, SIG_IGN) makes the kernel reap\n"
           " children automatically — that's what bite 49 does.)\n");
}

/* ------------------------------------------------------------------ */
/* Mini shell: split a line, fork, exec, wait, report exit status      */
/* ------------------------------------------------------------------ */

/* Split `line` in place on runs of blanks. Writes pointers into argv_out
 * and terminates it with NULL, exactly like a real execvp argv.
 * Returns the argument count, or -1 if the line has too many words. */
static int split_args(char *line, char *argv_out[], int max_args)
{
    int argc = 0;
    char *tok = strtok(line, " \t"); /* modifies line — that's fine, it's ours */

    while (tok != NULL) {
        if (argc == max_args - 1) { /* keep one slot for the NULL terminator */
            fprintf(stderr, "mini-sh: too many arguments (max %d)\n", max_args - 1);
            return -1;
        }
        argv_out[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv_out[argc] = NULL;
    return argc;
}

static void run_command(char *line)
{
    char *argv[MAX_ARGS];
    int argc = split_args(line, argv, MAX_ARGS);
    pid_t pid;
    int status = 0;

    if (argc <= 0)
        return; /* empty line or split error: nothing to run */

    fflush(stdout);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /* CHILD: become the requested program.
         * execlp searches $PATH for argv[0] — like a shell does.
         * Note the call shape: execlp(file, arg0, arg1, ..., NULL).
         * arg0 is conventionally the program name again (argv[0]). */
        execvp(argv[0], argv);
        /* If we are STILL here, exec failed: no program image was replaced. */
        fprintf(stderr, "mini-sh: %s: %s\n", argv[0], strerror(errno));
        _exit(127); /* 127 = "command not found", a shell convention */
    }

    /* PARENT: wait and decode the exit status. */
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        printf("[mini-sh] %s -> exit %d%s\n", argv[0], code,
               code == 0 ? "" : "  (nonzero = the program reported failure)");
    } else if (WIFSIGNALED(status)) {
        printf("[mini-sh] %s -> killed by signal %d\n", argv[0], WTERMSIG(status));
    }
}

int main(int argc, char **argv)
{
    char line[MAX_LINE];

    if (argc > 1 && strcmp(argv[1], "--zombie-demo") == 0) {
        demo_zombies(5);
        return 0;
    }

    printf("demo 1: one fork() call, two returns:\n");
    demo_fork();

    printf("\nmini shell — try: ls -l | false | zombies | exit\n");
    for (;;) {
        printf("mini-sh> ");
        fflush(stdout);

        if (fgets(line, sizeof line, stdin) == NULL) {
            printf("\n"); /* Ctrl-D / EOF */
            break;
        }
        line[strcspn(line, "\n")] = '\0'; /* strip the trailing newline */

        if (line[0] == '\0')
            continue;
        if (strcmp(line, "exit") == 0)
            break;
        if (strcmp(line, "zombies") == 0) {
            demo_zombies(5);
            continue;
        }
        run_command(line);
    }
    return 0;
}
