/*
 * gram - a small, freestanding PEG parser library for building languages.
 *
 * Describe a grammar as text, compile it at run time, parse input into an
 * AST. The core needs only <stddef.h>: no libc, no malloc, no stdio, no
 * floating point, no 64-bit integers. All memory comes from a buffer you
 * hand in, so it runs the same on a desktop, a bare-metal ARM / RISC-V
 * board, or a home-made CPU with a plain C compiler. Written in C89.
 *
 * Grammar syntax (see README.md for the full reference):
 *
 *     name <- e ;     syntax rule: whitespace skipped between terminals,
 *                     produces an AST node with children
 *     name <^ e ;     same, but the node is replaced by its child when it
 *                     has exactly one (keeps expression trees flat)
 *     name <= e ;     token rule: no skipping inside, produces a leaf
 *
 *     'lit' "lit" 'lit'i   literal (i = ASCII case-insensitive)
 *     [a-z_] [^"\n]  .     byte class, negated class, any byte
 *     e1 e2   e1 / e2      sequence, ordered choice
 *     e* e+ e?             repetition
 *     &e !e                lookahead (consume nothing)
 *     ~e                   match e but keep it out of the AST
 *     ( e )                grouping          # comment to end of line
 *
 * A rule named `skip`, if present, replaces the default whitespace skipper.
 */
#ifndef GRAM_H
#define GRAM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRAM_VERSION "0.1.0"

/* Maximum rule nesting while parsing. Bounds C stack use; lower it on
 * small targets (-DGRAM_MAX_DEPTH=48). */
#ifndef GRAM_MAX_DEPTH
#define GRAM_MAX_DEPTH 256
#endif

/* Maximum nesting of ( ) and operators inside one grammar rule. */
#ifndef GRAM_MAX_NEST
#define GRAM_MAX_NEST 32
#endif

/* Optional work limit for one gram_parse call, counted in matching steps.
 * 0 = unlimited. PEG parsing without memoization can take exponential time
 * on a badly written grammar; set this when input is untrusted or a
 * watchdog is running (-DGRAM_MAX_STEPS=200000). */
#ifndef GRAM_MAX_STEPS
#define GRAM_MAX_STEPS 0
#endif

/* How many "expected ..." alternatives an error remembers. */
#ifndef GRAM_MAX_EXPECTED
#define GRAM_MAX_EXPECTED 6
#endif

enum {
    GRAM_OK = 0,
    GRAM_E_GRAMMAR, /* the grammar text itself is wrong */
    GRAM_E_SYNTAX,  /* the input does not match the grammar */
    GRAM_E_NOMEM,   /* arena too small */
    GRAM_E_DEPTH,   /* input nested deeper than GRAM_MAX_DEPTH */
    GRAM_E_NORULE,  /* start rule not found */
    GRAM_E_LIMIT    /* parse exceeded GRAM_MAX_STEPS */
};

#define GRAM_LITERAL (-1) /* gram_node.rule of an anonymous literal leaf */

/* ---- memory ----------------------------------------------------------- */

typedef struct gram_arena {
    unsigned char *base;
    size_t cap;
    size_t used;
    size_t peak; /* high-water mark, handy for sizing the buffer */
} gram_arena;

void gram_arena_init(gram_arena *a, void *buf, size_t size);
#define gram_arena_mark(a) ((a)->used)
#define gram_arena_reset(a, m) ((a)->used = (m))

/* ---- errors ----------------------------------------------------------- */

typedef struct gram_error {
    int code;        /* GRAM_OK or GRAM_E_* */
    const char *msg; /* static string */
    size_t pos;      /* byte offset in the grammar text or the input */
    size_t line;     /* 1-based */
    size_t col;      /* 1-based, in bytes */
    const char *expected[GRAM_MAX_EXPECTED]; /* owned by the grammar */
    unsigned nexpected;
} gram_error;

/* ---- grammar ---------------------------------------------------------- */

typedef struct gram_grammar gram_grammar;

/* Compile NUL-terminated grammar text. Everything is allocated from `a`
 * and the text is not referenced afterwards. Returns NULL and fills `err`
 * (may be NULL) on failure. Rule ids are 0..n-1 in order of definition. */
gram_grammar *gram_compile(gram_arena *a, const char *text, gram_error *err);

/* Define GRAM_NO_COMPILER when building gram.c to leave gram_compile out;
 * use that with a grammar precompiled by gramgen (see README). */

int gram_rule_count(const gram_grammar *g);
int gram_rule_id(const gram_grammar *g, const char *name); /* -1 if none */
const char *gram_rule_name(const gram_grammar *g, int id);

/* ---- parsing ---------------------------------------------------------- */

typedef struct gram_node gram_node;
struct gram_node {
    int rule;         /* rule id, or GRAM_LITERAL */
    const char *tag;  /* rule name, "" for a literal */
    const char *text; /* points into the input; NOT NUL-terminated */
    size_t len;
    size_t nchildren;
    gram_node *child; /* first child */
    gram_node *next;  /* next sibling */
    gram_node *last_; /* internal */
};

/* Parse input[0..len) starting at rule `start` (NULL = first rule). The
 * whole input must match. The AST is allocated from `a` (which may be the
 * arena holding the grammar) and points into `input`. Returns NULL and
 * fills `err` (may be NULL) on failure, leaving `a` as it was. */
gram_node *gram_parse(const gram_grammar *g, const char *start,
                      const char *input, size_t len,
                      gram_arena *a, gram_error *err);

/* ---- helpers ---------------------------------------------------------- */

int gram_is(const gram_node *n, const char *tag);       /* tag == name?  */
int gram_text_is(const gram_node *n, const char *text); /* text == str?  */
gram_node *gram_child(const gram_node *n, size_t i);    /* NULL if none  */
void gram_locate(const char *text, size_t len, size_t pos,
                 size_t *line, size_t *col);

/* Output goes through a callback, so there is no stdio dependency. */
typedef void (*gram_putc)(int c, void *ctx);
void gram_dump(const gram_node *n, gram_putc put, void *ctx);
void gram_error_print(const gram_error *e, gram_putc put, void *ctx);

/* ---- precompiled grammars --------------------------------------------- */

/* Layout of a compiled grammar. It is public only so that gramgen can emit
 * a grammar as `const` tables that live in ROM; treat it as private and
 * regenerate the tables when you upgrade the library. */
#define GRAM_TABLES 1 /* bumped whenever this layout changes */

typedef struct gram_x gram_x;
struct gram_x {
    unsigned char op;
    unsigned char ci;          /* literal: ASCII case-insensitive */
    const gram_x *kid;         /* first operand */
    const gram_x *next;        /* next operand of the parent sequence/choice */
    const char *spell;         /* name of a terminal in error messages */
    const unsigned char *data; /* literal bytes, or a 32-byte class bitmap */
    size_t n;                  /* literal length, or called rule id */
};

typedef struct gram_rule {
    const char *name;
    const gram_x *body;
    size_t pos;
    unsigned char kind;
    unsigned char nullable;
    unsigned char mark;
} gram_rule;

struct gram_grammar {
    const gram_rule *rules;
    int nrules;
    int skip; /* id of the `skip` rule, or -1 */
};

#ifdef __cplusplus
}
#endif

#endif /* GRAM_H */
