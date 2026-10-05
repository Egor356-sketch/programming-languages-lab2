#ifndef REBUS_BENCH_API_H
#define REBUS_BENCH_API_H
#include <stddef.h>

#if defined(_MSC_VER)
#define BENCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define BENCH_NOINLINE __attribute__((noinline))
#else
#define BENCH_NOINLINE
#endif

typedef struct {
    unsigned long long nodes;
    unsigned long long complete_assignments;
} BenchStats;

typedef int (*BenchSolver)(const char *, char *, size_t, BenchStats *);
int bench_baseline(const char *, char *, size_t, BenchStats *);
int bench_opt1(const char *, char *, size_t, BenchStats *);
int bench_opt2(const char *, char *, size_t, BenchStats *);
int bench_opt3(const char *, char *, size_t, BenchStats *);
int bench_opt4(const char *, char *, size_t, BenchStats *);
#endif
