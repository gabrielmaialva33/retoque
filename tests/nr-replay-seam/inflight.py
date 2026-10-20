#!/usr/bin/env python3
"""Stress manifest/schedule controls; no GPU semantics are inferred from these host checks."""
import copy
import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = ROOT / 'tests/dlssnr-loopback'
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location('run_replay', HERE / 'run_replay.py')
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
checks = 0


def check(value):
    global checks
    assert value
    checks += 1


def refuses(call):
    try:
        call()
    except ValueError:
        check(True)
    else:
        raise AssertionError('mutation not refused')


check(r.replay_mode({}) == dict(ring=0, cancel_every=0, recreate_every=0, completion='synchronous'))
for value in ('1', '5', '-2', '2.0', 'wat', '9999999999999'):
    refuses(lambda: r.replay_mode({'LOOPBACK_REPLAY_INFLIGHT': value}))
refuses(lambda: r.replay_mode({'LOOPBACK_REPLAY_CANCEL_EVERY': '16'}))
for n in (2, 3, 4):
    mode = r.replay_mode({'LOOPBACK_REPLAY_INFLIGHT': str(n), 'LOOPBACK_REPLAY_CANCEL_EVERY': '17',
                          'LOOPBACK_REPLAY_RECREATE_EVERY': '19'})
    frames = [dict(id=100 + i, score=True, color=dict(sha256='input')) for i in range(48)]
    lines = []
    for i, f in enumerate(frames):
        cancel = (i + 1) % 17 == 0
        rec = i >= n and (i + 1) % 19 == 0 and (i - n + 1) % 17 != 0
        lines.append(f'FLIGHT_REUSE frame={f["id"]} prior={frames[i-n]["id"] if i >= n else 0} '
                     f'age={n if i >= n else 0} recreated={int(rec)} pending={min(i,n-1)} ready={max(0,i-n+1)}')
        lines.append(f'FLIGHT frame={f["id"]} slot={i%n} executed={int(not cancel)} cancelled={int(cancel)} '
                     f'fence={i+1} completed={i+1}')
    log = '\n'.join(lines)
    events = r.flight_rows(log, frames, mode)
    check(len(events) == 48 and events[100 + n]['reset_age'] == n)
    check(sum(x['cancelled'] for x in events.values()) == 2)
    check(sum(x['recreated'] for x in events.values()) > 0)
    refuses(lambda: r.flight_rows(log.replace('completed=1\n', 'completed=0\n', 1), frames, mode))
    refuses(lambda: r.flight_rows(log.replace('completed=1\n', 'completed=2\n', 1), frames, mode))
    refuses(lambda: r.flight_rows(log.replace('prior=100', 'prior=101', 1), frames, mode))
    refuses(lambda: r.flight_rows(log + '\n' + lines[-1], frames, mode))
    refuses(lambda: r.flight_rows('\n'.join(lines[1:]), frames, mode))
    run = dict(replay_mode=mode, settings={'Cadence': '1'}, capture_sha256='capture', environment={},
               producer=dict(dll_sha256='original'), frames=[])
    for f in frames:
        e = events[f['id']]
        run['frames'].append(dict(id=f['id'], kind='skipped' if e['cancelled'] else 'every', recorded=True,
            production_skip=None, flight=e, interval=1, requested_passes=1, completed_passes=1, model_result=1,
            skip='harness-cancelled-not-submitted' if e['cancelled'] else None, output=dict(sha256='input')))
    good = {'every': run}
    check(all(x['ok'] for x in r.baseline_gates(good, copy.deepcopy(good)).values()))
    check(all(x['ok'] for x in r.stress_gates(dict(frames=frames), good).values()))
    for mutate in ('mode', 'pixels', 'recorded', 'flight', 'ids', 'settings', 'environment', 'missing'):
        bad = copy.deepcopy(good)
        if mutate == 'mode': bad['every']['replay_mode']['ring'] = 0
        if mutate == 'pixels': bad['every']['frames'][0]['output']['sha256'] = 'bad'
        if mutate == 'recorded': bad['every']['frames'][0]['recorded'] = False
        if mutate == 'flight': bad['every']['frames'][0]['flight']['executed'] = False
        if mutate == 'ids': bad['every']['frames'][0]['id'] += 1
        if mutate == 'settings': bad['every']['settings']['Cadence'] = '4'
        if mutate == 'environment': bad['every']['environment']['gpu'] = 'other'
        if mutate == 'missing': bad.clear()
        check(any(x['ok'] is False for x in r.baseline_gates(good, bad).values()))
    bad = copy.deepcopy(good)
    bad['every']['frames'][16]['output']['sha256'] = 'model-output'
    check(r.stress_gates(dict(frames=frames), bad)['every-cancelled-input-identical']['ok'] is False)
    bad = copy.deepcopy(good)
    for f in bad['every']['frames']:
        if f['flight']['cancelled']: f['recorded'] = False
    check(r.stress_gates(dict(frames=frames), bad)['every-cancelled-NR-exercised']['ok'] is False)
    bad = copy.deepcopy(good)
    for f in bad['every']['frames']:
        if f['flight']['recreated']:
            bad['every']['frames'][f['flight']['previous_frame'] - 100]['recorded'] = False
    check(r.stress_gates(dict(frames=frames), bad)['every-released-NR-exercised']['ok'] is False)
    bad = copy.deepcopy(good)
    for f in bad['every']['frames']: f['recorded'] = False
    check(r.stress_gates(dict(frames=frames), bad)['every-stress-active']['ok'] is False)

# Same admissions are a pixel oracle; different history is explicitly informational, never falsely PASS.
sync = copy.deepcopy(run)
sync['replay_mode'] = r.replay_mode({})
for f in sync['frames']:
    f['kind'] = 'every'
    f['skip'] = None
    f['flight'] = None
candidate = copy.deepcopy(sync)
candidate['replay_mode'] = mode
check(r.sync_gates({'every': candidate}, {'every': sync})['every-sync-comparison']['ok'] is True)
candidate['frames'][0]['output']['sha256'] = 'bad'
check(r.sync_gates({'every': candidate}, {'every': sync})['every-sync-comparison']['ok'] is False)
candidate['frames'][1]['kind'] = 'skipped'
check(r.sync_gates({'every': candidate}, {'every': sync})['every-sync-identical-history-prefix']['ok'] is False)
candidate['frames'][0]['kind'] = 'skipped'
check(r.sync_gates({'every': candidate}, {'every': sync})['every-sync-comparison']['ok'] is None)

source = (HERE / 'replay_nr.h').read_text()
collect = source[source.index('auto collect ='):source.index('for (size_t i = 0; i < plan.frames.size(); ++i)')]
check(collect.index('Wait(owner, slot.serial);') < collect.index('slot.host->Consume('))
submit = source[source.index('slot.serial = ++owner.serial;'):source.index('// Oldest first')]
check(submit.index('queue->Wait(') < submit.index('ExecuteCommandLists') < submit.index('queue->Signal('))
check('Wait(owner' not in submit and 'Consume(' not in submit)
check(source.index('collect(slot);') < source.index('slot.outcome = slot.host->Record(plan, i);'))
check(source.index('CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,\n') >= 0)
check('ExitProcess(1);' in source)
print(f'PASS: {checks} in-flight schedule/manifest/mutation checks (GPU validation is separate)')
