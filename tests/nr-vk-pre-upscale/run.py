#!/usr/bin/env python3
"""Pre-upscale rules, observer, runtime and production scope against fake boundaries, plus compiling mutants."""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
RULE = 'DlssNr_VkPreUpscale.h'
OBSERVER = 'DlssNr_VkPreObserve.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-pthread']


def build(root, output, mutant=False, case='cases.cpp'):
    flags = ['g++', '-std=c++20', '-O0', '-Wall', '-Wextra', '-Werror', '-pthread'] if mutant else FLAGS
    result = subprocess.run(flags + ['-I'+str(root/'retoque'), str(HERE/case), '-o', str(output)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def compact(text):
    return re.sub(r'\s+', ' ', text).strip()


def mutants(folder):
    rules = {
        'off admission': ('if (!a.enabled)', 'if (false)'),
        'stage admission': ('if (a.stage != 1)', 'if (false)'),
        'RR admission': ('if (a.feature == Feature::RayReconstruction)', 'if (false)'),
        'FG admission': ('if (a.feature == Feature::FrameGeneration)', 'if (false)'),
        'unknown feature admission': ('if (a.feature != Feature::SuperSampling)', 'if (false)'),
        'live RR admission': ('if (a.backend == Backend::RayReconstruction)', 'if (false)'),
        'bridge admission': ('if (a.backend != Backend::Native)', 'if (false)'),
        'identity admission': ('if (!a.identity)', 'if (false)'),
        'other-picture admission': ('if (!a.mainPicture)', 'if (false)'),
        'observation admission': ('if (!a.observed)', 'if (false)'),
        'secondary recording': ('!a.primary || !a.oneTime || a.simultaneous', '!a.oneTime || a.simultaneous'),
        'reusable recording': ('!a.primary || !a.oneTime || a.simultaneous', '!a.primary || a.simultaneous'),
        'simultaneous recording': ('!a.primary || !a.oneTime || a.simultaneous', '!a.primary || !a.oneTime'),
        'outstanding admission': ('if (a.outstanding)', 'if (false)'),
        'automatic post fallback': ('return outcome == Outcome::Unselected;',
                                    'return outcome == Outcome::Unselected || outcome == Outcome::Rejected;'),
        'second scratch loan': ('if (Busy() || !ValidTicket(ticket))', 'if (!ValidTicket(ticket))'),
        'invalid scratch loan': ('if (Busy() || !ValidTicket(ticket))', 'if (Busy())'),
        'wrong epoch retirement': ('if (SameTicket(_ticket, ticket))', 'if (_ticket.buffer == ticket.buffer)'),
    }
    observations = {
        'feature incarnation ignored': ('return current && *current == identity;',
                                        'return current && current->handle == identity.handle;'),
        'failed begin retires': ('if (!success) return; std::lock_guard lock(_mutex); auto it = Unique(_commands, buffer);',
                                 'if (!success) {} std::lock_guard lock(_mutex); auto it = Unique(_commands, buffer);'),
        'image handle reuse': ('i->second.incarnation != v->second.imageIncarnation', 'false'),
        'layout contradiction': ('|| conflict ||', '|| (conflict && false) ||'),
        'queue transfer accepted': ('sourceFamily != ~0u || targetFamily != ~0u',
                                    '(sourceFamily != ~0u && targetFamily != ~0u && false)'),
        'renderpass read': ('|| c->second.inside ||', '|| false ||'),
        'traditional transition retained': ('if (traditional) c->layouts.clear();', 'if (traditional) {}'),
        'push data borrowed or lost': ('std::memcpy(b.bytes.data(), data, size);',
                                      'std::memset(b.bytes.data(), 0, size);'),
        'device omitted from key equality': ('bool operator==(const ObjectKey&) const = default;',
                                             'bool operator==(const ObjectKey& other) const {return handle==other.handle;}'),
        'view creation reused': ('fact.imageIncarnation = i->second.incarnation; fact.incarnation = Next();',
                                 'fact.imageIncarnation = i->second.incarnation; fact.incarnation = fact.imageIncarnation;'),
        'view epoch ignored': ('current->incarnation == identity.incarnation', 'identity.incarnation != 0'),
        'pool reset crosses devices': ('if (h.device == device && c.pool == pool)', 'if (c.pool == pool && device != 0)'),
        'feature release ignores source': ('p.second.source == source && p.first.handle == handle',
                                           'source != 0 && p.first.handle == handle'),
        'unchanged ticket accepted': ('(!before || !SameTicket(*before, *after))', '(before.has_value() || true)'),
        'ticket device unchecked': ('after->device == c.device', 'c.device != 0'),
        'ticket buffer unchecked': ('after->buffer == buffer', 'buffer != 0'),
        'ticket reuse unchecked': ('after->reuse == reuse', '(reuse != VkRecording::Reuse::Unknown)'),
        'current epoch ignored': ('!SameTicket(observed, *current)', 'observed.buffer != current->buffer'),
        'current reuse ignored': ('observed.reuse != current->reuse', 'false'),
        'closed recording accepted': ('|| !c->second.recording || !c->second.ticket', '|| !c->second.ticket'),
        'descriptor offsets lost': ('b.offsets.assign(offsets, offsets + offsetCount);',
                                    'b.offsets.assign(offsetCount, 0);'),
    }
    count = 0
    for header, changes in ((RULE, rules), (OBSERVER, observations)):
        original = (ROOT/'retoque/dlssnr'/header).read_text()
        # Strip comments before whitespace normalization: line comments must not swallow the following code.
        normalized = compact(re.sub(r'//[^\n]*', '', original))
        for name, (old, new) in changes.items():
            assert normalized.count(old) >= 1, f'mutant no longer matches: {name}'
            root = folder/f'm{count}'
            target = root/'retoque/dlssnr'
            target.mkdir(parents=True)
            for source in (RULE, OBSERVER, 'DlssNr_VkRecording.h'):
                shutil.copyfile(ROOT/'retoque/dlssnr'/source, target/source)
            # Preserve preprocessor newlines when transforming a compacted header.
            mutated = normalized.replace(old, new, 1)
            if name == 'device omitted from key equality':
                mutated = mutated.replace('std::hash<Handle> {}(key.device)', 'size_t(0)')
            mutated = re.sub(r'(#pragma once|#include [<"][^>"]+[>"])', r'\1\n', mutated)
            (target/header).write_text(mutated)
            build(root, root/'cases', mutant=True)
            ran = subprocess.run([str(root/'cases')], capture_output=True, text=True)
            rejected = ran.returncode != 0 and 'Assertion' in ran.stderr
            if header == OBSERVER:
                build(root, root/'identity', mutant=True, case='identity.cpp')
                identity = subprocess.run([str(root/'identity')], capture_output=True, text=True)
                rejected |= identity.returncode != 0 and 'Assertion' in identity.stderr
                build(root, root/'recording', mutant=True, case='recording.cpp')
                recording = subprocess.run([str(root/'recording')], capture_output=True, text=True)
                rejected |= recording.returncode != 0 and 'Assertion' in recording.stderr
            assert rejected, f'mutant survived: {name}: {ran.stderr}'
            count += 1
    gate = (ROOT/'retoque/dlssnr/DlssNr_DriverInitGate.h').read_text()
    for name, old, new in (
        ('blocking init gate', '_entry(gate.TryEnter(hold))', '_entry(gate.Enter(hold))'),
        ('busy gate admitted', 'if (_depth != 0 && _owner != std::this_thread::get_id())',
         'if (false && _depth != 0 && _owner != std::this_thread::get_id())'),
    ):
        assert old in gate, name
        root = folder/f'g{count}'
        target = root/'retoque/dlssnr'
        target.mkdir(parents=True)
        (target/'DlssNr_DriverInitGate.h').write_text(gate.replace(old,new,1))
        build(root, root/'gate', mutant=True, case='gate.cpp')
        result = subprocess.run([str(root/'gate')], capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and 'Assertion' in result.stderr, name
        count += 1
    placement = (ROOT/'retoque/dlssnr/DlssNr_VkPreScope.h').read_text()
    for name, old, new in (
        ('pre overrides a post caller', 'return expected != Post;', 'return true;'),
        ('post bypasses a pre loan', 'return expected == Pre;', 'return false;'),
    ):
        assert old in placement, name
        root = folder/f'p{count}'
        target = root/'retoque/dlssnr'
        target.mkdir(parents=True)
        (target/'DlssNr_VkPreScope.h').write_text(placement.replace(old,new,1))
        build(root, root/'placement', mutant=True, case='placement.cpp')
        result = subprocess.run([str(root/'placement')], capture_output=True, text=True)
        assert result.returncode != 0 and 'Assertion' in result.stderr, name
        count += 1
    print(f'PASS: {count} compiling rule/observation/gate/placement mutants rejected by behavioral assertions')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--no-mutants', action='store_true')
    args = parser.parse_args()
    from source_contracts import run
    run()
    for name in (RULE, OBSERVER):
        assert (ROOT/'retoque/dlssnr'/name).is_file(), f'missing production contract: {name}'
    (HERE/'artifacts').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=HERE/'artifacts', prefix='nr-vk-pre-') as tmp:
        folder = Path(tmp)
        out = folder/'cases'
        build(ROOT, out)
        subprocess.run([str(out)], check=True)
        for case in ('identity.cpp', 'recording.cpp', 'scope.cpp', 'gate.cpp', 'placement.cpp'):
            build(ROOT, out, case=case)
            subprocess.run([str(out)], check=True)
        from runtime_contracts import run as runtime_contracts
        runtime_contracts(folder, mutants=not args.no_mutants)
        from history_contracts import run as history_contracts
        history_contracts(folder, mutants=not args.no_mutants)
        from scope_contracts import run as scope_contracts
        scope_contracts(folder, mutants=not args.no_mutants)
        if not args.no_mutants:
            mutants(folder)
    print('PASS: host contracts only; no Vulkan, NGX, pixels or performance qualification')


if __name__ == '__main__':
    main()
