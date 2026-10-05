/* Подключаем неизменённый исходник как отдельную единицу трансляции.
   Исключаем демонстрационный main и даём решателю уникальное имя.
   Сам алгоритм и его счётчики остаются исходными. */
#include "../bench_api.h"
#define REBUS_LIBRARY 1
#define UNIQUE_DIGITS 1
#define solve_rebus opt1_solve_internal
#include "../versions/RebusSolver_opt1.c"
#undef solve_rebus

BENCH_NOINLINE int bench_opt1(const char *input, char *output,
    size_t capacity, BenchStats *stats)
{
    SearchStats original = { 0, 0 };
    char *answer = opt1_solve_internal(input, output, capacity, &original);
    if (stats != NULL) {
        stats->nodes = original.nodes;
        stats->complete_assignments = original.complete_assignments;
    }
    return answer != NULL;
}
