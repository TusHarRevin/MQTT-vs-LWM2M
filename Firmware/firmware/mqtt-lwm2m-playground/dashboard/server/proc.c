#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <process.h>
#else
#  include <pthread.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/wait.h>
extern char **environ;
#endif

typedef struct { thread_fn fn; void *arg; } trampoline_t;

#ifdef _WIN32
static unsigned __stdcall tramp(void *a)
#else
static void *tramp(void *a)
#endif
{
    trampoline_t t = *(trampoline_t *)a;
    free(a);
    t.fn(t.arg);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int thread_start(thread_fn fn, void *arg)
{
    trampoline_t *t = malloc(sizeof(*t));
    if (t == NULL) {
        return -1;
    }
    t->fn = fn;
    t->arg = arg;
#ifdef _WIN32
    {
        uintptr_t h = _beginthreadex(NULL, 0, tramp, t, 0, NULL);
        if (h == 0) {
            free(t);
            return -1;
        }
        CloseHandle((HANDLE)h);
    }
#else
    {
        pthread_t th;
        if (pthread_create(&th, NULL, tramp, t) != 0) {
            free(t);
            return -1;
        }
        pthread_detach(th);
    }
#endif
    return 0;
}

void mutex_init(lab_mutex_t *m)
{
#ifdef _WIN32
    CRITICAL_SECTION *cs = malloc(sizeof(*cs));
    InitializeCriticalSection(cs);
    m->h = cs;
#else
    pthread_mutex_t *mx = malloc(sizeof(*mx));
    pthread_mutex_init(mx, NULL);
    m->h = mx;
#endif
}

void mutex_lock(lab_mutex_t *m)
{
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)m->h);
#else
    pthread_mutex_lock((pthread_mutex_t *)m->h);
#endif
}

void mutex_unlock(lab_mutex_t *m)
{
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)m->h);
#else
    pthread_mutex_unlock((pthread_mutex_t *)m->h);
#endif
}

int proc_spawn(proc_t *p, char *const argv[])
{
    memset(p, 0, sizeof(*p));
#ifdef _WIN32
    {
        char cmd[4096];
        size_t o = 0;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        /* Quote every argument; the lab only passes simple validated tokens.
         * No handle inheritance: children must not keep the lab's sockets
         * open. They still share the console, so their logs appear there. */
        for (int i = 0; argv[i]; i++) {
            int n = snprintf(cmd + o, sizeof(cmd) - o, "%s\"%s\"", i ? " " : "", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(cmd) - o) {
                return -1;
            }
            o += (size_t)n;
        }
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            return -1;
        }
        CloseHandle(pi.hThread);
        p->handle = (intptr_t)pi.hProcess;
    }
#else
    {
        pid_t pid;
        if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) {
            return -1;
        }
        p->handle = pid;
    }
#endif
    p->running = true;
    return 0;
}

bool proc_poll(proc_t *p)
{
    if (!p->running) {
        return true;
    }
#ifdef _WIN32
    if (WaitForSingleObject((HANDLE)p->handle, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess((HANDLE)p->handle, &code);
        CloseHandle((HANDLE)p->handle);
        p->exit_code = (int)code;
        p->running = false;
        return true;
    }
#else
    {
        int st;
        pid_t r = waitpid((pid_t)p->handle, &st, WNOHANG);
        if (r == (pid_t)p->handle) {
            p->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            p->running = false;
            return true;
        }
    }
#endif
    return false;
}

void proc_kill(proc_t *p)
{
    if (!p->running) {
        return;
    }
#ifdef _WIN32
    TerminateProcess((HANDLE)p->handle, 1);
#else
    kill((pid_t)p->handle, SIGTERM);
#endif
}
