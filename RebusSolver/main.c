#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Ограничения реализации: латинские A..Z, цифры 0..9, сложение.
   Длина входа не больше 511 байт, включая пробелы, но без '\0'. */
enum { TEXT_LIMIT = 512, TERM_LIMIT = 8, LETTER_LIMIT = 26 };

/* Рабочее допущение: разные буквы обозначают разные цифры.
   В методичке это отдельно не уточнено. Значение 0 разрешает совпадения.
   Уже записанные в выражении цифры не занимают места в таблице букв.
   При сравнении версий значение этой настройки менять нельзя. */
#ifndef UNIQUE_DIGITS
#define UNIQUE_DIGITS 1
#endif

   /* Вместо копий слов сохраняем их положение в исходной строке. */
typedef struct {
    size_t begin;
    size_t length;
} Term;

typedef struct {
    char text[TEXT_LIMIT];
    size_t length;
    Term terms[TERM_LIMIT];
    int term_count;
    int index_of_letter[LETTER_LIMIT];
    int letter_count;
} Equation;

typedef struct {
    unsigned long long nodes;
    unsigned long long complete_assignments;
} SearchStats;

static int letter_id(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch - 'A' : -1;
}

static int number_symbol(char ch)
{
    return letter_id(ch) >= 0 || (ch >= '0' && ch <= '9');
}

static void skip_spaces(const char* text, size_t* at)
{
    while (isspace((unsigned char)text[*at]))
        ++*at;
}

/* Читаем одно слово-число и регистрируем впервые встретившиеся буквы. */
static int read_term(Equation* eq, size_t* at)
{
    size_t begin;
    skip_spaces(eq->text, at);
    begin = *at;
    if (eq->term_count == TERM_LIMIT || !number_symbol(eq->text[*at]))
        return 0;

    while (number_symbol(eq->text[*at])) {
        int id = letter_id(eq->text[*at]);
        if (id >= 0 && eq->index_of_letter[id] < 0)
            eq->index_of_letter[id] = eq->letter_count++;
        ++*at;
    }

    eq->terms[eq->term_count].begin = begin;
    eq->terms[eq->term_count].length = *at - begin;
    ++eq->term_count;
    skip_spaces(eq->text, at);
    return 1;
}

/* Сначала читаем 2..7 слагаемых, затем единственную правую часть. */
static int prepare_equation(const char* input, Equation* eq)
{
    size_t at = 0;
    memset(eq, 0, sizeof(*eq));
    for (int id = 0; id < LETTER_LIMIT; ++id)
        eq->index_of_letter[id] = -1;

    while (eq->length < TEXT_LIMIT && input[eq->length] != '\0')
        ++eq->length;
    if (eq->length == TEXT_LIMIT)
        return 0;
    memcpy(eq->text, input, eq->length + 1);

    for (;;) {
        if (!read_term(eq, &at))
            return 0;
        if (eq->text[at] == '=')
            break;
        if (eq->text[at] != '+' || eq->term_count == TERM_LIMIT - 1)
            return 0;
        ++at;
    }
    if (eq->term_count < 2)
        return 0;
    ++at;
    if (!read_term(eq, &at) || eq->text[at] != '\0')
        return 0;
    return !UNIQUE_DIGITS || eq->letter_count <= 10;
}

static int read_digit(const Equation* eq, const int digits[], size_t at)
{
    int id = letter_id(eq->text[at]);
    if (id < 0)
        return eq->text[at] - '0';
    return digits[eq->index_of_letter[id]];
}

/* Проверяем только ПОЛНОЕ назначение. Раннего отсечения пока нет.
   Длинные числа не переводим в int: складываем их десятичные цифры.
   Элемент sum[0] хранит единицы, sum[1] десятки и так далее. */
static int check_assignment(const Equation* eq, const int digits[])
{
    unsigned char sum[TEXT_LIMIT + 1] = { 0 };
    size_t width = 1;
    const Term* right = &eq->terms[eq->term_count - 1];

    for (int t = 0; t < eq->term_count; ++t) {
        const Term* word = &eq->terms[t];
        if (word->length > 1 && read_digit(eq, digits, word->begin) == 0)
            return 0;
    }

    /* По очереди прибавляем каждое слагаемое к накопленной сумме. */
    for (int t = 0; t < eq->term_count - 1; ++t) {
        const Term* word = &eq->terms[t];
        size_t column = 0;
        int carry = 0;
        while (column < word->length || carry != 0) {
            int next = (int)sum[column] + carry;
            if (column < word->length) {
                size_t at = word->begin + word->length - 1 - column;
                next += read_digit(eq, digits, at);
            }
            sum[column] = (unsigned char)(next % 10);
            carry = next / 10;
            ++column;
        }
        if (column > width)
            width = column;
    }
    while (width > 1 && sum[width - 1] == 0)
        --width;
    if (width != right->length)
        return 0;

    for (size_t column = 0; column < width; ++column) {
        size_t at = right->begin + right->length - 1 - column;
        if ((int)sum[column] != read_digit(eq, digits, at))
            return 0;
    }
    return 1;
}

/* Перебор с возвратом без рекурсивных вызовов.
   depth: какой букве сейчас назначается цифра.
   next_digit[depth]: с какой цифры продолжить после неудачного варианта.
   Занятую цифру пропускаем только при UNIQUE_DIGITS == 1. */
static int enumerate_assignments(const Equation* eq, int digits[],
    SearchStats* stats)
{
    int occupied[10] = { 0 };
    int next_digit[LETTER_LIMIT] = { 0 };
    int depth = 0;
    for (int i = 0; i < LETTER_LIMIT; ++i)
        digits[i] = -1;
    stats->nodes = 1;  /* Корень поиска: ещё ни одной назначенной буквы. */

    while (depth >= 0) {
        if (depth == eq->letter_count) {
            ++stats->complete_assignments;
            if (check_assignment(eq, digits))
                return 1;
            --depth;
        }
        else {
            int candidate = next_digit[depth];
            while (candidate < 10 && UNIQUE_DIGITS && occupied[candidate])
                ++candidate;

            if (candidate < 10) {
                digits[depth] = candidate;
                next_digit[depth] = candidate + 1;
                if (UNIQUE_DIGITS)
                    occupied[candidate] = 1;
                ++stats->nodes;
                ++depth;
                if (depth < eq->letter_count)
                    next_digit[depth] = 0;
                continue;
            }
            /* Цифры для текущей буквы закончились: возвращаемся назад. */
            next_digit[depth] = 0;
            --depth;
        }

        /* Отменяем назначение предыдущей буквы, чтобы попробовать следующее. */
        if (depth >= 0) {
            if (UNIQUE_DIGITS)
                occupied[digits[depth]] = 0;
            digits[depth] = -1;
        }
    }
    return 0;
}

static void write_answer(const Equation* eq, const int digits[], char* output)
{
    memcpy(output, eq->text, eq->length + 1);
    for (size_t at = 0; at < eq->length; ++at) {
        if (letter_id(eq->text[at]) >= 0)
            output[at] = (char)('0' + read_digit(eq, digits, at));
    }
}

/* Возвращаем строку решения в переданном буфере либо NULL.
   Буфер принадлежит вызывающей стороне; освобождать результат не надо.
   input и output могут указывать на один буфер. stats может быть NULL.
   При ошибке входа/буфера либо отсутствии решения результат пустой,
   если output != NULL и capacity > 0. */
char* solve_rebus(const char* input, char* output, size_t capacity,
    SearchStats* stats)
{
    Equation eq;
    int digits[LETTER_LIMIT];
    SearchStats counters = { 0, 0 };
    int ready;

    if (stats != NULL)
        *stats = counters;
    if (output == NULL || capacity == 0)
        return NULL;

    /* Сначала копируем вход, затем очищаем выход: это допускает input==output. */
    ready = input != NULL && prepare_equation(input, &eq);
    output[0] = '\0';
    if (!ready || capacity <= eq.length)
        return NULL;

    ready = enumerate_assignments(&eq, digits, &counters);
    if (stats != NULL)
        *stats = counters;
    if (!ready)
        return NULL;
    write_answer(&eq, digits, output);
    return output;
}

#ifndef REBUS_LIBRARY
/* Режим проверки: печать выполняется вне измеряемого участка.
   Одиночный замер нужен для пробного запуска, а не итогового сравнения. */
static int show_case(const char* input)
{
    char answer[TEXT_LIMIT];
    SearchStats counters;
    char* result;
#ifdef _WIN32
    LARGE_INTEGER frequency = { 0 }, start = { 0 }, stop = { 0 };
    int have_timer = QueryPerformanceFrequency(&frequency)
        && frequency.QuadPart > 0;
#endif

    printf("Input: %s\n", input);
    fflush(stdout);
#ifdef _WIN32
    if (have_timer)
        have_timer = QueryPerformanceCounter(&start);
#endif
    result = solve_rebus(input, answer, sizeof(answer), &counters);
#ifdef _WIN32
    if (have_timer)
        have_timer = QueryPerformanceCounter(&stop);
#endif

    if (result != NULL)
        printf("Solution: %s\n", result);
    else
        puts("No solution, or unsupported input/buffer.");
    printf("Complete assignments: %llu\n", counters.complete_assignments);
    printf("Search nodes: %llu\n", counters.nodes);
#ifdef _WIN32
    if (have_timer) {
        double ms = 1000.0 * (double)(stop.QuadPart - start.QuadPart)
            / (double)frequency.QuadPart;
        printf("Time (single run): %.3f ms\n", ms);
    }
    else {
        puts("Time: timer unavailable.");
    }
#endif
    putchar('\n');
    return result != NULL;
}

int main(int argc, char* argv[])
{
    const char* examples[] = {
        "BE + BE = MOO",
        "SEND + MORE = MONEY",
        "BIG + CAT = LION",
        "ELEVEN + NINE + FIVE + FIVE = THIRTY",
        "A + A + A + A + A + A + A = B"
    };
    int solved = 0;
    int total = (int)(sizeof(examples) / sizeof(examples[0]));

    puts("RebusSolver: baseline v2, iterative assignment search");
    printf("UNIQUE_DIGITS = %d\n\n", UNIQUE_DIGITS);
    if (argc == 2)
        return show_case(argv[1]) ? 0 : 1;
    if (argc != 1) {
        puts("Usage: RebusSolver.exe \"SEND + MORE = MONEY\"");
        return 1;
    }
    for (int test = 0; test < total; ++test)
        solved += show_case(examples[test]);
    printf("Solved: %d/%d\n", solved, total);
    return solved == total ? 0 : 1;
}
#endif
