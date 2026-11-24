#include "log.h"

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_cs;
/* 0 = uninitialized, 1 = one thread is initializing, 2 = ready. */
static volatile LONG g_log_cs_state = 0;
#endif

static FILE *g_log_file = NULL;
static int g_log_level = LOG_LEVEL_INFO;

#ifdef _WIN32
static void log_cs_init(void)
{
    LONG state = InterlockedCompareExchange(&g_log_cs_state, 1, 0);

    if (state == 0) {
        InitializeCriticalSection(&g_log_cs);
        InterlockedExchange(&g_log_cs_state, 2);
    } else {
        while (InterlockedCompareExchange(&g_log_cs_state, 2, 2) != 2) {
            Sleep(0);
        }
    }
}
#endif

/* Called with the critical section held (or on non-Win32 builds). */
static void log_close_file_locked(void)
{
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_init(const char *path, int level)
{
#ifdef _WIN32
    log_cs_init();
    EnterCriticalSection(&g_log_cs);
#endif

    /* Close directly while holding the lock; log_shutdown() also acquires it. */
    log_close_file_locked();
    g_log_level = level;
    if (path != NULL && path[0] != '\0') {
        g_log_file = fopen(path, "a");
        if (g_log_file == NULL) {
            g_log_file = NULL;
        }
    }

#ifdef _WIN32
    LeaveCriticalSection(&g_log_cs);
#endif
}

void log_shutdown(void)
{
#ifdef _WIN32
    log_cs_init();
    EnterCriticalSection(&g_log_cs);
#endif
    log_close_file_locked();
#ifdef _WIN32
    LeaveCriticalSection(&g_log_cs);
    /* Keep the process-wide lock alive. A concurrent or later logger can
     * safely acquire it; deleting/reinitializing it races with waiting writers. */
#endif
}

void log_set_level(int level)
{
#ifdef _WIN32
    log_cs_init();
    EnterCriticalSection(&g_log_cs);
    g_log_level = level;
    LeaveCriticalSection(&g_log_cs);
#else
    g_log_level = level;
#endif
}

int log_get_level(void)
{
#ifdef _WIN32
    int level;

    log_cs_init();
    EnterCriticalSection(&g_log_cs);
    level = g_log_level;
    LeaveCriticalSection(&g_log_cs);
    return level;
#else
    return g_log_level;
#endif
}

static const char *level_name(int level)
{
    switch (level) {
    case LOG_LEVEL_ERROR:
        return "ERROR";
    case LOG_LEVEL_WARN:
        return "WARN ";
    case LOG_LEVEL_INFO:
        return "INFO ";
    case LOG_LEVEL_DEBUG:
        return "DEBUG";
    case LOG_LEVEL_TRACE:
        return "TRACE";
    default:
        return "?????";
    }
}

void log_write(int level, const char *fmt, ...)
{
    char msg[2048];
    char line[2200];
    time_t now;
    struct tm local_tm;
    struct tm *tmv;
    va_list ap;
    int n;
#ifdef _WIN32
    int locked = 0;
#endif

#ifdef _WIN32
    log_cs_init();
    EnterCriticalSection(&g_log_cs);
    locked = 1;
#endif
    if (level > g_log_level) {
#ifdef _WIN32
        LeaveCriticalSection(&g_log_cs);
#endif
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    /* Pre-C99 CRTs return -1 on truncation; mark the clipped text so a
     * partial line is never mistaken for the complete message. */
    if (n < 0) {
        msg[sizeof(msg) - 1] = '\0';
    }
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        size_t m = strlen(msg);
        if (m > 3) {
            m -= 3;
        } else {
            m = 0;
        }
        msg[m] = '.';
        msg[m + 1] = '.';
        msg[m + 2] = '.';
        msg[m + 3] = '\0';
    }

    now = time(NULL);
    tmv = localtime(&now);
    if (tmv != NULL) {
        local_tm = *tmv;
        sprintf(line, "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
                local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
                level_name(level), msg);
    } else {
        sprintf(line, "[%s] %s\n", level_name(level), msg);
    }

    if (g_log_file != NULL) {
        fputs(line, g_log_file);
        fflush(g_log_file);
    }
#ifdef _WIN32
    OutputDebugStringA(line);
    if (locked) {
        LeaveCriticalSection(&g_log_cs);
    }
#else
    fputs(line, stderr);
#endif
}
