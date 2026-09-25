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

DIAGNOSTICS = [
    ('state x = 1\nstate x = 2\n', '2:7', "already declared as a state"),
    ('state a = b\n', '1:11', 'expected a numeric state initializer'),
    ('view v = dt\n', '1:10', 'dt is only available in handlers'),
    ('input k = key Space\ninput j = key Space\n', '2:15', "key Space is already used by input 'k'"),
    ('input k = key Tab\n', '1:15', "unknown key 'Tab'"),
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
    ('input m = message by key { a: u32 }\non m {\n  reply m.a\n}\n', '3:9', 'expected bytes, not a number'),
    ('input m = message by key {\n  b: bytes\n  a: u32\n}\n', '3:3', 'a bytes field must be the last field'),
    ('input m = message by key\non m.down {\n}\n', '2:4', "'m' is a message; write on m { }"),
    ('input k = key Space\non k {\n}\n', '2:4', "'k' is a key input; write on k.down or on k.up"),
    ('state d: bytes by key\ninput m = message by key { a: bytes }\non m {\n  d[1].replace(0 .. 0, m.a)\n}\n', '4:5',
     'a collection is indexed by the message key'),
    ('input m = message by key { a: u32 }\nview v = m.a\n', '2:10', "fields of 'm' are only available in its handler"),
    ('state x = 0\ninput m = message by key { a: bytes }\non m {\n  x = m.a\n}\n', '4:7', 'expected a number, not bytes'),
    ('input m = message by key { a: u32 }\non m {\n  reply m.z\n}\n', '3:9', "message 'm' has no field 'z'"),
    ('state x = 0\non frame {\n  require x.bytes\n}\n', '3:13', '.bytes needs bytes, not a number'),
    ('state d: bytes by key\nstate e: bytes by key\n', '2:7', 'at most one keyed collection is allowed'),
    ('state d: bytes by key\ninput m = message by key { a: bytes }\non m {\n  d[key].replace(0 .. m.a, m.a)\n}\n', '4:23',
     'expected a number, not bytes'),
    ('input m = message by key { a: u32 }\non frame {\n  reply m.a\n}\n', '3:3', 'reply is only available in a message handler'),
]

ACCEPTED = [
    # else on the next line, else if chains, multiline arguments, semicolons.
    'state a = 0\non frame {\n  if a > 1 {\n    a = 1\n  }\n  else {\n    a = 2\n  }\n}\n',
    'state a = 0; state b = 1\nview v = clamp(\n  a,\n  0,\n  b\n)\non frame { a = a + 1; b = b + 2 }\n',
    '# empty handlers and comments only\ninput k = key KeyZ\non k.down {\n}\non k.up { }\n',
    'state d: bytes by key\nstate edits = 0\ninput m = message by key { at: u32; text: bytes }\n'
    'input peek = message by key\non m {\n  d[key].replace(at_or_end .. at_or_end, m.text)\n}\n'.replace('at_or_end', 'min(m.at, d[key].bytes)')
    + 'on m {\n  edits = edits + 1\n  require m.text.bytes > 0\n}\non peek { reply d[key] }\n',
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, required=True)
    parser.add_argument('--pure-run', type=Path, required=True)
    parser.add_argument('--pure', type=Path, default=ROOT.parent / 'pure' / 'prototypes' / 'connections')
    parser.add_argument('--editor-seeds', type=int, default=4)
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
        report['editor_parity_operations'], report['editor_parity_outcomes'] = \
            editor_parity(aup, args.pure.resolve(), work, range(args.editor_seeds))
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
