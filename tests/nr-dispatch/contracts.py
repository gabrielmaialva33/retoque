"""Fault-inject the production scratch and guide decisions, without D3D12/NGX."""
from pathlib import Path
import subprocess
import tempfile


def run_contracts(root, renderer):
    scratch_start = renderer.index('    // Each member may fail independently', renderer.index('bool DlssNr_Dx12::Dispatch('))
    scratch_end = renderer.index('    // The down-leg target', scratch_start)
    retry_marker = '    // Retry failed mandatory scratch before any admission/rebuild'
    if retry_marker in renderer:
        retry_start = renderer.index(retry_marker)
        retry_end = renderer.index('    if (chainEnabled && (!g_nr.feature', retry_start)
        retry = renderer[retry_start:retry_end]
        assert retry_end < scratch_start
    else:
        retry = ''
    scratch = retry + '\n    if (deferScratch) return false;\n' + renderer[scratch_start:scratch_end]
    guide_end = renderer.index('    RestoreResourceState depthMatchedRead', scratch_end)
    guide_start = renderer.rfind('    else\n    {', scratch_end, guide_end)
    if guide_start < 0:
        guide_start = renderer.index('    else if (DlssNr::GuideMatch::Wanted(', scratch_end)
    # Also accepts the old Wanted branch so the same fault cases reproduce the original bugs.
    guide = renderer[guide_start:guide_end]
    guide = '    if (false) {}\n' + guide
    # A successful freeze copy is a write to the game image too, even before the resolve.
    # These integration guards complement the sliced fault cases (which cannot draw pixels).
    assert guide_end < renderer.index('copyHeld(target, g_nr.heldColor);')
    assert scratch_end < renderer.index('g_nr.create(')
    assert guide_end < renderer.index('result = g_nr.evaluate(')
    assert guide_end < renderer.index('wrote = DispatchPass(cmdList, resolveParams')
    assert renderer.index('ReadResourceScope depthRead') < guide_start
    assert renderer.index('ReadResourceScope motionRead') < guide_start
    assert renderer.index('RestoreResourceState depthCloneRead') < guide_start
    assert renderer.index('RestoreResourceState motionCloneRead') < guide_start
    unit = (Path(__file__).parent / 'contracts_fakes.h').read_text()
    internal = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
    state_start = internal.find('    struct ScratchRetry\n')
    state = '' if state_start < 0 else internal[state_start:internal.index('    } scratchRetry;', state_start) + len('    } scratchRetry;')]
    unit = unit.replace('    // Production scratch retry state.', state)
    unit += '\nbool scratch(bool chainEnabled, bool reduced, unsigned width=1920, unsigned height=1080, unsigned workWidth=0, unsigned workHeight=0, int format=0) {\n'
    unit += ' (void)chainEnabled; if (!workWidth) workWidth=reduced?width/2:width; if (!workHeight) workHeight=reduced?height/2:height; const uint64_t observedFrame=frameNumber; (void)observedFrame;\n'
    unit += ' const Desc desc{format, width, height};\n' + scratch + '\n return true; }\n'
    unit += r'''
bool guides(bool enabled, bool available, unsigned workWidth=960, unsigned workHeight=540) {
    cfg.DlssNrMatchGuides.value=enabled; shaderAvailable=available;
    const unsigned guideWidth=1920, guideHeight=1080, motionWidth=1280, motionHeight=720;
    const unsigned depthBaseX=7, depthBaseY=9, motionBaseX=11, motionBaseY=13;
    ID3D12Resource *depthIn=&gameDepth, *motionIn=&gameMotion;
    ID3D12Resource *depthForModel=depthIn, *motionForModel=motionIn;
    unsigned modelDepthWidth=guideWidth, modelDepthHeight=guideHeight;
    unsigned modelMotionWidth=motionWidth, modelMotionHeight=motionHeight;
    unsigned modelDepthBaseX=depthBaseX, modelDepthBaseY=depthBaseY;
    unsigned modelMotionBaseX=motionBaseX, modelMotionBaseY=motionBaseY;
    bool guidesMatched=false;
'''
    unit += guide
    unit += r'''
    if (enabled && (guideWidth>workWidth || guideHeight>workHeight)) {
        check(guidesMatched, "model must receive matched guides");
        check(depthForModel==g_nr.depthMatched && motionForModel==g_nr.motionMatched, "matched resources");
        check(modelDepthWidth==workWidth && modelMotionWidth==workWidth &&
              modelDepthHeight==workHeight && modelMotionHeight==workHeight, "matched extents");
        check(!(modelDepthBaseX || modelDepthBaseY || modelMotionBaseX || modelMotionBaseY), "matched bases");
    } else {
        check(!guidesMatched && depthForModel==depthIn && motionForModel==motionIn, "game resources unchanged");
        check(modelDepthBaseX==7 && modelDepthBaseY==9 && modelMotionBaseX==11 && modelMotionBaseY==13, "game bases unchanged");
    }
    ++evaluations; return true;
}
'''
    unit += (Path(__file__).parent / 'contracts_cases.h').read_text()
    with tempfile.TemporaryDirectory(prefix='nr-contracts-') as directory:
        source = Path(directory) / 'contracts.cpp'
        binary = Path(directory) / 'contracts'
        source.write_text(unit)
        subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I'+str(root),
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
