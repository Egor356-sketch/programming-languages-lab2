/* Повторные измерения пяти сохранённых версий решателя.
   Каждый алгоритм компилируется отдельно; main.c рабочего проекта не меняется.
   Время: полный вызов solve_rebus, подготовка данных и счётчики включены.
   Печать, независимая проверка ответа и запись файлов выполняются вне замера. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "bench_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <process.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

enum { VARIANTS = 5, CASES = 5, ROUNDS = 9, BUFFER_SIZE = 512 };
static const double target_batch_ms = 50.0;
static const unsigned int max_repeats = 1048576U;
static volatile unsigned long long result_sink;
static uint64_t ticks_per_second;
static const char *tags[VARIANTS] = { "baseline", "opt1", "opt2", "opt3", "opt4" };
static BenchSolver solvers[VARIANTS] = {
    bench_baseline, bench_opt1, bench_opt2, bench_opt3, bench_opt4
};
static const char *puzzles[CASES] = {
    "BE + BE = MOO",
    "SEND + MORE = MONEY",
    "BIG + CAT = LION",
    "ELEVEN + NINE + FIVE + FIVE = THIRTY",
    "A + A + A + A + A + A + A = B"
};
typedef struct {
    char answer[BUFFER_SIZE];
    BenchStats stats;
    unsigned int repeats;
    double ms_per_call[ROUNDS];
    double median;
    double minimum;
    double maximum;
} Measurement;
static Measurement measurements[VARIANTS][CASES];

static void fail(const char *message)
{
    fprintf(stderr, "\nBENCHMARK FAILED: %s\n", message);
    fputs("Do not use incomplete results as final measurements.\n", stderr);
    exit(EXIT_FAILURE);
}
static FILE *open_output(const char *folder, const char *filename)
{
    char path[1024];
    FILE *stream = NULL;
    int n = snprintf(path, sizeof(path), "%s/%s", folder, filename);
    if (n < 0 || (size_t)n >= sizeof(path))
        fail("Output path is too long.");
#ifdef _MSC_VER
    if (fopen_s(&stream, path, "w") != 0)
        stream = NULL;
#else
    stream = fopen(path, "w");
#endif
    if (stream == NULL)
        fail("Cannot create results file. Check folder write permission.");
    return stream;
}
static void close_output(FILE *stream)
{
    int error = ferror(stream);
    if (fclose(stream) != 0 || error)
        fail("Could not finish writing a results file.");
}
static uint64_t ticks_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter))
        fail("QueryPerformanceCounter failed.");
    return (uint64_t)counter.QuadPart;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        fail("clock_gettime failed.");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}
static void initialize_timer(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        fail("High resolution timer is unavailable.");
    ticks_per_second = (uint64_t)frequency.QuadPart;
#else
    ticks_per_second = UINT64_C(1000000000);
#endif
}
static double ticks_to_ms(uint64_t ticks)
{
    return 1000.0 * (double)ticks / (double)ticks_per_second;
}

/* Независимый валидатор для коротких пяти тестовых ребусов стенда.
   Не использует код решателей. У проверяемых тестов числа помещаются в ULL;
   для произвольных более длинных чисел этот валидатор не предназначен. */
static void whitespace(const char **at)
{
    while (isspace((unsigned char)**at))
        ++*at;
}
static int read_number(const char **at, unsigned long long *value)
{
    const char *start;
    whitespace(at);
    start = *at;
    *value = 0;
    if (**at < '0' || **at > '9')
        return 0;
    while (**at >= '0' && **at <= '9') {
        unsigned int digit = (unsigned int)(**at - '0');
        if (*value > (ULLONG_MAX - digit) / 10)
            return 0;
        *value = *value * 10 + digit;
        ++*at;
    }
    return !(*at - start > 1 && *start == '0');
}
static int valid_answer(const char *input, const char *output)
{
    int mapping[26], owner[10];
    size_t input_length = strlen(input), length = 0;
    const char *cursor = output;
    unsigned long long total = 0, value = 0;
    int terms = 0;
    for (int i = 0; i < 26; ++i) mapping[i] = -1;
    for (int i = 0; i < 10; ++i) owner[i] = -1;
    while (length < BUFFER_SIZE && output[length] != '\0') ++length;
    if (length == BUFFER_SIZE || length != input_length)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        if (input[i] >= 'A' && input[i] <= 'Z') {
            int id = input[i] - 'A';
            int digit = output[i] - '0';
            if (digit < 0 || digit > 9 ||
                (mapping[id] >= 0 && mapping[id] != digit) ||
                (owner[digit] >= 0 && owner[digit] != id))
                return 0;
            mapping[id] = digit;
            owner[digit] = id;
        }
        else if (input[i] != output[i]) return 0;
    }
    for (;;) {
        if (!read_number(&cursor, &value) || total > ULLONG_MAX - value)
            return 0;
        total += value;
        ++terms;
        whitespace(&cursor);
        if (*cursor == '=') { ++cursor; break; }
        if (*cursor != '+' || terms >= 7) return 0;
        ++cursor;
    }
    if (terms < 2 || !read_number(&cursor, &value)) return 0;
    whitespace(&cursor);
    return *cursor == '\0' && value == total;
}

/* Таймер вызывается только до и после пакета, не при каждом решении.
   Накладные расходы цикла, адаптера и чтения результатов не вычитаются;
   одинаковая схема измерения применяется ко всем пяти алгоритмам. */
static uint64_t run_batch(int version, int test, unsigned int repeats)
{
    char output[BUFFER_SIZE];
    BenchStats stats = { 0, 0 };
    unsigned long long checksum = 0;
    Measurement *record = &measurements[version][test];
    BenchSolver solve = solvers[version];
    uint64_t begin = ticks_now(), end;
    int successful = 1;
    for (unsigned int r = 0; r < repeats; ++r) {
        if (!solve(puzzles[test], output, sizeof(output), &stats)) {
            successful = 0;
            break;
        }
        checksum += stats.nodes + stats.complete_assignments + (unsigned char)output[0];
    }
    end = ticks_now();
    result_sink ^= checksum;
    if (!successful || end < begin)
        fail("Solver or timer failure inside batch.");
    if (!valid_answer(puzzles[test], output) || strcmp(output, record->answer) != 0 ||
        stats.nodes != record->stats.nodes ||
        stats.complete_assignments != record->stats.complete_assignments)
        fail("Solution or counters changed within the same algorithm.");
    return end - begin;
}

static void make_directory(const char *path, int allow_existing)
{
#ifdef _WIN32
    int status = _mkdir(path);
#else
    int status = mkdir(path, 0777);
#endif
    if (status != 0 && !(allow_existing && errno == EEXIST))
        fail("Cannot create a new results directory.");
}
static void make_run_folder(char *folder, size_t capacity)
{
    char stamp[64];
    time_t current = time(NULL);
    struct tm utc;
    long pid;
    if (current == (time_t)-1) fail("Cannot read system date.");
#ifdef _WIN32
    if (gmtime_s(&utc, &current) != 0) fail("Cannot convert system date.");
    pid = (long)_getpid();
#else
    if (gmtime_r(&current, &utc) == NULL) fail("Cannot convert system date.");
    pid = (long)getpid();
#endif
    if (!strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S_UTC", &utc))
        fail("Cannot format date.");
    int n = snprintf(folder, capacity, "results/run_%s_%ld", stamp, pid);
    if (n < 0 || (size_t)n >= capacity) fail("Result directory path is too long.");
    make_directory("results", 1);
    make_directory(folder, 0);
}
static void metadata(FILE *out)
{
    fprintf(out, "RebusBenchmark protocol version: 1\n");
    fprintf(out, "Algorithms: baseline_v2, opt1, opt2, opt3, opt4\n");
    fprintf(out, "Cases: %d; series per algorithm/case: %d\n", CASES, ROUNDS);
    fprintf(out, "UNIQUE_DIGITS=1 in all wrappers\n");
    fprintf(out, "Calibration target per batch: %.1f ms; max repetitions: %u\n",
        target_batch_ms, max_repeats);
    fprintf(out, "Pointer bits: %u\n", (unsigned int)(sizeof(void *) * CHAR_BIT));
    fprintf(out, "Timer frequency: %llu ticks/second\n", (unsigned long long)ticks_per_second);
    fprintf(out, "Compiled: %s %s\n", __DATE__, __TIME__);
#ifdef _MSC_VER
    fprintf(out, "Compiler: MSVC _MSC_VER=%d _MSC_FULL_VER=%d\n", _MSC_VER, _MSC_FULL_VER);
#elif defined(__clang__)
    fprintf(out, "Compiler: Clang %s (not the user's MSVC measurement)\n", __clang_version__);
#elif defined(__GNUC__)
    fprintf(out, "Compiler: GCC %s (not the user's MSVC measurement)\n", __VERSION__);
#endif
#ifdef _WIN32
    SYSTEM_INFO info;
    MEMORYSTATUSEX memory = { 0 };
    SYSTEM_POWER_STATUS power;
    GetNativeSystemInfo(&info);
    fprintf(out, "OS family: Windows; timer: QueryPerformanceCounter\n");
    fprintf(out, "Logical processors: %lu\n", (unsigned long)info.dwNumberOfProcessors);
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        fprintf(out, "Physical RAM bytes: %llu\n", (unsigned long long)memory.ullTotalPhys);
    if (GetSystemPowerStatus(&power))
        fprintf(out, "AC line: %u (0 battery, 1 connected, 255 unknown)\n", (unsigned int)power.ACLineStatus);
#ifdef _MSC_VER
#if defined(_M_X64) || defined(_M_IX86)
    int data[4];
    char brand[49] = { 0 };
    __cpuid(data, (int)0x80000000U);
    if ((unsigned int)data[0] >= 0x80000004U) {
        for (unsigned int part = 0; part < 3; ++part) {
            __cpuid(data, (int)(0x80000002U + part));
            memcpy(brand + part * 16, data, 16);
        }
        fprintf(out, "CPU: %s\n", brand);
    }
#endif
#endif
#else
    fputs("OS: POSIX test environment; timer: CLOCK_MONOTONIC\n", out);
#endif
    fputs("Intended VS configuration: Release|x64, v143, C11, /O2 /utf-8 /MD /GS, no /GL.\n", out);
    fputs("All solvers compiled as separate translation units with the same flags.\n", out);
    fputs("One warm-up per pair; calibration excluded; 9 measured series.\n", out);
    fputs("Version and case order rotates between series; execution is single-threaded.\n", out);
    fputs("Each sample is batch elapsed time / repetitions, not a single-call timestamp.\n", out);
    fputs("Summary: median, min and max of the 9 batch-normalized times.\n", out);
    fputs("Timed: parser, plan, search, counters, answer creation, wrapper and loop.\n", out);
    fputs("Not timed: independent validation, printing and file I/O. No empty-loop subtraction.\n", out);
    fputs("Counter nodes have different per-node work in opt4; do not equate node ratios with speed.\n", out);
    fputs("Different correct answers between versions are allowed; fixed test input per case.\n", out);
    fputs("Sources: exact copies of chat attachments, see sources_manifest.json.\n", out);
    fputs("Commit IDs in manifest are references from screenshots, not verified byte hashes of Git objects.\n", out);
}
static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}
static void calculate_statistics(void)
{
    for (int v = 0; v < VARIANTS; ++v) {
        for (int t = 0; t < CASES; ++t) {
            Measurement *m = &measurements[v][t];
            double sorted[ROUNDS];
            memcpy(sorted, m->ms_per_call, sizeof(sorted));
            qsort(sorted, ROUNDS, sizeof(sorted[0]), compare_double);
            m->minimum = sorted[0];
            m->median = sorted[ROUNDS / 2];
            m->maximum = sorted[ROUNDS - 1];
        }
    }
}
static void print_table(FILE *out)
{
    fputs("Median ms per solve (median of 9 batch averages)\n", out);
    fprintf(out, "%-6s %12s %12s %12s %12s %12s\n",
        "Case", "baseline", "opt1", "opt2", "opt3", "opt4");
    for (int t = 0; t < CASES; ++t) {
        fprintf(out, "T%-5d", t + 1);
        for (int v = 0; v < VARIANTS; ++v)
            fprintf(out, " %12.6f", measurements[v][t].median);
        fputc('\n', out);
    }
    fputc('\n', out);
    for (int t = 0; t < CASES; ++t)
        fprintf(out, "T%d: %s\n", t + 1, puzzles[t]);
}
int main(void)
{
    char folder[256];
    FILE *raw, *summary, *report, *info, *solutions;
#ifdef _DEBUG
    fail("Debug build. Select Release / x64 and rebuild the benchmark solution.");
#endif
#ifdef _WIN32
#ifndef _WIN64
    fail("Select x64, not Win32, and rebuild.");
#endif
    if (IsDebuggerPresent())
        fail("Debugger is attached. Use Debug -> Start Without Debugging (Ctrl+F5).");
#endif
    initialize_timer();
    make_run_folder(folder, sizeof(folder));
    info = open_output(folder, "environment.txt");
    metadata(info);
    close_output(info);
    puts("RebusBenchmark: five algorithms, repeated measurements");
    printf("Results folder: %s\n", folder);
    puts("Phase 1/3: validating answers, warming up and calibrating batches...");
    fflush(stdout);
    solutions = open_output(folder, "solutions.txt");
    for (int t = 0; t < CASES; ++t) {
        for (int v = 0; v < VARIANTS; ++v) {
            Measurement *m = &measurements[v][t];
            if (!solvers[v](puzzles[t], m->answer, sizeof(m->answer), &m->stats) ||
                !valid_answer(puzzles[t], m->answer))
                fail("Initial independent correctness check failed.");
            fprintf(solutions, "%s T%d\nInput: %s\nSolution: %s\nNodes: %llu\nComplete assignments: %llu\n\n",
                tags[v], t + 1, puzzles[t], m->answer, m->stats.nodes, m->stats.complete_assignments);
            m->repeats = 1;
            uint64_t elapsed;
            for (;;) {
                elapsed = run_batch(v, t, m->repeats);
                if (ticks_to_ms(elapsed) >= target_batch_ms || m->repeats >= max_repeats)
                    break;
                m->repeats *= 2;
            }
            printf("  %-8s T%d: valid, batch=%u solves (calibration %.1f ms)\n",
                tags[v], t + 1, m->repeats, ticks_to_ms(elapsed));
            fflush(stdout);
        }
    }
    close_output(solutions);
    puts("Initial independent correctness checks: 25/25");
    puts("Phase 2/3: nine measurement series. Please leave the computer idle.");
    raw = open_output(folder, "raw.csv");
    fputs("series,sequence,version,test_id,repetitions,batch_ticks,timer_hz,batch_ms,ms_per_solve,nodes_per_solve,assignments_per_solve\n", raw);
    int sequence = 0;
    for (int round = 0; round < ROUNDS; ++round) {
        for (int ti = 0; ti < CASES; ++ti) {
            int t = (ti + round) % CASES;
            for (int vi = 0; vi < VARIANTS; ++vi) {
                int v = (vi + round + t) % VARIANTS;
                Measurement *m = &measurements[v][t];
                uint64_t elapsed = run_batch(v, t, m->repeats);
                double total_ms = ticks_to_ms(elapsed);
                if (elapsed == 0) fail("Zero timer interval; results are not usable.");
                m->ms_per_call[round] = total_ms / m->repeats;
                fprintf(raw, "%d,%d,%s,T%d,%u,%llu,%llu,%.9f,%.9f,%llu,%llu\n",
                    round + 1, ++sequence, tags[v], t + 1, m->repeats,
                    (unsigned long long)elapsed, (unsigned long long)ticks_per_second,
                    total_ms, m->ms_per_call[round], m->stats.nodes, m->stats.complete_assignments);
            }
        }
        if (fflush(raw) != 0) fail("Could not flush raw results.");
        printf("  Series %d/%d complete.\n", round + 1, ROUNDS);
        fflush(stdout);
    }
    close_output(raw);
    puts("Phase 3/3: writing summary...");
    calculate_statistics();
    summary = open_output(folder, "summary.csv");
    fputs("version,test_id,repetitions,series_count,median_ms,min_ms,max_ms,speedup_vs_baseline,nodes,complete_assignments\n", summary);
    for (int t = 0; t < CASES; ++t) {
        for (int v = 0; v < VARIANTS; ++v) {
            Measurement *m = &measurements[v][t];
            fprintf(summary, "%s,T%d,%u,%d,%.9f,%.9f,%.9f,%.6f,%llu,%llu\n",
                tags[v], t + 1, m->repeats, ROUNDS, m->median, m->minimum, m->maximum,
                measurements[0][t].median / m->median, m->stats.nodes, m->stats.complete_assignments);
        }
    }
    close_output(summary);
    report = open_output(folder, "summary.txt");
    print_table(report);
    fputs("\nTimes belong to this local run only. Do not mix them with earlier single-run times.\n", report);
    fprintf(report, "\nBENCHMARK COMPLETE\nChecksum: %llu\n", result_sink);
    close_output(report);
    print_table(stdout);
    printf("\nBENCHMARK COMPLETE\nResults folder: %s\n", folder);
    puts("Send the entire new run folder (raw.csv, summary.csv, summary.txt, environment.txt, solutions.txt).");
    return EXIT_SUCCESS;
}
