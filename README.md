# aup

The language and toolchain for [Pure](https://github.com/yeowoo-labs/pure). What Java and javac
are to the JVM, aup is to Pure: you declare an app, aup checks it and emits a module that Pure
verifies again before it runs.

**Status: v0, pre-1.0.** The language covers states, events, views, records, lists, functions and
library imports; it is usable for real apps and unstable. No release, no package, no
compatibility promise. There has been no external security audit.

## What a program looks like

An app declares what exists, what comes in, what that changes, and what is shown. Storage,
transactions and the screen model belong to the runtime, not to the app.

```aup
record Task {
  title: text
  done = false
}

state tasks: list of Task
state at: index of tasks

input typed = text
input toggle = key Space

on typed {
  tasks.append(Task { title: typed })
  at = tasks.last
}

on toggle.down { at.done = !at.done }

view counts = tasks.count
```

`examples/` holds a counter, a note editor, a task list, keyed documents, a tally, a layout — and,
as a demonstration that the core is complete, SHA-256, inflate, DNS messages, JSON parsing, line
breaking and a TCP retransmission timer written in the language itself. See the
[language design](https://github.com/yeowoo-labs/aup/blob/master/docs/design/001-language-v0.md).

## Build and run

A C17 toolchain and `make` are enough; there are no external dependencies. `make check` needs a
Pure checkout beside this one.

```sh
git clone https://github.com/yeowoo-labs/pure.git ../pure && make -C ../pure
make                                                  # build/aup
build/aup build examples/counter.aup -o counter.pmod
printf 'down tap\nup tap\nshow\n' | ../pure/build/pure-run counter.pmod

build/aup build examples/tasks.aup -o tasks.pmod
printf 'type typed %s\ndown again\ndown toggle\nshow\n' "$(printf 'milk' | xxd -p)" \
  | ../pure/build/pure-run tasks.pmod --store /tmp/tasks

build/aup watch examples/tasks.aup -o tasks.pmod     # rebuild on save; Vega's --watch picks it up
make check                                            # verification, determinism, diagnostics, reference-model parity
```

`make check` runs Pure's verifier against every example, compares the workbench, text, list and
document models with reference implementations, and repeats the persistence checks across restarts.

## What the toolchain guarantees

- The compiler emits a module format Pure can verify before loading. No source and no AST is kept
  at run time.
- Type errors, stack depth, ownership of cursors and indexes, and per-event work and memory budgets
  are checked at load, not discovered by the app.
- The toolchain is C17 with no external dependencies; the verification device is a Raspberry Pi 5.

## Layout

| Path | What lives there |
|---|---|
| `compiler/` | Lexer, parser, checker, module emitter |
| `examples/` | Counter, notes, tasks, documents, kanban board |
| `docs/design/` | Language design |
| `tests/` | Verification, diagnostics and reference-model tests |

## Related

- [Pure](https://github.com/yeowoo-labs/pure) — the runtime these modules run on
- [Vega](https://github.com/yeowoo-labs/vega) — surfaces for the same modules

## License

Apache-2.0. See [LICENSE](LICENSE).
