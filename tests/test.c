/* gram test suite (hosted). */
#include <stdio.h>
#include <string.h>

#include "../gram.h"
#include "tiny_grammar.h"   /* generated from tests/tiny.gram into build/ */

static unsigned char mem[64 * 1024];
static gram_arena arena;
static gram_error err;
static char out[4096];
static size_t outn;
static int fails, checks;

static void put(int c, void *ctx)
{
    (void)ctx;
    if (outn + 1 < sizeof out) out[outn++] = (char)c;
    out[outn] = 0;
}

#define CHECK(cond) \
    do { checks++; if (!(cond)) { fails++; \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* compile + parse, return the dumped tree ("" on failure) */
static const char *tree(const char *grammar, const char *input)
{
    gram_grammar *g;
    gram_node *n;
    outn = 0;
    out[0] = 0;
    gram_arena_init(&arena, mem, sizeof mem);
    g = gram_compile(&arena, grammar, &err);
    if (!g) return "";
    n = gram_parse(g, NULL, input, strlen(input), &arena, &err);
    gram_dump(n, put, NULL);
    return out;
}

static const char *errtext(void)
{
    outn = 0;
    out[0] = 0;
    gram_error_print(&err, put, NULL);
    return out;
}

#define CALC \
    "expr <^ term (('+' / '-') term)* ;" \
    "term <^ factor (('*' / '/') factor)* ;" \
    "factor <^ number / ~'(' expr ~')' ;" \
    "number <= [0-9]+ ;"

int main(void)
{
    /* --- trees ---------------------------------------------------------- */
    CHECK(strcmp(tree(CALC, "42"), "number '42'\n") == 0);
    CHECK(strcmp(tree(CALC, " 1+2 * 3 "),
                 "expr\n"
                 "  number '1'\n"
                 "  '+'\n"
                 "  term\n"
                 "    number '2'\n"
                 "    '*'\n"
                 "    number '3'\n") == 0);
    CHECK(strcmp(tree(CALC, "((7))"), "number '7'\n") == 0);

    /* `<-` always keeps its node; span covers the matched text */
    CHECK(strcmp(tree("list <- ~'[' item* ~']' ; item <= [a-z]+ ;", "[ ab  c ]"),
                 "list\n  item 'ab'\n  item 'c'\n") == 0);
    CHECK(strcmp(tree("list <- ~'[' item* ~']' ; item <= [a-z]+ ;", "[]"),
                 "list '[]'\n") == 0);

    /* tokens do not skip inside */
    CHECK(tree("s <- w w ; w <= [a-z]+ ;", "ab cd")[0] != 0);
    CHECK(tree("s <- w ; w <= [a-z] [a-z] ;", "a b")[0] == 0);

    /* case-insensitive literals, escapes, negated classes, any, predicates */
    CHECK(tree("s <- 'print'i n ; n <= [0-9]+ ;", "PrInT 7")[0] != 0);
    CHECK(strcmp(tree("s <= '\"' (!'\"' .)* '\"' ;", "\"a b\""), "s '\"a b\"'\n") == 0);
    CHECK(strcmp(tree("s <= [^,]+ (',' [^,]+)* ;", "a,b"), "s 'a,b'\n") == 0);
    CHECK(tree("s <= '\\x41' [\\t\\-\\]] '\\n' ;", "A-\n")[0] != 0);
    CHECK(tree("s <= &'a' [a-z]+ ;", "abc")[0] != 0);
    CHECK(tree("s <= &'a' [a-z]+ ;", "bcd")[0] == 0);

    /* keywords are not identifiers */
    CHECK(tree("s <- id ; id <= !kw [a-z]+ ; kw <= 'if' ![a-z] ;", "iffy")[0] != 0);
    CHECK(tree("s <- id ; id <= !kw [a-z]+ ; kw <= 'if' ![a-z] ;", "if")[0] == 0);

    /* user-defined skip with comments; `|` works as choice too */
    CHECK(strcmp(tree("s <- n+ ; n <= [0-9]+ ;"
                      "skip <= ([ \\n] | '//' [^\\n]*)* ;",
                      "1 // one\n2"),
                 "s\n  n '1'\n  n '2'\n") == 0);

    /* rules may be used before they are defined; `*` of an empty match ends */
    CHECK(tree("a <- b ; b <= 'x' ;", "x")[0] != 0);
    CHECK(tree("a <- ('x'?)* 'y' ;", "y")[0] != 0);

    /* --- input errors --------------------------------------------------- */
    CHECK(tree(CALC, "1 +\n  * 2")[0] == 0);
    CHECK(err.code == GRAM_E_SYNTAX && err.line == 2 && err.col == 3);
    CHECK(strcmp(errtext(), "2:3: syntax error: expected number or '('\n") == 0);

    CHECK(tree(CALC, "1 2")[0] == 0);
    CHECK(err.line == 1 && err.col == 3);
    CHECK(strstr(errtext(), "end of input") != NULL);

    CHECK(tree(CALC, "")[0] == 0);
    CHECK(err.code == GRAM_E_SYNTAX);

    /* --- grammar errors ------------------------------------------------- */
    CHECK(tree("a <- a 'x' / 'y' ;", "y")[0] == 0);
    CHECK(err.code == GRAM_E_GRAMMAR && strstr(err.msg, "left recursion"));
    CHECK(tree("a <- b 'x' ; b <- 'q'? a ;", "x")[0] == 0);
    CHECK(err.code == GRAM_E_GRAMMAR && strstr(err.msg, "left recursion"));
    CHECK(tree("a <- 'x' a / 'y' ;", "xxy")[0] != 0); /* right recursion ok */

    CHECK(tree("a <- b ;", "x")[0] == 0);
    CHECK(err.code == GRAM_E_GRAMMAR && strcmp(err.msg, "undefined rule") == 0);
    CHECK(err.line == 1 && err.col == 6);
    CHECK(tree("a <- 'x' ;\na <- 'y' ;", "x")[0] == 0);
    CHECK(strcmp(err.msg, "rule defined twice") == 0 && err.line == 2);
    CHECK(tree("a <- 'x' b <- 'y' ;", "x")[0] == 0);
    CHECK(tree("a <- 'x ;", "x")[0] == 0);
    CHECK(tree("a <- [z-a] ;", "x")[0] == 0);
    CHECK(tree("a <- ( 'x' ;", "x")[0] == 0);
    CHECK(tree("# nothing here\n", "x")[0] == 0);
    CHECK(tree("a <- 'x' $ ;", "x")[0] == 0);

    /* nesting inside one rule is bounded (compiler recursion) */
    {
        char deep[3 * GRAM_MAX_NEST + 64];
        int i, n = 0;
        n += sprintf(deep + n, "a <- ");
        for (i = 0; i < GRAM_MAX_NEST + 1; i++) deep[n++] = '(';
        deep[n++] = '.';
        for (i = 0; i < GRAM_MAX_NEST + 1; i++) deep[n++] = ')';
        sprintf(deep + n, " ;");
        CHECK(tree(deep, "x")[0] == 0);
        CHECK(err.code == GRAM_E_GRAMMAR && strstr(err.msg, "nested too deeply"));
        deep[5] = ' ';                 /* one level less is fine */
        deep[n - 1] = ' ';
        CHECK(tree(deep, "x")[0] != 0);
        CHECK(tree("a <- !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!'x' ;", "x")[0] == 0);
        CHECK(tree("a <- 'x'*+? ;", "x")[0] == 0);
        CHECK(tree("a <- 'x'*? ;", "x")[0] != 0);
    }

    /* --- precompiled tables (gramgen) behave like the compiled grammar --- */
    {
        static const char src[] =
            "n = 27; while n != 1 { if n % 2 == 0 { n = n / 2; }"
            " else { n = 3 * n + 1; } } print -(n);";
        static char first[sizeof out];
        gram_grammar *g;
        gram_node *n;

        CHECK(tree(TINY_GRAMMAR, src)[0] != 0);
        strcpy(first, out);
        CHECK(strlen(first) > 200);

        gram_arena_init(&arena, mem, sizeof mem);
        outn = 0;
        n = gram_parse(&tiny_grammar, "program", src, strlen(src), &arena, &err);
        gram_dump(n, put, NULL);
        CHECK(strcmp(first, out) == 0);
        CHECK(gram_rule_count(&tiny_grammar) == TINY_R_COUNT);
        CHECK(gram_rule_id(&tiny_grammar, "while") == TINY_R_while);

        /* same error from both */
        CHECK(gram_parse(&tiny_grammar, NULL, "x = ;", 5, &arena, &err) == NULL);
        strcpy(first, errtext());
        g = gram_compile(&arena, TINY_GRAMMAR, NULL);
        CHECK(g && gram_parse(g, NULL, "x = ;", 5, &arena, &err) == NULL);
        CHECK(strcmp(first, errtext()) == 0 && err.col == 5);
    }

#if GRAM_MAX_STEPS
    /* a grammar that backtracks exponentially is cut off, not left to spin */
    {
        const char *in = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        CHECK(tree("s <- a !. ; a <- 'x' a 'y' / 'x' a / 'x' ;", in)[0] == 0);
        CHECK(err.code == GRAM_E_LIMIT);
        CHECK(tree("s <- a !. ; a <- 'x' a 'y' / 'x' a / 'x' ;", "xxx")[0] != 0);
    }
#endif

    /* --- resource limits ------------------------------------------------ */
    {
        gram_grammar *g;
        gram_node *n;
        size_t before;
        char deep[2 * GRAM_MAX_DEPTH + 8];
        int i;

        gram_arena_init(&arena, mem, 64); /* far too small */
        CHECK(gram_compile(&arena, CALC, &err) == NULL);
        CHECK(err.code == GRAM_E_NOMEM && arena.used == 0);

        gram_arena_init(&arena, mem, sizeof mem);
        g = gram_compile(&arena, CALC, &err);
        CHECK(g != NULL);
        CHECK(gram_rule_count(g) == 4 && gram_rule_id(g, "term") == 1);
        CHECK(strcmp(gram_rule_name(g, 3), "number") == 0);
        CHECK(gram_rule_id(g, "nope") == -1);

        /* failed parses give all their memory back */
        before = arena.used;
        CHECK(gram_parse(g, NULL, "1+2+", 4, &arena, &err) == NULL);
        CHECK(arena.used == before);

        arena.cap = arena.used + 3 * sizeof(gram_node); /* room for 3 nodes */
        CHECK(gram_parse(g, NULL, "1+2+3+4+5", 9, &arena, &err) == NULL);
        CHECK(err.code == GRAM_E_NOMEM && arena.used == before);
        arena.cap = sizeof mem - 64;

        for (i = 0; i < GRAM_MAX_DEPTH; i++) deep[i] = '(';
        deep[i++] = '1';
        for (; i < 2 * GRAM_MAX_DEPTH + 1; i++) deep[i] = ')';
        CHECK(gram_parse(g, NULL, deep, (size_t)i, &arena, &err) == NULL);
        CHECK(err.code == GRAM_E_DEPTH);

        CHECK(gram_parse(g, "missing", "1", 1, &arena, &err) == NULL);
        CHECK(err.code == GRAM_E_NORULE);

        /* input need not be NUL-terminated; helpers */
        n = gram_parse(g, "term", "6*7junk", 3, &arena, &err);
        CHECK(n && gram_is(n, "term") && n->nchildren == 3);
        CHECK(gram_text_is(gram_child(n, 1), "*") && gram_child(n, 1)->rule == GRAM_LITERAL);
        CHECK(gram_text_is(gram_child(n, 2), "7") && gram_child(n, 3) == NULL);
        CHECK(gram_parse(g, NULL, "1", 1, &arena, NULL) != NULL);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
