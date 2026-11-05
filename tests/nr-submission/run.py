#!/usr/bin/env python3
"""Test the production lifetime model; no GPU, Wine or game side effects."""
from pathlib import Path
import subprocess

here = Path(__file__).resolve().parent
root = here.parents[1]
out = here / 'artifacts'
out.mkdir(exist_ok=True)
subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(root),
                str(here / 'model.cpp'), '-o', str(out / 'model')], check=True)
subprocess.run([str(out / 'model')], check=True)

# Boundary guard: all original queue-call paths must notify, always after the call.
source = (root / 'OptiScaler/resource_tracking/ResTrack_dx12.cpp').read_text()
body = source.split('void ResTrack_Dx12::hkExecuteCommandLists(', 1)[1].split('#pragma region Heap hooks', 1)[0]
call_count = body.count('o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);')
assert call_count >= 1
assert body.count('nrSubmission.Submitted();') == call_count
for tail in body.split('o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);')[1:]:
    assert tail.lstrip().startswith('nrSubmission.Submitted();')
assert body.index('Submission::Batch') < body.index('o_ExecuteCommandLists(')
print('PASS: queue-hook paths reserve before execution and notify after original call')

# COMPUTE lists are followed only for a caller that opts in, and today that is the comparison capture alone: every
# other Track caller, the NR chain first, keeps the DIRECT-only rule it was validated under.
import re
track_impl = (root / 'retoque/dlssnr/DlssNr_Submission.cpp').read_text()
assert 'Detail::ListTypeAdmitted(static_cast<uint32_t>(list->GetType()), lists, chain)' in track_impl
assert 'static_assert(Detail::ListTypeCompute == D3D12_COMMAND_LIST_TYPE_COMPUTE);' in track_impl
assert 'list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT' not in track_impl
opted = []
for path in list((root / 'retoque').rglob('*.cpp')) + list((root / 'retoque').rglob('*.h')):
    text = path.read_text(errors='replace')
    if 'Lists::DirectOrCompute' in text and path.name not in ('DlssNr_SubmissionModel.h',):
        opted.append(path.name)
assert opted == ['DlssNr_PairCapture.cpp'], opted
capture = (root / 'retoque/dlssnr/DlssNr_PairCapture.cpp').read_text()
assert 'DlssNr::Submission::Track(recording.cmd, ticket, false, DlssNr::Submission::Lists::DirectOrCompute)' in capture
print('PASS: COMPUTE lists followed only for the comparison capture, never for the chain')

# A COMPUTE list is accepted only after a COMPUTE queue on the admitted device was seen to use the very
# ExecuteCommandLists the queue hook detoured (HookToQueue probes a DIRECT queue); otherwise its submissions would
# never reach Batch and the capture would wait for a fence that never comes. Probed once, refusal named.
track_body = track_impl[track_impl.index('bool Track(ID3D12GraphicsCommandList* list, Usage& usage, bool chain, Lists lists)'):]
track_body = track_body[:track_body.index('\n}\n')]
probe = track_impl[track_impl.index('bool ComputeQueuesHooked()'):]
probe = probe[:probe.index('\n}\n')]
assert 'desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;' in probe
assert '(*reinterpret_cast<void***>(real))[10] == hooked' in probe
assert 'void* const hooked = ResTrack_Dx12::NrQueueImplementation();' in probe
assert 'if (computeProbe != ComputeProbe::Unknown)' in probe and 'computeProbe = ComputeProbe::Unhooked;' in probe
assert probe.index('computeProbe = ComputeProbe::Unhooked;') < probe.index('CreateCommandQueue'), 'unknown fails closed'
check = 'if (list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE && !ComputeQueuesHooked())'
assert check in track_body and 'refusal = "compute-queue-not-hooked";' in track_body
assert track_body.index('!Install(list)') < track_body.index(check) < track_body.index('model.Track('), \
    'the probe runs on the admitted device, before any epoch is created'
print('PASS: COMPUTE lists only after a COMPUTE queue is proven to use the hooked ExecuteCommandLists')
