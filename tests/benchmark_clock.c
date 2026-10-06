#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

/* Measure one generated executable repeatedly. Cycle values are deliberately
 * unavailable where the host has no unprivileged per-process counter API. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>

static double now(void) {
    LARGE_INTEGER tick, frequency;
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&frequency);
    return (double)tick.QuadPart / (double)frequency.QuadPart;
}

static int run(const char *path, uint64_t *cycles, int *have_cycles) {
    size_t length = 0;
    while (path[length]) ++length;
    char *command = (char *)malloc(length + 3);
    if (!command) return 0;
    snprintf(command, length + 3, "\"%s\"", path);
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof startup;
    BOOL created = CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    free(command);
    if (!created) return 0;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    BOOL cycle_ok = QueryProcessCycleTime(process.hProcess, (PULONG64)cycles);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    *have_cycles = cycle_ok != 0;
    return exit_code == 0;
}

#else
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#ifdef __linux__
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
static int perf_open(pid_t pid) {
    struct perf_event_attr event = {0};
    event.type = PERF_TYPE_HARDWARE;
    event.size = sizeof event;
    event.config = PERF_COUNT_HW_CPU_CYCLES;
    event.disabled = 1;
    event.exclude_kernel = 1;
    event.exclude_hv = 1;
    return (int)syscall(__NR_perf_event_open, &event, pid, -1, -1, 0);
}
#endif

static double now(void) {
    struct timespec tick;
    if (clock_gettime(CLOCK_MONOTONIC, &tick)) exit(1);
    return (double)tick.tv_sec + (double)tick.tv_nsec / 1e9;
}

static int wait_for(pid_t child, int flags, int *status) {
    while (waitpid(child, status, flags) < 0) {
        if (errno != EINTR) return 0;
    }
    return 1;
}

static int run(const char *path, uint64_t *cycles, int *have_cycles) {
    pid_t child = fork();
    if (child < 0) return 0;
    if (!child) {
#ifdef __linux__
        /* Let the parent attach before executable instructions run. */
        raise(SIGSTOP);
#endif
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    int counter = -1;
#ifdef __linux__
    if (!wait_for(child, WUNTRACED, &status) || !WIFSTOPPED(status)) return 0;
    counter = perf_open(child);
    if (counter >= 0) {
        ioctl(counter, PERF_EVENT_IOC_RESET, 0);
        ioctl(counter, PERF_EVENT_IOC_ENABLE, 0);
    }
    if (kill(child, SIGCONT)) { if (counter >= 0) close(counter); return 0; }
#endif
    if (!wait_for(child, 0, &status)) { if (counter >= 0) close(counter); return 0; }
#ifdef __linux__
    if (counter >= 0) {
        uint64_t measured = 0;
        ioctl(counter, PERF_EVENT_IOC_DISABLE, 0);
        ssize_t count = read(counter, &measured, sizeof measured);
        close(counter);
        if (count == (ssize_t)sizeof measured) { *cycles = measured; *have_cycles = 1; }
    }
#endif
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    unsigned runs = (unsigned)strtoul(argv[1], NULL, 10);
    if (!runs || runs > 10000) return 2;
    uint64_t ignored = 0;
    int ignored_cycles = 0;
    if (!run(argv[2], &ignored, &ignored_cycles)) return 1;
    double started = now();
    uint64_t cycle_sum = 0;
    int have_cycles = 1;
    for (unsigned i = 0; i < runs; ++i) {
        uint64_t cycles = 0;
        int this_has_cycles = 0;
        if (!run(argv[2], &cycles, &this_has_cycles)) return 1;
        if (!this_has_cycles) have_cycles = 0;
        cycle_sum += cycles;
    }
    double milliseconds = (now() - started) * 1000.0 / runs;
#ifdef _WIN32
    const char *source = have_cycles ? "windows-query-process-cycle-time" : "unavailable";
#elif defined(__linux__)
    const char *source = have_cycles ? "linux-perf-event-cycles-user" : "unavailable";
#else
    const char *source = "unavailable";
#endif
    if (have_cycles) printf("%.6f,%llu,%s\n", milliseconds, (unsigned long long)(cycle_sum / runs), source);
    else printf("%.6f,,%s\n", milliseconds, source);
    return 0;
}
