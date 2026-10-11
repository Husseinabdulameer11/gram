# gram

A small parser library for building programming languages in C. You write a
grammar as text; `gram` turns it into a parser that produces a syntax tree
you walk to interpret or compile. It fills the role of `mpc` from *Build Your
Own Lisp*, with the grammar-file-plus-generated-skeleton workflow of ANTLR,
and it is built to run wherever a C compiler exists.

- **Two files**: `gram.c` and `gram.h`. Add them to your build.
- **Freestanding C89**: the only header used is `<stddef.h>`. No libc, no
  `malloc`, no stdio, no floats, no 64-bit integers, no division.
- **You own the memory**: everything comes from one buffer you pass in.
- **ROM-able grammars**: `gramgen` can precompile a grammar into `const`
  tables. The target then needs only the parser: about 3 KB of code on a
  Cortex-M3, 5 KB on RV32IM, and no RAM for the grammar.

Status: **0.1, early**. See [What is tested](#what-is-tested) and
[Limitations](#limitations) before relying on it.

## Quick start

```c
#include "gram.h"

static unsigned char mem[16 * 1024];

static const char grammar[] =
    "expr   <^ term (('+' / '-') term)* ;"
    "term   <^ factor (('*' / '/') factor)* ;"
    "factor <^ number / ~'(' expr ~')' ;"
    "number <= [0-9]+ ;";

gram_arena arena;
gram_error err;

gram_arena_init(&arena, mem, sizeof mem);
gram_grammar *g = gram_compile(&arena, grammar, &err);
gram_node *ast  = gram_parse(g, "expr", "1 + 2 * 3", 9, &arena, &err);
```

`ast` for `1 + 2 * 3`:

```
expr
  number '1'
  '+'
  term
    number '2'
    '*'
    number '3'
```

## Using gram in another project

Install it once:

```
make install                    # /usr/local/{bin/gramgen, include/gram.h, lib/libgram.a}
make install PREFIX=~/.local    # for one user, no sudo
gramgen --version
```

Then your project runs `gramgen` on its grammar and builds with
`-I$PREFIX/include $PREFIX/lib/libgram.a`. Or skip installing and compile
`gram.c` straight into your build.

Projects built on gram:

- **[Crib](https://github.com/Husseinabdulameer11/crib)**, a language for cryptography maths with step-by-step simulations.

## Grammar reference

A grammar is a list of rules, each ending in `;`. `#` starts a comment.

| Rule form     | Meaning |
|---------------|---------|
| `name <- e ;` | **Syntax rule.** Whitespace is skipped between its parts. Produces a node whose children are what `e` matched. |
| `name <^ e ;` | Same, but if the node ends up with exactly one child it is replaced by that child. Use it for precedence levels so `42` is a `number`, not `expr > term > factor > number`. |
| `name <= e ;` | **Token rule.** No skipping inside; the whole match becomes one leaf. Use it for numbers, identifiers, strings, keywords. |

| Expression        | Matches |
|-------------------|---------|
| `'if'` `"if"`     | that text. `'if'i` ignores ASCII case. |
| `[a-zA-Z_]`       | one byte in the set. `[^"\n]` is one byte not in the set. |
| `.`               | any one byte |
| `e1 e2`           | `e1` then `e2` |
| `e1 / e2`         | `e1`, or else `e2` (first match wins; `\|` also works) |
| `e*` `e+` `e?`    | zero or more, one or more, optional |
| `&e` `!e`         | succeeds if `e` would / would not match here; consumes nothing |
| `~e`              | matches `e` but leaves it out of the tree (punctuation, keywords) |
| `( e )`           | grouping |

Escapes in literals and classes: `\n \t \r \0 \\ \' \" \] \- \xHH`.

Things worth knowing:

- **Whitespace.** By default spaces, tabs and newlines are skipped between
  tokens in syntax rules. Define a rule called `skip` to change that, e.g. to
  add comments: `skip <= ([ \t\r\n] / '//' [^\n]*)* ;`
- **Literals in syntax rules become leaves** with `rule == GRAM_LITERAL`, so
  `'+'` and `'-'` show up in the tree. Prefix with `~` when you do not want them.
- **Choice is ordered.** `'<' / '<='` never matches `<=`; put the longer one first.
- **Keywords.** `'if'` also matches the start of `iffy`. Write keyword tokens as
  `IF <= 'if' ![A-Za-z0-9_] ;` and exclude them from identifiers with
  `ident <= !keyword [a-z]+ ;` (see `tests/tiny.gram`).
- **No left recursion.** `expr <- expr '+' term` is rejected at compile time
  with a clear error. Write `expr <- term ('+' term)*` and fold left to right.
- **Bytes, not characters.** UTF-8 input works; classes and `.` see single bytes.

## The tree

```c
struct gram_node {
    int rule;            /* rule id (definition order), or GRAM_LITERAL */
    const char *tag;     /* rule name, "" for a literal                 */
    const char *text;    /* slice of the input, not NUL-terminated      */
    size_t len;
    size_t nchildren;
    gram_node *child;    /* first child  */
    gram_node *next;     /* next sibling */
};
```

Helpers: `gram_is(n, "expr")`, `gram_text_is(n, "+")`, `gram_child(n, i)`,
`gram_dump(n, putc_fn, ctx)`. Output always goes through a
`void put(int c, void *ctx)` callback, so on bare metal you point it at your UART.

## gramgen: skeleton and precompiled grammars

`gramgen` is a host-side tool:

```
gramgen mylang.gram mylang [outdir]
```

| Output | Contents |
|--------|----------|
| `mylang_grammar.h` | Enum of rule ids (`MYLANG_R_expr`, ...) so you can `switch (n->rule)` with no string compares, and the grammar text as the macro `MYLANG_GRAMMAR`. |
| `mylang_grammar.c` | The grammar already compiled, as the const table `mylang_grammar`. |
| `mylang_visitor.c` | A tree walker with one `case` per rule to fill in. Written once, never overwritten. |

Regenerate the first two whenever the grammar changes.

To run from the precompiled tables, add `mylang_grammar.c` to your build and
skip `gram_compile`:

```c
#include "gram.h"
#include "mylang_grammar.h"

gram_arena_init(&arena, mem, sizeof mem);          /* trees only */
ast = gram_parse(&mylang_grammar, NULL, src, len, &arena, &err);
```

Build `gram.c` with `-DGRAM_NO_COMPILER` to leave the grammar compiler out.
Sizes with clang `-Os`:

| | Cortex-M3 | RV32IM |
|---|---|---|
| `gram.c`, full | 7.3 KB | 11.5 KB |
| `gram.c`, `-DGRAM_NO_COMPILER` | 3.1 KB | 5.0 KB |

The tables are plain C, so they work on any target, but they depend on the
library's internal layout: regenerate them when you update `gram.c`
(a mismatch is a compile error, not a runtime surprise).

## Errors

```c
if (!ast) gram_error_print(&err, put, ctx);
/* 2:23: syntax error: expected '-', number, ident or '(' */
```

`gram_error` has `code`, `msg`, `pos`, `line`, `col` and the `expected[]` list
if you want to format it yourself. Grammar mistakes (undefined rule, duplicate
rule, left recursion, bad syntax) are reported the same way by `gram_compile`,
with a position in the grammar text. A failed parse returns all its memory.

## Memory and limits

- The arena only grows. `gram_arena_mark` / `gram_arena_reset` free everything
  allocated after a point, e.g. to drop each tree in a REPL loop while keeping
  the grammar. `arena.peak` shows how much was needed.
- A compiled grammar of 24 rules takes about 8 KB of arena on a 64-bit host,
  roughly half on a 32-bit CPU (or none, precompiled). A tree node is 8 words.
- The input, and the grammar, must outlive the tree (nodes point into both).

Compile-time settings (`-DNAME=value`):

| Macro | Default | Purpose |
|-------|---------|---------|
| `GRAM_MAX_DEPTH` | 256 | Maximum rule nesting while parsing. The parser is recursive, so this bounds C stack use; deeper input gets `GRAM_E_DEPTH` instead of a stack overflow. Lower it on small targets. |
| `GRAM_MAX_NEST` | 32 | Maximum nesting of parentheses and operators inside one grammar rule. |
| `GRAM_MAX_STEPS` | 0 (off) | Work limit per parse; exceeding it returns `GRAM_E_LIMIT`. Turn it on for untrusted input, because a badly written grammar can backtrack exponentially. |
| `GRAM_MAX_EXPECTED` | 6 | How many "expected" alternatives an error keeps. |
| `GRAM_NO_COMPILER` | unset | Leave out `gram_compile`. |

## What is tested

- `make test`: 77 checks under AddressSanitizer and UBSan (gcc and clang, Linux).
- `make qemu`: the same suite executed under QEMU on RISC-V 64, ARM 32-bit,
  AArch64 and s390x (big-endian).
- `make cross`: the core compiled freestanding for x86-64, i386, AArch64,
  ARMv7-M, ARMv6-M, RV64, RV32IM, RV32I, RV32E, MSP430 and AVR, checking that
  it needs no C library. On the 32/64-bit targets it has no external symbols
  at all. MSP430 references the compiler's multiply helper and AVR the
  compiler's data-init symbol.
- `make fuzz`: a libFuzzer target feeding arbitrary grammars and inputs. About
  4.5 million runs before release found no crash; it reaches 96% of the lines
  in `gram.c`.

Not yet verified: execution on real microcontroller hardware or on 8/16-bit
CPUs (compile-checked only), and the CI workflow itself, which was written
but has not run yet.

## Limitations

- No left recursion (reported as an error; rewrite with repetition).
- No memoization: worst-case time is exponential for some grammars. Use
  `GRAM_MAX_STEPS` to bound it.
- Parsing stops at the first syntax error; there is no error recovery.
- The parser recurses on the C stack. Worst-case depth is `GRAM_MAX_DEPTH`
  rule calls, each a few frames deep.
- Grammars are trusted input in the sense that a hostile grammar can be slow;
  the compiler itself is fuzzed and bounds-checked.
- Classes and `.` match bytes, not Unicode code points.
- The precompiled table layout is not a stable interface between versions.

## Building

```
make          # gramgen and libgram.a into build/
make test     # test suite under ASan/UBSan
make install  # install gramgen, gram.h and libgram.a (PREFIX=/usr/local)
make cross    # freestanding compile for 11 CPU targets (needs clang)
make qemu     # run the tests on other CPUs (needs qemu-user + cross gcc)
make fuzz     # libFuzzer, FUZZ_TIME=seconds (needs clang)
```

For your own project, just compile `gram.c` with your compiler.

### Windows (MSYS2)

Open the **UCRT64** shell (MINGW64 and CLANG64 work the same way) and install
a compiler and make once:

```
pacman -S --needed mingw-w64-ucrt-x86_64-gcc make
```

Then `make` and `make test` as above; the Makefile detects Windows, adds
`.exe` and picks `gcc`. MinGW gcc has no sanitizer runtime, so `make test`
runs without ASan/UBSan there; in the CLANG64 shell you can turn them back on
with `make test SAN="-fsanitize=address,undefined"`.

## License

MIT, see `LICENSE`.
