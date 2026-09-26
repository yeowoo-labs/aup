#!/usr/bin/env python3
"""aup v0a checks.

1. Every example compiles, and Pure's verifier accepts the module.
2. The same source always yields the same bytes.
3. Diagnostics name the right line, column and rule. Generated programs always
   compile and verify; damaged sources never crash the compiler, and every
   module it does emit passes Pure's verifier.
4. Parity: random event scripts produce the same states, views, ticks, event
   counts and event failures as the workbench Zig engine (via its Node CLI).
   Workbench JSON prints -0 as 0, so signed zeros compare numerically there;
   Pure's unit tests check signed zeros directly. Other values compare exactly.
6. Text model: examples/notes.aup, run by pure-run, keeps its text, cursor and
   mark exactly as a reference Python model under random typing (ASCII,
   Korean, emoji, combining marks, NUL, newlines), moves and deletions; string
   escapes and number text follow their stated rules.
9. Server: the HTTP document server running editor.aup answers random
   requests exactly as the server running editor.pure, on persistent and
   pipelined connections too; a read lends the stored extents and still
   carries its version while the document changes, and lent answers count
   against the payload budget; concurrent edits share commits, and every
   acknowledged edit survives kill -9.
8. Stored state: the list app and the text editor run on a tree store,
   restarting at random points (every other run saving in bursts), and still
   match their reference models.
7. List model: examples/tasks.aup keeps its list, index, views (conditional
   ones and a table with a row per task) and failed events exactly as a
   reference Python model under random appends, moves, toggles, copies,
   renames and removals; replacing it with a module whose record changed
   keeps each item's fields by name.
5. Editor parity: the document editor written in aup, run by pure-edit-module,
   answers random show/edit/apply operations exactly as pure-edit running the
   original editor.pure (stdout, stderr, exit code), on the same tree store
   format, and each CLI reads the other's store identically.
"""
import argparse
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

RECORD = 'record T {\n  a = 0\n}\n'
ON_K = 'input k = key KeyK\non k.down {\n'
LIST = RECORD + 'state xs: list of T\nstate at: index of xs\n'

DIAGNOSTICS = [
    ('state x = 1\nstate x = 2\n', '2:7', "already declared as a state"),
    ('state a = b\n', '1:11', 'expected a numeric state initializer'),
    ('view v = dt\n', '1:10', 'dt is only available in handlers'),
    ('input k = key Space\ninput j = key Space\n', '2:15', "key Space is already used by input 'k'"),
    ('input k = key F13\n', '1:15', "unknown key 'F13'"),
    ('on missing.down {\n}\n', '1:4', "unknown input 'missing'"),
    ('state a = 0\non a.down {\n}\n', '2:4', "'a' is a state, not an input"),
    ('state a = 0\non frame {\n  b = 1\n}\n', '3:3', "unknown state 'b'"),
    ('state a = 0\nview v = a\non frame {\n  v = 1\n}\n', '4:3', "cannot assign to view 'v'"),
    ('input k = key Space\nview v = k\n', '2:10', "'k' is a key input, not a state"),
    ('state a = 0\nview v = min(a)\n', '2:10', 'min expects 2 arguments'),
    ('state if = 0\n', '1:7', "'if' is reserved"),
    ('state a = 0 $\n', '1:13', "unexpected character '$'"),
    ('state a = 1e400\n', '1:11', 'number must be finite'),
    ('view v = 1 +\n', '1:13', 'expected a number, state, or expression'),
    ('state a = 0\non frame {\n  a = 1 a\n}\n', '3:9', 'expected newline or semicolon'),
    ('state a = 0\non frame {\n  if a {\n    a = 1\n  } els {\n  }\n}\n', '5:5', 'expected newline or semicolon'),
    ('view v = ' + '+'.join(['1'] * 300) + '\n', '1:', 'program exceeds 512 instructions'),
    ('view v = ' + '(' * 70 + '1' + ')' * 70 + '\n', '1:', 'expression nesting exceeds 64'),
    ('state s = 0\n' * 1 + ''.join(f'state s{i} = 0\n' for i in range(32)), '33:7', 'at most 32 states are allowed'),
    # v0b: messages, keyed documents and types
    ('state d: bytes by key\ninput m = message\non m {\n  reply d[key]\n}\n', '4:9', 'd[key] needs the handler of a message by key'),
    ('input m = message by key { a: u32 }\non m {\n  reply m.a\n}\n', '3:9', 'reply takes bytes or text, not a number'),
    ('input m = message by key {\n  b: bytes\n  a: u32\n}\n', '3:3', 'a bytes field must be the last field'),
    ('input m = message by key\non m.down {\n}\n', '2:4', "'m' is a message; write on m { }"),
    ('input k = key Space\non k {\n}\n', '2:4', "'k' is a key input; write on k.down or on k.up"),
    ('state d: bytes by key\ninput m = message by key { a: bytes }\non m {\n  d[1].replace(0 .. 0, m.a)\n}\n', '4:5',
     'a collection is indexed by the message key'),
    ('input m = message by key { a: u32 }\nview v = m.a\n', '2:10', "fields of 'm' are only available in its handler"),
    ('state x = 0\ninput m = message by key { a: bytes }\non m {\n  x = m.a\n}\n', '4:7', 'expected a number, not bytes'),
    ('input m = message by key { a: u32 }\non m {\n  reply m.z\n}\n', '3:9', "message 'm' has no field 'z'"),
    ('state x = 0\non frame {\n  require x.bytes\n}\n', '3:11', "a number has no 'bytes'"),
    ('state d: bytes by key\nstate e: bytes by key\n', '2:7', 'at most one keyed collection is allowed'),
    ('state d: bytes by key\ninput m = message by key { a: bytes }\non m {\n  d[key].replace(0 .. m.a, m.a)\n}\n', '4:23',
     'expected a number, not bytes'),
    ('input m = message by key { a: u32 }\non frame {\n  reply m.a\n}\n', '3:3', 'reply is only available in a message handler'),
    # text and cursors
    ('state at: cursor in missing\n', '1:7', "a cursor moves in a text state; 'missing' is not one"),
    ('state a: text\nstate b: text\nstate at: cursor in a\ninput k = key KeyK\non k.down {\n  b.insert(at, "x")\n}\n',
     '6:12', "a cursor of 'a' cannot point into 'b'"),
    ('state a: text\nstate at: cursor in a\ninput k = key KeyK\non k.down {\n  at = 3\n}\n', '5:8',
     'expected a cursor, not a number'),
    ('state a: text\nstate at: cursor in a\nview v = at\n', '3:10', 'a view is a number or text, not a cursor'),
    ('state a: text\nstate at: cursor in a\nview v = "{at}"\n', '3:12', 'a string shows numbers and text, not a cursor'),
    ('state x = 1\nstate t: text = "{x}"\n', '2:17', 'a text state starts from a plain string'),
    ('view v = "a\\qb"\n', '1:12', 'unknown escape'),
    ('view v = "\\u{D800}"\n', '1:11', 'Unicode scalar'),
    ('state x = 1\nview v = "{x"\n', '2:11', "expected '}' to end the {expression}"),
    ('state x = 1\nview v = x + 1 char\n', '2:12', 'only a cursor moves by a distance, not a number'),
    ('view v = 1 char\n', '1:12', 'a distance moves a cursor'),
    ('view v = "abc\n"\n', '1:10', 'a string ends on the line it starts'),
    ('state a: text\ninput typed = text\nview v = typed\n', '3:10', "'typed' is only available in its handler"),
    ('input typed = text\non typed.down {\n}\n', '2:4', "'typed' is a text input; write on typed { }"),
    ('view v = text(3)\n', '1:10', 'text(...) converts bytes, not a number'),
    ('view v = "é"\nview w = 1 é\n', '2:12', 'unexpected byte 0xc3 outside a string'),
    # records, lists and indices
    ('record T {\n  a = 0\n  a = 1\n}\n', '3:3', "duplicate field 'a'"),
    ('record T {\n}\n', '1:8', 'a record needs at least one field'),
    ('record T {\n  a: number\n}\n', '2:6', 'a field is a number (field = 0) or text (field: text)'),
    ('record T {\n  a = 0\n}\nstate T = 0\n', '4:7', "'T' is already declared as a record"),
    ('state xs: list of Missing\n', '1:7', "a list holds a record; 'Missing' is not one"),
    ('state x = 0\nstate at: index of x\n', '2:7', "an index points into a list state; 'x' is not one"),
    (RECORD + 'state xs: list T\n', '4:16', "expected 'of'"),
    (RECORD + 'state xs: list of T\nstate ys: list of T\nstate at: index of xs\nview v = ys[at].a\n', '7:13',
     "an index of 'xs' cannot point into 'ys'"),
    (RECORD + 'state xs: list of T\nstate at: index of xs\nview v = xs[at].b\n', '6:10', "record 'T' has no field 'b'"),
    (RECORD + 'state xs: list of T\nstate at: index of xs\nview v = xs[at]\n', '6:10', 'read a field of the item'),
    (RECORD + 'state xs: list of T\nview v = xs\n', '5:10', "'xs' is a list; read xs.count"),
    (RECORD + 'state xs: list of T\nview v = xs.size\n', '5:10', "a list has count, first and last, not 'size'"),
    (RECORD + 'state xs: list of T\nstate at: index of xs\nview v = at\n', '6:10', 'a view is a number or text, not an index'),
    (RECORD + 'state xs: list of T\nstate at: index of xs\n' + ON_K + '  at = 3\n}\n', '8:8', 'expected an index, not a number'),
    (RECORD + 'state xs: list of T\n' + ON_K + '  xs = 1\n}\n', '7:3', 'a list changes by append, insert, remove'),
    (RECORD + 'record U {\n  a = 0\n}\nstate xs: list of T\n' + ON_K + '  xs.append(U { a: 1 })\n}\n', '10:13',
     "'xs' holds T, not U"),
    (RECORD + 'state xs: list of T\n' + ON_K + '  xs.append(1)\n}\n', '7:13', "'xs' holds T records"),
    (RECORD + 'state xs: list of T\n' + ON_K + '  xs.append(T { a: "x" })\n}\n', '7:20', 'expected a number, not text'),
    (RECORD + 'state xs: list of T\n' + ON_K + '  xs.append(T { b: 1 })\n}\n', '7:17', "record 'T' has no field 'b'"),
    (RECORD + 'state xs: list of T\n' + ON_K + '  xs.append(T { a: 1, a: 2 })\n}\n', '7:23', "field 'a' is given twice"),
    (RECORD + 'state xs: list of T\n' + ON_K + '  xs.append(T { a: 1 a: 2 })\n}\n', '7:22',
     "expected ',' or a new line between fields"),
    (RECORD + 'state xs: list of T\nstate at: index of xs\n' + ON_K + '  xs.remove(at .. at)\n}\n', '8:19',
     'a list removes one item'),
    (RECORD + 'state xs: list of T\nstate at: index of xs\n' + ON_K + '  xs[at].a = "x"\n}\n', '8:14',
     'expected a number, not text'),
    (RECORD + 'state n: text\nstate at: cursor in n\n' + ON_K + '  n.insert(at, T { a: 1 })\n}\n', '8:16',
     'a record value goes into a list'),
    ('state n: text\nstate at: cursor in n\n' + ON_K + '  n.remove(at)\n}\n', '5:3', 'a text removes a range'),
    ('record T {\n  position = 0\n}\n', '2:3', "'position' is where an index is"),
    (LIST + 'view v = at.b\n', '6:10', "record 'T' has no field 'b'"),
    (LIST + ON_K + '  at.a = "x"\n}\n', '8:10', 'expected a number, not text'),
    (LIST + 'state n = 0\n' + ON_K + '  n.a = 1\n}\n', '9:3', 'expected an index, not a number'),
    # conditional expressions
    ('view v = if 1 { 2 }\n', '1:20', 'an if expression needs else'),
    ('view v = if 1 { 2 } else if 0 { 3 }\n', '1:36', 'an if expression needs else'),
    ('view v = if 1 { 2 } else { "x" }\n', '1:28', 'the branches of if give a number and text'),
    ('state a: text\nstate b: text\nstate x: cursor in a\nstate y: cursor in b\n' + ON_K + '  x = if 1 { x } else { y }\n}\n',
     '7:25', 'the branches of if give cursors of different texts'),
    ('view v = ' + '1 + (if 1 { ' * 9 + '1' + ' } else { 0 })' * 9 + '\n', '1:111', 'conditional expressions nest too deeply'),
    # tables
    ('view t = each x in missing {\n  a = 1\n}\n', '1:20', "unknown list 'missing'"),
    (LIST + 'view t = each x in xs {\n}\n', '7:1', 'a table needs at least one column'),
    (LIST + 'view t = each x in xs {\n  a = 1\n  a = 2\n}\n', '8:3', "duplicate column 'a'"),
    (LIST + 'view t = each x in xs {\n  a = x.a\n}\nview v = x.a\n', '9:10', "unknown state 'x'"),
    (LIST + 'view t = each at in xs {\n  a = 1\n}\n', '6:15', "'at' is already declared as a list index"),
    (LIST + 'view t = each x in xs {\n  c = x\n}\n', '7:7', 'a column is a number or text, not an index'),
    ('view each = 1\n', '1:6', "'each' is reserved"),
    # fns
    ('fn f(x) = x\nview v = f(1, 2)\n', '2:10', 'f expects 1 argument'),
    ('fn f(x) = f(x)\nview v = f(1)\n', '1:11', "fn 'f' calls itself (in fn 'f' called at 2:10)"),
    ('fn f(x, x) = 1\n', '1:9', "duplicate parameter 'x'"),
    ('view v = g(1)\n', '1:10', "unknown fn 'g'"),
    ('fn f(x) = x + "a"\nview v = f(1)\n', '1:15', "expected a number, not text (in fn 'f' called at 2:10)"),
    ('fn text(x) = x\n', '1:4', "'text' is a built-in conversion"),
    ('state f = 0\nfn f = 1\n', '2:4', "'f' is already declared as a state"),
    ('state n = 0\nview v = n(1)\n', '2:10', "'n' is a state, not a fn"),
    ('use missing\n', '1:5', "cannot find library 'missing' (missing.aup)"),
    ('view v = shapes.area(1, 2)\n', '1:17', "unknown library 'shapes'; write use shapes first"),
]

ACCEPTED = [
    # else on the next line, else if chains, multiline arguments, semicolons.
    'state a = 0\non frame {\n  if a > 1 {\n    a = 1\n  }\n  else {\n    a = 2\n  }\n}\n',
    'state a = 0; state b = 1\nview v = clamp(\n  a,\n  0,\n  b\n)\non frame { a = a + 1; b = b + 2 }\n',
    '# empty handlers and comments only\ninput k = key KeyZ\non k.down {\n}\non k.up { }\n',
    'state d: bytes by key\nstate edits = 0\ninput m = message by key { at: u32; text: bytes }\n'
    'input peek = message by key\non m {\n  d[key].replace(at_or_end .. at_or_end, m.text)\n}\n'.replace('at_or_end', 'min(m.at, d[key].bytes)')
    + 'on m {\n  edits = edits + 1\n  require m.text.bytes > 0\n}\non peek { reply d[key] }\n',
    # a keyed document loaded into a text state, edited with cursors and saved back as its UTF-8 bytes
    'state documents: bytes by key\nstate note: text\nstate at: cursor in note\n'
    'input open = message by key\ninput save = message by key\ninput typed = text\n'
    'on open {\n  note = text(documents[key])\n  at = note.end\n}\n'
    'on typed { note.insert(at, typed) }\n'
    'on save {\n  documents[key].replace(0 .. documents[key].bytes, note)\n  reply "saved {note.chars} characters"\n}\n',
    # a record literal over several lines, text defaults, inserting at the first item
    'record Item {\n  name: text = "untitled"\n  price = 0; count = -1\n}\n'
    'state items: list of Item\nstate at: index of items\ninput add = key KeyA\n'
    'on add.down {\n  items.insert(items.first, Item {\n    price: 2.5,\n    count: items.count + 1,\n  })\n'
    '  at = items.first\n  items[at].name = "{items[at].name} #{items.count}"\n}\nview total = items.count\n',
    # index arithmetic and positions in conditions and views
    'record Row {\n  label: text\n}\nstate rows: list of Row\nstate focus: index of rows\ninput next = key Tab\n'
    'on next.down {\n  if focus.position + 1 < rows.count {\n    focus = focus + 1\n  } else {\n    focus = rows.first\n  }\n}\n'
    'view summary = "{rows.count} rows, focus {focus.position}"\n',
    # conditionals: else-if chains, on the next line, inside arithmetic, over cursors, seven deep
    'state n: text = "ab"\nstate at: cursor in n\nstate x = 2\ninput k = key KeyK\n'
    'on k.down {\n  x = 1 + if x > 1 { 10 } else if x > 0 { 20 }\n  else { 30 }\n'
    '  at = if x > 5 { n.end } else { n.start }\n}\n'
    'view size = if x > 1 {\n  "big"\n} else {\n  "small"\n}\n'
    'view deep = ' + '1 + (if 1 { ' * 7 + '1' + ' } else { 0 })' * 7 + '\n',
    # a table whose columns read their row, other states and the row's position
    'record Cell {\n  label: text = "-"\n  weight = 1\n}\nstate cells: list of Cell\nstate at: index of cells\n'
    'input add = key KeyA\non add.down {\n  cells.append(Cell { weight: cells.count })\n  at.label = "#{at.weight}"\n}\n'
    'view grid = each cell in cells {\n  label = cell.label\n  heavy = cell.weight > 2\n'
    '  here = if cell.position == at.position { ">" } else { " " }\n}\n'
    'view other = each cell in cells { weight = cell.weight }\n',
    # fns: parameters of any kind, constants, calls inside fns, tables and conditionals
    'record T {\n  a = 0\n  s: text\n}\nstate xs: list of T\nstate at: index of xs\nstate note: text\n'
    'state c: cursor in note\nfn tag(i) = "{i.s}#{i.a}"\nfn limit = 3\nfn clip(x) = min(x, limit)\n'
    'fn later(cursor, n) = cursor + n char\ninput k = key KeyK\n'
    'on k.down {\n  xs.append(T { a: clip(xs.count), s: "q" })\n  c = later(c, 2)\n}\n'
    'view board = each r in xs {\n  tag = tag(r)\n}\nview cur = if xs.count > 0 { tag(at) } else { "-" }\n',
]


def run(*args, stdin=None, check=True):
    return subprocess.run([str(a) for a in args], input=stdin, capture_output=True, text=True,
                          check=check, timeout=60)


def run_bytes(*args):
    """Document bytes are arbitrary; compare outputs as bytes."""
    return subprocess.run([str(a) for a in args], capture_output=True, check=False, timeout=60)


def compile_module(aup, source_path, out):
    run(aup, 'build', source_path, '-o', out)
    return out.read_bytes()


def script(inputs, seed, steps=250):
    rng = random.Random(seed)
    held, lines = set(), []
    for _ in range(steps):
        if rng.random() < 0.4:
            lines.append(f'frame {rng.randint(1, 8)}')
        else:
            name = rng.choice(inputs)
            lines.append(f"{'up' if name in held else 'down'} {name}")
            held ^= {name}
        if rng.random() < 0.1:
            lines.append('show')
    lines.extend(f'up {name}' for name in sorted(held))
    return '\n'.join(lines) + '\n'


def same(a, b):
    return a == b and (a is None) == (b is None)


def parity(aup, pure_run, workbench, pair, work, seeds):
    source, reference, inputs, views = pair
    module = work / (Path(source).stem + '.pmod')
    compile_module(aup, ROOT / source, module)
    compared = failures = 0
    for seed in seeds:
        commands = script(inputs, seed)
        ours = run(pure_run, module, stdin=commands, check=False).stdout.splitlines()
        theirs = run('node', workbench / 'cli.mjs', reference, stdin=commands, check=False).stdout.splitlines()
        assert len(ours) == len(theirs) == commands.count('\n'), (source, seed, len(ours), len(theirs))
        for number, (line, other, command) in enumerate(zip(ours, theirs, commands.splitlines())):
            a, b = json.loads(line), json.loads(other)
            where = f'{source} seed {seed} command {number} ({command})'
            assert a['states'].keys() == b['states'].keys(), where
            for name, value in a['states'].items():
                assert value == b['states'][name], (where, name, value, b['states'][name])
            wb_views = {view['id']: view for view in b['views']}
            for name, (view_id, field) in views.items():
                assert same(a['views'][name], wb_views[view_id].get(field)), (where, name)
            assert a['tick'] == b['tick'] and a['successfulEvents'] == b['successfulEvents'], where
            # lastError is sticky in the workbench host; lastEvent is the latest event.
            status = (b['lastEvent'] or {}).get('status', 0)
            assert (a['fault'] != 'none') == (status != 0), (where, a['fault'], status)
            failures += status != 0
            compared += 1
    return compared, failures


KEYS = ['Space', 'Enter', 'ArrowLeft', 'ArrowRight', 'KeyA', 'KeyQ', 'Digit0', 'Digit9']


def random_program(rng):
    """A random well-formed v0a program: every one must compile and verify."""
    states = [f's{i}' for i in range(rng.randint(1, 6))]
    inputs = [f'i{i}' for i in range(rng.randint(0, 4))]
    keys = rng.sample(KEYS, len(inputs))

    def expr(depth, handler):
        choice = rng.random()
        if depth <= 0 or choice < 0.25:
            return rng.choice([str(rng.choice([0, 1, 2, 0.5, 1e-3, 1e300, 7.25])), rng.choice(states)]
                              + (['dt'] if handler else []))
        if choice < 0.4:
            return rng.choice(['-', '!']) + expr(depth - 1, handler)
        if choice < 0.55:
            name, count = rng.choice([('min', 2), ('max', 2), ('clamp', 3)])
            return f'{name}(' + ', '.join(expr(depth - 1, handler) for _ in range(count)) + ')'
        op = rng.choice(['+', '-', '*', '/', '<', '<=', '>', '>=', '==', '!=', '&&', '||'])
        return f'({expr(depth - 1, handler)} {op} {expr(depth - 1, handler)})'

    def body(depth, indent):
        lines = []
        for _ in range(rng.randint(0, 3)):
            if depth > 0 and rng.random() < 0.3:
                lines.append(f'{indent}if {expr(3, True)} {{')
                lines += body(depth - 1, indent + '  ')
                if rng.random() < 0.5:
                    lines.append(f'{indent}}} else {{')
                    lines += body(depth - 1, indent + '  ')
                lines.append(f'{indent}}}')
            else:
                lines.append(f'{indent}{rng.choice(states)} = {expr(3, True)}')
        return lines

    lines = [f'state {name} = {rng.choice(["0", "-1", "3.5", "+2"])}' for name in states]
    lines += [f'input {name} = key {key}' for name, key in zip(inputs, keys)]
    lines += [f'view v{i} = {expr(3, False)}' for i in range(rng.randint(0, 3))]
    events = ['frame'] + [f'{name}.{edge}' for name in inputs for edge in ('down', 'up')]
    for _ in range(rng.randint(0, 5)):
        lines.append(f'on {rng.choice(events)} {{')
        lines += body(3, '  ')
        lines.append('}')
    return '\n'.join(lines) + '\n', inputs


def properties(aup, pure_run, work, rounds):
    """Generated programs compile and verify; damaged sources never crash the
    compiler, and whatever it accepts, Pure accepts."""
    rng = random.Random(2026)
    accepted_damaged = over_limit = 0
    examples = [path.read_text() for path in sorted(ROOT.glob('examples/*.aup'))]
    for number in range(rounds):
        source, inputs = random_program(rng)
        path, module = work / 'generated.aup', work / 'generated.pmod'
        path.write_text(source)
        result = run(aup, 'build', path, '-o', module, check=False)
        if result.returncode == 1 and 'program exceeds 512 instructions' in result.stderr:
            over_limit += 1  # well formed but longer than a module may be
            continue
        assert result.returncode == 0, (source, result.stderr)
        commands = ''.join(rng.choice(['frame\n'] + [f'down {i}\nup {i}\n' for i in inputs]) for _ in range(8))
        lines = run(pure_run, module, stdin=commands + 'show\n', check=False).stdout.splitlines()
        assert lines and json.loads(lines[-1])['error'] is None, (source, lines[-1:])

        text = list(rng.choice(examples))
        for _ in range(rng.randint(1, 6)):
            at = rng.randrange(len(text))
            action = rng.random()
            if action < 0.4: del text[at]
            elif action < 0.8: text.insert(at, rng.choice('{}()=+-*/!<>.,;#\n abc0129eE'))
            else: text[at] = rng.choice('{}()=+-*/\n x')
        path.write_text(''.join(text))
        result = run(aup, 'build', path, '-o', module, check=False)
        assert result.returncode in (0, 1) and (result.returncode == 0) == (not result.stderr), result.stderr
        if result.returncode == 0:
            accepted_damaged += 1
            shown = run(pure_run, module, stdin='frame\nshow\n', check=False).stdout.splitlines()
            assert json.loads(shown[-1])['error'] is None or 'event failed' in shown[-1], shown
    return accepted_damaged, over_limit


def editor_parity(aup, pure, work, seeds, operations=160):
    """Differential run of the old and the new editor on separate stores."""
    old, new = pure / 'build/tree/pure-edit', pure / 'build/pure-edit-module'
    variants = [('examples/editor.aup', pure / 'examples/editor.pure'),
                ('examples/bounded-editor.aup', pure / 'examples/bounded-editor.pure')]
    compared, outcomes = 0, {}
    for source, program in variants:
        module = work / (Path(source).stem + '.pmod')
        compile_module(aup, ROOT / source, module)
        for seed in seeds:
            rng = random.Random(seed * 31 + len(source))
            stores = {'old': work / f'old-{seed}', 'new': work / f'new-{seed}'}
            for store in stores.values():
                if store.exists():
                    for path in sorted(store.rglob('*'), reverse=True):
                        path.unlink() if path.is_file() else path.rmdir()
            revision, size = {}, {}
            keys = ['a', 'b', 'long-key_1']
            for step in range(operations):
                key = rng.choice(keys) if rng.random() > 0.03 else 'bad.key'
                current, length = revision.get(key, 0), size.get(key, 0)
                choice = rng.random()
                pick = rng.random()
                base = (current if pick < 0.7 else max(current - 1, 0) if pick < 0.8 else 0 if pick < 0.9
                        else rng.choice([7, 9223372036854775807]))
                budget = ['--budget', str(rng.choice([4, 16]))] if rng.random() < 0.05 else []
                if choice < 0.35:
                    command = ['show', key]
                elif choice < 0.85:
                    offset = rng.randint(0, length + 3)
                    remove = rng.choice([0, 0, 1, 2, 5, length])
                    body = bytes(rng.randrange(256) for _ in range(rng.choice([0, 1, 5, 50, 5000])))
                    insert = work / f'insert-{step}'
                    insert.write_bytes(body)
                    command = ['edit', key, str(base), str(offset), str(remove), str(insert)]
                else:
                    if rng.random() < 0.5:
                        payload = bytes(rng.randrange(256) for _ in range(rng.randint(0, 12)))
                    else:
                        payload = (rng.randint(0, length).to_bytes(4, 'little') +
                                   rng.randint(0, 3).to_bytes(4, 'little') + b'raw')
                    raw = work / f'raw-{step}'
                    raw.write_bytes(payload)
                    command = ['apply', key, str(base), str(raw)]
                theirs = run_bytes(old, '--program', program, '--store', stores['old'], *budget, *command)
                ours = run_bytes(new, '--module', module, '--store', stores['new'], *budget, *command)
                where = f'{source} seed {seed} step {step}: {command}'
                assert (ours.returncode, ours.stdout, ours.stderr) == (theirs.returncode, theirs.stdout, theirs.stderr), \
                    (where, ours, theirs)
                compared += 1
                text = theirs.stderr.decode(errors='replace').strip().splitlines()
                kind = (json.loads(text[-1]).get('error', 'ok') if text and text[-1].startswith('{')
                        else text[-1] if text else 'ok') if theirs.returncode else 'ok'
                outcomes[f'{command[0]}:{kind}'] = outcomes.get(f'{command[0]}:{kind}', 0) + 1
                report = theirs.stderr if command[0] == 'show' else theirs.stdout
                if theirs.returncode == 0:
                    revision[key] = int(json.loads(report.decode().splitlines()[-1])['revision'])
                    shown = theirs if command[0] == 'show' else \
                        run_bytes(old, '--program', program, '--store', stores['old'], 'show', key)
                    size[key] = len(shown.stdout)
            # Each CLI reads the store the other one wrote.
            for key in keys:
                across = [run_bytes(old, '--program', program, '--store', stores['new'], 'show', key),
                          run_bytes(new, '--module', module, '--store', stores['old'], 'show', key),
                          run_bytes(old, '--program', program, '--store', stores['old'], 'show', key)]
                assert len({(r.returncode, r.stdout, r.stderr) for r in across}) == 1, (source, seed, key)
    return compared, dict(sorted(outcomes.items()))


class NotesModel:
    """Reference semantics of examples/notes.aup: positions are Unicode
    scalars; every cursor at or after an insertion point moves after it; a
    removal gathers cursors inside it; line moves keep the column."""

    def __init__(self):
        self.note, self.at, self.mark = '', 0, 0

    def line_start(self, i):
        return self.note.rfind('\n', 0, i) + 1

    def line_end(self, i):
        end = self.note.find('\n', i)
        return len(self.note) if end < 0 else end

    def insert(self, text):
        p = self.at
        self.note = self.note[:p] + text + self.note[p:]
        self.at, self.mark = [c + len(text) if c >= p else c for c in (self.at, self.mark)]

    def remove(self, a, b):
        self.note = self.note[:a] + self.note[b:]
        self.at, self.mark = [c - (b - a) if c >= b else a if c > a else c for c in (self.at, self.mark)]

    def key(self, name):
        n, at = len(self.note), self.at
        if name == 'left': self.at = max(0, at - 1)
        elif name == 'right': self.at = min(n, at + 1)
        elif name in ('up', 'down'):
            start = self.line_start(at)
            column = at - start
            if name == 'up':
                if start == 0: self.at = 0
                else:
                    previous = self.line_start(start - 1)
                    self.at = previous + min(column, start - 1 - previous)
            else:
                end = self.line_end(at)
                if end == n: self.at = n
                else:
                    self.at = end + 1 + min(column, self.line_end(end + 1) - end - 1)
        elif name == 'back': self.remove(max(0, at - 1), at)
        elif name == 'forward': self.remove(at, min(n, at + 1))
        elif name == 'home': self.at = 0
        elif name == 'finish': self.at = n
        elif name == 'set_mark': self.mark = at

    def expected(self):
        byte = lambda i: len(self.note[:i].encode())
        return ({'title': '메모', 'note': self.note, 'at': byte(self.at), 'mark': byte(self.mark)},
                {'header': f'메모 · {len(self.note)}자',
                 'current': self.note[self.line_start(self.at):self.line_end(self.at)],
                 'position': f"line {self.note[:self.at].count(chr(10)) + 1}, char {self.at}",
                 'size': len(self.note.encode()), 'lines': self.note.count('\n') + 1})


class TasksModel:
    """Reference semantics of examples/tasks.aup: the index names an item (0 in
    an empty list), stays on its item across inserts and removes before it,
    names the next item when its item is removed, and stops at the ends. An
    event that reads a missing item fails and changes nothing."""

    def __init__(self):
        self.tasks, self.at, self.made = [], 0, 0

    def last(self):
        return max(len(self.tasks) - 1, 0)

    def typed(self, word):
        self.tasks.append({'title': word, 'done': 0})
        self.at, self.made = self.last(), self.made + 1
        return 'none'

    def key(self, name):
        if name in ('toggle', 'drop', 'again', 'shout') and not self.tasks:
            return 'not_found'
        if name == 'up': self.at = max(self.at - 1, 0)
        elif name == 'down': self.at = min(self.at + 1, self.last())
        elif name == 'toggle': self.tasks[self.at]['done'] = int(self.tasks[self.at]['done'] == 0)
        elif name == 'drop':
            del self.tasks[self.at]
            self.at = min(self.at, self.last())
        elif name == 'top': self.at = 0
        elif name == 'bottom': self.at = self.last()
        elif name == 'again':
            self.tasks.insert(self.at, dict(self.tasks[self.at]))
            self.at += 1
        elif name == 'shout': self.tasks[self.at]['title'] += '!'
        return 'none'

    def expected(self):
        item = self.tasks[self.at] if self.tasks else None
        board = [{'title': task['title'], 'mark': 'x' if task['done'] else ' ', 'here': int(i == self.at)}
                 for i, task in enumerate(self.tasks)]
        return ({'tasks': [dict(task) for task in self.tasks], 'at': self.at, 'made': self.made},
                {'count': len(self.tasks), 'position': self.at,
                 'current': item['title'] if item else '(empty)',
                 'status': f'{self.at + 1}/{len(self.tasks)}' if item else 'empty',
                 'finished': item and item['done'],
                 'summary': f"{len(self.tasks)} {'task' if len(self.tasks) == 1 else 'tasks'}", 'board': board})


def tasks_parity(aup, pure_run, work, seeds, steps=400):
    module = work / 'tasks.pmod'
    compile_module(aup, ROOT / 'examples/tasks.aup', module)
    words = ['milk', '빵', '', 'a"b\\', '😀\n', '\u0000', '👩\u200d💻']
    keys = ['up', 'down', 'toggle', 'drop', 'drop', 'top', 'bottom', 'again', 'shout']
    compared = failed = 0
    for seed in seeds:
        rng = random.Random(seed + 9000)
        model, commands, expected = TasksModel(), [], []
        for _ in range(steps):
            # Appends and copies balance drops, so the list keeps coming back to empty.
            if rng.random() < 0.1:
                word = rng.choice(words)
                commands.append('type typed ' + (word.encode().hex() or '-'))
                fault = model.typed(word)
            else:
                name = rng.choice(keys)
                commands.append(f'down {name}')
                fault = model.key(name)
            expected.append((fault, *model.expected()))
        lines = run(pure_run, module, stdin='\n'.join(commands) + '\n', check=False).stdout.splitlines()
        assert len(lines) == len(commands), (seed, len(lines))
        for line, command, (fault, states, views) in zip(lines, commands, expected):
            snapshot = json.loads(line)
            where = (seed, command)
            assert snapshot['fault'] == fault and (snapshot['error'] is None) == (fault == 'none'), (where, snapshot)
            assert snapshot['states'] == states, (where, snapshot['states'], states)
            assert snapshot['views'] == views, (where, snapshot['views'], views)
            compared += 1
            failed += fault != 'none'
    # A replacement whose record gained and reordered fields keeps every item.
    (work / 'tasks2.aup').write_text('record Task {\n  urgent = 1\n  done = 0\n  title: text = "-"\n}\n'
                                     'state tasks: list of Task\nstate at: index of tasks\n')
    compile_module(aup, work / 'tasks2.aup', work / 'tasks2.pmod')
    lines = run(pure_run, module, stdin='type typed 6d696c6b\ntype typed 62\ndown toggle\ndown up\n'
                                        f'replace {work / "tasks2.pmod"}\n').stdout.splitlines()
    final = json.loads(lines[-1])
    assert final['states'] == {'tasks': [{'urgent': 1, 'done': 0, 'title': 'milk'},
                                         {'urgent': 1, 'done': 1, 'title': 'b'}], 'at': 0}, final
    return compared, failed


def libraries(aup, pure_run, work):
    """fn calls compute each argument once, libraries are found beside the
    source or on AUP_PATH, a library sees only its parameters, and errors in a
    library name its file."""
    lib, other = work / 'libs', work / 'libs' / 'far'
    other.mkdir(parents=True, exist_ok=True)
    (lib / 'shapes.aup').write_text('fn area(w, h) = w * h\nfn square(s) = area(s, s)\nfn unit = 1\n')
    (lib / 'loopa.aup').write_text('use loopb\nfn a = 1\n')
    (lib / 'loopb.aup').write_text('use loopa\nfn b = 2\n')
    (lib / 'stateful.aup').write_text('fn f = 1\nstate x = 0\n')
    (lib / 'peek.aup').write_text('fn f(a) = a + x\n')
    (other / 'far.aup').write_text('fn twice(x) = x * 2\n')
    env = {'AUP_PATH': f'{lib / "missing"}:{other}', 'PATH': '/usr/bin:/bin'}

    def build(source):
        path = lib / 'main.aup'
        path.write_text(source)
        return subprocess.run([str(aup), 'build', str(path), '-o', str(lib / 'main.pmod')], capture_output=True,
                              text=True, env=env, timeout=60)

    good = [
        ('use shapes\nuse far\nstate n = 3\nview a = shapes.square(n)\nview b = shapes.area(2, n + 1) + shapes.unit\n'
         'view c = far.twice(n)\n', {'a': 9, 'b': 9, 'c': 6}),
        ('fn sq(x) = x * x\nstate n = 3\nview v = sq(n + 1)\nview t = sq(sq(2))\n', {'v': 16, 't': 16}),
        ('fn first(a, b) = a\nview v = first(1, 1 / 0)\n', {'v': None}),   # arguments are always computed
    ]
    for source, views in good:
        result = build(source)
        assert result.returncode == 0, (source, result.stderr)
        first = (lib / 'main.pmod').read_bytes()
        assert build(source).returncode == 0 and (lib / 'main.pmod').read_bytes() == first, source
        shown = json.loads(run(pure_run, lib / 'main.pmod', stdin='show\n').stdout)
        assert shown['views'] == views, (source, shown['views'])
    bad = [
        ('use loopa\n', 'loopb.aup:1:5', "library 'loopa' uses itself through its uses"),
        ('use stateful\n', 'stateful.aup:2:1', 'a library declares only fn and use'),
        ('use peek\nstate x = 1\nview v = peek.f(1)\n', 'peek.aup:1:15',
         "'x' is not a parameter; a library fn reads only its parameters (in fn 'f' called at 3:15)"),
        ('use shapes\nview v = shapes.volume(1)\n', 'main.aup:2:17', "library 'shapes' has no fn 'volume'"),
    ]
    for source, where, message in bad:
        result = build(source)
        assert result.returncode == 1 and f'{where}: {message}' in result.stderr, (source, result.stderr)
    return len(good), len(bad)


def stored_runs(aup, pure_run, work, seeds, steps=240):
    """Declared state is stored state: the list app and the text editor run on
    a store, restarting (a new pure-run process) at random points, and keep
    matching their reference models across the restarts."""
    import shutil
    compared = restarts = 0
    for source in ('examples/tasks.aup', 'examples/notes.aup'):
        module = work / (Path(source).stem + '-stored.pmod')
        compile_module(aup, ROOT / source, module)
        for seed in seeds:
            rng = random.Random(seed + 11000)
            store = work / f'store-{Path(source).stem}-{seed}'
            if store.exists():
                shutil.rmtree(store)
            commands, expected = [], []
            if 'tasks' in source:
                model, words = TasksModel(), ['milk', '빵', '', 'a"b', '😀\n']
                keys = ['up', 'down', 'toggle', 'drop', 'top', 'bottom', 'again', 'shout']
                for _ in range(steps):
                    if rng.random() < 0.3:
                        word = rng.choice(words)
                        commands.append('type typed ' + (word.encode().hex() or '-'))
                        fault = model.typed(word)
                    else:
                        name = rng.choice(keys)
                        commands.append(f'down {name}')
                        fault = model.key(name)
                    expected.append((fault, *model.expected()))
            else:
                model, words = NotesModel(), ['a', '안녕', '\n', '😀', 'e\u0301']
                keys = ['left', 'right', 'up', 'down', 'back', 'forward', 'home', 'finish', 'set_mark']
                for _ in range(steps):
                    if rng.random() < 0.45:
                        word = rng.choice(words)
                        commands.append('type typed ' + word.encode().hex())
                        model.insert(word)
                    else:
                        name = rng.choice(keys)
                        commands.append(f'down {name}')
                        model.key(name)
                    expected.append(('none', *model.expected()))
            cuts = sorted(rng.sample(range(1, steps), 8))
            lines = []
            for number, (start, end) in enumerate(zip([0] + cuts, cuts + [steps])):
                chunk = '\n'.join(commands[start:end]) + '\n'
                batch = ['--batch', str(rng.randint(2, 9))] if number % 2 else []  # deferred saving, in bursts
                lines += run(pure_run, module, '--store', store, *batch, stdin=chunk, check=False).stdout.splitlines()
                restarts += 1
            assert len(lines) == steps, (source, seed, len(lines))
            for line, command, (fault, states, views) in zip(lines, commands, expected):
                snapshot = json.loads(line)
                where = (source, seed, command)
                assert snapshot['fault'] == fault, (where, snapshot)
                assert snapshot['states'] == states and snapshot['views'] == views, (where, snapshot, states, views)
                compared += 1
    return compared, restarts


class HttpHost:
    """A Pure HTTP host on a fresh port; stops with its statistics."""

    def __init__(self, args):
        import select
        self.process = subprocess.Popen([str(a) for a in args] + ['--port', '0'], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        ready, _, _ = select.select([self.process.stdout], [], [], 10)
        assert ready, 'HTTP host did not start'
        line = self.process.stdout.readline()
        assert line, self.process.communicate(timeout=5)
        self.port = json.loads(line)['port']

    def request(self, method, key, body=b'', expected=None):
        import http.client
        headers = {'Content-Length': str(len(body))}
        if expected is not None:
            headers['If-None-Match' if expected == 0 else 'If-Match'] = '*' if expected == 0 else f'"{expected}"'
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=30)
        try:
            connection.request(method, f'/objects/{key}', body, headers)
            response = connection.getresponse()
            return response.status, response.getheader('ETag'), response.getheader('X-Pure-Error'), response.read()
        finally:
            connection.close()

    def stop(self):
        self.process.terminate()
        _, error = self.process.communicate(timeout=30)
        assert self.process.returncode == 0, error[-1000:]
        return [json.loads(line) for line in error.decode().splitlines() if line.startswith('{')]


def server_parity(aup, pure, work, seeds, operations=150, threads=1):
    """The document server running editor.aup answers every request exactly
    as the server running editor.pure: status, revision, error and body."""
    import struct
    module = work / 'editor-server.pmod'
    compile_module(aup, ROOT / 'examples/editor.aup', module)
    compared = 0
    for seed in seeds:
        rng = random.Random(seed + 13000)
        old = HttpHost([pure / 'build/tree/pure-connections', '--program', pure / 'examples/editor.pure',
                        '--store', work / f'http-old-{seed}-{threads}'])
        new = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / f'http-new-{seed}-{threads}',
                        '--threads', str(threads)])
        try:
            revision, size = {}, {}
            for _ in range(operations):
                key = rng.choice(['a', 'b', 'long-key_1'])
                current, length = revision.get(key, 0), size.get(key, 0)
                if rng.random() < 0.35:
                    request = ('GET', key, b'', None)
                else:
                    pick = rng.random()
                    base = current if pick < 0.75 else max(current - 1, 0) if pick < 0.9 else current + 3
                    if rng.random() < 0.05:
                        body = bytes(rng.randrange(256) for _ in range(rng.randint(0, 7)))  # too short
                    else:
                        insert = bytes(rng.randrange(256) for _ in range(rng.choice([0, 1, 7, 300, 5000])))
                        body = struct.pack('<II', rng.randint(0, length + 2), rng.choice([0, 1, 3, length])) + insert
                    request = ('PUT', key, body, base)
                theirs, ours = old.request(*request), new.request(*request)
                assert ours == theirs, (seed, request[:2], request[3], ours[:3], theirs[:3])
                if theirs[0] == 200 and theirs[1]:
                    revision[key] = int(theirs[1].strip('"'))
                    if request[0] == 'GET':
                        size[key] = len(theirs[3])
                    else:
                        size[key] = len(old.request('GET', key)[3])
                        assert new.request('GET', key) == old.request('GET', key)
                compared += 1
        finally:
            old.stop()
            new.stop()
    return compared


def server_pipelining(aup, pure, work):
    """One connection, many requests: a keep-alive client and a pipelined
    burst (all requests in one write, one split across writes) get the same
    answers, in order, as separate connections would."""
    import http.client
    import socket
    import struct
    module = work / 'editor-pipeline.pmod'
    compile_module(aup, ROOT / 'examples/editor.aup', module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'pipeline'])
    try:
        connection = http.client.HTTPConnection('127.0.0.1', host.port, timeout=30)
        answers = []
        for body, headers in [(struct.pack('<II', 0, 0) + b'hello', {'If-None-Match': '*'}),
                              (struct.pack('<II', 5, 0) + b'!', {'If-Match': '"1"'}),
                              (struct.pack('<II', 0, 0) + b'x', {'If-Match': '"1"'})]:
            connection.request('PUT', '/objects/p', body, headers)
            response = connection.getresponse()
            answers.append((response.status, response.getheader('ETag'), response.read()))
        connection.request('GET', '/objects/p')
        response = connection.getresponse()
        answers.append((response.status, response.getheader('ETag'), response.read()))
        connection.close()
        assert answers == [(200, '"1"', b'hello'), (200, '"2"', b'!'), (412, '"2"', b''), (200, '"2"', b'hello!')], answers

        def request(method, key, body=b'', condition=''):
            return (f'{method} /objects/{key} HTTP/1.1\r\nHost: x\r\nContent-Length: {len(body)}\r\n{condition}\r\n'
                    .encode() + body)
        pipeline = (request('GET', 'p') + request('PUT', 'q', struct.pack('<II', 0, 0) + b'q1', 'If-None-Match: *\r\n') +
                    request('GET', 'q') + request('GET', 'missing') +
                    request('PUT', 'q', struct.pack('<II', 2, 0) + b'!', 'If-Match: "1"\r\n') + request('GET', 'q'))
        expected = [(200, b'hello!'), (200, b'q1'), (200, b'q1'), (404, b''), (200, b'!'), (200, b'q1!')]
        for split in (len(pipeline), 7, 61):  # one write, and small writes that cut heads and bodies
            with socket.create_connection(('127.0.0.1', host.port), timeout=30) as sock:
                for at in range(0, len(pipeline), split):
                    sock.sendall(pipeline[at:at + split])
                reader = sock.makefile('rb')
                for status, body in expected:
                    code = int(reader.readline().split()[1])
                    length = 0
                    for line in iter(reader.readline, b'\r\n'):
                        name, _, value = line.decode().partition(':')
                        if name.lower() == 'content-length':
                            length = int(value)
                    assert (code, reader.read(length)) == (status, body), (split, code, status, body)
            # The next round starts from the same documents.
            host.request('PUT', 'q', struct.pack('<II', 0, 3) + b'q1', int(host.request('GET', 'q')[1].strip('"')))
            expected = [(200, b'hello!'), (412, b''), (200, b'q1'), (404, b''), (412, b''), (200, b'q1')]
            pipeline = (request('GET', 'p') + request('PUT', 'q', struct.pack('<II', 0, 0) + b'q1', 'If-None-Match: *\r\n') +
                        request('GET', 'q') + request('GET', 'missing') +
                        request('PUT', 'q', struct.pack('<II', 2, 0) + b'!', 'If-Match: "1"\r\n') + request('GET', 'q'))
    finally:
        stats = host.stop()
    assert stats[0]['active'] == 0 and stats[0]['retained_payload_bytes'] == 0, stats
    return len(answers) + 3 * len(expected)


def server_lent_reads(aup, pure, work):
    """A read lends the stored document's extents instead of copying them: a
    1 MiB answer sent slowly still carries the version read while the
    document is replaced five times meanwhile."""
    import socket
    import struct
    module = work / 'editor-lent.pmod'
    compile_module(aup, ROOT / 'examples/editor.aup', module)
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'lent'])
    big = bytes(range(256)) * 4096
    try:
        assert host.request('PUT', 'big', struct.pack('<II', 0, 0) + big, 0)[0] == 200
        with socket.create_connection(('127.0.0.1', host.port), timeout=30) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.sendall(b'GET /objects/big HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n')
            data = sock.recv(100)
            for revision in range(1, 6):
                edit = struct.pack('<II', 0, len(big) if revision == 1 else 1) + b'Z'
                assert host.request('PUT', 'big', edit, revision)[0] == 200
            while b'\r\n\r\n' not in data or len(data) < data.index(b'\r\n\r\n') + 4 + len(big):
                chunk = sock.recv(65536)
                assert chunk, len(data)
                data += chunk
        assert data[data.index(b'\r\n\r\n') + 4:] == big
        assert host.request('GET', 'big') == (200, '"6"', 'none', b'Z')
    finally:
        stats = host.stop()
    assert stats[0]['active'] == 0 and stats[0]['retained_payload_bytes'] == 0, stats
    return len(big)


def server_lent_budget(aup, pure, work):
    """Lent extents count against the payload budget while an answer waits
    on its socket. Two readers pipeline 1 MiB reads without reading: the
    kernel takes what it can and each reader leaves one answer waiting. A
    third request finds no room (503 payload_budget) until a reader goes."""
    import socket
    import struct
    import time
    module = work / 'editor-budget.pmod'
    compile_module(aup, ROOT / 'examples/editor.aup', module)
    big = bytes(range(256)) * 4096
    budget = 2 * len(big) + len(big) // 2
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', work / 'lent-budget',
                     '--budget', budget])
    readers = []
    try:
        assert host.request('PUT', 'big', struct.pack('<II', 0, 0) + big, 0)[0] == 200
        for _ in range(2):
            sock = socket.create_connection(('127.0.0.1', host.port), timeout=30)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.sendall(b'GET /objects/big HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n' * 32)
            readers.append(sock)
        time.sleep(0.3)
        refused = host.request('GET', 'big')
        assert refused[0] == 503 and refused[2] == 'payload_budget', refused[:3]
        readers.pop().close()
        for _ in range(100):
            answered = host.request('GET', 'big')
            if answered[0] == 200:
                break
            time.sleep(0.05)
        assert answered[0] == 200 and answered[3] == big, answered[:3]
    finally:
        for sock in readers:
            sock.close()
        stats = host.stop()
    assert stats[0]['active'] == 0 and stats[0]['retained_payload_bytes'] == 0, stats
    return budget


def server_bursts(aup, pure, work, clients=8, edits=30, shards=1):
    """Concurrent edits share commits, and every acknowledged edit survives a
    kill -9 of the server."""
    import http.client
    import signal
    import struct
    import threading
    import time
    module = work / 'editor-bursts.pmod'
    compile_module(aup, ROOT / 'examples/editor.aup', module)

    def load(host, rounds, acknowledged):
        def client(number):
            key, base = f'c{number}', acknowledged[number][1]
            for _ in range(rounds):
                try:
                    status, tag, _, _ = host.request('PUT', key, struct.pack('<II', 0, 0) + b'x', base)
                except (OSError, http.client.HTTPException):  # the server died mid-answer: not acknowledged
                    return
                if status != 200:
                    return
                base = int(tag.strip('"'))
                acknowledged[number] = (acknowledged[number][0] + 1, base)
        threads = [threading.Thread(target=client, args=(n,)) for n in range(clients)]
        start = time.perf_counter()
        for thread in threads:
            thread.start()
        return threads, start

    timings = {}
    for name, args in [('program', [pure / 'build/tree/pure-connections', '--program', pure / 'examples/editor.pure']),
                       ('module', [pure / 'build/pure-serve', '--module', module, '--threads', str(shards)])]:
        host = HttpHost(args + ['--store', work / f'bursts-{name}-{shards}'])
        acknowledged = [(0, 0)] * clients
        threads, start = load(host, edits, acknowledged)
        for thread in threads:
            thread.join()
        timings[name] = time.perf_counter() - start
        assert all(count == edits for count, _ in acknowledged), acknowledged
        stats = host.stop()
        if name == 'module':
            bursts = stats[0]['bursts']
            assert stats[0]['burst_events'] == clients * edits and (clients == 1 or bursts < clients * edits), stats
    # Kill the module server while clients edit; what it acknowledged is stored.
    store = work / f'bursts-module-{shards}'
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', str(shards)])
    acknowledged = [(edits, edits)] * clients
    threads, _ = load(host, 1000, acknowledged)
    time.sleep(0.5)
    host.process.send_signal(signal.SIGKILL)
    host.process.wait()
    for thread in threads:
        thread.join()
    host = HttpHost([pure / 'build/pure-serve', '--module', module, '--store', store, '--threads', str(shards)])
    try:
        for number, (count, base) in enumerate(acknowledged):
            status, tag, _, body = host.request('GET', f'c{number}')
            stored = int(tag.strip('"'))
            assert status == 200 and stored in (base, base + 1) and len(body) == stored, (number, count, base, tag)
    finally:
        host.stop()
    return {'writes': clients * edits, 'bursts': bursts, 'program_seconds': round(timings['program'], 3),
            'module_seconds': round(timings['module'], 3),
            'acknowledged_before_kill': sum(count for count, _ in acknowledged) - clients * edits}


def conditionals(aup, pure_run, work):
    """Only the chosen branch runs (10 / x is never computed at x = 0), else-if
    chains pick the first true arm, and conditionals nest inside arithmetic."""
    source = work / 'conditionals.aup'
    source.write_text('state x = 0\ninput k = key KeyK\non k.down { x = x + 1 }\n'
                      'view grade = if x > 2 { "high" } else if x > 0 { "some" } else { "none" }\n'
                      'view safe = if x > 0 { 10 / x } else { 0 }\n'
                      'view nested = 1 + (if x == 1 { 2 * (if x > 0 { 3 } else { 4 }) } else { 5 })\n')
    compile_module(aup, source, work / 'conditionals.pmod')
    lines = run(pure_run, work / 'conditionals.pmod', stdin='show\n' + 'down k\n' * 3).stdout.splitlines()
    expected = [('none', 0, 6), ('some', 10, 7), ('some', 5, 6), ('high', 10 / 3, 6)]
    for line, (grade, safe, nested) in zip(lines, expected):
        views = json.loads(line)['views']
        assert views == {'grade': grade, 'safe': safe, 'nested': nested}, (views, grade, safe, nested)
    return len(expected)


def text_parity(aup, pure_run, work, seeds, steps=400):
    module = work / 'notes.pmod'
    compile_module(aup, ROOT / 'examples/notes.aup', module)
    words = ['a', 'Pure', '안녕', '\n', '😀', 'e\u0301', '한글\n줄', 'xy z', '', '\u0000', '👩\u200d💻']
    keys = ['left', 'right', 'up', 'down', 'back', 'forward', 'home', 'finish', 'set_mark']
    compared = 0
    for seed in seeds:
        rng = random.Random(seed + 7000)
        model, commands, expected = NotesModel(), [], []
        for _ in range(steps):
            if rng.random() < 0.45:
                word = rng.choice(words)
                commands.append('type typed ' + (word.encode().hex() or '-'))
                model.insert(word)
            else:
                name = rng.choice(keys)
                commands.append(f'down {name}')
                model.key(name)
            expected.append(model.expected())
        lines = run(pure_run, module, stdin='\n'.join(commands) + '\n').stdout.splitlines()
        assert len(lines) == len(commands), (seed, len(lines))
        for line, command, (states, views) in zip(lines, commands, expected):
            snapshot = json.loads(line)
            assert snapshot['error'] is None, (seed, command, snapshot)
            assert snapshot['states'] == states, (seed, command, snapshot['states'], states)
            assert snapshot['views'] == views, (seed, command, snapshot['views'], views)
            compared += 1
    # Escapes, interpolation and number text.
    source = work / 'strings.aup'
    source.write_text('state t: text = "a\\tb\\u{AC00}\\{x\\}\\"q\\"\\\\"\n'
                      'view v = "{t}|{1 / 4}|{1e21}|{0.1 + 0.2}|{-0}|{100}|{2.5e-7}|{-1234567.5}"\n')
    compile_module(aup, source, work / 'strings.pmod')
    shown = json.loads(run(pure_run, work / 'strings.pmod', stdin='show\n').stdout)
    assert shown['views']['v'] == 'a\tb가{x}"q"\\|0.25|1e+21|0.30000000000000004|0|100|2.5e-7|-1234567.5', shown
    return compared


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, required=True)
    parser.add_argument('--pure-run', type=Path, required=True)
    parser.add_argument('--pure', type=Path, default=ROOT.parent / 'pure' / 'prototypes' / 'connections')
    parser.add_argument('--editor-seeds', type=int, default=4)
    parser.add_argument('--text-seeds', type=int, default=12)
    parser.add_argument('--workbench', type=Path, default=ROOT.parent / 'pure' / 'prototypes' / 'workbench')
    parser.add_argument('--seeds', type=int, default=12)
    parser.add_argument('--generated', type=int, default=600)
    args = parser.parse_args()
    aup, pure_run, workbench = args.aup.resolve(), args.pure_run.resolve(), args.workbench.resolve()
    report = {}
    with tempfile.TemporaryDirectory(prefix='aup-check-') as directory:
        work = Path(directory)
        sources = sorted(ROOT.glob('examples/*.aup')) + sorted(HERE.glob('oracle/*.aup'))
        for path in sources:
            first = compile_module(aup, path, work / 'first.pmod')
            second = compile_module(aup, path, work / 'second.pmod')
            assert first == second, f'{path} is not deterministic'
            shown = json.loads(run(pure_run, work / 'first.pmod', stdin='show\n').stdout)
            assert shown['error'] is None, path
        report['verified_deterministic_modules'] = len(sources)

        for number, (source, where, message) in enumerate(DIAGNOSTICS):
            path = work / f'bad{number}.aup'
            path.write_text(source)
            result = run(aup, 'check', path, check=False)
            assert result.returncode == 1, (number, result.stderr)
            assert f'{path}:{where}' in result.stderr and message in result.stderr, (number, result.stderr)
        for number, source in enumerate(ACCEPTED):
            path = work / f'good{number}.aup'
            path.write_text(source)
            compile_module(aup, path, work / f'good{number}.pmod')
            assert json.loads(run(pure_run, work / f'good{number}.pmod', stdin='frame\nshow\n').stdout.splitlines()[-1])['error'] is None
        report['diagnostics'] = len(DIAGNOSTICS)
        report['accepted_forms'] = len(ACCEPTED)

        # Replacing a running module keeps states by name.
        compile_module(aup, ROOT / 'examples/counter.aup', work / 'counter.pmod')
        (work / 'counter2.aup').write_text('state bonus = 7\nstate count = 100\ninput tap = key Space\n'
                                           'view doubled = count * 2\non tap.down {\n  count = count + bonus\n}\n')
        compile_module(aup, work / 'counter2.aup', work / 'counter2.pmod')
        lines = run(pure_run, work / 'counter.pmod',
                    stdin=f'down tap\nup tap\ndown tap\nreplace {work / "counter2.pmod"}\ndown tap\n').stdout.splitlines()
        final = json.loads(lines[-1])
        assert final['states'] == {'bonus': 7, 'count': 9} and final['views'] == {'doubled': 18}, final
        report['replace_keeps_states_by_name'] = True
        accepted_damaged, over_limit = properties(aup, pure_run, work, args.generated)
        report['generated_programs_verified'] = args.generated - over_limit
        report['generated_programs_over_instruction_limit'] = over_limit
        report['damaged_sources_accepted_and_verified'] = accepted_damaged

        wb = workbench / 'examples'
        pairs = [
            ('examples/counter.aup', wb / 'counter.pure', ['tap'],
             {'label': ('label', 'value'), 'tile_width': ('tile', 'width')}),
            ('examples/toggle.aup', wb / 'toggle.pure', ['toggle', 'move'],
             {'marker_x': ('marker', 'x'), 'marker_radius': ('marker', 'radius'),
              'tile_x': ('tile', 'x'), 'value': ('value', 'value')}),
            ('examples/charge.aup', wb / 'charge.pure', ['jump'],
             {'actor_y': ('actor', 'y'), 'meter_width': ('meter', 'width'), 'amount': ('amount', 'value')}),
            ('examples/steps.aup', HERE / 'oracle/steps.pure', ['up', 'down'], {'shown': ('shown', 'value')}),
            ('tests/oracle/faults.aup', HERE / 'oracle/faults.pure', ['divide', 'grow', 'bound'],
             {'shown': ('shown', 'value')}),
        ]
        results = [parity(aup, pure_run, workbench, pair, work, range(args.seeds)) for pair in pairs]
        report['parity_snapshots'] = sum(compared for compared, _ in results)
        report['parity_failed_events_matched'] = sum(failures for _, failures in results)
        report['parity_programs'] = len(pairs)
        report['text_model_snapshots'] = text_parity(aup, pure_run, work, range(args.text_seeds))
        report['list_model_snapshots'], report['list_model_failed_events'] = \
            tasks_parity(aup, pure_run, work, range(args.text_seeds))
        report['conditional_snapshots'] = conditionals(aup, pure_run, work)
        report['stored_snapshots'], report['stored_restarts'] = stored_runs(aup, pure_run, work, range(args.text_seeds))
        report['library_programs'], report['library_diagnostics'] = libraries(aup, pure_run, work)
        report['editor_parity_operations'], report['editor_parity_outcomes'] = \
            editor_parity(aup, args.pure.resolve(), work, range(args.editor_seeds))
        report['server_parity_requests'] = sum(server_parity(aup, args.pure.resolve(), work, range(args.editor_seeds),
                                                             threads=threads) for threads in (1, 3))
        report['server_pipelined_answers'] = server_pipelining(aup, args.pure.resolve(), work)
        report['server_lent_read_bytes'] = server_lent_reads(aup, args.pure.resolve(), work)
        report['server_lent_budget_bytes'] = server_lent_budget(aup, args.pure.resolve(), work)
        report['server_bursts'] = server_bursts(aup, args.pure.resolve(), work)
        report['server_bursts_three_threads'] = server_bursts(aup, args.pure.resolve(), work, shards=3)
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
