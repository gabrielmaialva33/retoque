#!/usr/bin/env python3
"""Transfer 2 ("Matched residual + DLSS"): its rules, its NGX half, and its place in the pass, on the host.

Three binaries and a set of source guards:
  rules.cpp    DlssNr_Enlarge.h: the decision table, the carrier through FP16, the menu table, resets, guides.
  adapter.cpp  DlssNr_PrivateSr.cpp as built, against a recording driver core (ngx_fakes.h).
  host         enlargement definitions and core retirement/retry, assembled explicitly per owner out of
               production and compiled against D3D12 fakes that hold every barrier to the state it claims.
The guards pin the shader's numbers and branches to the header's, and Dispatch, the Vulkan resolve and the
menu to the shape the host binary drives. No GPU, no NGX, no shader is executed: see README.md.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_internal_header  # noqa: E402

here = Path(__file__).resolve().parent
root = here.parents[1]
shader = (root / 'retoque/shaders/dlssnr/precompile/dlssnr.hlsl').read_text()
common = (root / 'retoque/shaders/dlssnr/DlssNr_Common.h').read_text()
rules = (root / 'retoque/dlssnr/DlssNr_Enlarge.h').read_text()
vulkan = (root / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
menu = (root / 'retoque/dlssnr/DlssNr_Menu.cpp').read_text()

FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']
INCLUDES = ['-I' + str(root / 'retoque'), '-I' + str(root / 'OptiScaler'), '-I' + str(root / 'external/nvngx_dlss_sdk'), '-I' + str(here)]


def between(source, start, end):
    assert source.count(start) == 1, f'production boundary changed: {start!r}'
    begin = source.index(start)
    stop = source.index(end, begin)
    return source[begin:stop]


def once(source, text, what):
    assert source.count(text) == 1, f'{what}: expected exactly one {text!r}, found {source.count(text)}'
    return source.index(text)


# Assemble exact definitions from their owners. No slice crosses a TU or namespace boundary.
render_dir = root / 'retoque/shaders/dlssnr'
core = (render_dir / 'DlssNr_Dx12.cpp').read_text()
model = (render_dir / 'DlssNr_Dx12_Model.cpp').read_text()
enlargement = (render_dir / 'DlssNr_Dx12_Enlarge.cpp').read_text()
internal = renderer_internal_header()


def definition(text, signature):
    assert text.count(signature) == 1, f'definition must be unique: {signature!r}'
    start = text.index(signature)
    brace = text.index('{', start)
    assert ';' not in text[start:brace], f'not a definition: {signature!r}'
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def shared_type(name):
    signature = 'struct ' + name + '\n'
    assert internal.count(signature) == 1, f'missing/duplicate shared type: {name}'
    start = internal.index(signature)
    return internal[start:internal.index('\n};', start) + 4] + '\n'


def storage(signature):
    assert core.count(signature) == 1, f'missing/duplicate core storage: {signature}'
    return signature + '\n'



# --- The shader holds the header's numbers and takes Transfer 2 only where it says it does. ----------------

modes = dict(re.findall(r'DlssNrMode_(\w+) = (\d+)', common))
assert modes.get('Carrier') == '5' and modes.get('UnitTexel') == '6', f'mode numbers moved: {modes}'
carrier_mode = between(shader, 'if (gMode == 5)', 'if (gMode == 6)')
for text in ('gSource.Load(int3(id.xy, 0)).rgb', 'gModel.Load(int3(id.xy, 0)).rgb', 'if (gPassthrough == 0)',
             'all(answer <= 1e-5)', 'SanitizeFinite3(answer - source, none)',
             '0.5 + 0.5 * d / (1.0 / 64.0 + abs(d))'):
    assert text in carrier_mode, f'carrier encode lost {text!r}'
unit_mode = between(shader, 'if (gMode == 6)', 'if (gMode == 0)')
assert 'gTarget[id.xy] = float4(1.0, 1.0, 1.0, 1.0);' in unit_mode

# The decode: the header's 1/64 and 0.999, read from the model slot, before the Difference view.
assert 'constexpr float kCarrierScale = 1.0f / 64.0f;' in rules and 'constexpr float kCarrierClamp = 0.999f;' in rules
decode_at = once(shader, 'edit = (1.0 / 64.0) * carrier / (1.0 - abs(carrier));', 'decode')
once(shader, 'clamp(2.0 * SanitizeFinite3(modelSample.rgb, neutral) - 1.0, -0.999, 0.999)', 'decode clamp')
assert once(shader, 'float3 edit = model - proxy;', 'edit') < decode_at < shader.index('if (gDebugView == 3)')

# Transfer 2 appears in exactly four places: the empty test, the decode, the matched path, the replace input.
assert shader.count('gTransfer == 2') == 3 and shader.count('gTransfer != 2') == 1, 'a new Transfer 2 branch'
once(shader, 'const bool emptyModel = gTransfer != 2 && all(modelDirect <= 1e-5);', 'emptyModel')
matched = between(shader, 'if (((gTransfer == 1 || gTransfer == 3) && modelRanSmall) || gTransfer == 2)',
                  '// The composition.')
assert 'model = CubeScaleResidual(fullProxy, fullProxy + edit);' in matched
assert matched.index('model = CubeScaleResidual') < matched.index('if (gTransfer == 2)\n            modelDirect = model;')
# The passthrough gate travels with the proxy rebuild the matched path already had.
assert 'float3 fullProxy = gPassthrough != 0' in matched

# --- Dispatch has the shape host.cpp drives. ------------------------------------------------------------------

dispatch = definition(core, 'bool DlssNr_Dx12::Dispatch(')
plans = list(re.finditer(r'const auto enlargePlan =\s*PlanEnlarge\(\s*cfg\.DlssNrTransfer\.value_or_default\(\), '
                         r'useProxy, peripheryActive,', dispatch))
assert len(plans) == 1 and dispatch.count('PlanEnlarge(') == 1, \
    'Dispatch plans once, from the key, told about the proxy and peripheral compression (each its own reason)'
# The plan reads the periphery's decision, so it has to come after it.
assert dispatch.index('const bool peripheryActive = periphery.active;') < plans[0].start()
plan_at = plans[0].start()
assert plan_at < dispatch.index('timing.SetMetadata(')
once(dispatch, 'enlargePlan.transfer, DlssNr::Enlarge::Token(enlargePlan.why)));', 'timing contract')
once(dispatch, 'resolveParams.Transfer = enlargePlan.transfer;', 'resolve transfer')
assert 'resolveParams.Transfer = cfg.DlssNrTransfer' not in dispatch, 'the resolve reads the key directly again'
resolve_at = once(dispatch, 'wrote = DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer,', 'resolve')
enlarge_at = once(dispatch, 'enlarged = EnlargeEdit(*this, cmdList, device, enlargePlan, ef);', 'enlarge')
assert dispatch.index('DlssNr::Chain::Resolve(') < dispatch.index('if (enlargePlan.keep)') < enlarge_at < resolve_at
fallback = between(dispatch, 'else if (enlargePlan.evaluate)', '{\n            ReadResourceScope exposureRead')
assert 'resolveParams.Transfer = DlssNr::Enlarge::kTransferMatched;' in fallback
assert 'timing.Reject(' in fallback
restore = dispatch.index('Barrier(cmdList, enlarged, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,')
assert resolve_at < restore
# The private SR is not the model: no model marker around it, no forwarder call for it.
assert dispatch.count('timing.ModelBegin();') == 2 and dispatch.count('g_nr.evaluate(') == 2

# The creation frame never evaluates.
enlarge = definition(enlargement, 'ID3D12Resource* EnlargeEdit(')
assert re.search(r'BuildEnlarger\(cmdList, device, f\);\s*return nullptr;', enlarge)

# Released last, after the list is whole; the bundle is one entry.
tick = definition(core, 'void TickNrRetired(bool shutdown = false)')
assert tick.index('g_nrRetired.pop_back();') < tick.index('delete enlarger;')
shutdown = definition(core, 'void Shutdown()\n{')
assert shutdown.index('g_nrRetired.clear();') < shutdown.index('delete enlarger;')
assert 'enlargers.push_back(g_enlarge.live);' in shutdown
retry = definition(core, 'void RetryAfterFailure()') + definition(core, 'void RetryEnlargement()')
assert retry.count('ParkNrEnlarger(g_enlarge.live);') == 2 and retry.count('g_enlarge.failed = false;') == 2
narrow = definition(core, 'void RetryEnlargement()')
assert 'g_nr.' not in narrow and 'Extent' not in narrow, 'the enlargement Retry reaches past the enlargement'

# --- Native Vulkan maps 2 to 1, and the menu goes through the header's table. --------------------------------

assert len(re.findall(r'\bDlssNrTransfer\b', vulkan)) == 1, 'the Vulkan route reads Transfer somewhere unmapped'
once(vulkan, 'encode.Transfer = Enlarge::VulkanTransfer(cfg.DlssNrTransfer.value_or_default());', 'Vulkan map')
once(menu, 'int enlarge = Enlarge::MenuIndex(transfer);', 'menu index')
once(menu, 'config->DlssNrTransfer = Enlarge::MenuTransfer(enlarge);', 'menu write')
once(menu, 'Localization::Label("Matched residual + DLSS")', 'menu entry')
status = between(menu, 'if (transfer == Enlarge::kTransferDlss && enabled && (nativeVk || DlssNr::IsRunning()))',
                 'ImGui::SeparatorText(Localization::Tr("How much')
assert 'DlssNr::EnlargeStatus()' in status and 'Retry###nrEnlargeRetry' in status
assert 'DlssNr::RetryEnlargement();' in status and 'RetryAfterFailure' not in status
assert status.index('status.why == Enlarge::Why::Failed') < status.index('Retry###nrEnlargeRetry')
whys = re.findall(r'^\s+(\w+),?\s*(?://.*)?$', between(rules, 'enum class Why : uint8_t\n{', '};'), re.M)
for why in whys:
    assert f'case Enlarge::Why::{why}:' in status, f'the status line has no text for Why::{why}'

# --- Compile and run. ----------------------------------------------------------------------------------------

retirement_names = ['void ParkNrFeature(', 'void ParkNrResource(', 'void ParkNrScaler(',
                    'void ParkNrStabilizer(', 'void ParkNrUiMask(', 'void ParkNrCadence(',
                    'void ParkNrEnlarger(', 'void TickNrRetired(bool shutdown = false)',
                    'bool RetiredCapacity()\n{']
enlarge_names = ['void PublishEnlarge(', 'DlssNr::Enlarge::Decision PlanEnlarge(',
                 'void FailEnlarge(', 'void BuildEnlarger(', 'ID3D12Resource* EnlargeEdit(']
retry_names = ['void RetryAfterFailure()', 'void RetryEnlargement()', 'Enlarge::Status EnlargeStatus()']
host = ('#include "host_fakes.h"\nnamespace DlssNr::Dx12Internal\n{\n' +
        definition(model, 'const char* NgxResultName(unsigned int r)') +
        shared_type('NrEnlarger') + shared_type('NrRetired') + shared_type('EnlargeSession') +
        shared_type('EnlargeFrame') + storage('std::vector<NrRetired> g_nrRetired;') +
        storage('EnlargeSession g_enlarge;') +
        definition(enlargement, 'DlssNr::PrivateSr::Api PrivateSrApi()') +
        definition(enlargement, 'bool NrEnlarger::CoreGone() const') +
        definition(enlargement, 'NrEnlarger::~NrEnlarger()') +
        ''.join(definition(core, name) for name in retirement_names) +
        ''.join(definition(enlargement, name) for name in enlarge_names) +
        '\n} // namespace DlssNr::Dx12Internal\nusing namespace DlssNr::Dx12Internal;\n' +
        'namespace DlssNr\n{\nusing namespace Dx12Internal;\n' +
        ''.join(definition(core, name) for name in retry_names) + '} // namespace DlssNr\n' +
        (here / 'host_cases.h').read_text())

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-enlarge-') as directory:
    out = Path(directory)
    subprocess.run(FLAGS + INCLUDES + [str(here / 'rules.cpp'), '-o', str(out / 'rules')], check=True)
    subprocess.run([str(out / 'rules')], check=True)

    subprocess.run(FLAGS + ['-I' + str(here / 'stubs')] + INCLUDES +
                   ['-c', str(root / 'retoque/dlssnr/DlssNr_PrivateSr.cpp'), '-o', str(out / 'privatesr.o')],
                   check=True)
    subprocess.run(FLAGS + INCLUDES + [str(here / 'adapter.cpp'), str(out / 'privatesr.o'), '-o', str(out / 'adapter')],
                   check=True)
    subprocess.run([str(out / 'adapter')], check=True)

    (out / 'host.cpp').write_text(host)
    subprocess.run(FLAGS + INCLUDES + [str(out / 'host.cpp'), str(out / 'privatesr.o'), '-o', str(out / 'host')],
                   check=True)
    subprocess.run([str(out / 'host')], check=True)

print('PASS: shader carrier/decode pinned to the header (1/64, 0.999, modes 5 and 6, four Transfer 2 branches); '
      'Dispatch, the Vulkan map and the menu in the shape the host cases drive')
