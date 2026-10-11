/* gram - freestanding PEG parser library. See gram.h. */
#include "gram.h"

typedef unsigned char u8;

/* ====================================================================== */
/* internal representation                                                */
/* ====================================================================== */

enum { X_LIT, X_CLASS, X_ANY, X_CALL, X_SEQ, X_ALT,
       X_STAR, X_PLUS, X_OPT, X_AND, X_NOT, X_DROP };

enum { K_NODE, K_HOIST, K_TOKEN };

typedef gram_x gx;     /* struct layouts are in gram.h */
typedef gram_rule grule;

/* ====================================================================== */
/* small utilities (no libc)                                              */
/* ====================================================================== */

#ifndef GRAM_NO_COMPILER
static size_t g_strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
#endif

static int g_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* does NUL-terminated `name` equal s[0..n) ? */
static int g_nameeq(const char *name, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (name[i] == 0 || name[i] != s[i]) return 0;
    return name[n] == 0;
}

static int g_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

union g_align { void *p; size_t s; long l; void (*f)(void); };
#define G_ALIGN (sizeof(union g_align))

void gram_arena_init(gram_arena *a, void *buf, size_t size)
{
    size_t mis = (size_t)buf % G_ALIGN;
    size_t pad = mis ? G_ALIGN - mis : 0;
    if (pad > size) pad = size;
    a->base = (unsigned char *)buf + pad;
    a->cap = size - pad;
    a->used = 0;
    a->peak = 0;
}

static void *g_alloc(gram_arena *a, size_t n)
{
    void *p;
    size_t r = n % G_ALIGN;
    if (r) {
        if (n > (size_t)-1 - G_ALIGN) return 0;
        n += G_ALIGN - r;
    }
    if (n > a->cap - a->used) return 0;
    p = a->base + a->used;
    a->used += n;
    if (a->used > a->peak) a->peak = a->used;
    return p;
}

void gram_locate(const char *text, size_t len, size_t pos,
                 size_t *line, size_t *col)
{
    size_t i, l = 1, c = 1;
    if (pos > len) pos = len;
    for (i = 0; i < pos; i++) {
        if (text[i] == '\n') { l++; c = 1; }
        else c++;
    }
    if (line) *line = l;
    if (col) *col = c;
}

static void g_err_clear(gram_error *e)
{
    unsigned i;
    e->code = GRAM_OK;
    e->msg = "";
    e->pos = 0;
    e->line = 0;
    e->col = 0;
    e->nexpected = 0;
    for (i = 0; i < GRAM_MAX_EXPECTED; i++) e->expected[i] = 0;
}

#ifndef GRAM_NO_COMPILER

/* ====================================================================== */
/* grammar lexer                                                          */
/* ====================================================================== */

enum { T_EOF, T_IDENT, T_ARROW, T_LIT, T_CLASS, T_CHAR, T_ERR };

typedef struct lex {
    const char *src;
    size_t pos;
    int kind;
    int ch;        /* T_CHAR: the character, T_ARROW: rule kind */
    int ci;        /* T_LIT: trailing i */
    size_t ts, te; /* whole token */
    size_t s, e;   /* T_LIT / T_CLASS: contents without delimiters */
    const char *err;
} lex;

static int is_idstart(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_idchar(int c)
{
    return is_idstart(c) || (c >= '0' && c <= '9');
}

static void lx_next(lex *l)
{
    const char *s = l->src;
    size_t i = l->pos;
    int c;

    for (;;) {
        c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') i++;
        else if (c == '#') { while (s[i] && s[i] != '\n') i++; }
        else break;
    }
    l->ts = i;
    l->ci = 0;
    if (c == 0) {
        l->kind = T_EOF;
    } else if (is_idstart(c)) {
        while (is_idchar(s[i])) i++;
        l->kind = T_IDENT;
    } else if (c == '<' && (s[i + 1] == '-' || s[i + 1] == '^' || s[i + 1] == '=')) {
        l->kind = T_ARROW;
        l->ch = s[i + 1] == '-' ? K_NODE : s[i + 1] == '^' ? K_HOIST : K_TOKEN;
        i += 2;
    } else if (c == '\'' || c == '"' || c == '[') {
        int q = c == '[' ? ']' : c;
        i++;
        l->s = i;
        while (s[i] && s[i] != q) {
            if (s[i] == '\\' && s[i + 1]) i++;
            i++;
        }
        if (!s[i]) {
            l->kind = T_ERR;
            l->err = c == '[' ? "unterminated character class"
                              : "unterminated literal";
            l->te = l->pos = i;
            return;
        }
        l->e = i++;
        if (c == '[') {
            l->kind = T_CLASS;
        } else {
            l->kind = T_LIT;
            if (s[i] == 'i' && !is_idchar(s[i + 1])) { l->ci = 1; i++; }
        }
    } else if (c == '/' || c == '|' || c == '*' || c == '+' || c == '?' ||
               c == '&' || c == '!' || c == '~' || c == '(' || c == ')' ||
               c == '.' || c == ';') {
        l->kind = T_CHAR;
        l->ch = c;
        i++;
    } else {
        l->kind = T_ERR;
        l->err = "unexpected character in grammar";
        i++;
    }
    l->te = l->pos = i;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = g_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* read one possibly escaped byte from s[*pi..e); -1 on a bad escape */
static int unesc(const char *s, size_t *pi, size_t e)
{
    size_t i = *pi;
    int c = (u8)s[i++];
    if (c == '\\' && i < e) {
        c = (u8)s[i++];
        switch (c) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case '0': c = 0; break;
        case 'x': {
            int h, lo;
            if (i + 2 > e) return -1;
            h = hexval((u8)s[i]);
            lo = hexval((u8)s[i + 1]);
            if (h < 0 || lo < 0) return -1;
            c = h * 16 + lo;
            i += 2;
            break;
        }
        default: break; /* \\ \' \" \] \- \^ ... stand for themselves */
        }
    }
    *pi = i;
    return c;
}

/* ====================================================================== */
/* grammar compiler                                                       */
/* ====================================================================== */

typedef struct comp {
    lex lx;
    gram_arena *a;
    gram_grammar *g;
    grule *rules; /* writable view of g->rules */
    gram_error *err;
    int failed;
    int nest;
} comp;

static void c_fail(comp *c, int code, const char *msg, size_t pos)
{
    if (c->failed) return;
    c->failed = 1;
    c->err->code = code;
    c->err->msg = msg;
    c->err->pos = pos;
}

static void *c_alloc(comp *c, size_t n)
{
    void *p = g_alloc(c->a, n);
    if (!p) c_fail(c, GRAM_E_NOMEM, "arena too small for grammar", c->lx.ts);
    return p;
}

static char *c_dup(comp *c, const char *s, size_t n)
{
    char *d = (char *)c_alloc(c, n + 1);
    size_t i;
    if (!d) return 0;
    for (i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
    return d;
}

static gx *c_new(comp *c, int op)
{
    gx *x = (gx *)c_alloc(c, sizeof(gx));
    if (!x) return 0;
    x->op = (u8)op;
    x->ci = 0;
    x->kid = 0;
    x->next = 0;
    x->spell = 0;
    x->data = 0;
    x->n = 0;
    return x;
}

static void c_next(comp *c)
{
    lx_next(&c->lx);
    if (c->lx.kind == T_ERR) c_fail(c, GRAM_E_GRAMMAR, c->lx.err, c->lx.ts);
}

static int c_ischar(comp *c, int ch)
{
    return c->lx.kind == T_CHAR && c->lx.ch == ch;
}

static int c_find(comp *c, const char *s, size_t n)
{
    int i;
    for (i = 0; i < c->g->nrules; i++)
        if (c->rules[i].name && g_nameeq(c->rules[i].name, s, n))
            return i;
    return -1;
}

static gx *p_alt(comp *c);

static gx *p_literal(comp *c)
{
    const char *s = c->lx.src;
    size_t i = c->lx.s, e = c->lx.e, n = 0;
    gx *x = c_new(c, X_LIT);
    u8 *d = (u8 *)c_alloc(c, e - i + 1);
    if (!x || !d) return 0;
    while (i < e) {
        int b = unesc(s, &i, e);
        if (b < 0) {
            c_fail(c, GRAM_E_GRAMMAR, "bad escape sequence", c->lx.ts);
            return 0;
        }
        d[n++] = (u8)b;
    }
    x->data = d;
    x->n = n;
    x->ci = (u8)c->lx.ci;
    return x;
}

static gx *p_class(comp *c)
{
    const char *s = c->lx.src;
    size_t i = c->lx.s, e = c->lx.e;
    int neg = 0, k;
    gx *x = c_new(c, X_CLASS);
    u8 *bits = (u8 *)c_alloc(c, 32);
    if (!x || !bits) return 0;
    for (k = 0; k < 32; k++) bits[k] = 0;
    if (i < e && s[i] == '^') { neg = 1; i++; }
    while (i < e) {
        int lo = unesc(s, &i, e), hi = lo;
        if (lo >= 0 && i + 1 < e && s[i] == '-') {
            i++;
            hi = unesc(s, &i, e);
        }
        if (lo < 0 || hi < 0) {
            c_fail(c, GRAM_E_GRAMMAR, "bad escape sequence", c->lx.ts);
            return 0;
        }
        if (lo > hi) {
            c_fail(c, GRAM_E_GRAMMAR, "character range is backwards", c->lx.ts);
            return 0;
        }
        for (k = lo; k <= hi; k++) bits[k >> 3] |= (u8)(1 << (k & 7));
    }
    if (neg) for (k = 0; k < 32; k++) bits[k] = (u8)~bits[k];
    x->data = bits;
    return x;
}

static int c_enter(comp *c)
{
    if (c->nest >= GRAM_MAX_NEST) {
        c_fail(c, GRAM_E_GRAMMAR, "rule is nested too deeply", c->lx.ts);
        return 0;
    }
    c->nest++;
    return 1;
}

static gx *p_primary(comp *c)
{
    lex *l = &c->lx;
    gx *x = 0;
    if (c->failed) return 0;
    if (l->kind == T_IDENT) {
        int r = c_find(c, l->src + l->ts, l->te - l->ts);
        if (r < 0) {
            c_fail(c, GRAM_E_GRAMMAR, "undefined rule", l->ts);
            return 0;
        }
        x = c_new(c, X_CALL);
        if (x) x->n = (size_t)r;
    } else if (l->kind == T_LIT) {
        x = p_literal(c);
    } else if (l->kind == T_CLASS) {
        x = p_class(c);
    } else if (c_ischar(c, '.')) {
        x = c_new(c, X_ANY);
    } else if (c_ischar(c, '(')) {
        if (!c_enter(c)) return 0;
        c_next(c);
        x = p_alt(c);
        c->nest--;
        if (!x) return 0;
        if (!c_ischar(c, ')')) {
            c_fail(c, GRAM_E_GRAMMAR, "expected ')'", l->ts);
            return 0;
        }
        c_next(c);
        return c->failed ? 0 : x;
    } else {
        c_fail(c, GRAM_E_GRAMMAR, "expected an expression", l->ts);
        return 0;
    }
    if (!x) return 0;
    if (x->op != X_CALL) {
        x->spell = c_dup(c, l->src + l->ts, l->te - l->ts);
        if (!x->spell) return 0;
    }
    c_next(c);
    return c->failed ? 0 : x;
}

static gx *c_wrap(comp *c, int op, gx *kid)
{
    gx *x;
    if (!kid) return 0;
    x = c_new(c, op);
    if (x) x->kid = kid;
    return x;
}

static gx *p_suffix(comp *c)
{
    gx *x = p_primary(c);
    int count = 0;
    while (x && c->lx.kind == T_CHAR &&
           (c->lx.ch == '*' || c->lx.ch == '+' || c->lx.ch == '?')) {
        int op = c->lx.ch == '*' ? X_STAR : c->lx.ch == '+' ? X_PLUS : X_OPT;
        if (++count > 2) { /* e*+? and the like only add depth */
            c_fail(c, GRAM_E_GRAMMAR, "too many repetition operators", c->lx.ts);
            return 0;
        }
        x = c_wrap(c, op, x);
        c_next(c);
        if (c->failed) return 0;
    }
    return x;
}

static gx *p_prefix(comp *c)
{
    int op = -1;
    gx *x;
    if (c_ischar(c, '&')) op = X_AND;
    else if (c_ischar(c, '!')) op = X_NOT;
    else if (c_ischar(c, '~')) op = X_DROP;
    if (op < 0) return p_suffix(c);
    if (!c_enter(c)) return 0;
    c_next(c);
    x = c_wrap(c, op, p_prefix(c));
    c->nest--;
    return x;
}

static int starts_prefix(comp *c)
{
    lex *l = &c->lx;
    if (l->kind == T_IDENT || l->kind == T_LIT || l->kind == T_CLASS) return 1;
    return l->kind == T_CHAR && (l->ch == '&' || l->ch == '!' || l->ch == '~' ||
                                 l->ch == '(' || l->ch == '.');
}

static gx *p_seq(comp *c)
{
    gx *first = 0, *last = 0, *x;
    size_t count = 0;
    while (!c->failed && starts_prefix(c)) {
        x = p_prefix(c);
        if (!x) return 0;
        if (last) last->next = x; else first = x;
        last = x;
        count++;
    }
    if (c->failed) return 0;
    if (count == 0) { /* empty sequence: matches nothing, always succeeds */
        x = c_new(c, X_LIT);
        if (x) { x->data = (const u8 *)""; x->spell = "''"; }
        return x;
    }
    if (count == 1) return first;
    x = c_new(c, X_SEQ);
    if (x) x->kid = first;
    return x;
}

static gx *p_alt(comp *c)
{
    gx *first = p_seq(c), *last = first, *x;
    if (!first) return 0;
    if (!c_ischar(c, '/') && !c_ischar(c, '|')) return first;
    while (c_ischar(c, '/') || c_ischar(c, '|')) {
        c_next(c);
        x = p_seq(c);
        if (!x) return 0;
        last->next = x;
        last = x;
    }
    x = c_new(c, X_ALT);
    if (x) x->kid = first;
    return x;
}

/* pass 1: find every `name <-` so rules can be used before their
 * definition and get ids in definition order */
static int c_scan(comp *c, const char *text, int fill)
{
    lex l;
    int count = 0, was_ident = 0;
    size_t is = 0, ie = 0;
    l.src = text;
    l.pos = 0;
    for (;;) {
        lx_next(&l);
        if (l.kind == T_EOF) break;
        if (l.kind == T_ERR) {
            c_fail(c, GRAM_E_GRAMMAR, l.err, l.ts);
            return -1;
        }
        if (l.kind == T_ARROW && was_ident) {
            if (fill) {
                grule *r = &c->rules[count];
                if (c_find(c, text + is, ie - is) >= 0) {
                    c_fail(c, GRAM_E_GRAMMAR, "rule defined twice", is);
                    return -1;
                }
                r->name = c_dup(c, text + is, ie - is);
                if (!r->name) return -1;
                r->pos = is;
                r->kind = (u8)l.ch;
            }
            count++;
        }
        was_ident = l.kind == T_IDENT;
        is = l.ts;
        ie = l.te;
    }
    return count;
}

static int x_nullable(const gram_grammar *g, const gx *e)
{
    const gx *k;
    switch (e->op) {
    case X_LIT: return e->n == 0;
    case X_CLASS: case X_ANY: return 0;
    case X_CALL: return g->rules[e->n].nullable;
    case X_SEQ:
        for (k = e->kid; k; k = k->next) if (!x_nullable(g, k)) return 0;
        return 1;
    case X_ALT:
        for (k = e->kid; k; k = k->next) if (x_nullable(g, k)) return 1;
        return 0;
    case X_PLUS: case X_DROP: return x_nullable(g, e->kid);
    default: return 1; /* STAR OPT AND NOT */
    }
}

static int lr_rule(comp *c, int r);

/* does `e` reach a rule that is currently being visited without consuming
 * input first? */
static int lr_expr(comp *c, const gx *e)
{
    const gx *k;
    switch (e->op) {
    case X_CALL: return lr_rule(c, (int)e->n);
    case X_SEQ:
        for (k = e->kid; k; k = k->next) {
            if (lr_expr(c, k)) return 1;
            if (!x_nullable(c->g, k)) break;
        }
        return 0;
    case X_ALT:
        for (k = e->kid; k; k = k->next) if (lr_expr(c, k)) return 1;
        return 0;
    case X_LIT: case X_CLASS: case X_ANY: return 0;
    default: return lr_expr(c, e->kid);
    }
}

static int lr_rule(comp *c, int r)
{
    grule *R = &c->rules[r];
    int found;
    if (R->mark == 2) return 0;
    if (R->mark == 1) {
        c_fail(c, GRAM_E_GRAMMAR,
               "left recursion (rule reaches itself without consuming input)",
               R->pos);
        return 1;
    }
    R->mark = 1;
    found = lr_expr(c, R->body);
    R->mark = 2;
    return found;
}

gram_grammar *gram_compile(gram_arena *a, const char *text, gram_error *err)
{
    comp c;
    gram_error dummy;
    gram_grammar *g;
    size_t mark = a->used;
    int n, i, changed;

    if (!err) err = &dummy;
    g_err_clear(err);
    c.a = a;
    c.err = err;
    c.failed = 0;
    c.nest = 0;
    c.rules = 0;
    c.g = 0;
    c.lx.src = text;
    c.lx.pos = 0;
    c.lx.ts = 0;
    c.lx.kind = T_EOF;

    n = c_scan(&c, text, 0);
    if (n == 0) c_fail(&c, GRAM_E_GRAMMAR, "grammar defines no rules", 0);
    if (c.failed) goto fail;

    g = (gram_grammar *)c_alloc(&c, sizeof(gram_grammar));
    if (!g) goto fail;
    c.rules = (grule *)c_alloc(&c, (size_t)n * sizeof(grule));
    if (!c.rules) goto fail;
    g->rules = c.rules;
    g->nrules = n;
    g->skip = -1;
    for (i = 0; i < n; i++) {
        c.rules[i].name = 0;
        c.rules[i].body = 0;
        c.rules[i].pos = 0;
        c.rules[i].kind = 0;
        c.rules[i].nullable = 0;
        c.rules[i].mark = 0;
    }
    c.g = g;
    if (c_scan(&c, text, 1) < 0) goto fail;

    /* pass 2: rule bodies */
    c_next(&c);
    while (!c.failed && c.lx.kind != T_EOF) {
        int r;
        if (c.lx.kind != T_IDENT) {
            c_fail(&c, GRAM_E_GRAMMAR, "expected a rule name", c.lx.ts);
            break;
        }
        r = c_find(&c, text + c.lx.ts, c.lx.te - c.lx.ts);
        c_next(&c);
        if (c.failed) break;
        if (c.lx.kind != T_ARROW || r < 0) {
            c_fail(&c, GRAM_E_GRAMMAR, "expected '<-', '<^' or '<=' after rule name",
                   c.lx.ts);
            break;
        }
        c_next(&c);
        c.nest = 0;
        c.rules[r].body = p_alt(&c);
        if (c.failed || !c.rules[r].body) break;
        if (!c_ischar(&c, ';')) {
            c_fail(&c, GRAM_E_GRAMMAR, "expected ';' at end of rule", c.lx.ts);
            break;
        }
        c_next(&c);
    }
    if (c.failed) goto fail;

    g->skip = c_find(&c, "skip", 4);

    do { /* nullable: least fixed point */
        changed = 0;
        for (i = 0; i < n; i++) {
            if (!c.rules[i].nullable && x_nullable(g, c.rules[i].body)) {
                c.rules[i].nullable = 1;
                changed = 1;
            }
        }
    } while (changed);

    for (i = 0; i < n; i++)
        if (lr_rule(&c, i)) goto fail;

    return g;

fail:
    a->used = mark;
    if (err->code == GRAM_OK) { err->code = GRAM_E_GRAMMAR; err->msg = "bad grammar"; }
    gram_locate(text, g_strlen(text), err->pos, &err->line, &err->col);
    return 0;
}

#endif /* GRAM_NO_COMPILER */

int gram_rule_count(const gram_grammar *g) { return g->nrules; }

int gram_rule_id(const gram_grammar *g, const char *name)
{
    int i;
    for (i = 0; i < g->nrules; i++)
        if (g_streq(g->rules[i].name, name)) return i;
    return -1;
}

const char *gram_rule_name(const gram_grammar *g, int id)
{
    return (id >= 0 && id < g->nrules) ? g->rules[id].name : "";
}

/* ====================================================================== */
/* parser                                                                 */
/* ====================================================================== */

typedef struct P {
    const gram_grammar *g;
    const u8 *in;
    size_t len;
    size_t pos;
    gram_arena *a;
    gram_error *err;
    size_t fpos;       /* farthest position a terminal failed at */
    size_t skipped_at; /* position already known to be past skippable text */
    int tight;         /* inside a token rule: no skipping, no nodes */
    int quiet;         /* >0: do not record failures */
    int depth;
    int fatal;
#if GRAM_MAX_STEPS
    unsigned long steps;
#endif
} P;

static int eval(P *p, const gx *e, gram_node *par);

static void p_fatal(P *p, int code, const char *msg)
{
    if (p->fatal) return;
    p->fatal = 1;
    p->err->code = code;
    p->err->msg = msg;
    p->err->pos = p->pos;
}

static void p_expect(P *p, size_t pos, const char *what)
{
    gram_error *e = p->err;
    unsigned i;
    if (p->quiet || pos < p->fpos) return;
    if (pos > p->fpos) { p->fpos = pos; e->nexpected = 0; }
    for (i = 0; i < e->nexpected; i++)
        if (g_streq(e->expected[i], what)) return;
    if (e->nexpected < GRAM_MAX_EXPECTED) e->expected[e->nexpected++] = what;
}

static void p_skip(P *p)
{
    if (p->tight || p->pos == p->skipped_at) return;
    if (p->g->skip >= 0) {
        size_t pos0 = p->pos;
        p->tight = 1;
        p->quiet++;
        if (!eval(p, p->g->rules[p->g->skip].body, 0)) p->pos = pos0;
        p->quiet--;
        p->tight = 0;
    } else {
        while (p->pos < p->len) {
            int c = p->in[p->pos];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            p->pos++;
        }
    }
    p->skipped_at = p->pos;
}

static gram_node *p_node(P *p, int rule, const char *tag)
{
    gram_node *n = (gram_node *)g_alloc(p->a, sizeof(gram_node));
    if (!n) { p_fatal(p, GRAM_E_NOMEM, "arena too small for syntax tree"); return 0; }
    n->rule = rule;
    n->tag = tag;
    n->text = (const char *)p->in + p->pos;
    n->len = 0;
    n->nchildren = 0;
    n->child = 0;
    n->next = 0;
    n->last_ = 0;
    return n;
}

static void p_append(gram_node *par, gram_node *n)
{
    n->next = 0;
    if (par->last_) par->last_->next = n; else par->child = n;
    par->last_ = n;
    par->nchildren++;
}

static int p_leaf(P *p, gram_node *par, int rule, const char *tag, size_t start)
{
    gram_node *n = p_node(p, rule, tag);
    if (!n) return 0;
    n->text = (const char *)p->in + start;
    n->len = p->pos - start;
    p_append(par, n);
    return 1;
}

static int call_rule(P *p, int ri, gram_node *par)
{
    const grule *r = &p->g->rules[ri];
    size_t start;
    int ok;

    if (p->depth >= GRAM_MAX_DEPTH) {
        p_fatal(p, GRAM_E_DEPTH, "input is nested too deeply");
        return 0;
    }
    p->depth++;
    p_skip(p);
    start = p->pos;

    if (p->tight) {
        ok = eval(p, r->body, 0);
    } else if (r->kind == K_TOKEN) {
        p->tight = 1;
        p->quiet++;
        ok = eval(p, r->body, 0);
        p->quiet--;
        p->tight = 0;
        if (!ok) p_expect(p, start, r->name);
        else if (par) ok = p_leaf(p, par, ri, r->name, start);
    } else if (!par) {
        ok = eval(p, r->body, 0);
    } else {
        gram_node *n = p_node(p, ri, r->name);
        ok = n && eval(p, r->body, n);
        if (ok) {
            if (r->kind == K_HOIST && n->nchildren == 1) {
                p_append(par, n->child);
            } else {
                n->len = p->pos - start;
                p_append(par, n);
            }
        }
    }
    p->depth--;
    return ok;
}

#define SAVE() \
    do { pos0 = p->pos; mark = p->a->used; \
         if (par) { last0 = par->last_; n0 = par->nchildren; } } while (0)

#define RESTORE() \
    do { p->pos = pos0; p->a->used = mark; \
         if (par) { par->last_ = last0; par->nchildren = n0; \
                    if (last0) last0->next = 0; else par->child = 0; } } while (0)

static int eval(P *p, const gx *e, gram_node *par)
{
    size_t pos0 = 0, mark = 0, n0 = 0, i;
    gram_node *last0 = 0;
    const gx *k;
    int ok;

    if (p->fatal) return 0;
#if GRAM_MAX_STEPS
    if (++p->steps > (unsigned long)GRAM_MAX_STEPS) {
        p_fatal(p, GRAM_E_LIMIT, "input took too many steps to parse");
        return 0;
    }
#endif

    switch (e->op) {
    case X_LIT:
        if (e->n == 0) return 1;
        p_skip(p);
        ok = e->n <= p->len - p->pos;
        for (i = 0; ok && i < e->n; i++) {
            int a = p->in[p->pos + i], b = e->data[i];
            if (a != b && !(e->ci && g_lower(a) == g_lower(b))) ok = 0;
        }
        if (!ok) { p_expect(p, p->pos, e->spell); return 0; }
        p->pos += e->n;
        return par ? p_leaf(p, par, GRAM_LITERAL, "", p->pos - e->n) : 1;

    case X_CLASS:
    case X_ANY:
        p_skip(p);
        if (p->pos >= p->len ||
            (e->op == X_CLASS &&
             !(e->data[p->in[p->pos] >> 3] & (1 << (p->in[p->pos] & 7))))) {
            p_expect(p, p->pos, e->spell);
            return 0;
        }
        p->pos++;
        return par ? p_leaf(p, par, GRAM_LITERAL, "", p->pos - 1) : 1;

    case X_CALL:
        return call_rule(p, (int)e->n, par);

    case X_SEQ:
        for (k = e->kid; k; k = k->next)
            if (!eval(p, k, par)) return 0;
        return 1;

    case X_ALT:
        SAVE();
        for (k = e->kid; k; k = k->next) {
            if (eval(p, k, par)) return 1;
            if (p->fatal) return 0;
            RESTORE();
        }
        return 0;

    case X_PLUS:
        if (!eval(p, e->kid, par)) return 0;
        /* fall through */
    case X_STAR:
        for (;;) {
            SAVE();
            if (!eval(p, e->kid, par)) {
                if (p->fatal) return 0;
                RESTORE();
                break;
            }
            if (p->pos == pos0) break; /* matched nothing: stop looping */
        }
        return 1;

    case X_OPT:
        SAVE();
        if (!eval(p, e->kid, par)) {
            if (p->fatal) return 0;
            RESTORE();
        }
        return 1;

    case X_AND:
        pos0 = p->pos;
        ok = eval(p, e->kid, 0);
        p->pos = pos0;
        return ok;

    case X_NOT:
        pos0 = p->pos;
        p->quiet++;
        ok = eval(p, e->kid, 0);
        p->quiet--;
        p->pos = pos0;
        return p->fatal ? 0 : !ok;

    default: /* X_DROP */
        return eval(p, e->kid, 0);
    }
}

gram_node *gram_parse(const gram_grammar *g, const char *start,
                      const char *input, size_t len,
                      gram_arena *a, gram_error *err)
{
    P p;
    gram_error dummy;
    gram_node root;
    size_t mark = a->used;
    int ri = 0, ok;

    if (!err) err = &dummy;
    g_err_clear(err);
    if (start) {
        ri = gram_rule_id(g, start);
        if (ri < 0) {
            err->code = GRAM_E_NORULE;
            err->msg = "start rule not found";
            return 0;
        }
    }

    p.g = g;
    p.in = (const u8 *)input;
    p.len = len;
    p.pos = 0;
    p.a = a;
    p.err = err;
    p.fpos = 0;
    p.skipped_at = (size_t)-1;
    p.tight = 0;
    p.quiet = 0;
    p.depth = 0;
    p.fatal = 0;
#if GRAM_MAX_STEPS
    p.steps = 0;
#endif

    root.rule = GRAM_LITERAL;
    root.tag = "";
    root.text = input;
    root.len = 0;
    root.nchildren = 0;
    root.child = 0;
    root.next = 0;
    root.last_ = 0;

    ok = call_rule(&p, ri, &root);
    if (ok) {
        p_skip(&p);
        if (p.pos != len) {
            p_expect(&p, p.pos, "end of input");
            ok = 0;
        }
    }
    if (ok && root.child) return root.child;

    a->used = mark;
    if (!p.fatal) {
        err->code = GRAM_E_SYNTAX;
        err->msg = "syntax error";
        err->pos = p.fpos;
    } else {
        err->nexpected = 0;
    }
    gram_locate(input, len, err->pos, &err->line, &err->col);
    return 0;
}

/* ====================================================================== */
/* helpers                                                                */
/* ====================================================================== */

int gram_is(const gram_node *n, const char *tag)
{
    return n && g_streq(n->tag, tag);
}

int gram_text_is(const gram_node *n, const char *text)
{
    return n && g_nameeq(text, n->text, n->len);
}

gram_node *gram_child(const gram_node *n, size_t i)
{
    gram_node *c = n ? n->child : 0;
    while (c && i--) c = c->next;
    return c;
}

static void put_str(const char *s, gram_putc put, void *ctx)
{
    while (*s) put(*s++, ctx);
}

/* decimal output without division or multiplication, so that targets
 * lacking those instructions need no compiler runtime library */
static void put_num(size_t v, gram_putc put, void *ctx)
{
    size_t pw[3 * sizeof(size_t)];
    volatile size_t p10; /* volatile: keeps optimisers from turning the */
    int n = 0;           /* loops back into mul/div library calls       */
    pw[0] = 1;
    while (pw[n] <= (size_t)-1 / 10) {
        p10 = pw[n] << 1;
        p10 = (p10 << 2) + p10; /* x10 */
        if (p10 > v) break;
        pw[++n] = p10;
    }
    for (; n >= 0; n--) {
        int d = '0';
        p10 = pw[n];
        while (v >= p10) { v -= p10; d++; }
        put(d, ctx);
    }
}

static void put_text(const char *s, size_t n, gram_putc put, void *ctx)
{
    size_t i;
    put('\'', ctx);
    for (i = 0; i < n; i++) {
        if (s[i] == '\n') put_str("\\n", put, ctx);
        else if (s[i] == '\t') put_str("\\t", put, ctx);
        else if (s[i] == '\r') put_str("\\r", put, ctx);
        else put(s[i], ctx);
    }
    put('\'', ctx);
}

static void dump(const gram_node *n, int depth, gram_putc put, void *ctx)
{
    const gram_node *c;
    int i;
    for (i = 0; i < depth; i++) put_str("  ", put, ctx);
    put_str(n->tag, put, ctx);
    if (!n->child) {
        if (n->tag[0]) put(' ', ctx);
        put_text(n->text, n->len, put, ctx);
    }
    put('\n', ctx);
    for (c = n->child; c; c = c->next) dump(c, depth + 1, put, ctx);
}

void gram_dump(const gram_node *n, gram_putc put, void *ctx)
{
    if (n) dump(n, 0, put, ctx);
}

void gram_error_print(const gram_error *e, gram_putc put, void *ctx)
{
    unsigned i;
    if (e->code == GRAM_OK) { put_str("no error\n", put, ctx); return; }
    if (e->line) {
        put_num(e->line, put, ctx);
        put(':', ctx);
        put_num(e->col, put, ctx);
        put_str(": ", put, ctx);
    }
    put_str(e->msg, put, ctx);
    for (i = 0; i < e->nexpected; i++) {
        put_str(i == 0 ? ": expected " : i + 1 == e->nexpected ? " or " : ", ",
                put, ctx);
        put_str(e->expected[i], put, ctx);
    }
    put('\n', ctx);
}
