#!/usr/bin/env python3
"""Source contract for [DlssNr] PauseWithoutFrameGen (docs/dlssnr/design/pause-without-frame-gen.md)."""
from pathlib import Path
import re
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

ROOT = Path(__file__).resolve().parents[2]
renderer = renderer_source()
config_h = (ROOT / 'OptiScaler/Config.h').read_text()
config_cpp = (ROOT / 'OptiScaler/Config.cpp').read_text()
ini = (ROOT / 'OptiScaler.ini').read_text()
menu = (ROOT / 'retoque/dlssnr/DlssNr_Menu.cpp').read_text()


def body(source, signature):
    start = source.index(signature)
    open_brace = source.index('{', start)
    depth = 0
    for i in range(open_brace, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[open_brace:i + 1]
    raise AssertionError(signature)


assert 'CustomOptional<bool> DlssNrPauseWithoutFrameGen { false };' in config_h
assert 'readBool("DlssNr", "PauseWithoutFrameGen")' in config_cpp
assert '"PauseWithoutFrameGen"' in config_cpp.split('readBool("DlssNr", "PauseWithoutFrameGen")', 1)[1]
assert re.search(r'^PauseWithoutFrameGen=auto\r?$', ini, re.M)

rule = body(renderer, 'bool PausedWithoutFrameGen(const Config& cfg)')
# The game's own request, recorded before OptiScaler's policy -- not the post-policy mode or shared FG telemetry.
assert 'dlssgGameRequestsFg.load(' in rule
assert 'dlssgLastSetMode' not in rule and 'dlssgDetectedInterpolationCount' not in rule and 'currentFG' not in rule
state_h = (ROOT / 'OptiScaler/State.h').read_text()
assert 'std::atomic<bool> dlssgGameRequestsFg { false };' in state_h
hook = (ROOT / 'OptiScaler/hooks/Streamline_Hooks.cpp').read_text()
setter = hook[hook.index('sl::Result StreamlineHooks::hkslDLSSGSetOptions('):]
record = setter.index('dlssgGameRequestsFg.store(newOptions.mode != sl::DLSSGMode::eOff')
assert setter.index('lastDlssgOptions = newOptions;') < record < setter.index('applyMenuDlssgInterlock(newOptions')
assert record < setter.index('if (state.activeFgInput != FGInput::DLSSG && state.activeFgOutput == FGOutput::DLSSG)')
assert 'if (on)\n        seenOn = true;' in rule                        # arms only after the game asked for DLSS-G
assert '!MenuOverlayBase::IsVisible()' in rule                         # never for our own menu
assert 'cfg.DlssNrPauseWithoutFrameGen.value_or_default()' in rule     # off by default
assert 'std::chrono::milliseconds(500)' in rule                        # half a second, by the clock
assert 'if (!candidate)\n        offSince.reset();' in rule             # resumes at once
assert 'if (now)\n        g_resetOnReturn = true;' in rule              # history reset on resume

dispatch = renderer[renderer.index('bool DlssNr_Dx12::Dispatch('):]
check = dispatch.index('if (PausedWithoutFrameGen(cfg))')
assert check < dispatch.index('TrackNrRecording(cmdList')              # before any tracking or recording
assert check < dispatch.index('g_activePasses = 0;')
assert '###nrPauseNoFg' in menu
assert menu.index('if (!nativeVulkan)', menu.index('pause-without-frame-gen.md')) < menu.index('###nrPauseNoFg')
print('PASS: PauseWithoutFrameGen config, arm/debounce/menu exclusion and its place before any recording')
