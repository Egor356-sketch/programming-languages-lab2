#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Ограничения реализации: латинские A..Z, цифры 0..9, сложение.
   Длина входа не больше 511 байт, включая пробелы, но без '\0'. */
enum { TEXT_LIMIT = 512, TERM_LIMIT = 8, LETTER_LIMIT = 26 };
/* Первые ячейки содержат значения букв, последние десять: константы 0..9. */
enum { VALUE_LIMIT = LETTER_LIMIT + 10 };

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

/* Оптимизация 3: постоянная схема сложения готовится один раз.
   В разряде храним индексы значений, а не позиции символов в строке.
   Индексы 0..25 относятся к буквам, 26..35 к фиксированным цифрам.
   Все индексы и количество слагаемых разряда помещаются в unsigned char. */
typedef struct {
    unsigned char addend_index[TERM_LIMIT - 1];
    unsigned char addend_count;
    unsigned char result_index;
} SumColumn;

typedef struct {
    SumColumn columns[TEXT_LIMIT];
    size_t width;
    int leading_index[TERM_LIMIT];
    int leading_count;
} SumPlan;

typedef struct {
    /* Узлы: корень + назначения очередной буквы, включая вынужденные.
       Полные подстановки: назначения всем буквам, переданные в check_assignment.
       Порядок поиска отличается от версий 0..3; первый ответ может измениться. */
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

/* Переводим символ в индекс таблицы значений только при подготовке. */
static int value_index(const Equation* eq, size_t at)
{
    int id = letter_id(eq->text[at]);
    return id >= 0 ? eq->index_of_letter[id]
        : LETTER_LIMIT + (eq->text[at] - '0');
}

/* Вход уже разобран prepare_equation: слова непустые, символы допустимы.
   До начала перебора вычисляем ширину, состав каждого разряда и список
   первых символов многозначных чисел. Готовый план не зависит от цифр букв. */
static void prepare_sum_plan(const Equation* eq, SumPlan* plan)
{
    const Term* right = &eq->terms[eq->term_count - 1];
    plan->width = 0;
    plan->leading_count = 0;
    for (int t = 0; t < eq->term_count; ++t) {
        const Term* word = &eq->terms[t];
        if (word->length > plan->width)
            plan->width = word->length;
        if (word->length > 1)
            plan->leading_index[plan->leading_count++] = value_index(eq, word->begin);
    }

    for (size_t column = 0; column < plan->width; ++column) {
        SumColumn* prepared = &plan->columns[column];
        prepared->addend_count = 0;
        for (int t = 0; t < eq->term_count - 1; ++t) {
            const Term* word = &eq->terms[t];
            if (column < word->length) {
                size_t at = word->begin + word->length - 1 - column;
                prepared->addend_index[prepared->addend_count++] =
                    (unsigned char)value_index(eq, at);
            }
        }
        /* Если разряда справа нет, сравниваем с фиксированным нулём. */
        prepared->result_index = LETTER_LIMIT;
        if (column < right->length) {
            size_t at = right->begin + right->length - 1 - column;
            prepared->result_index = (unsigned char)value_index(eq, at);
        }
    }
}

/* Сохраняем оптимизацию 2: проверяем ПОЛНОЕ назначение по разрядам
   и выходим при первом несовпадении. Теперь используем готовую схему:
   не пересчитываем позиции символов, длины и индексы букв на каждом варианте. */
static int check_assignment(const SumPlan* plan, const int digits[])
{
    int carry = 0;
    /* Дополнительный контроль ведущих нулей, включая фиксированные цифры. */
    for (int t = 0; t < plan->leading_count; ++t) {
        if (digits[plan->leading_index[t]] == 0)
            return 0;
    }

    for (size_t column = 0; column < plan->width; ++column) {
        const SumColumn* prepared = &plan->columns[column];
        int column_sum = carry;
        for (int t = 0; t < prepared->addend_count; ++t)
            column_sum += digits[prepared->addend_index[t]];
        if (column_sum % 10 != digits[prepared->result_index])
            return 0;
        carry = column_sum / 10;
    }
    /* При семи слагаемых column_sum <= 69, перенос от 0 до 6.
       Ненулевой перенос после последнего разряда означает лишнюю цифру. */
    return carry == 0;
}

/* Оптимизация 4: назначаем цифры по ходу сложения, начиная с единиц.
   Неправильный разряд отсекает ветвь до назначения остальных букв.
   Все изменяемые данные одного поиска находятся в этом контексте. */
typedef struct {
    const SumPlan* plan;
    int* digits;
    int letter_count;
    int occupied[10];
    int nonzero[LETTER_LIMIT];
    SearchStats* stats;
} ColumnSearch;

/* column: текущий разряд; term: следующее слагаемое в этом разряде;
   subtotal: перенос из младшего разряда плюс уже учтённые цифры;
   assigned: количество букв, которым сейчас назначены цифры.
   Рекурсивный вызов делаем только при назначении НОВОЙ буквы.
   Известные цифры и готовые разряды обрабатываем циклами.
   Поэтому глубина рекурсии ограничена количеством разных букв + 1,
   а не длиной входных чисел. */
static int search_columns(ColumnSearch* search, size_t column, int term,
    int subtotal, int assigned)
{
    ++search->stats->nodes;

    /* Полную подстановку считаем один раз и независимо проверяем целиком.
       Проверка из оптимизаций 2 и 3 остаётся контрольной проверкой ответа. */
    if (assigned == search->letter_count) {
        ++search->stats->complete_assignments;
        return check_assignment(search->plan, search->digits);
    }

    while (column < search->plan->width) {
        const SumColumn* prepared = &search->plan->columns[column];
        while (term < prepared->addend_count) {
            int index = prepared->addend_index[term];
            int digit = search->digits[index];
            if (digit >= 0) {
                subtotal += digit;
                ++term;
                continue;
            }

            /* Буква слагаемого ещё неизвестна: пробуем допустимые цифры.
               Повторные вхождения этой буквы используют то же назначение. */
            for (int candidate = search->nonzero[index] ? 1 : 0;
                candidate < 10; ++candidate) {
                if (UNIQUE_DIGITS && search->occupied[candidate])
                    continue;
                search->digits[index] = candidate;
                if (UNIQUE_DIGITS)
                    search->occupied[candidate] = 1;

                if (search_columns(search, column, term + 1,
                    subtotal + candidate, assigned + 1))
                    return 1;

                /* Ветвь не подошла: освобождаем назначение этой буквы. */
                if (UNIQUE_DIGITS)
                    search->occupied[candidate] = 0;
                search->digits[index] = -1;
            }
            return 0;
        }

        /* Все слагаемые текущего разряда известны. Цифра результата
           однозначно задаётся остатком от деления суммы на 10. */
        int result_index = prepared->result_index;
        int required = subtotal % 10;
        int actual = search->digits[result_index];
        if (actual >= 0) {
            if (actual != required)
                return 0;
            subtotal /= 10;  /* Перенос в следующий разряд, от 0 до 6. */
            ++column;
            term = 0;
            continue;
        }

        /* Для новой буквы результата не перебираем все десять цифр:
           допустима только required. Ведущий ноль и занятую цифру
           отбрасываем так же, как для букв слагаемых. */
        if ((required == 0 && search->nonzero[result_index]) ||
            (UNIQUE_DIGITS && search->occupied[required]))
            return 0;
        search->digits[result_index] = required;
        if (UNIQUE_DIGITS)
            search->occupied[required] = 1;

        if (search_columns(search, column + 1, 0, subtotal / 10, assigned + 1))
            return 1;

        if (UNIQUE_DIGITS)
            search->occupied[required] = 0;
        search->digits[result_index] = -1;
        return 0;
    }

    /* Каждая буква есть хотя бы в одном разряде. Успех возможен только
       после полного назначения и контрольной проверки в начале функции. */
    return 0;
}

static int enumerate_assignments(const Equation* eq, int digits[],
    SearchStats* stats)
{
    SumPlan plan;
    ColumnSearch search = { 0 };

    prepare_sum_plan(eq, &plan);
    search.plan = &plan;
    search.digits = digits;
    search.letter_count = eq->letter_count;
    search.stats = stats;
    for (int i = 0; i < LETTER_LIMIT; ++i)
        digits[i] = -1;
    for (int digit = 0; digit < 10; ++digit)
        digits[LETTER_LIMIT + digit] = digit;

    /* Сохраняем ранний запрет ведущих нулей из оптимизации 1.
       Фиксированный ведущий ноль недопустим при любой подстановке. */
    for (int t = 0; t < plan.leading_count; ++t) {
        int index = plan.leading_index[t];
        if (index < LETTER_LIMIT)
            search.nonzero[index] = 1;
        else if (digits[index] == 0)
            return 0;
    }
    return search_columns(&search, 0, 0, 0, 0);
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
    int digits[VALUE_LIMIT];
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

    puts("RebusSolver: optimization 4, column-guided search with carries");
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
