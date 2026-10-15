#ifndef PROC_H
#define PROC_H

/* Threads, a mutex and child processes - the little OS glue the lab needs. */

#include <stdbool.h>
#include <stdint.h>

typedef void (*thread_fn)(void *arg);
int thread_start(thread_fn fn, void *arg);

typedef struct { void *h; } lab_mutex_t;
void mutex_init(lab_mutex_t *m);
void mutex_lock(lab_mutex_t *m);
void mutex_unlock(lab_mutex_t *m);

typedef struct
{
    bool running;
    intptr_t handle; /* HANDLE on Windows, pid elsewhere */
    int exit_code;
} proc_t;

/* argv[0] is the executable path. stdout/stderr are inherited. */
int proc_spawn(proc_t *p, char *const argv[]);
/* Non-blocking check; returns true if it has exited (exit_code set). */
bool proc_poll(proc_t *p);
void proc_kill(proc_t *p);

#endif /* PROC_H */
