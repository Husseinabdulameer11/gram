/*
 * Fuzz target (libFuzzer):  make fuzz
 *
 * The input is split at the first NUL byte into a grammar and a text to
 * parse with it. Whatever the bytes are, gram must return an error or a
 * tree; it must never crash, hang or touch memory outside its arena.
 * A second pass parses the whole input with a fixed, realistic grammar.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../gram.h"

static size_t sunk;
static void sink(int c, void *ctx) { (void)ctx; sunk += (size_t)c; }

static const char fixed[] =
    "program <- stmt* ;"
    "stmt <^ if / block / assign ;"
    "if <- ~'if'i expr block (~'else' block)? ;"
    "block <- ~'{' stmt* ~'}' ;"
    "assign <- ident ~'=' expr ~';' ;"
    "expr <^ term (op term)* ;"
    "term <^ neg / number / string / ident / ~'(' expr ~')' ;"
    "neg <- ~'-' term ;"
    "op <= [-+*/<>] / '==' ;"
    "number <= [0-9]+ ;"
    "string <= '\"' ('\\\\' . / [^\"\\\\])* '\"' ;"
    "ident <= !('if'i ![a-z]) [a-zA-Z_] [a-zA-Z0-9_]* ;"
    "skip <= ([ \\t\\r\\n] / '#' [^\\n]*)* ;";

static void check_tree(const gram_node *n, const char *in, size_t len)
{
    const gram_node *c;
    size_t count = 0;
    if (n->text < in || n->text + n->len > in + len) abort();
    for (c = n->child; c; c = c->next) { check_tree(c, in, len); count++; }
    if (count != n->nchildren) abort();
}

static void run(const char *grammar, const char *in, size_t len, unsigned char *mem, size_t cap)
{
    gram_arena a;
    gram_error err;
    gram_grammar *g;
    gram_node *n;
    size_t mark;

    gram_arena_init(&a, mem, cap);
    g = gram_compile(&a, grammar, &err);
    gram_error_print(&err, sink, 0);
    if (!g) { if (err.code == GRAM_OK || a.used != 0) abort(); return; }
    mark = a.used;
    n = gram_parse(g, 0, in, len, &a, &err);
    gram_error_print(&err, sink, 0);
    if (n) { check_tree(n, in, len); gram_dump(n, sink, 0); }
    else if (err.code == GRAM_OK || a.used != mark) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* exact-size heap copies, so ASan sees any read past either end */
    const uint8_t *nul = memchr(data, 0, size);
    size_t glen = nul ? (size_t)(nul - data) : size;
    size_t ilen = nul ? size - glen - 1 : 0;
    char *grammar = malloc(glen + 1), *in = malloc(ilen + 1), *all = malloc(size + 1);
    unsigned char *mem = malloc(16 * 1024);

    memcpy(grammar, data, glen);
    grammar[glen] = 0;
    memcpy(in, data + size - ilen, ilen);
    memcpy(all, data, size);

    run(grammar, in, ilen, mem, 16 * 1024);
    run(grammar, in, ilen, mem, 600); /* provoke out-of-memory paths */
    run(fixed, all, size, mem, 16 * 1024);

    free(grammar); free(in); free(all); free(mem);
    return 0;
}
