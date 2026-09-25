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
    ('input k = key Space\nview v = k\n', '2:10', "'k' is a input, not a state"),
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
]

ACCEPTED = [
    # else on the next line, else if chains, multiline arguments, semicolons.
    'state a = 0\non frame {\n  if a > 1 {\n    a = 1\n  }\n  else {\n    a = 2\n  }\n}\n',
    'state a = 0; state b = 1\nview v = clamp(\n  a,\n  0,\n  b\n)\non frame { a = a + 1; b = b + 2 }\n',
    '# empty handlers and comments only\ninput k = key KeyZ\non k.down {\n}\non k.up { }\n',
]


def run(*args, stdin=None, check=True):
    return subprocess.run([str(a) for a in args], input=stdin, capture_output=True, text=True,
                          check=check, timeout=60)


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
    accepted_damaged = 0
    examples = [path.read_text() for path in sorted(ROOT.glob('examples/*.aup'))]
    for number in range(rounds):
        source, inputs = random_program(rng)
        path, module = work / 'generated.aup', work / 'generated.pmod'
        path.write_text(source)
        result = run(aup, 'build', path, '-o', module, check=False)
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
    return accepted_damaged


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, required=True)
    parser.add_argument('--pure-run', type=Path, required=True)
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
        report['generated_programs_verified'] = args.generated
        report['damaged_sources_accepted_and_verified'] = properties(aup, pure_run, work, args.generated)

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
    print(json.dumps(report))


if __name__ == '__main__':
    sys.exit(main())
