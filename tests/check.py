#!/usr/bin/env python3
"""aup v0a checks. (Pure's HTTP host is checked in Pure: its tests/check.py
builds its apps with this compiler.)

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
8. Stored state: the list app and the text editor run on a tree store,
   restarting at random points (every other run saving in bursts), and still
   match their reference models.
7. List model: examples/tasks.aup keeps its list, index, views (conditional
   ones and a table with a row per task) and failed events exactly as a
   reference Python model under random appends, moves, toggles, copies,
   renames and removals; replacing it with a module whose record changed
   keeps each item's fields by name.
10. Watch: aup watch builds again when the source or a library it read is
   written, or a library appears where one was looked for; it keeps the last
   module while the source has an error (a library's error too), saving the
   same bytes builds nothing, a file caught halfway through being written is
   not built, and a module is only ever replaced whole.
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
KEYED = 'record T {\n  a = 0\n  s: text\n}\n'
KEYED_LIST = KEYED + 'state xs: list of T by s\nstate at: index of xs\n'

# Two lists, a number, a text and a key for lets.
LETS = ('record T {\n  a = 0\n}\nstate xs: list of T\nstate ys: list of T\nstate n = 0\nstate t: text\n'
        'input k = key KeyK\n')
# A list and a number beside views with arguments.
ARGS = 'record T {\n  a = 0\n}\nstate xs: list of T\nstate n = 0\n'
# A message with rows (a list of a record) beside a list state.
ROWS = 'record L {\n  item = 0\n  qty = 1\n}\nstate xs: list of L\nstate n = 0\ninput o = message {\n  rows: list of L\n}\n'
# An app with an output: bought items go out as orders.
# A keyed list, an index into it, a message and an output of the same fields: spreads and whole items.
CARDS = ('record Card {\n  title: text\n  lane = 0\n  note: text\n}\nstate cards: list of Card by title\n'
         'state at: index of cards\ninput m = message {\n  title: text\n  lane: u32\n  note: text\n}\n'
         'output o = message {\n  title: text\n  lane: u32\n  note: text\n}\n')
SENDS = ('state n = 0\ninput buy = message {\n  item: text\n  count: u32\n}\noutput ordered = message {\n'
         '  item: text\n  count: u32\n  to: {\n    city: text\n  }\n}\n')
DIAGNOSTICS = [
    ('state x = 1\nstate x = 2\n', '2:7', "already declared as a state"),
    ('state a = b\n', '1:11', 'expected a numeric state initializer'),
    ('view v = dt\n', '1:10', 'dt is only available in handlers'),
    ('input k = key Space\ninput j = key Space\n', '2:15', "key Space is already used by input 'k'"),
    ('input k = key F13\n', '1:15', "unknown key 'F13'"),
    ('on missing.down {\n}\n', '1:4', "unknown input 'missing'"),
    ('state a = 0\non a.down {\n}\n', '2:4', "'a' is a state, not an input"),
    ('state a = 0\non frame {\n  b = 1\n}\n', '3:3', "unknown state 'b'"),
    ('state a = 0\nview v = a\non frame {\n  v = 1\n}\n', '4:3', "cannot assign to a view 'v'"),
    ('input k = key Space\nview v = k\n', '2:10', "'k' is a key input, not a state"),
    ('state a = 0\nview v = min(a)\n', '2:10', 'min expects 2 arguments'),
    ('state if = 0\n', '1:7', "'if' is reserved"),
    ('state a = 0 $\n', '1:13', "unexpected character '$'"),
    ('state a = 1e400\n', '1:11', 'number must be finite'),
    ('view v = 1 +\n', '1:13', 'expected a number, state, or expression'),
    ('state a = 0\non frame {\n  a = 1 a\n}\n', '3:9', 'expected newline or semicolon'),
    ('state a = 0\ninput m = message { x: u32 y: number }\n', '2:28', 'expected a comma, newline or semicolon'),
    ('state a = 0\non frame {\n  if a {\n    a = 1\n  } els {\n  }\n}\n', '5:5', 'expected newline or semicolon'),
    # Each fn doubles what it inlines: f14 is past Pure's 65,535 instructions.
    ('\n'.join(['fn f0(x) = x + 1'] + [f'fn f{i}(x) = f{i - 1}(f{i - 1}(x))' for i in range(1, 15)] + ['view v = f14(1)']) + '\n',
     '1:14', "program exceeds 65535 instructions (in fn 'f0' called at 2:15)"),
    ('view v = ' + '(' * 70 + '1' + ')' * 70 + '\n', '1:', 'expression nesting exceeds 64'),
    ('state s = 0\n' * 1 + ''.join(f'state s{i} = 0\n' for i in range(32)), '33:7', 'at most 32 states are allowed'),
    # v0b: messages, keyed documents and types
    ('state d: bytes by key\ninput m = message\non m {\n  reply d[key]\n}\n', '4:9', 'd[key] needs the handler of a message by key'),
    ('input m = message by key { a: u32 }\non m {\n  reply m.a\n}\n', '3:9', 'reply takes bytes or text, not a number'),
    ('input m = message {\n  a: float\n}\n', '2:6', 'field types are u32, i64, number, bool, bytes, text and list of a record'),
    ('input m = message by key\non m.down {\n}\n', '2:4', "'m' is a message; write on m { }"),
    ('input k = key Space\non k {\n}\n', '2:4', "'k' is a key input; write on k.down or on k.up"),
    ('state d: bytes by key\ninput m = message by key { a: bytes }\non m {\n  d[1].replace(0 .. 0, m.a)\n}\n', '4:5',
     'a collection is indexed by the message key'),
    ('input m = message by key { a: u32 }\nview v = m.a\n', '2:10', "fields of 'm' are only available in its handler"),
    ('state x = 0\ninput m = message by key { a: bytes }\non m {\n  x = m.a\n}\n', '4:7', 'expected a number, not bytes'),
    ('input m = message by key { a: u32 }\non m {\n  reply m.z\n}\n', '3:9', "message 'm' has no field 'z'"),
    ('input m = message {\n  g: {\n    a: text\n  }\n}\non m {\n  reply m.g\n}\n', '7:9',
     "'m.g' is a group of fields; read one of them, as in m.g.a"),
    ('input m = message {\n  g: {\n    a: text\n  }\n}\non m {\n  reply m.g.b\n}\n', '7:9', "message 'm' has no field 'g.b'"),
    ('input m = message {\n  g: text\n  g: {\n    a: text\n  }\n}\n', '3:3', "duplicate field 'g'"),
    ('input m = message {\n  g: {\n    a: text\n  }\n  g: {\n    b: text\n  }\n}\n', '5:3', "duplicate field 'g'"),
    ('input m = message {\n  g: {\n  }\n}\n', '3:3', "the group 'g' has no fields"),
    (SENDS + 'on buy {\n  send ordered { item: buy.item, count: buy.count }\n}\n', '14:3', "send ordered needs 'to.city'"),
    (SENDS + 'on buy {\n  send ordered { item: buy.item, count: 1, to: "x" }\n}\n', '14:44',
     "'to' is a group of fields; give them in braces"),
    (SENDS + 'on buy {\n  send ordered { item: buy.item, count: 1, to: { town: "x" } }\n}\n', '14:50',
     "output 'ordered' has no field 'to.town'"),
    (SENDS + 'on buy {\n  send ordered { item: buy.count, count: 1, to: { city: "x" } }\n}\n', '14:24', 'expected text, not a number'),
    (SENDS + 'on buy {\n  send buy { item: buy.item }\n}\n', '14:3', "'buy' is a message, not an output"),
    (SENDS + 'on ordered {\n}\n', '13:4', "'ordered' is an output: the app sends it"),
    (SENDS + 'on buy {\n  send ordered { item: buy.item, item: buy.item, count: 1, to: { city: "x" } }\n}\n', '14:34',
     "field 'item' is given twice"),
    (SENDS + 'view v = ordered.item\n', '13:10', "'ordered' is an output"),
    ('input m = message {\n  group_of_things: {\n    another_long_name: text\n  }\n}\n', '3:5',
     "'group_of_things.another_long_name' is longer than 31 characters"),
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
    ('state a: text\nstate at: cursor in a\nview v = at\n', '3:10', 'a view is a number, a bool, an i64 or text, not a cursor'),
    ('state a: text\nstate at: cursor in a\nview v = "{at}"\n', '3:12', 'a string shows numbers, bools, i64s and text, not a cursor'),
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
    ('record T {\n  a: number\n}\n', '2:6', 'a field is a number (field = 0), a bool (field = false), text (field: text) or an i64 (field: i64)'),
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
    (RECORD + 'state xs: list of T\nstate at: index of xs\nview v = at\n', '6:10', 'a view is a number, a bool, an i64 or text, not an index'),
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
    ('view v = if true { 2 } else { "x" }\n', '1:31', 'the branches of if give a number and text'),
    ('state a: text\nstate b: text\nstate x: cursor in a\nstate y: cursor in b\n' + ON_K + '  x = if true { x } else { y }\n}\n',
     '7:28', 'the branches of if give cursors of different texts'),
    ('view v = ' + '1 + (if true { ' * 9 + '1' + ' } else { 0 })' * 9 + '\n', '1:135', 'conditional expressions nest too deeply'),
    # tables
    ('view t = each x in missing {\n  a = 1\n}\n', '1:20', "unknown list 'missing'"),
    (LIST + 'view t = each x in xs {\n}\n', '7:1', 'a table needs at least one column'),
    (LIST + 'view t = each x in xs {\n  a = 1\n  a = 2\n}\n', '8:3', "duplicate column 'a'"),
    (LIST + 'view t = each x in xs {\n  a = x.a\n}\nview v = x.a\n', '9:10', "unknown state 'x'"),
    (LIST + 'view t = each at in xs {\n  a = 1\n}\n', '6:15', "'at' is already declared as a list index"),
    (LIST + 'view t = each x in xs {\n  c = x\n}\n', '7:7', 'a column is a number, a bool, an i64 or text, not an index'),
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
    (LIST + 'view v = count(x in xs where count(y in xs) > 0)\n', '6:30', 'a count, sum, max or min cannot hold another'),
    (LIST + 'view v = count(x in at)\n', '6:10', "'at' is not a list"),
    ('record T {\n  t: text\n}\nstate xs: list of T\nview v = sum(x.t for x in xs)\n', '5:14',
     'sum, max, min and distinct take numbers or i64s, not text'),
    (LIST + 'fn f(n) = count(x in xs where x.a == n)\nview v = f(1)\n', '6:38',
     "'n' is a parameter outside this count, sum, max or min"),
    (LIST + 'view v = sum(dt for x in xs)\n', '6:14', 'dt is not available in count, sum, max or min'),
    (LIST + 'view v = sum(x.a for x xs)\n', '6:24', "expected 'in' and a list, as in sum(x in tasks)"),
    (LIST + 'view w = each r in xs where r {\n  n = r.a\n}\n', '6:29', 'expected a bool, not an index'),
    (LIST + 'view v = first(x in xs)\n', '6:10', 'a view is a number, a bool, an i64 or text, not an index'),
    (LIST + 'view w = each r in xs where count(y in xs) > 0 {\n  n = r.a\n}\n', '6:29',
     "a table's where cannot hold a count, sum, max or min"),
    # Keyed lists: a key field of the record, placed by add, found by key, never changed.
    (KEYED + 'state xs: list of T by missing\n', '5:24', "record 'T' has no field 'missing'"),
    (KEYED_LIST + ON_K + '  xs.append(T { s: "a" })\n}\n', '9:3', "'xs' is keyed by s: xs.add(T { ... }) places an item by its key"),
    (KEYED_LIST + ON_K + '  xs.insert(at, T { s: "a" })\n}\n', '9:3', "'xs' is keyed by s: xs.add(T { ... })"),
    (LIST + ON_K + '  xs.add(T { a: 1 })\n}\n', '8:3', "'xs' has no key: append or insert places its items (add needs list of T by a field)"),
    (KEYED_LIST + ON_K + '  at.s = "b"\n}\n', '9:3', "the key of 'xs' does not change; remove the item and add it again"),
    (KEYED_LIST + ON_K + '  xs["a"].s = "b"\n}\n', '9:3', "the key of 'xs' does not change"),
    (LIST + 'view v = xs.has(1)\n', '6:13', "'xs' has no key; declare it as list of T by a field to ask has"),
    (KEYED_LIST + 'view v = xs[1].a\n', '7:13', "'xs' is keyed by s, text: expected text or an index, not a number"),
    (KEYED_LIST + 'view v = xs.has(1)\n', '7:17', "'xs' is keyed by s, text: expected text, not a number"),
    (KEYED_LIST + 'view v = xs.has("a", "b")\n', '7:13', 'xs.has takes one key'),
    (KEYED_LIST + ON_K + '  at = xs[at]\n}\n', '9:8', 'xs[index] is the index itself; read a field, as in xs[index].a'),
    # Texts compare with texts, by their bytes.
    ('view v = "a" < 1\n', '1:16', 'expected text, not a number'),
    ('view v = 1 < "a"\n', '1:14', 'expected a number, not text'),
    # Time: now in handlers, timers, floor and a floored modulo.
    ('view v = now\n', '1:10', 'now is only available in handlers; a view shows a state that a timer or an event sets'),
    ('input t = every 0 ms\n', '1:17', 'a timer ticks every 1 ms to 24 hours, in whole milliseconds'),
    ('input t = every 1.5 ms\n', '1:17', 'a timer ticks every 1 ms to 24 hours, in whole milliseconds'),
    ('input t = every 25 hours\n', '1:17', 'a timer ticks every 1 ms to 24 hours, in whole milliseconds'),
    ('input t = every 2 days\n', '1:19', "a timer's unit is ms, second, minute or hour"),
    ('input t = every second\n', '1:17', 'expected a period, as in every 1 second'),
    ('input t = every 1 second\non t.down {\n}\n', '2:4', "'t' is a timer; write on t { }"),
    ('view v = floor(1, 2)\n', '1:10', 'floor expects 1 argument'),
    ('state now = 1\n', '1:7', "'now' is reserved"),
    # A for walks a list once; a message's rows are a list its handler reads.
    (ROWS + 'on o {\n  for a in xs {\n    for b in xs {\n      n = n + 1\n    }\n  }\n}\n', '12:5',
     'a for cannot hold another for'),
    (ROWS + 'on o {\n  for a in xs {\n    xs.append(L { item: 1 })\n  }\n}\n', '12:5',
     'a for does not add to the list it walks; add to another list, or after the for'),
    (ROWS + 'on o {\n  for a in o.rows {\n    reply "x"\n  }\n}\n', '12:5', 'a message replies once: reply after the for'),
    ('record L {\n  item = 0\n}\noutput o = message {\n  rows: list of L\n}\n', '5:9',
     'an output sends no rows: send one message a row'),
    ('input a = message like nothing\n', '1:24', "'nothing' is no message input or output declared before"),
    (CARDS + 'on m {\n  cards.add(Card { ..m, title: "x" })\n}\n', '19:25', 'a spread goes last'),
    (CARDS + 'on m {\n  cards.add(Card { ..1 })\n}\n', '19:22',
     "a spread takes an item (an index, or list[key]) or this handler's message"),
    (CARDS + 'record Other {\n  title: text\n}\nstate others: list of Other\nstate it: index of others\n'
     'on m {\n  send o { ..it }\n}\n', '24:3', "send o needs 'lane'"),
    (CARDS + 'on m {\n  cards[m.title] = Card { title: m.title }\n}\n', '19:20', "cards[...] = Card { ... } needs 'lane'"),
    (CARDS + 'on m {\n  cards[m.title] = 1\n}\n', '19:20', "'cards' holds Card records"),
    (CARDS + 'output g = message {\n  to: {\n    city: text\n  }\n}\non m {\n  send g { to: { ..m } }\n}\n', '24:18',
     "a spread goes in the send's own braces, not a group's"),
    (CARDS + 'input k = key Space\non k.down {\n  cards.add(Card { ..m })\n}\n', '20:22',
     "a spread takes an item (an index, or list[key]) or this handler's message"),
    ('input k = key Space\ninput a = message like k\n', '2:24', "'k' is no message input or output declared before"),
    ('input a = message like a\n', '1:24', "'a' is no message input or output declared before"),
    ('record L {\n  item = 0\n}\ninput i = message {\n  rows: list of L\n}\noutput o = message like i\n', '7:25',
     "an output sends no rows: 'i' has rows (rows)"),
    (ROWS + 'view v = o.rows.count\n', '10:10', "the rows of 'o' are only available in its handler"),
    (ROWS + 'on o {\n  n = o.rows\n}\n', '11:7',
     "'o.rows' holds a message's rows: walk them (for r in o.rows) or read .count, .first or .last"),
    (ROWS + 'on o {\n  for a in n {\n  }\n}\n', '11:12', "'n' is a state, not a list"),
    (ROWS + 'on o {\n  for n in xs {\n  }\n}\n', '11:7', "'n' is already declared as a state"),
    ('input o = message {\n  rows: list of Nope\n}\n', '2:17', "a list holds a record; 'Nope' is not one"),
    # A view's arguments are named, typed, its own, and only its.
    (ARGS + 'view v(from: u32) = from\n', '6:8', "from and count page a view's rows; name the argument otherwise"),
    (ARGS + 'view v(k: u32, k: u32) = k\n', '6:16', "duplicate argument 'k'"),
    (ARGS + 'view v(n: u32) = n\n', '6:8', "'n' is already declared as a state"),
    (ARGS + 'view t(r: u32) = each r in xs {\n  a = r.a\n}\n', '6:8', "'r' names the table's row"),
    (ARGS + 'view v(k: list) = 1\n', '6:11', 'an argument is a u32, i64, number, bool, bytes or text'),
    (ARGS + 'view v() = 1\n', '6:6', "'v()' takes no argument: write view v = ..."),
    (ARGS + 'view averylongviewnamethatistoolong(k: u32) = k\n', '6:6',
     'a view with arguments has a name of at most 26 characters'),
    (ARGS + 'view v(k: u32) = k\nview w = k\n', '7:10', "unknown state 'k'"),
    # A let holds a number or an index for the rest of its block, in its handler.
    (LETS + 'on k.down {\n  let n = 1\n}\n', '10:7', "'n' is already declared as a state"),
    (LETS + 'on k.down {\n  let x = 1\n  let x = 2\n}\n', '11:7', "'x' is already a let here"),
    (LETS + 'on k.down {\n  let x = t\n}\n', '10:11', 'a let holds a number, a bool, an i64 or an index, not text'),
    (LETS + 'on k.down {\n  let x = 1\n  x = xs.first\n}\n', '11:7', "'x' holds a number"),
    (LETS + 'on k.down {\n  let i = xs.first\n  i = ys.first\n}\n', '11:7', "'i' holds an index of its list"),
    (LETS + 'on k.down {\n  if n > 0 {\n    let x = 1\n  }\n  n = x\n}\n', '13:7', "unknown state 'x'"),
    (LETS + 'on k.down {\n  for r in xs {\n    let r = 1\n  }\n}\n', '11:9', "'r' is the item this for walks"),
    (LETS + 'view v = x\non k.down {\n  let x = 1\n}\n', '9:10', "unknown state 'x'"),
    (LETS + 'on k.down {\n  let let = 1\n}\n', '10:7', "'let' is reserved"),
    # A slice takes a list keyed by a number.
    (LETS + 'view v = count(x in xs[0 .. 3])\n', '9:10', "'xs' is not keyed by a number or an i64: a slice takes one"),
    (LETS + 'on k.down {\n  for x in xs[0 .. 1] {\n    n = 1\n  }\n}\n', '10:12', "'xs' is not keyed by a number or an i64: a slice takes one"),
    # A bool is not a number: conditions take bools, and a number says how to become one.
    ('state n = 0\non frame {\n  if n {\n    n = 1\n  }\n}\n', '3:6', 'if takes a bool, not a number: compare it, as in n != 0'),
    ('view v = if 1 { 2 } else { 3 }\n', '1:13', 'if takes a bool, not a number'),
    ('state n = 0\non frame {\n  require n\n}\n', '3:11', 'require takes a bool, not a number'),
    (LIST + 'view v = count(x in xs where x.a)\n', '6:30', 'where takes a bool, not a number'),
    (LIST + 'view t = each x in xs where x.a {\n  a = x.a\n}\n', '6:29', 'where takes a bool, not a number'),
    (LIST + ON_K + '  for x in xs where x.a {\n  }\n}\n', '8:21', 'where takes a bool, not a number'),
    ('state n = 0\nview v = !n\n', '2:11', '! takes a bool, not a number'),
    ('state n = 0\nview v = n > 0 && n\n', '2:19', '&& takes a bool, not a number'),
    ('state lit = false\nview v = lit + 1\n', '2:10', 'expected a number, not a bool'),
    ('state lit = false\nview v = -lit\n', '2:11', 'expected a number, not a bool'),
    ('state lit = false\nview v = lit < true\n', '2:14', 'bools are equal or not (== and !=); they have no order'),
    ('state lit = false\nview v = lit == 1\n', '2:17', 'expected a bool, not a number'),
    ('state lit = true\nstate n = 0\non frame {\n  n = lit\n}\n', '4:7', 'expected a number, not a bool'),
    ('state lit = true\non frame {\n  lit = 1\n}\n', '3:9', 'expected a bool, not a number'),
    ('record T {\n  a = false\n}\nstate xs: list of T by a\n', '4:24', "a list is keyed by a number or text; 'a' is a bool"),
    ('record T {\n  a = false\n}\nstate xs: list of T\nview v = sum(x.a for x in xs)\n', '5:', 'sum, max, min and distinct take numbers or i64s, not a bool'),
    ('record T {\n  a = false\n}\nstate xs: list of T\n' + ON_K + '  xs.append(T { a: 1 })\n}\n', '7:20', 'expected a bool, not a number'),
    ('input m = message {\n  ok: bool\n}\nstate n = 0\non m {\n  n = m.ok\n}\n', '6:7', 'expected a number, not a bool'),
    ('state lit = true\noutput o = message {\n  n: number\n}\n' + ON_K + '  send o { n: lit }\n}\n', '7:15', 'expected a number, not a bool'),
    ('state true = 1\n', '1:7', "'true' is reserved"),
    # An i64 is exact and whole, and does not mix with numbers: a literal takes the type the other side has.
    ('state n: i64 = 1.5\n', '1:16', "expected a whole number for an i64 state"),
    ('state n: i64 = 9223372036854775808\n', '1:16', "the number is past an i64's range"),
    ('state n: i64\nstate x = 0\nview v = n + x\n', '3:14', 'expected an i64, not a number: convert it, as in i64(x)'),
    ('state n: i64\nstate x = 0\nview v = x + n\n', '3:14', 'an i64 and a number do not mix: convert one, as in i64(x) or number(n)'),
    ('state n: i64\non frame {\n  n = 2.5\n}\n', '3:7', 'an i64 is a whole number; this one is not'),
    ('state n: i64\nstate x = 0\non frame {\n  n = x\n}\n', '4:7', 'expected an i64, not a number: convert it, as in i64(x)'),
    ('state n: i64\nstate x = 0\non frame {\n  x = n\n}\n', '4:7', 'expected a number, not an i64'),
    ('state n: i64\nview v = clamp(n, 0, 1)\n', '2:16', 'expected a number, not an i64'),
    ('state n: i64\nview v = floor(n)\n', '2:16', 'expected a number, not an i64'),
    ('state n: i64\nview v = i64(n)\n', '2:14', 'expected a number, not an i64'),
    ('state x = 0\nview v = number(x)\n', '2:17', 'expected an i64, not a number: convert it, as in i64(x)'),
    ('record T {\n  id: i64 = 1.5\n}\n', '2:13', "expected a whole number for an i64 field's default"),
    ('record T {\n  id: i64\n}\nstate xs: list of T by id\nview v = xs[1.5].id\n', '5:13',
     "'xs' is keyed by id, an i64: expected an i64 or an index, not a number"),
    ('state n: i64\nview v = if true { n } else { 1.5 }\n', '2:31', 'the branches of if give an i64 and a number'),
    ('state n: i64\nview v = n < true\n', '2:14', 'expected an i64, not a bool'),
    ('view v = 9223372036854775808 + i64(1)\n', '1:10', "the number is past an i64's range"),
    ('input m = message {\n  id: i64\n}\nstate x = 0\non m {\n  x = m.id\n}\n', '6:7', 'expected a number, not an i64'),
    ('record T {\n  a: i64\n}\nstate xs: list of T\nview v = sum(x.a for x in xs) + 1.5\n', '5:33',
     'an i64 is a whole number; this one is not'),
]

ACCEPTED = [
    # Literals beside numbers and i64s, nested deep: each chain is typed in one probe (a probe per level would take 2^30).
    'state x = 0\nstate y: i64\nview v = ' + '1 + (' * 30 + 'x' + ')' * 30 + '\nview w = ' + '1 + (' * 30 + 'y' + ')' * 30 + '\n',
    # i64s: states, fields, keys, slices, messages, outputs, arguments, lets, arithmetic, conditionals, folds, text.
    'record Order {\n  id: i64\n  total: i64 = -1\n  note: text\n}\nstate orders: list of Order by id\n'
    'state next: i64 = 9007199254740993\nstate sum: i64\nstate seen = 0\ninput put = message {\n  id: i64\n  total: i64\n}\n'
    'output told = message {\n  id: i64\n  big: bool\n}\ninput go = key KeyG\n'
    'on put {\n  orders.add(Order { id: put.id, total: put.total * 2 + 1, note: "{put.id}" })\n'
    '  send told { id: put.id, big: put.total > 1000 }\n}\n'
    'on go.down {\n  let step: = 0\n}\n'.replace('  let step: = 0\n', '  let step = next / 3 % 7 - 1\n  next = next + step\n'
    '  sum = sum + max(-step, 0) + min(1, next)\n  seen = number(step) + seen\n'
    '  for o in orders[0 .. next] where o.total != 0 {\n    o.total = -o.total\n  }\n  if orders.has(5) { orders[5].total = 9 }\n')
    + 'view count = count(o in orders[-5 .. 5])\nview most = max(o.total for o in orders)\nview kinds = distinct(o.total for o in orders)\n'
    'view pick = if seen > 0 { next } else if seen < 0 { 0 } else { -1 }\nview line = "{next}/{sum}"\n'
    'view from(low: i64) = sum(o.total for o in orders where o.id >= low)\nview back = i64(2.0) + i64(seen)\n'
    'view board = each o in orders where o.id > 0 {\n  id = o.id\n  total = o.total\n}\n',
    # Bools: states, fields, message and output fields, arguments, lets, views, columns, interpolation, has.
    'state lit = false\nstate n = 0\nrecord T {\n  done = false\n  title: text\n}\nstate ts: list of T\n'
    'record K {\n  name: text\n  flag = true\n}\nstate ks: list of K by name\ninput flip = key Space\n'
    'input m = message {\n  ok: bool\n  n: number\n  at: {\n    ok: bool\n  }\n}\noutput o = message {\n  ok: bool\n  n: number\n}\n'
    'on flip.down {\n  lit = !lit\n  let big = n > 2\n  if big == lit || !ks.has("a") { n = n + 1 }\n'
    '  ts.append(T { title: "{lit}" })\n  send o { ok: lit && big, n: n }\n}\n'
    'on m {\n  lit = m.ok != m.at.ok\n  ts.append(T { done: m.ok, title: "x" })\n  for t in ts where t.done {\n    t.done = false\n  }\n}\n'
    'view shown = lit\nview label = "{lit} {n > 1} {ts.count > 0 && lit}"\nview open = count(t in ts where !t.done)\n'
    'view first_done = first(t in ts where t.done).position\nview pick = if lit { "on" } else { "off" }\n'
    'view board = each t in ts where t.done != lit {\n  done = t.done\n  title = t.title\n  here = t.position == 0\n}\n'
    'view some(want: bool) = count(t in ts where t.done == want)\n',
    # Declared fields apart by commas, as a value's are (a message's, a group's, a record's, one after the last too).
    'record P { x = 0, y = 0, name: text }\nstate ps: list of P by x\n'
    'input add = message { x: u32, y: number, name: text }\n'
    'input ship = message {\n  to: { city: text, zip: u32 },\n  note: text,\n}\n'
    'on add {\n  ps.add(P { x: add.x, y: add.y, name: add.name })\n}\n'
    'on ship {\n  ps.add(P { x: ship.to.zip, name: ship.to.city })\n}\n',
    # distinct, and tests bounding a keyed list's key: a range walk (Pure reads only the items in bounds) and a range view.
    'record Line {\n  id = 0\n  item = 0\n  qty = 0\n  done = 0\n}\nstate lines: list of Line by id\n'
    'input deliver = message {\n  order: u32\n}\non deliver {\n  let start = deliver.order * 16\n'
    '  for l in lines where l.id >= start && l.id < start + 16 {\n    l.done = 1\n  }\n}\n'
    'view items(order: u32) = distinct(l.item for l in lines where l.id >= order * 16 && l.id < order * 16 + 16)\n'
    'view level = distinct(l.item for l in lines where l.qty < 10)\n',
    # Slices: a fold over the keys of one order whose test may fault, a walk over them, one that does nothing.
    'record L {\n  id = 0\n  item = 0\n  qty = 0\n}\nrecord S {\n  id = 0\n  quantity = 0\n}\nstate lines: list of L by id\n'
    'state stock: list of S by id\ninput k = message {\n  o: u32\n}\n'
    'view level(o: u32, t: number) = distinct(l.item for l in lines[o * 16 .. o * 16 + 16] where stock[l.item].quantity < t)\n'
    'on k {\n  for l in lines[k.o * 16 .. k.o * 16 + 16] {\n    l.qty = 0\n  }\n  for l in lines[0 .. 1] {\n  }\n}\n',
    # Lets: a sum in a for, an index let changing an item, a let in a for, one read in a fold, one per branch.
    'record Stock {\n  item = 0\n  left = 0\n  price = 0\n}\nrecord Line {\n  item = 0\n  qty = 1\n}\n'
    'state stock: list of Stock by item\nstate revenue = 0\nstate sold = 0\ninput order = message {\n  lines: list of Line\n}\n'
    'on order {\n  let total = 0\n  let count = 0\n  for l in order.lines {\n    let s = stock[l.item]\n'
    '    s.left = s.left - l.qty\n    require s.left >= 0\n    total = total + l.qty * s.price\n    count = count + l.qty\n  }\n'
    '  let extra = sum(l.qty for l in order.lines where l.qty > total)\n  if extra > 0 {\n    let bonus = extra * 2\n'
    '    revenue = revenue + bonus\n  } else {\n    let bonus = stock.first\n    bonus.left = bonus.left + 1\n  }\n'
    '  revenue = revenue + total\n  sold = sold + count\n}\n',
    # Views with arguments: a keyed lookup, a fold and a filtered table reading them, texts, a handler's fold reading its message.
    'record A {\n  aid = 0\n  bid = 0\n  balance = 0\n  owner: text\n}\nstate accounts: list of A by aid\nstate seen = 0\n'
    'input pay = message {\n  bid: u32\n}\non pay {\n  seen = count(a in accounts where a.bid == pay.bid)\n}\n'
    'view balance(aid: u32) = accounts[aid].balance\nview richer(low: number) = count(a in accounts where a.balance > low)\n'
    'view hello(name: text, times: u32) = "{name} x{times}"\n'
    'view branch(b: u32, over: number) = each a in accounts where a.bid == b && a.balance > over {\n  aid = a.aid\n'
    '  share = a.balance / sum(x.balance for x in accounts where x.bid == b)\n  owner = a.owner\n}\n'
    'view owned(who: text) = count(a in accounts where a.owner == who)\n',
    # A message's rows (in a group too) walked with for, a fold reading the walk's item, a walk that removes.
    'record Line {\n  item = 0\n  qty = 1\n  note: text\n}\nrecord Stock {\n  item = 0\n  left = 0\n}\n'
    'state stock: list of Stock by item\nstate lines = 0\n'
    'input order = message {\n  customer: u32\n  lines: list of Line\n  ship: {\n    rows: list of Line\n  }\n}\n'
    'on order {\n  for l in order.lines where l.qty > 0 {\n    stock[l.item].left = stock[l.item].left - l.qty\n'
    '    lines = lines + sum(m.qty for m in order.lines where m.item == l.item)\n  }\n'
    '  lines = lines + order.lines.count + order.ship.rows.count\n}\n'
    'input clean = key KeyC\non clean.down {\n  for s in stock where s.left <= 0 {\n    stock.remove(s)\n  }\n}\n'
    'view left = sum(s.left for s in stock)\n',
    # A fold in a column reads the column's row too (a correlated subquery).
    LIST + 'view w = each r in xs {\n  n = count(x in xs where x.a == r.a)\n}\n',
    SENDS + 'on buy {\n  n = n + 1\n  send ordered { to: { city: "Seoul" }, count: buy.count, item: buy.item }\n}\nview sent = n\n',
    # fns named like folds stay calls, and max of two values stays max
    'fn sum(a, b) = a + b\nfn count(x) = x\nview v = sum(1, count(2)) + max(3, 4) + min(\n  5,\n  6\n)\n',
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
    'view deep = ' + '1 + (if true { ' * 7 + '1' + ' } else { 0 })' * 7 + '\n',
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
    # A keyed list: added, found, changed, asked and removed by key; texts ordered by bytes.
    KEYED_LIST + ON_K + '  xs.add(T { s: "q" })\n  at = xs["q"]\n  xs["q"].a = xs["q"].a + 1\n  xs.remove("q")\n}\n'
    'view v = if xs.has("q") { 1 } else { 0 }\nview w = "a" < "b" && "b" >= "b" && "a" != "ab"\n',
    # Message fields of every type, variable ones anywhere and several of them.
    'state n = 0\nstate t: text\ninput m = message {\n  a: text\n  b: number\n  c: bytes\n  d: u32\n  e: text\n}\n'
    'on m {\n  n = m.b + m.d + m.c.bytes\n  t = "{m.a}{m.e}"\n}\n',
    # A timer, now, floor and %.
    'state last = 0\ninput tick = every 500 ms\ninput slow = every 2 hours\non tick { last = floor(now / 1000) % 60 }\n'
    'on slow { last = -7 % 3 }\nview v = last % 7\n',
    # A module of some 12,000 instructions (200 KB): hosts read it whole.
    '\n'.join(['fn f0(x) = x + 1'] + [f'fn f{i}(x) = f{i - 1}(f{i - 1}(x))' for i in range(1, 12)] + ['view v = f11(1)']) + '\n',
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


def strict(a, b):
    """Equal, and a bool is never a number (in Python True == 1)."""
    if isinstance(a, bool) or isinstance(b, bool):
        return type(a) is type(b) and a == b
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(strict(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(strict(x, y) for x, y in zip(a, b))
    return a == b


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
    """A random well-formed program of numbers, bools and i64s: every one must compile and verify."""
    states = [(f's{i}', rng.choice(['number', 'number', 'bool', 'i64'])) for i in range(rng.randint(1, 6))]
    numbers = [name for name, kind in states if kind == 'number']
    flags = [name for name, kind in states if kind == 'bool']
    integers = [name for name, kind in states if kind == 'i64']
    inputs = [f'i{i}' for i in range(rng.randint(0, 4))]
    keys = rng.sample(KEYS, len(inputs))

    def number(depth, handler):
        choice = rng.random()
        if depth <= 0 or choice < 0.25:
            return rng.choice([str(rng.choice([0, 1, 2, 0.5, 1e-3, 1e300, 7.25]))] + numbers + (['dt'] if handler else []))
        if choice < 0.35:
            return '-' + number(depth - 1, handler)
        if choice < 0.45:
            name, count = rng.choice([('min', 2), ('max', 2), ('clamp', 3)])
            return f'{name}(' + ', '.join(number(depth - 1, handler) for _ in range(count)) + ')'
        if choice < 0.5:
            return f'number({integer(depth - 1, handler)})'
        if choice < 0.6:
            return f'(if {truth(depth - 1, handler)} {{ {number(depth - 1, handler)} }} else {{ {number(depth - 1, handler)} }})'
        op = rng.choice(['+', '-', '*', '/'])
        return f'({number(depth - 1, handler)} {op} {number(depth - 1, handler)})'

    def integer(depth, handler):
        choice = rng.random()
        if depth <= 0 or choice < 0.25:
            literal = str(rng.choice([0, 1, 7, -3, 2**31, 2**53 + 1, 2**62, -2**62, 2**63 - 1]))
            return rng.choice([f'i64({literal})' if not integers or rng.random() < 0.5 else literal] + integers)
        if choice < 0.35:
            return f'i64({number(depth - 1, handler)})'
        if choice < 0.45:
            return f'{rng.choice(["min", "max"])}({integer(depth - 1, handler)}, {integer(depth - 1, handler)})'
        if choice < 0.55:
            return f'(if {truth(depth - 1, handler)} {{ {integer(depth - 1, handler)} }} else {{ {rng.choice(["0", "-1"])} }})'
        if choice < 0.6:
            return '-' + integer(depth - 1, handler)
        op = rng.choice(['+', '-', '*', '/', '%'])
        right = str(rng.choice([1, 2, 10, -3])) if rng.random() < 0.3 else integer(depth - 1, handler)
        return f'({integer(depth - 1, handler)} {op} {right})'

    def truth(depth, handler):
        choice = rng.random()
        if depth <= 0 or choice < 0.2:
            return rng.choice(['true', 'false'] + flags)
        if choice < 0.35:
            return '!' + truth(depth - 1, handler)
        if choice < 0.55:
            op = rng.choice(['<', '<=', '>', '>=', '==', '!='])
            return f'({number(depth - 1, handler)} {op} {number(depth - 1, handler)})'
        if choice < 0.7:
            op = rng.choice(['<', '<=', '>', '>=', '==', '!='])
            return f'({integer(depth - 1, handler)} {op} {integer(depth - 1, handler)})'
        op = rng.choice(['&&', '||', '==', '!='])
        return f'({truth(depth - 1, handler)} {op} {truth(depth - 1, handler)})'

    make = {'number': number, 'bool': truth, 'i64': integer}

    def body(depth, indent):
        lines = []
        for _ in range(rng.randint(0, 3)):
            if depth > 0 and rng.random() < 0.3:
                lines.append(f'{indent}if {truth(3, True)} {{')
                lines += body(depth - 1, indent + '  ')
                if rng.random() < 0.5:
                    lines.append(f'{indent}}} else {{')
                    lines += body(depth - 1, indent + '  ')
                lines.append(f'{indent}}}')
            else:
                name, kind = rng.choice(states)
                lines.append(f'{indent}{name} = {make[kind](3, True)}')
        return lines

    starts = {'number': ['0', '-1', '3.5', '+2'], 'bool': ['true', 'false'], 'i64': ['0', '-9', '9007199254740993']}
    lines = [f'state {name}{": i64" if kind == "i64" else ""} = {rng.choice(starts[kind])}' for name, kind in states]
    lines += [f'input {name} = key {key}' for name, key in zip(inputs, keys)]
    lines += [f'view v{i} = {rng.choice(list(make.values()))(3, False)}' for i in range(rng.randint(0, 3))]
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
        if result.returncode == 1 and 'program exceeds 65535 instructions' in result.stderr:
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
    old, new = pure / 'prototypes/connections/build/tree/pure-edit', pure / 'build/pure-edit-module'
    variants = [('examples/editor.aup', pure / 'prototypes/connections/examples/editor.pure'),
                ('examples/bounded-editor.aup', pure / 'prototypes/connections/examples/bounded-editor.pure')]
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
        self.at = self.at + len(text) if self.at >= p else self.at
        self.mark = self.mark + len(text) if self.mark > p else self.mark  # a mark stays at the insertion point

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
        self.tasks.append({'title': word, 'done': False})
        self.at, self.made = self.last(), self.made + 1
        return 'none'

    def key(self, name):
        if name in ('toggle', 'drop', 'again', 'shout') and not self.tasks:
            return 'not_found'
        if name == 'up': self.at = max(self.at - 1, 0)
        elif name == 'down': self.at = min(self.at + 1, self.last())
        elif name == 'toggle': self.tasks[self.at]['done'] = not self.tasks[self.at]['done']
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
        board = [{'title': task['title'], 'mark': 'x' if task['done'] else ' ', 'here': i == self.at}
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
            assert strict(snapshot['states'], states), (where, snapshot['states'], states)
            assert strict(snapshot['views'], views), (where, snapshot['views'], views)
            compared += 1
            failed += fault != 'none'
    # A replacement whose record gained and reordered fields keeps every item; done, a bool there, is a
    # number here (false and true are 0 and 1).
    (work / 'tasks2.aup').write_text('record Task {\n  urgent = 1\n  done = 0\n  title: text = "-"\n}\n'
                                     'state tasks: list of Task\nstate at: index of tasks\n')
    compile_module(aup, work / 'tasks2.aup', work / 'tasks2.pmod')
    lines = run(pure_run, module, stdin='type typed 6d696c6b\ntype typed 62\ndown toggle\ndown up\n'
                                        f'replace {work / "tasks2.pmod"}\n').stdout.splitlines()
    final = json.loads(lines[-1])
    assert strict(final['states'], {'tasks': [{'urgent': 1, 'done': 0, 'title': 'milk'},
                                              {'urgent': 1, 'done': 1, 'title': 'b'}], 'at': 0}), final
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
        ('use stateful\n', 'stateful.aup:2:1', 'a library declares only fn, record and use'),
        ('use peek\nstate x = 1\nview v = peek.f(1)\n', 'peek.aup:1:15',
         "'x' is not a parameter; a library fn reads only its parameters (in fn 'f' called at 3:15)"),
        ('use shapes\nview v = shapes.volume(1)\n', 'main.aup:2:17', "library 'shapes' has no fn 'volume'"),
    ]
    for source, where, message in bad:
        result = build(source)
        assert result.returncode == 1 and f'{where}: {message}' in result.stderr, (source, result.stderr)
    return len(good), len(bad)



def library_records(aup, pure_run, work):
    """A library declares records (docs/design/002, Pure's review 005): an
    app writes them LIBRARY.NAME, and they are NAME in its module, so the
    task list with its record moved into a library builds the same module,
    byte for byte. Only the records the app's lists and rows hold go into
    its module. A second module using the same library (a migration) reads
    and changes what the app stored, and the app reads it after. Names are
    one namespace in a module, however they were written."""
    import re
    import struct
    lib = work / 'record-libs'
    lib.mkdir(parents=True, exist_ok=True)
    env = {'PATH': '/usr/bin:/bin'}

    def build(name, source):
        path = lib / f'{name}.aup'
        path.write_text(source)
        return subprocess.run([str(aup), 'build', str(path), '-o', str(lib / f'{name}.pmod')], capture_output=True,
                              text=True, env=env, timeout=60)

    # The task list, its record moved into a library: the same module.
    tasks = (ROOT / 'examples' / 'tasks.aup').read_text()
    record = re.search(r'record Task \{.*?\n\}\n', tasks, re.S).group(0)
    (lib / 'words.aup').write_text((ROOT / 'examples' / 'words.aup').read_text())
    (lib / 'taskrecords.aup').write_text(record)
    moved = 'use taskrecords\n' + tasks.replace(record, '').replace('list of Task', 'list of taskrecords.Task') \
        .replace('Task {', 'taskrecords.Task {')
    assert build('tasks-before', tasks).returncode == 0 and build('tasks-moved', moved).returncode == 0
    assert (lib / 'tasks-before.pmod').read_bytes() == (lib / 'tasks-moved.pmod').read_bytes()
    # Only the records used: one of three.
    (lib / 'people.aup').write_text('record Person { name: text }\nrecord Team { name: text, size = 0 }\n'
                                    'record Badge { label: text }\nfn big(n) = n > 10\n')
    assert build('teams', 'use people\nstate teams: list of people.Team\ninput add = text\n'
                          'on add { teams.append(people.Team { name: add, size: 12 }) }\n'
                          'view big = count(t in teams where people.big(t.size))\n').returncode == 0
    module = (lib / 'teams.pmod').read_bytes()
    assert struct.unpack_from('<H', module, 30)[0] == 1, struct.unpack_from('<H', module, 30)
    lines = run(pure_run, lib / 'teams.pmod', stdin='type add 61\nshow\n').stdout.splitlines()
    assert json.loads(lines[-1])['views'] == {'big': 1}, lines[-1]
    # A migration: another module of the same library changes what the app stored.
    (lib / 'store.aup').write_text('record User { name: text, points = 0 }\n')
    assert build('main', 'use store\nstate users: list of store.User by name\ninput join = text\n'
                         'on join { users.add(store.User { name: join }) }\n'
                         'view count = users.count\nview points = sum(u.points for u in users)\n').returncode == 0
    assert build('migrate', 'use store\nstate users: list of store.User by name\ninput bump = key Space\n'
                            'on bump.down {\n  for u in users {\n    u.points = u.points + 10\n  }\n}\n').returncode == 0
    store = lib / 'users-store'
    run(pure_run, lib / 'main.pmod', '--store', store, stdin='type join 616e6e\ntype join 626f\ntype join 6379\n')
    run(pure_run, lib / 'migrate.pmod', '--store', store, stdin='down bump\nup bump\n')
    shown = json.loads(run(pure_run, lib / 'main.pmod', '--store', store, stdin='show\n').stdout.splitlines()[-1])
    assert shown['views'] == {'count': 3, 'points': 30}, shown
    # A message's rows of a library's record.
    assert build('rows', 'use store\ninput load = message {\n  rows: list of store.User\n}\nstate n = 0\n'
                         'on load {\n  n = n + load.rows.count\n}\nview loaded = n\n').returncode == 0, \
        build('rows', '').stderr
    (lib / 'other.aup').write_text('record User { name: text }\n')
    (lib / 'twofold.aup').write_text('record Item { n = 0 }\nfn Item = 1\n')
    (lib / 'stateful.aup').write_text('record R { n = 0 }\nstate x = 0\n')
    bad = [
        ('use store\nstate users: list of User\n', 'main.aup:2:7', "'User' is library store's record: write store.User"),
        ('use store\nstate users: list of store.Nope\n', 'main.aup:2:28', "library 'store' has no record 'Nope'"),
        ('state users: list of nope.User\n', 'main.aup:1:22', "unknown library 'nope'; write use nope first"),
        ('use store\nrecord User { n = 0 }\nstate users: list of store.User\n', 'main.aup:3:28',
         "store.User is 'User' in the module, as User is: two records cannot share a name"),
        ('use store\nuse other\nstate a: list of store.User\nstate b: list of other.User\n', 'main.aup:4:24',
         "other.User is 'User' in the module, as store.User is: two records cannot share a name"),
        ('use store\nstate User = 0\nstate users: list of store.User\n', 'main.aup:3:28',
         "store.User is 'User' in the module, and 'User' is a state here"),
        ('use store\nuse other\nstate users: list of store.User\ninput j = text\n'
         'on j { users.append(other.User { name: j }) }\n', 'main.aup:5:27', "'users' holds store.User, not other.User"),
        ('use store\nstate users: list of store.User\ninput j = text\non j { users.append(User { name: j }) }\n',
         'main.aup:4:21', "'users' holds store.User, not User"),
        ('use store\nstate users: list of store.User\ninput j = text\non j { users.append(store.Nope { name: j }) }\n',
         'main.aup:4:27', "library 'store' has no record 'Nope'"),
        ('use twofold\n', 'twofold.aup:2:4', "'Item' is already declared as a record"),
        ('use stateful\n', 'stateful.aup:2:1', 'a library declares only fn, record and use'),
    ]
    for source, where, message in bad:
        result = build('main', source)
        assert result.returncode == 1 and f'{where}: {message}' in result.stderr, (source, where, message, result.stderr)
    return {'moved_module_bytes': len((lib / 'tasks-moved.pmod').read_bytes()), 'migrated_points': 30,
            'diagnostics': len(bad)}


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
                assert strict(snapshot['states'], states) and strict(snapshot['views'], views), (where, snapshot, states, views)
                compared += 1
    return compared, restarts


FOLDS_SOURCE = """record Task {
  title: text
  done = false
  points = 0
}
state tasks: list of Task
state at: index of tasks
input add = text
input toggle = key Space
input up = key ArrowUp
input down = key ArrowDown
input drop = key Delete
input jump = key KeyJ
on add {
  tasks.append(Task { title: add, points: add.chars })
  at = tasks.last
}
on jump.down { at = first(t in tasks where !t.done) }
on toggle.down { at.done = !at.done }
on up.down { at = at - 1 }
on down.down { at = at + 1 }
on drop.down { tasks.remove(at) }
view open = count(t in tasks where !t.done)
view points = sum(t.points for t in tasks)
view done_points = sum(
  t.points for t in tasks
  where t.done
)
view longest = max(t.title.chars for t in tasks)
view fewest_open = min(t.points for t in tasks where !t.done)
view summary = "{count(t in tasks where !t.done)} open of {tasks.count}"
view first_open = first(t in tasks where !t.done).position
view at_now = at.position
view open_list = each t in tasks where !t.done {
  label = t.title
  item = t.position
}
"""


KEYED_SOURCE = """record Person {
  name: text
  visits = 0
}
record Score {
  value = 0
  label: text
}
state people: list of Person by name
state at: index of people
state known = false
state n = 0
state scores: list of Score by value
input join = text
input visit = text
input ask = text
input drop = key Delete
input up = key ArrowUp
input more = key KeyN
input less = key KeyP
input keep = key KeyM
input forget = key KeyR
on join {
  people.add(Person { name: join })
  at = people[join]
}
on visit {
  if !people.has(visit) { people.add(Person { name: visit }) }
  people[visit].visits = people[visit].visits + 1
  at = people[visit]
}
on ask { known = people.has(ask) }
on drop.down { people.remove(at) }
on up.down { at = at - 1 }
on more.down { n = n + 3 }
on less.down { n = n - 5 }
on keep.down { scores.add(Score { value: n, label: "{n}" }) }
on forget.down { scores.remove(n) }
view current = at.name
view position = at.position
view total = sum(p.visits for p in people)
view after_m = count(p in people where p.name > "m")
view lowest = if people.count > 0 { people.first.name } else { "-" }
view labels = each s in scores {
  label = s.label
}
view board = each p in people where p.visits > 0 {
  label = "{p.name} ({p.visits})"
}
"""


def keyed(aup, pure_run, work, seeds, steps=300):
    """A list keyed by a text and one keyed by a number, against a Python
    model: items in key order (texts by their bytes), each key once (a second
    add fails with exists and changes nothing), found by key, asked with has,
    removed by key; indices follow their items; texts compare by bytes."""
    source = work / 'keyed.aup'
    source.write_text(KEYED_SOURCE)
    compile_module(aup, source, work / 'keyed.pmod')
    words = ['m', 'b', 'zed', 'a', 'ab', '', '가', 'Z', 'm2', '😀', 'b\n']
    compared = failures = 0
    for seed in seeds:
        rng, people, at, known, n, scores = random.Random(seed + 7700), [], 0, False, 0, []
        commands, expected, faults = [], [], []
        key = lambda name: name.encode()

        def place(name):
            """Insert a person in key order; the index follows its item."""
            nonlocal at
            position = sum(1 for p in people if key(p['name']) < key(name))
            people.insert(position, {'name': name, 'visits': 0})
            if len(people) > 1 and at >= position: at += 1
            return position

        for _ in range(steps):
            roll, fault = rng.random(), 'none'
            names = [p['name'] for p in people]
            if roll < 0.45:
                name, word = rng.choice(['join', 'visit', 'ask']), rng.choice(words)
                commands.append(f'type {name} ' + (word.encode().hex() or '-'))
                if name == 'join':
                    if word in names: fault = 'exists'
                    else: at = place(word)
                elif name == 'visit':
                    if word not in names: place(word)
                    at = [p['name'] for p in people].index(word)
                    people[at]['visits'] += 1
                else: known = word in names
            else:
                name = rng.choice(['drop', 'up', 'more', 'less', 'keep', 'forget'])
                commands.append(f'down {name}')
                if name == 'drop':
                    if not people: fault = 'not_found'
                    else:
                        del people[at]
                        at = min(at, max(len(people) - 1, 0))
                elif name == 'up': at = max(at - 1, 0)
                elif name == 'more': n += 3
                elif name == 'less': n -= 5
                elif name == 'keep':
                    if any(s['value'] == n for s in scores): fault = 'exists'
                    else: scores.append({'value': n, 'label': str(n)}); scores.sort(key=lambda s: s['value'])
                elif not any(s['value'] == n for s in scores): fault = 'not_found'
                else: scores = [s for s in scores if s['value'] != n]
            faults.append(fault)
            expected.append(({'people': [dict(p) for p in people], 'at': at, 'known': known, 'n': n,
                              'scores': [dict(s) for s in scores]},
                             {'current': people[at]['name'] if people else None, 'position': at,
                              'total': sum(p['visits'] for p in people),
                              'after_m': sum(1 for p in people if key(p['name']) > b'm'),
                              'lowest': people[0]['name'] if people else '-',
                              'labels': [{'label': s['label']} for s in scores],
                              'board': [{'label': f"{p['name']} ({p['visits']})"} for p in people if p['visits'] > 0]}))
        lines = run(pure_run, work / 'keyed.pmod', stdin='\n'.join(commands) + '\n', check=False).stdout.splitlines()
        shown = [json.loads(line) for line in lines]
        assert strict([[s['states'], s['views']] for s in shown], [list(e) for e in expected]), \
            (seed, next(i for i, (s, e) in enumerate(zip(shown, expected)) if not strict([s['states'], s['views']], list(e))))
        # A failed event leaves the last fault; a later success does not clear it, so compare where it is set.
        got = [s['fault'] if s['error'] else 'none' for s in shown]
        assert got == faults, (seed, [(i, a, b) for i, (a, b) in enumerate(zip(got, faults)) if a != b][:3])
        compared += len(shown)
        failures += sum(1 for f in faults if f != 'none')
    return compared, failures


def folds(aup, pure_run, work, seeds, steps=300):
    """count, sum, max and min over a list, and a table filtered by a where,
    against a Python model after random adds, toggles, moves and removals (an
    empty max or min has no value; a filtered table's rows keep the list's
    order and its columns read the item)."""
    source = work / 'folds.aup'
    source.write_text(FOLDS_SOURCE)
    compile_module(aup, source, work / 'folds.pmod')
    words = ['a', 'bbb', '가나', '😀', '', 'x y z', '\n']
    compared = 0
    for seed in seeds:
        rng, tasks, at, commands, expected = random.Random(seed + 4200), [], 0, [], []
        for _ in range(steps):
            roll = rng.random()
            if roll < 0.35:
                word = rng.choice(words)
                commands.append('type add ' + (word.encode().hex() or '-'))
                tasks.append({'title': word, 'done': False, 'points': len(word)})
                at = len(tasks) - 1
            else:
                name = rng.choice(['toggle', 'up', 'down', 'drop', 'jump'])
                commands.append(f'down {name}')
                open_now = [i for i, t in enumerate(tasks) if not t['done']]
                if name == 'jump' and open_now: at = open_now[0]  # none open: the event fails
                elif name == 'up': at = max(at - 1, 0)
                elif name == 'down': at = min(at + 1, max(len(tasks) - 1, 0))
                elif tasks and name == 'toggle': tasks[at]['done'] = not tasks[at]['done']
                elif tasks and name == 'drop':
                    del tasks[at]
                    at = min(at, max(len(tasks) - 1, 0))
            open_tasks = [t for t in tasks if not t['done']]
            expected.append({'open': len(open_tasks), 'points': sum(t['points'] for t in tasks),
                             'done_points': sum(t['points'] for t in tasks if t['done']),
                             'longest': max((len(t['title']) for t in tasks), default=None),
                             'fewest_open': min((t['points'] for t in open_tasks), default=None),
                             'summary': f'{len(open_tasks)} open of {len(tasks)}',
                             'first_open': next((i for i, t in enumerate(tasks) if not t['done']), None), 'at_now': at,
                             'open_list': [{'label': t['title'], 'item': i} for i, t in enumerate(tasks) if not t['done']]})
        # Toggling or removing in an empty list fails; pure-run then exits 1 but shows every event.
        lines = run(pure_run, work / 'folds.pmod', stdin='\n'.join(commands) + '\n', check=False).stdout.splitlines()
        shown = [json.loads(line)['views'] for line in lines]
        assert strict(shown, expected), (seed, [(a, b) for a, b in zip(shown, expected) if not strict(a, b)][:2])
        compared += len(shown)
    return compared


TIME_SOURCE = """state started = 0
state last = 0
state ticks = 0
state spread = 0
input tick = every 250 ms
input slow = every 1 hour
input mix = key KeyM
on tick {
  if started == 0 { started = now }
  last = now
  ticks = ticks + 1
}
on slow { spread = (now - started) % 7 }
on mix.down { spread = floor(spread * 3.5 - 2) % -4 }
view elapsed = floor((last - started) / 1000)
view clock = "{floor(last / 3600000) % 24}:{floor(last / 60000) % 60}:{floor(last / 1000) % 60}"
view day = floor(last / 86400000)
"""


def timing(aup, pure_run, work, seeds, steps=300):
    """now, timers, floor and % against a Python model under random times
    (whole and fractional milliseconds, forwards and back), ticks of two
    timers and a key: % takes the divisor's sign as Python's does."""
    import math
    source = work / 'time.aup'
    source.write_text(TIME_SOURCE)
    compile_module(aup, source, work / 'time.pmod')
    compared = 0
    for seed in seeds:
        rng, now, started, last, ticks, spread = random.Random(seed + 5100), 0.0, 0.0, 0.0, 0, 0.0
        commands, expected = [], []
        for _ in range(steps):
            roll = rng.random()
            if roll < 0.3:
                now = rng.choice([now + rng.choice([250, 1000, 61000, 3600000]), rng.uniform(0, 2e12),
                                  float(rng.randrange(0, 10 ** 12)), now - 5000 if now > 5000 else now])
                commands.append(f'time {now!r}')
            elif roll < 0.6:
                commands.append('tick tick')
                if started == 0: started = now
                last, ticks = now, ticks + 1
            elif roll < 0.8:
                commands.append('tick slow')
                spread = (now - started) % 7
            else:
                commands.append('down mix')
                spread = math.floor(spread * 3.5 - 2) % -4
            views = {'elapsed': math.floor((last - started) / 1000),
                     'clock': f'{math.floor(last / 3600000) % 24}:{math.floor(last / 60000) % 60}:{math.floor(last / 1000) % 60}',
                     'day': math.floor(last / 86400000)}
            expected.append(({'started': started, 'last': last, 'ticks': ticks, 'spread': spread}, views))
        lines = run(pure_run, work / 'time.pmod', stdin='\n'.join(commands) + '\n').stdout.splitlines()
        shown = [json.loads(line) for line in lines]
        for number, (got, (states, views)) in enumerate(zip(shown, expected)):
            assert got['states'] == states and got['views'] == views, (seed, number, commands[number], got, states, views)
        compared += len(shown)
    return compared


def watching(aup, pure_run, work):
    """aup watch: whatever the last build read, read differently, builds
    again; errors keep the last module; the module is only ever replaced whole."""
    import os
    import time
    directory, far = work / 'watch', work / 'watch-far'
    directory.mkdir()
    far.mkdir()
    source, out = directory / 'app.aup', directory / 'app.pmod'
    library, beside = far / 'shared.aup', directory / 'shared.aup'
    library.write_text('fn twice(n) = n * 2\n')
    program = 'use shared\nstate count = {}\ninput tap = key Space\nview doubled = shared.twice(count)\n' \
              'on tap.down {{\n  count = count + 1\n}}\n'
    source.write_text(program.format(1))
    watcher = subprocess.Popen([str(aup), 'watch', str(source), '-o', str(out)], stderr=subprocess.PIPE, text=True,
                               env=dict(os.environ, AUP_PATH=str(far)))
    lines = []

    def heard(expected):
        """The next stderr line, which must contain expected."""
        line = watcher.stderr.readline()
        lines.append(line)
        assert expected in line, (expected, lines)

    def shown():
        return json.loads(run(pure_run, out, stdin='show\n').stdout)['views']['doubled']

    def write(path, text):
        path.write_text(text)

    try:
        heard(f'built {out}')
        assert shown() == 2
        write(source, program.format(21))
        heard(f'built {out}')
        assert shown() == 42
        write(source, program.format(21).replace('twice(count)', 'twice(missing)'))
        heard(f'{source}:4:')
        assert shown() == 42  # the last module stays
        write(source, program.format(5))
        heard(f'built {out}')
        assert shown() == 10
        write(library, 'fn twice(n) = n * 2 + 100\n')
        heard(f'built {out}')
        assert shown() == 110
        write(library, 'fn twice(n) = n * oops\n')
        heard(f'{library}:1:')
        assert shown() == 110
        write(library, 'fn twice(n) = n + n\n')  # the fix in the library alone builds again
        heard(f'built {out}')
        assert shown() == 10
        write(beside, 'fn twice(n) = n * 3\n')  # found first from now on
        heard(f'built {out}')
        assert shown() == 15
        write(library, 'fn twice(n) = 0\n')  # no longer read: nothing to build
        write(source, program.format(5))  # the same bytes again: nothing to build
        write(source, program.format(6))
        heard(f'built {out}')
        assert shown() == 18
        # Written in two parts at random moments; the first part alone (and the
        # empty file before it) compiles too, into a module without the view.
        rng = random.Random(10)
        for count in range(7, 17):
            time.sleep(rng.uniform(0, 0.05))
            text = program.format(count)
            cut = text.index('view')
            with open(source, 'w') as file:
                file.write(text[:cut])
                file.flush()
                time.sleep(0.02)
                file.write(text[cut:])
            heard(f'built {out}')
            assert shown() == 3 * count
    finally:
        watcher.terminate()
        assert watcher.wait(timeout=5) == 0
    assert sorted(path.name for path in directory.iterdir()) == ['app.aup', 'app.pmod', 'shared.aup'], 'no temporary left'
    return len(lines)


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


BOOLS_SOURCE = """record Task {
  title: text
  done = false
}
state lit = false
state n = 0
state tasks: list of Task
input flip = key Space
input add = text
input bump = key KeyB
on flip.down { lit = !lit }
on add { tasks.append(Task { title: add, done: lit }) }
on bump.down {
  let big = n > 1
  if lit && !big { n = n + 1 }
}
view shown = lit
view label = "{lit}/{n > 1}"
view open = count(t in tasks where !t.done)
view board = each t in tasks {
  title = t.title
  done = t.done
}
"""


def bools(aup, pure_run, work):
    """Bools are true and false in every output (states, list items, views,
    columns, interpolation), never 1 and 0; a module replaced keeps a number
    that becomes a bool when it is 0 or 1 (else the new default) and makes a
    bool a number, 0 or 1."""
    (work / 'bools.aup').write_text(BOOLS_SOURCE)
    (work / 'bools2.aup').write_text('state lit = 7\nstate n = false\n')
    compile_module(aup, work / 'bools.aup', work / 'bools.pmod')
    compile_module(aup, work / 'bools2.aup', work / 'bools2.pmod')
    commands = ['show', 'down flip', 'type add 61', 'down bump', 'down bump', 'down bump', 'down flip', 'type add 62',
                f'replace {work / "bools2.pmod"}']
    lines = run(pure_run, work / 'bools.pmod', stdin='\n'.join(commands) + '\n').stdout.splitlines()
    shown = [json.loads(line) for line in lines]
    a, b = {'title': 'a', 'done': True}, {'title': 'b', 'done': False}
    expected = [
        ({'lit': False, 'n': 0, 'tasks': []}, {'shown': False, 'label': 'false/false', 'open': 0, 'board': []}),
        ({'lit': True, 'n': 0, 'tasks': []}, {'shown': True, 'label': 'true/false', 'open': 0, 'board': []}),
        ({'lit': True, 'n': 0, 'tasks': [a]}, {'shown': True, 'label': 'true/false', 'open': 0, 'board': [a]}),
        ({'lit': True, 'n': 1, 'tasks': [a]}, {'shown': True, 'label': 'true/false', 'open': 0, 'board': [a]}),
        ({'lit': True, 'n': 2, 'tasks': [a]}, {'shown': True, 'label': 'true/true', 'open': 0, 'board': [a]}),
        ({'lit': True, 'n': 2, 'tasks': [a]}, {'shown': True, 'label': 'true/true', 'open': 0, 'board': [a]}),
        ({'lit': False, 'n': 2, 'tasks': [a]}, {'shown': False, 'label': 'false/true', 'open': 0, 'board': [a]}),
        ({'lit': False, 'n': 2, 'tasks': [a, b]}, {'shown': False, 'label': 'false/true', 'open': 1, 'board': [a, b]}),
        ({'lit': 0, 'n': False}, {}),  # lit, false, is 0; n, 2, is no bool: bools2's default
    ]
    assert len(shown) == len(expected), lines
    for snapshot, command, (states, views) in zip(shown, commands, expected):
        assert snapshot['error'] is None, (command, snapshot)
        assert strict(snapshot['states'], states) and strict(snapshot['views'], views), (command, snapshot, states, views)
    return len(shown)


I64_SOURCE = """record Row {
  id: i64
  n = 0
}
state rows: list of Row by id
state next: i64 = 9007199254740990
state n = 0
input add = key KeyA
input grow = key KeyG
input half = key KeyH
input far = key KeyF
on add.down {
  rows.add(Row { id: next, n: n })
  next = next + 1
  n = n + 1
}
on grow.down { next = next * 1024 }
on half.down { next = -next / 2 }
on far.down { n = number(next) }
view total = sum(r.id for r in rows)
view line = "{next} {i64(-7) / 2} {i64(-7) % 2} {i64(7) % -2} {-7 / 2}"
view least = min(r.id for r in rows)
"""


def integers(aup, pure_run, work):
    """i64s are exact past 2^53 in every output (states, items, views, text),
    arithmetic past their range fails as overflow and changes nothing,
    division and remainders floor, a conversion to a number fails where
    the value does not fit, and a module replaced keeps what fits."""
    (work / 'integers.aup').write_text(I64_SOURCE)
    (work / 'integers2.aup').write_text('record Row {\n  id = 0\n  n: i64\n}\nstate rows: list of Row\nstate next = 0\nstate n: i64\n')
    compile_module(aup, work / 'integers.aup', work / 'integers.pmod')
    compile_module(aup, work / 'integers2.aup', work / 'integers2.pmod')
    commands = ['show', 'down add', 'down add', 'down add', 'down add', 'down far', 'down grow', 'down half',
                f'replace {work / "integers2.pmod"}']
    shown = [json.loads(line) for line in run(pure_run, work / 'integers.pmod', stdin='\n'.join(commands) + '\n',
                                                 check=False).stdout.splitlines()]
    start = 2**53 - 2  # four rows: the last past 2^53
    rows = [{'id': start + k, 'n': k} for k in range(4)]
    assert strict(shown[0]['states'], {'rows': [], 'next': start, 'n': 0}), shown[0]
    assert shown[0]['views']['line'] == f'{start} -4 1 -1 -3.5', shown[0]  # i64s floored; literals alone are numbers
    assert strict(shown[4]['states'], {'rows': rows, 'next': start + 4, 'n': 4}), shown[4]
    assert strict(shown[4]['views'], {'total': 4 * start + 6, 'line': f'{start + 4} -4 1 -1 -3.5', 'least': start}), shown[4]
    assert shown[5]['fault'] == 'limit' and shown[5]['states']['n'] == 4, shown[5]  # 2^53 + 2 is no number
    assert shown[6]['fault'] == 'overflow' and shown[6]['states']['next'] == start + 4, shown[6]
    assert shown[7]['states']['next'] == -(start + 4) // 2, shown[7]  # floored, as Python's //
    # Replaced: next, within 2^53 now, a number; n an i64; ids past 2^53 no number (the default), the rest kept.
    final = shown[8]
    assert strict(final['states'], {'rows': [{'id': start + k if k < 3 else 0, 'n': k} for k in range(4)],
                                    'next': -(start + 4) // 2, 'n': 4}), final
    return len(shown)


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
    parser.add_argument('--pure', type=Path, default=ROOT.parent / 'pure')
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

        # message like X is X's fields, written once: the same module, byte for byte.
        fields = '  title: text\n  lane: u32\n  to: {\n    city: text\n  }\n'
        body = 'on remote {\n  n = remote.lane\n}\non tap.down {\n  send changed { title: "t", lane: 1, to: { city: "c" } }\n}\n'
        written = (f'state n = 0\ninput tap = key Space\noutput changed = message {{\n{fields}}}\n'
                   f'input remote = message {{\n{fields}}}\n{body}')
        liked = (f'state n = 0\ninput tap = key Space\noutput changed = message {{\n{fields}}}\n'
                 f'input remote = message like changed\n{body}')
        (work / 'written.aup').write_text(written)
        (work / 'liked.aup').write_text(liked)
        assert compile_module(aup, work / 'written.aup', work / 'written.pmod') == \
            compile_module(aup, work / 'liked.aup', work / 'liked.pmod'), 'message like differs from its fields written'
        report['message_like'] = True

        # A spread (..source) and a whole item (list[k] = Record { ... }) are their fields written out: the same module.
        spreads = (CARDS + 'on m {\n  if !cards.has(m.title) {\n    cards.add(Card { lane: 1, ..m })\n  } else {\n'
                   '    cards[m.title] = Card { ..m }\n  }\n  at = cards[m.title]\n  send o { note: "x", ..at }\n'
                   '  send o { ..cards[m.title] }\n}\n')
        written = (CARDS + 'on m {\n  if !cards.has(m.title) {\n    cards.add(Card { title: m.title, lane: 1, note: m.note })\n'
                   '  } else {\n    require cards[m.title].title == m.title\n    cards[m.title].lane = m.lane\n'
                   '    cards[m.title].note = m.note\n  }\n  at = cards[m.title]\n'
                   '  send o { title: at.title, lane: at.lane, note: "x" }\n'
                   '  send o { title: cards[m.title].title, lane: cards[m.title].lane, note: cards[m.title].note }\n}\n')
        (work / 'spreads.aup').write_text(spreads)
        (work / 'spreads-written.aup').write_text(written)
        assert compile_module(aup, work / 'spreads.aup', work / 'spreads.pmod') == \
            compile_module(aup, work / 'spreads-written.aup', work / 'spreads-written.pmod'), 'a spread differs from its fields'
        # A whole item keeps its key: one that names another fails its event, and changes nothing.
        (work / 'whole.aup').write_text('record T {\n  name: text\n  n = 0\n}\nstate ts: list of T by name\n'
                                        'input make = key KeyM\ninput right = key KeyR\ninput wrong = key KeyW\n'
                                        'on make.down { ts.add(T { name: "a", n: 1 }) }\n'
                                        'on right.down { ts["a"] = T { name: "a", n: 5 } }\n'
                                        'on wrong.down { ts["a"] = T { name: "b", n: 7 } }\n'
                                        'view total = sum(t.n for t in ts)\nview count = ts.count\n')
        compile_module(aup, work / 'whole.aup', work / 'whole.pmod')
        shown = [json.loads(line) for line in run(pure_run, work / 'whole.pmod', stdin='down make\ndown wrong\ndown right\n',
                                                  check=False).stdout.splitlines()]
        assert [(v['views']['total'], v['fault'], v['error']) for v in shown] == \
            [(1, 'none', None), (1, 'limit', 'event failed'), (5, 'none', None)], shown
        report['spreads'] = True

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
        report['bool_snapshots'] = bools(aup, pure_run, work)
        report['i64_snapshots'] = integers(aup, pure_run, work)
        report['time_snapshots'] = timing(aup, pure_run, work, range(args.seeds))
        report['watched_builds'] = watching(aup, pure_run, work)
        report['fold_snapshots'] = folds(aup, pure_run, work, range(args.seeds if hasattr(args, 'seeds') else 8))
        report['keyed_snapshots'], report['keyed_failed_events'] = keyed(aup, pure_run, work, range(args.seeds))
        report['stored_snapshots'], report['stored_restarts'] = stored_runs(aup, pure_run, work, range(args.text_seeds))
        report['library_programs'], report['library_diagnostics'] = libraries(aup, pure_run, work)
        report['library_records'] = library_records(aup, pure_run, work)
        report['editor_parity_operations'], report['editor_parity_outcomes'] = \
            editor_parity(aup, args.pure.resolve(), work, range(args.editor_seeds))
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
