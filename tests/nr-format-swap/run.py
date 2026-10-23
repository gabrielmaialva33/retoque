#!/usr/bin/env python3
"""Format swap keeps NR history only for channel-order twins (docs/dlssnr/design/format-swap-history.md).

Three parts, all on the host, none of which runs a GPU, NGX or a shader:
1. scenarios.cpp drives the production ReleaseSurfacesIfFormatChanged, ForgetCalibration, ParkAltSurfaces and
   AltSurfacesFit, extracted from DlssNr_Dx12_Lifetime.cpp, against fake resources that only answer GetDesc.
2. policy.cpp checks the pure rule in retoque/dlssnr/DlssNr_FormatTwin.h on its own.
3. Text guards on Dispatch: the settling gates and the cadence signature watch the canonical format, the call passes
   the frame's transfer, and the game's Reset is set before the format check.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
sys.path.insert(0, str(here.parent / '_support'))
from nr_sources import renderer_source  # noqa: E402

lifetime = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12_Lifetime.cpp').read_text(encoding='utf-8-sig')


def function(source, head):
    """The whole definition that starts with `head` (its return type and name), through its closing brace."""
    match = re.search(r'^' + re.escape(head) + r'\(', source, re.M)
    if match is None:
        raise SystemExit(f'FAIL: {head} not found')
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


release = function(lifetime, 'void ReleaseSurfacesIfFormatChanged')
takes_transfer = 'FormatTwin::Transfer' in release[:release.index('{')]
call = ('ReleaseSurfacesIfFormatChanged(f, w, h, ww, wh, t)' if takes_transfer
        else '((void) (t), ReleaseSurfacesIfFormatChanged(f, w, h, ww, wh))')

prelude = r'''
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#if __has_include(<dlssnr/DlssNr_FormatTwin.h>)
#include <dlssnr/DlssNr_FormatTwin.h>
#else
// The tree before the fix has no policy header; the scenarios still compile and say what the function does.
namespace DlssNr::FormatTwin
{
struct Transfer
{
    bool gameSaysHdr = false;
    float presentWhite = 0.0f;
    float presentQuantRel = 0.0f;
};
}
#endif

enum DXGI_FORMAT : int
{
    DXGI_FORMAT_UNKNOWN = 0,
};
constexpr DXGI_FORMAT kRgba16Float = DXGI_FORMAT(10), kRgb10A2Unorm = DXGI_FORMAT(24),
                      kR11G11B10Float = DXGI_FORMAT(26), kRgba8Typeless = DXGI_FORMAT(27),
                      kRgba8Unorm = DXGI_FORMAT(28), kRgba8Srgb = DXGI_FORMAT(29), kBgra8Unorm = DXGI_FORMAT(87),
                      kBgrx8Unorm = DXGI_FORMAT(88), kBgra8Typeless = DXGI_FORMAT(90), kBgra8Srgb = DXGI_FORMAT(91);

struct D3D12_RESOURCE_DESC
{
    DXGI_FORMAT Format;
    uint64_t Width;
    unsigned int Height;
};
struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }
};

static int g_logs = 0;
#define LOG_INFO(...) (++g_logs)

namespace DlssNr::Dx12Internal
{
// The fields of the renderer state these functions touch, with the real names and defaults.
struct NrState
{
    ID3D12Resource* output = nullptr;
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* hdrCopy = nullptr;
    ID3D12Resource* colorSmall = nullptr;
    ID3D12Resource* outputNative = nullptr;
    ID3D12Resource* passPing = nullptr;
    struct AltSurfaces
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int workWidth = 0;
        unsigned int workHeight = 0;
        ID3D12Resource* output = nullptr;
        ID3D12Resource* colorCopy = nullptr;
        ID3D12Resource* hdrCopy = nullptr;
        ID3D12Resource* colorSmall = nullptr;
        ID3D12Resource* outputNative = nullptr;
        ID3D12Resource* passPing = nullptr;
    } alt;
    DlssNr::FormatTwin::Transfer surfaceTransfer {};
    bool reset = true;
    bool passReset[3] { true, true, true };
    unsigned int calibCount = 0;
    float calibSuggestion = 0.0f;
    float calibSteadiness = 0.0f;
    bool calibUsable = false;
    const char* calibWhy = "measuring...";
};
static NrState g_nr;

// Parking hands a surface to the retirement list; here it only counts, and the pointer goes as it does there.
static int g_parked = 0;
static void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;
    ++g_parked;
    res = nullptr;
}
'''

functions = '\n'.join(function(lifetime, head) for head in (
    'void ForgetCalibration', 'void ParkAltSurfaces', 'bool AltSurfacesFit', 'void ReleaseSurfacesIfFormatChanged'))

bridge = f'''
#define RELEASE_SURFACES(f, w, h, ww, wh, t) {call}
}} // namespace DlssNr::Dx12Internal
'''

cxx = os.environ.get('CXX', 'g++')
flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-g', f'-I{root / "retoque"}']

failed = False
with tempfile.TemporaryDirectory(prefix='nr-format-swap-') as directory:
    work = Path(directory)
    (work / 'scenarios_main.cpp').write_text(prelude + functions + bridge +
                                             f'#include "{here / "scenarios.cpp"}"\n')
    subprocess.run([cxx, *flags, str(work / 'scenarios_main.cpp'), '-o', str(work / 'scenarios')], check=True)
    if subprocess.run([str(work / 'scenarios')]).returncode != 0:
        failed = True
        print('FAIL: the production ReleaseSurfacesIfFormatChanged does not follow the rule')

    if (root / 'retoque/dlssnr/DlssNr_FormatTwin.h').exists():
        subprocess.run([cxx, *flags, str(here / 'policy.cpp'), '-o', str(work / 'policy')], check=True)
        if subprocess.run([str(work / 'policy')]).returncode != 0:
            failed = True
    else:
        failed = True
        print('FAIL: retoque/dlssnr/DlssNr_FormatTwin.h does not exist')

# Text guards on the Dispatch side, which the scenarios cannot reach.
source = renderer_source()
dispatch = source[source.index('bool DlssNr_Dx12::Dispatch('):]


def guard(ok, what):
    global failed
    print(('PASS: ' if ok else 'FAIL: ') + what)
    failed = failed or not ok


# The canonical format, inline or through one named value defined from it.
canonical = re.search(r'const auto (\w+) = static_cast<uint32_t>\(DlssNr::FormatTwin::ChannelOrderCanonical\(\(int\) '
                      r'desc\.Format\)\);', dispatch)
names = ['ChannelOrderCanonical'] + ([canonical.group(1)] if canonical else [])


def watches_canonical(text):
    return text is not None and any(n in text for n in names) and 'desc.Format' not in text


chain_gate = re.search(r'g_chainExtent\.Observe\(workWidth, workHeight, now,[^;]*;', dispatch, re.S)
guard(watches_canonical(chain_gate and chain_gate.group(0)), 'the chain settling gate watches the canonical format')
post_gate = re.search(r'g_postExtent\.Observe\(workWidth, workHeight, now,[^;]*;', dispatch, re.S)
guard(watches_canonical(post_gate and post_gate.group(0)),
      'the after-upscale settling gate watches the canonical format')
signature = dispatch[dispatch.index('const Cadence::Signature cadenceSignature {'):]
signature = signature[:signature.index('};')]
guard(watches_canonical(signature), 'the cadence signature carries the canonical format')
if canonical is not None:
    guard(canonical.start() < chain_gate.start(), 'the canonical format is defined before the first gate reads it')

call_site = re.search(r'ReleaseSurfacesIfFormatChanged\(desc\.Format, width, height, workWidth, workHeight, '
                      r'(\w+)\);', dispatch)
guard(call_site is not None, 'Dispatch hands the format check the frame\'s transfer')
if call_site is not None:
    name = call_site.group(1)
    built = re.search(r'const DlssNr::FormatTwin::Transfer ' + name + r' \{([^;]*)\};', dispatch, re.S)
    guard(built is not None and all(f in built.group(1) for f in
                                    ('frame.ColourIsLinearHdr', 'frame.PresentWhite', 'frame.PresentQuantRel')),
          'the transfer is built from the game\'s HDR claim, the present white and the output code')
    game_reset = dispatch.index('    if (frame.Reset)\n    {\n        g_nr.reset = true;')
    guard(game_reset < call_site.start(), 'the game\'s Reset is set before the format check runs')

guard('reset = false' not in release, 'the format check never clears a reset')
guard('DlssNr::FormatTwin::KeepsHistory' in release, 'the format check asks the policy header')

if failed:
    print('FAIL: nr-format-swap')
    sys.exit(1)
print('PASS: nr-format-swap')
