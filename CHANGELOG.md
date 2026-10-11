# Changelog

## Unreleased

- `make install` / `make uninstall` (`PREFIX`, `DESTDIR`): installs
  `gramgen`, `gram.h` and `libgram.a` so other projects can build on gram
- `gramgen --version`
- The example languages have been removed: calculator, `tiny`, calculator
  REPL, Tiny BASIC and Pebble. Crib moved to its own repository. The tests'
  fixture grammar is now `tests/tiny.gram`, generated into `build/`.

## 0.1.0

First public version.

- PEG grammar compiler and parser in freestanding C89 (`gram.c`, `gram.h`)
- Syntax rules, hoisting rules and token rules; `~` to drop nodes;
  case-insensitive literals; user-defined `skip` rule
- Arena allocation from a caller-supplied buffer
- Errors with line, column and expected set; left recursion detected at
  grammar compile time
- Limits: `GRAM_MAX_DEPTH`, `GRAM_MAX_NEST`, optional `GRAM_MAX_STEPS`
- `gramgen`: rule-id enum, visitor skeleton and precompiled `const` tables
- `GRAM_NO_COMPILER` build for targets that only use precompiled tables
- Examples: calculator, `tiny` language interpreter, calculator REPL,
  Tiny BASIC REPL, and Pebble, a scripting language with functions
