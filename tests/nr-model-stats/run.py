#!/usr/bin/env python3
"""The model's own VRAM figure (docs/dlssnr/design/model-vram-stats.md).

The forwarder's dlssnr_query_stats and the direct runtime's QueryStats are sliced from their sources and run against a
fake parameter block and fake stats callbacks under ASan/UBSan; DlssNr_ModelStats.h is compiled as it is; source guards
hold the host's wiring (asked only after a successful build, optional on an old forwarder, read by the panel and the
diagnostic export only). Runs no model, no NGX and no GPU.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
repo = here.parents[1]
failures = []


def fail(message):
    print(f'FAIL: {message}', file=sys.stderr)
    sys.exit(1)


def require(condition, message):
    if not condition:
        failures.append(message)


def read(relative):
    return (repo / relative).read_text(encoding='utf-8')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def body(source, signature, path):
    """The text of the function whose definition starts at `signature`, braces balanced."""
    start = source.find(signature)
    if start < 0:
        fail(f'{signature!r} not found in {path}')
    open_brace = source.index('{', start)
    depth = 0
    for i in range(open_brace, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    fail(f'unbalanced braces after {signature!r} in {path}')


def constant(source, name, path):
    match = re.search(r'^\s*(constexpr unsigned long long ' + name + r' = [^;]+;)', source, re.M)
    if not match:
        fail(f'{name} not found in {path}')
    return match.group(1)


# --- 1. Slice both loaders' stats query and run them against the fakes ------------------------------------------------

forwarder_path = 'retoque/dlssnr/forwarder/dlssnr_forwarder.cpp'
direct_path = 'retoque/dlssnr/DlssNr_DirectRuntime.cpp'
forwarder = read(forwarder_path)
direct = read(direct_path)

forwarder_query = body(forwarder, 'int QueryStatsOn(', forwarder_path)
forwarder_exports = [body(forwarder, '__declspec(dllexport) int ' + name + '(', forwarder_path)
                     for name in ('dlssnr_query_stats', 'dlssnr_vk_query_stats')]
direct_query = body(direct, 'int QueryStats(', direct_path)
forwarder_slice = '\n'.join([
    constant(forwarder, 'kStatsUnwritten', forwarder_path),
    body(forwarder, 'void setULL(', forwarder_path),
    body(forwarder, 'void setU64(', forwarder_path),
    forwarder_query,
    *[export.replace('__declspec(dllexport) ', '', 1) for export in forwarder_exports],
])
direct_slice = '\n'.join([
    constant(direct, 'kStatsUnwritten', direct_path),
    body(direct, 'void setULL(', direct_path),
    body(direct, 'void setU64(', direct_path),
    direct_query,
    body(direct, 'const int* LastStatsResult()', direct_path),
])

# The pieces the harness supplies itself must be what the sources say.
for path, source in ((forwarder_path, forwarder), (direct_path, direct)):
    clean = strip_comments(source)
    for declaration in ('using PFN_NrStatsCallback = int(__cdecl*)(void*);',
                        'using PFN_GetULL = int(__thiscall*)(void*, const char*, unsigned long long*);',
                        'using PFN_SetULL = void(__thiscall*)(void*, const char*, unsigned long long);',
                        'constexpr int VT_SET_ULL = 0;', 'constexpr int VT_GET_ULL = VT_SET_ULL + 8;',
                        'constexpr int VT_SET_U64 = 7;', 'constexpr int VT_GET_U64 = VT_SET_U64 + 8;',
                        'constexpr int kFaulted = static_cast<int>(0xBAD0FA17u);'):
        require(declaration in clean, f'{path} no longer declares {declaration!r}; the harness assumes it')
require(constant(forwarder, 'kStatsUnwritten', forwarder_path) == constant(direct, 'kStatsUnwritten', direct_path)
        == 'constexpr unsigned long long kStatsUnwritten = ~0ull;',
        'kStatsUnwritten must be ~0ull in both loaders: no allocation has that size')

# The query is read-only toward the model: no populate, no feature, no create/evaluate/release inside it.
for path, query in ((forwarder_path, forwarder_query), (direct_path, direct_query)):
    clean = strip_comments(query)
    for forbidden in ('PopulateParameters', 'LoadSnippet', 'loadSnippet', 'g_snip.create', 'g_snip.evaluate',
                      'g_snip.release', 'g_snip.init'):
        require(re.search(re.escape(forbidden) + r'\b', clean) is None,
                f'{path}: the stats query must not reach {forbidden}')
    require(re.search(r'volatile int result = Guarded\(reinterpret_cast<PFN_NrStatsCallback>\(\(void\*\) raw\), '
                      r'capabilityParams\);', clean),
            f'{path}: the callback is called through Guarded, assigned to a volatile (never a tail call)')
    require(re.search(r'return\s+Guarded\(', clean) is None, f'{path}: a Guarded call is returned directly')
    # The model writes SizeInBytes through the block's unsigned 64-bit setter (slot 7), so the sentinel goes through
    # the same setter and the figure comes back through its getter (slot 15), never the pointer pair at 0/8.
    require('setU64(capabilityParams, "SizeInBytes", kStatsUnwritten);' in clean
            and re.search(r'vt\[VT_GET_U64\]\)\(capabilityParams, "SizeInBytes", &bytes\)', clean)
            and 'setULL(capabilityParams, "SizeInBytes"' not in clean
            and not re.search(r'vt\[VT_GET_ULL\]\)\(capabilityParams, "SizeInBytes"', clean),
            f'{path}: SizeInBytes is cleared and read through the unsigned 64-bit pair (slots 7/15)')

# Each export is the shared query behind its own API's gate, and nothing else.
require(' '.join(strip_comments(forwarder_exports[0]).split()).endswith(
            '{ return QueryStatsOn(g_snip.initialised, capabilityParams, outBytes); }'),
        'dlssnr_query_stats is the shared query gated on NGX being up for D3D12 in the forwarder')
vk_export = ' '.join(strip_comments(forwarder_exports[1]).split())
require('const bool initialised = g_vk.initialisedDevice != nullptr;' in vk_export
        and vk_export.endswith('return QueryStatsOn(initialised, capabilityParams, outBytes); }')
        and 'volatile int populated = Guarded(g_vk.populate, capabilityParams);' in vk_export
        and 'return Guarded' not in vk_export,
        'dlssnr_vk_query_stats populates a block without the callback through Guarded, then runs the shared query '
        'gated on NGX being up on a Vulkan device')
require('GetProcAddress(g_vk.module, "NVSDK_NGX_VULKAN_PopulateParameters_Impl")' in strip_comments(forwarder),
        'the forwarder resolves the Vulkan populate from the snippet')

harness = (here / 'cases.cpp').read_text(encoding='utf-8')
for marker in ('// @@FORWARDER@@', '// @@DIRECT@@'):
    if marker not in harness:
        fail(f'injection marker {marker} missing from cases.cpp')
generated_text = harness.replace('// @@FORWARDER@@', forwarder_slice).replace('// @@DIRECT@@', direct_slice)

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-model-stats-') as directory:
    generated = Path(directory) / 'cases.cpp'
    generated.write_text(generated_text, encoding='utf-8')
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
                    '-I', str(repo / 'retoque'), str(generated), '-o', str(binary)], check=True)
    cases = subprocess.run([str(binary)])

# --- 2. The header: standard library only ----------------------------------------------------------------------------

header = read('retoque/dlssnr/DlssNr_ModelStats.h')
includes = re.findall(r'^#include\s+(\S+)', header, re.M)
require(includes and all(i.startswith('<') and '/' not in i and '.' not in i for i in includes),
        f'DlssNr_ModelStats.h must include only standard headers (found {includes})')

# --- 3. Host wiring --------------------------------------------------------------------------------------------------

internal = strip_comments(read('retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h'))
require('int (*queryStats)(void*, unsigned long long*) = nullptr;' in internal,
        'NrState carries the stats query beside the ratio query')
require('uint64_t modelVramBytes = ~0ull;' in internal,
        'NrState keeps one 64-bit figure, unknown (~0) until the model reports one')
require('static_assert(DlssNr::ModelStats::kUnknown == ~0ull' in read('retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp'),
        'the NrState default is pinned to ModelStats::kUnknown')

model_path = 'retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp'
model = strip_comments(read(model_path))
ensure_forwarder = body(model, 'bool EnsureForwarder()', model_path)
ensure_direct = body(model, 'bool EnsureDirectRuntime()', model_path)
require('GetProcAddress(g_nr.forwarder, "dlssnr_query_stats")' in ensure_forwarder,
        'EnsureForwarder resolves dlssnr_query_stats')
stats_at = ensure_forwarder.find('"dlssnr_query_stats"')
refuse_at = ensure_forwarder.find('if (g_nr.create == nullptr || g_nr.evaluate == nullptr)')
require(0 <= stats_at and 0 <= refuse_at and not re.search(r'queryStats\s*==\s*nullptr', ensure_forwarder),
        'the stats export is optional: an older forwarder without it is not refused')
require('g_nr.queryStats = &Direct::QueryStats;' in ensure_direct, 'EnsureDirectRuntime points at Direct::QueryStats')

report = body(model, 'void ReportModelVram()', model_path)
forget = re.search(r'g_nr\.modelVramBytes = (?:DlssNr::ModelStats|MS)::kUnknown;', report)
require(forget and 0 <= forget.start() < report.find('g_nr.queryStats(g_nr.capabilityParams'),
        'ReportModelVram forgets the last figure before asking, so a failed query is not shown as current')
require(report.count('LOG_INFO(') == 1 and report.count('LOG_DEBUG(') == 1 and 'LOG_ERROR' not in report
        and 'LOG_WARN' not in report, 'one INFO line on a figure, one DEBUG line otherwise, nothing louder')
require('g_nr.failed' not in report and 'g_nr.reason' not in report and 'g_nr.feature' not in report,
        'an absent or refusing callback changes nothing about the pass (a fault goes through NoteModelFault)')

core_path = 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp'
core = strip_comments(read(core_path))
require(core.count('ReportModelVram();') == 1, 'the model is asked once per build, from one place')
create_at = core.find('g_nr.create(snippet->wstring().c_str()')
failed_at = core.find('if (g_nr.feature == nullptr)', create_at)
running_at = core.find('LOG_INFO("DLSS-NR running {}', create_at)
asked_at = core.find('ReportModelVram();')
require(0 <= create_at < failed_at < running_at < asked_at,
        'ReportModelVram is called only after the build succeeded, after the "running" line')

api = strip_comments(read('retoque/dlssnr/DlssNrFeature_Dx12.h'))
require('std::optional<unsigned long long> ModelVramBytes();' in api, 'the panel reads the figure through the NR API')
menu = strip_comments(read('retoque/dlssnr/DlssNr_Menu.cpp'))
require(menu.count('DlssNr::ModelVramBytes()') == 2, 'the figure is read by the status line and the export only')
collect = body(menu, 'static std::string CollectDiagnosticReport(Config* config)', 'DlssNr_Menu.cpp')
require('in.modelVramBytes' in collect, 'the diagnostic export carries the figure')
require(collect.find('in.modelVramBytes') > collect.find('if (!in.nrEnabled)'),
        'with NR off the export does not ask for the figure')

# Native Vulkan: the same query through the forwarder's Vulkan export, asked after each route's build.
vk_path = 'retoque/dlssnr/DlssNrFeature_Vk.cpp'
vk = strip_comments(read(vk_path))
load_fwd = body(vk, 'const char* LoadForwarderShared()', vk_path)
require('GetProcAddress(g_fwd.module, "dlssnr_vk_query_stats")' in load_fwd
        and not re.search(r'queryStats\s*==\s*nullptr\)\s*return', load_fwd),
        'the Vulkan loader resolves dlssnr_vk_query_stats, optionally')
ask = body(vk, 'uint64_t AskModelVramVk(', vk_path)
require('g_fwd.queryStats(params, &bytes)' in ask and ask.count('LOG_INFO(') == 1 and ask.count('LOG_DEBUG(') == 1
        and 'LOG_ERROR' not in ask and 'LOG_WARN' not in ask and 'Fail(' not in ask,
        'AskModelVramVk: one INFO line on a figure, one DEBUG line otherwise, and it never fails the route itself')
require(vk.count('AskModelVramVk(') == 3, 'the Vulkan query is asked from exactly two build sites')
up_at = vk.find('LOG_INFO("DLSS-NR Vulkan: feature up at')
after_at = vk.find('g_vk.modelVramBytes = AskModelVramVk(g_vk.capabilityParams, "after upscale");')
require(0 <= vk.find('if (g_vk.feature == nullptr)', vk.find('g_fwd.create((void*) cmdBuffer')) < up_at < after_at,
        'the after-upscale route asks only after its feature was built')
require(re.match(r'\s*if \(NoteModelFault\(\)\)\s*return;', vk[after_at + len('g_vk.modelVramBytes = AskModelVramVk('
                                                                         'g_vk.capabilityParams, "after upscale");'):]),
        'a fault inside the stats callback stops the after-upscale route right after the query')
require('std::atomic<uint64_t> modelVramBytes' in vk and 'g_vk.modelVramBytes.load()' in vk,
        'the Vulkan figure is atomic: the menu reads it on the presenting thread')
submit_at = vk.find('if (submitted == VK_SUCCESS)')
present_at = vk.find('AskModelVramVk(params, "present host");')
require(0 <= submit_at < present_at < vk.find('if (submitted == VK_NOT_READY)'),
        'the present host asks only once the build\'s GPU work has completed')
require(vk.count('g_vk.modelVramBytes = DlssNr::ModelStats::kUnknown;') == 3,
        'the after-upscale figure is forgotten wherever its feature is released or abandoned')
vk_api = body(vk, 'std::optional<unsigned long long> ModelVramBytesVk()', vk_path)
require('IsRunningVk()' in vk_api, 'the Vulkan figure is shown only while that path runs')
require(menu.count('DlssNr::ModelVramBytesVk()') == 2, 'the Vulkan figure is read by the status line and the export')

pack = read('retoque/translations/optional/pt-BR.lang')
require('(model: %llu MiB)\t' in pack, 'the status line\'s figure has a pt-BR entry')

# --- Result ----------------------------------------------------------------------------------------------------------

if failures:
    for failure in failures:
        print('FAIL:', failure)
if failures or cases.returncode != 0:
    raise SystemExit(1)

print('PASS: model stats -- forwarder and direct query identical against present/absent/refusing/silent/faulting '
      'fakes, 64-bit size, header text, host wiring')
