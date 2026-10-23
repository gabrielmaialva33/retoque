#!/usr/bin/env python3
"""Native D3D12 and Vulkan rules over the code and bytecode this fork owns, checked statically.

vkd3d-proton accepts calls native Windows answers by removing the device: the guide resample's 48-byte
constant-buffer view ran here and removed a native RTX 3060's device at the first frame (13f10d60). This suite
holds the rules a script can read off the source and the committed bytecode, so they do not depend on a run on
native hardware to be found. The audit behind it is docs/dlssnr/design/native-api-rules.md.

1. DXIL bindings are inside the root signature. Every resource the committed DXIL of a pass declares (read from
   the container's PSV0 part, so it is exactly the bytecode the build embeds, decoded from the header) lies in a
   range of the root signature the pass creates, with its type, register and space; samplers in a static sampler.
   A binding outside is E_INVALIDARG at CreateComputePipelineState on native (D3D12 "Root signature version
   1.1": the shader's bindings must be a subset of the root signature) and a pass that silently does not exist.
2. Root signature cost and root constants. A root signature costs at most 64 DWORDs (a table 1, a root constant 1
   each, a root descriptor 2); SetComputeRoot32BitConstants writes within the parameter's Num32BitValues and only
   into a constants parameter; SetComputeRootDescriptorTable only into a table parameter. A table mixing SAMPLER
   ranges with CBV/SRV/UAV ranges is invalid (D3D12_ROOT_DESCRIPTOR_TABLE).
3. Upload and readback heaps. A committed resource on an UPLOAD heap is created in GENERIC_READ and on a READBACK
   heap in COPY_DEST, and neither is ever a transition's before or after state for another value (D3D12_HEAP_TYPE:
   "must be created with ... and cannot be changed away from this").
4. Transition barriers name two different states (D3D12_RESOURCE_TRANSITION_BARRIER: StateBefore and StateAfter
   must differ; the debug layer rejects the barrier, drivers drop it), checked where both are literals.
5. Constant-buffer view placement. Every CBV's BufferLocation is a resource's own GPU address, with no offset that
   could break the 256-byte placement (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT). nr-invariants check 5
   holds the size of `SizeInBytes = sizeof(T)`; the aggregate `D3D12_CONSTANT_BUFFER_VIEW_DESC v { address, size }`
   it never saw is held here: sizeof an alignas(256) struct, or a literal multiple of 256.
6. SPIR-V. Every committed .spv's push-constant block fits 128 bytes (maxPushConstantsSize's guaranteed minimum,
   VUID-VkPushConstantRange-size-00298), with 4-byte offsets (VUID-VkPushConstantRange-offset-00295).
   A storage image written or read with format Unknown needs shaderStorageImageWriteWithoutFormat /
   ReadWithoutFormat (VUID-RuntimeSpirv-apiVersion-07955 / -07954), which the code that dispatches it must check:
   the module and the .cpp that loads it are named together. With spirv-val installed, each module also validates
   for Vulkan 1.1 (skipped, and said, without it).

7. Typed UAV loads beyond R32. A pass whose DXIL carries SFI0's TypedUAVLoadAdditionalFormats reads a UAV of
   more than one 32-bit component; formats past the guaranteed set (R16G16_*, R11G11B10_FLOAT, R10G10B10A2_UNORM,
   ...) are each optional ("Typed unordered access view loads", D3D12 docs), so the pass must ask
   CheckFeatureSupport for D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD before it dispatches.
8. Zero-guide clears are settled. Every object a file calls RecordClear on (a one-way UNORDERED_ACCESS -> rest
   transition recorded once), the same file answers with ConfirmExecuted and AbandonRecording, so a dropped list
   does not leave the guides believed to be in a state they never reached.
10-11. Open findings, held where the audit left them. Two findings are mechanical but not fixed (the fix changes
   pixels or needs a shader rebuild): upstream's TranslateTypelessFormats maps R10G10B10A2_TYPELESS to an integer view
   that the NR passes read through float declarations, and the NR and HDR Vulkan modules declare their storage images
   Rgba32f while their slots take views of other formats. Each is listed with its reason; a new instance fails, and a
   listed one that is gone fails until the list and native-api-rules.md are updated.
9. Shader module words. VkShaderModuleCreateInfo::pCode is an array of uint32_t; a create_header.py array is
   unsigned char, so pCode is never one of them cast (VUID-VkShaderModuleCreateInfo-pCode-parameter).

Each check fails on a seeded violation, so a pattern that stops matching the code is not a pass.
Pure Python: no compiler, no Wine prefix, no GPU. It reads source and bytecode; it does not prove a pass correct.
"""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

here = Path(__file__).resolve().parent
root = here.parents[1]
nr = root / 'retoque'

PASS_DIRS = [nr / 'shaders/dlssnr', nr / 'shaders/dlssnr/motion', nr / 'shaders/synth_motion']
HOST_DIRS = [nr / 'dlssnr', nr / 'shaders/dlssnr', nr / 'shaders/dlssnr/motion', nr / 'shaders/synth_motion']


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def line_of(text, offset):
    return text.count('\n', 0, offset) + 1


# --- DXIL container ---------------------------------------------------------------------------------------------

# PSVResourceType (DxilPipelineStateValidation.h)
PSV_SAMPLER, PSV_CBV = 1, 2
PSV_SRV = {3, 4, 5}
PSV_UAV = {6, 7, 8, 9}


def header_bytes(path):
    """The byte array a create_header.py header embeds."""
    text = path.read_text(errors='replace')
    body = text[text.index('{') + 1:text.rindex('}')]
    return bytes(int(token, 16) for token in re.findall(r'0x([0-9A-Fa-f]{1,2})', body))


def dxil_parts(blob):
    if blob[:4] != b'DXBC':
        raise ValueError('not a DXIL container')
    count = struct.unpack_from('<I', blob, 28)[0]
    parts = {}
    for offset in struct.unpack_from(f'<{count}I', blob, 32):
        fourcc = blob[offset:offset + 4].decode('latin1')
        size = struct.unpack_from('<I', blob, offset + 4)[0]
        parts[fourcc] = blob[offset + 8:offset + 8 + size]
    return parts


def dxil_bindings(blob):
    """[(kind, space, lower, upper)] from PSV0, kind in {'srv', 'uav', 'cbv', 'sampler'}."""
    data = dxil_parts(blob)['PSV0']
    info_size = struct.unpack_from('<I', data, 0)[0]
    at = 4 + info_size
    count = struct.unpack_from('<I', data, at)[0]
    at += 4
    out = []
    if count:
        entry = struct.unpack_from('<I', data, at)[0]
        at += 4
        for _ in range(count):
            kind, space, lower, upper = struct.unpack_from('<4I', data, at)
            at += entry
            name = ('sampler' if kind == PSV_SAMPLER else 'cbv' if kind == PSV_CBV else 'srv' if kind in PSV_SRV
                    else 'uav' if kind in PSV_UAV else f'type{kind}')
            out.append((name, space, lower, upper))
    return out


# --- C++ constants ------------------------------------------------------------------------------------------------

def resolve(expr, texts, depth=0):
    """An integer constant expression of literals and constexpr names found in texts; None when it is not one."""
    expr = expr.strip()
    if depth > 8:
        return None
    expr = re.sub(r'\b(?:static_cast<\w+>|UINT|uint32_t)\s*\(', '(', expr)
    expr = re.sub(r'(\d+)[uU]?\b', r'\1', expr)
    for name in set(re.findall(r'\b[A-Za-z_]\w*\b', expr)):
        value = None
        for text in texts:
            match = re.search(rf'\b{name}\s*=\s*([^;,{{}}]+);', text)
            if match:
                value = resolve(match.group(1), texts, depth + 1)
                if value is not None:
                    break
        if value is None:
            return None
        expr = re.sub(rf'\b{name}\b', str(value), expr)
    if not re.fullmatch(r'[\d\s+*()/-]+', expr):
        return None
    try:
        return int(eval(expr, {'__builtins__': {}}))  # noqa: S307  digits and + - * / ( ) only
    except Exception:  # noqa: BLE001
        return None


def split_args(text, start):
    """Arguments of the call whose '(' is at text[start]; returns (args, end)."""
    depth, at, args, current = 0, start, [], ''
    while at < len(text):
        ch = text[at]
        if ch in '([{':
            depth += 1
            if depth > 1:
                current += ch
        elif ch in ')]}':
            depth -= 1
            if depth == 0:
                args.append(current.strip())
                return args, at
            current += ch
        elif ch == ',' and depth == 1:
            args.append(current.strip())
            current = ''
        else:
            current += ch
        at += 1
    return args, at


RANGE_KIND = {'SRV': 'srv', 'UAV': 'uav', 'CBV': 'cbv', 'SAMPLER': 'sampler'}


def root_signatures(path, text):
    """Root signatures the file creates: [{'line', 'params': [...], 'static': [(reg, space)]}].

    A parameter is ('table', [(kind, base, count, space)]), ('constants', reg, space, n) or ('root', kind, reg,
    space). Two shapes: upstream's Shader_Dx12::SetupRootSignature (one table: SRV, UAV, CBV, sampler ranges at
    register 0, space 0) and an explicit CD3DX12_ROOT_PARAMETER1 array.
    """
    texts = [text] + [p.read_text(errors='replace') for p in sibling_headers(path)]
    found = []
    for match in re.finditer(r'\bSetupRootSignature\s*\(', text):
        args, _ = split_args(text, match.end() - 1)
        values = [resolve(a, texts) for a in args[1:6]] + [0] * 5
        srv, uav, cbv, _rtv, samplers = (values + [0] * 5)[:5]
        static_count = resolve(args[6], texts) if len(args) > 6 else 0
        if None in (srv, uav, cbv, samplers, static_count):
            found.append({'line': line_of(text, match.start()), 'error': f'unresolved counts {args[1:7]}'})
            continue
        ranges = [(k, 0, n, 0) for k, n in (('srv', srv), ('uav', uav), ('cbv', cbv), ('sampler', samplers)) if n]
        statics = []
        if static_count:
            name = args[7].lstrip('&').strip() if len(args) > 7 else ''
            reg = re.search(rf'\b{re.escape(name)}\.ShaderRegister\s*=\s*(\w+)\s*;', text) if name else None
            space = re.search(rf'\b{re.escape(name)}\.RegisterSpace\s*=\s*(\w+)\s*;', text) if name else None
            base = resolve(reg.group(1), texts) if reg else 0
            statics = [(base + i, resolve(space.group(1), texts) if space else 0) for i in range(static_count)]
        found.append({'line': line_of(text, match.start()), 'params': [('table', ranges)], 'static': statics})

    for match in re.finditer(r'\bCD3DX12_ROOT_PARAMETER1\s+(\w+)\s*\[\s*(\w+)\s*\]\s*;', text):
        array, size = match.group(1), resolve(match.group(2), texts)
        scope = text[match.end():match.end() + 4000]
        params = [None] * (size or 0)
        ranges = {}
        for r in re.finditer(r'\bCD3DX12_DESCRIPTOR_RANGE1\s+(\w+)\s*\(', text):
            args, _ = split_args(text, r.end() - 1)
            kind = RANGE_KIND.get(args[0].replace('D3D12_DESCRIPTOR_RANGE_TYPE_', ''))
            ranges[r.group(1)] = (kind, resolve(args[2], texts), resolve(args[1], texts),
                                  resolve(args[3], texts) if len(args) > 3 else 0)
        for p in re.finditer(rf'\b{array}\s*\[\s*(\d+)\s*\]\s*\.\s*(InitAs\w+)\s*\(', scope):
            index, how = int(p.group(1)), p.group(2)
            args, _ = split_args(scope, p.end() - 1)
            if index >= len(params):
                params.extend([None] * (index + 1 - len(params)))
            if how == 'InitAsDescriptorTable':
                names = [a.lstrip('&').strip() for a in args[1:]]
                params[index] = ('table', [ranges.get(n, ('?', None, None, None)) for n in names])
            elif how == 'InitAsConstants':
                params[index] = ('constants', resolve(args[1], texts),
                                 resolve(args[2], texts) if len(args) > 2 else 0, resolve(args[0], texts))
            else:
                kind = {'InitAsConstantBufferView': 'cbv', 'InitAsShaderResourceView': 'srv',
                        'InitAsUnorderedAccessView': 'uav'}.get(how, '?')
                params[index] = ('root', kind, resolve(args[0], texts), resolve(args[1], texts) if len(args) > 1 else 0)
        found.append({'line': line_of(text, match.start()), 'params': params, 'static': []})
    return found


def sibling_headers(path):
    out = [path.with_suffix('.h')]
    out += [p for p in path.parent.glob('*.h') if p.stem in path.read_text(errors='replace')]
    return [p for p in out if p.is_file()]


def shader_arrays(text):
    """Names of the DXIL arrays the file hands to a pipeline: X_cso."""
    return sorted(set(re.findall(r'\b(\w+_cso)\s*,\s*sizeof\s*\(\s*\1\s*\)', text)))


def find_header(array):
    for base in PASS_DIRS:
        for header in (base / 'precompile').glob('*.h'):
            if re.search(rf'\bunsigned\s+char\s+{array}\s*\[', header.read_text(errors='replace')):
                return header
    return None


def covered(binding, signature):
    kind, space, lower, upper = binding
    if kind == 'sampler':
        return all(any(reg == r and space == s for r, s in signature['static']) for reg in range(lower, upper + 1)) \
            or any(p and p[0] == 'table' and any(rk == 'sampler' and rs == space and rb <= lower and upper < rb + rn
                                                 for rk, rb, rn, rs in p[1]) for p in signature['params'])
    for p in signature['params']:
        if p is None:
            continue
        if p[0] == 'table':
            for rk, rb, rn, rs in p[1]:
                if rk == kind and rs == space and rb is not None and rn is not None and rb <= lower and upper < rb + rn:
                    return True
        elif p[0] == 'constants' and kind == 'cbv' and p[2] == space and lower == upper == p[1]:
            return True
        elif p[0] == 'root' and p[1] == kind and p[3] == space and lower == upper == p[2]:
            return True
    return False


def cost(signature):
    total = 0
    for p in signature['params']:
        if p is None:
            continue
        total += 1 if p[0] == 'table' else 2 if p[0] == 'root' else (p[3] or 0)
    return total


SFI0_TYPED_UAV_LOAD_ADDITIONAL_FORMATS = 0x800  # D3D_SHADER_REQUIRES_TYPED_UAV_LOAD_ADDITIONAL_FORMATS
typed_loads = []


def dxil_flags(blob):
    parts = dxil_parts(blob)
    return struct.unpack_from('<Q', parts['SFI0'], 0)[0] if 'SFI0' in parts else 0


def check_root_signatures(sources, fail):
    """Checks 1, 2 and 7 over {path: text}; returns (passes, bindings) checked."""
    passes = bindings = 0
    for path, raw in sources.items():
        text = strip_comments(raw)
        signatures = root_signatures(path, text)
        arrays = shader_arrays(text)
        if not signatures:
            if arrays:
                fail(f'{rel(path)}: creates pipelines from {arrays} but no root signature this suite can read')
            continue
        for signature in signatures:
            where = f'{rel(path)}:{signature["line"]}'
            if 'error' in signature:
                fail(f'{where}: root signature with {signature["error"]}')
                continue
            if None in signature['params']:
                fail(f'{where}: a root parameter is declared and never initialised')
                continue
            if cost(signature) > 64:
                fail(f'{where}: the root signature costs {cost(signature)} DWORDs, over the 64 allowed')
            for p in signature['params']:
                if p[0] == 'table':
                    kinds = {k for k, *_ in p[1]}
                    if 'sampler' in kinds and kinds - {'sampler'}:
                        fail(f'{where}: a descriptor table mixes SAMPLER ranges with CBV/SRV/UAV ranges')
                    if '?' in kinds or any(None in r for r in p[1]):
                        fail(f'{where}: a descriptor range this suite cannot resolve: {p[1]}')
        if len(signatures) > 1 and arrays:
            fail(f'{rel(path)}: {len(signatures)} root signatures and {len(arrays)} pipelines; which shader goes '
                 'with which is not readable here, split the file or teach the suite')
            continue
        signature = signatures[0]
        if 'error' in signature or None in signature['params']:
            continue
        passes += 1
        # Root-argument writes against the parameter they name.
        for call in re.finditer(r'\bSetComputeRoot(32BitConstants|DescriptorTable|ConstantBufferView|'
                                r'ShaderResourceView|UnorderedAccessView|32BitConstant)\s*\(', text):
            args, _ = split_args(text, call.end() - 1)
            texts = [text] + [p.read_text(errors='replace') for p in sibling_headers(path)]
            index = resolve(args[0], texts)
            where = f'{rel(path)}:{line_of(text, call.start())}'
            if index is None or index >= len(signature['params']):
                fail(f'{where}: SetComputeRoot{call.group(1)} names parameter {args[0]}, which the root signature '
                     f'(of {len(signature["params"])}) does not have')
                continue
            param = signature['params'][index]
            want = {'32BitConstants': 'constants', '32BitConstant': 'constants', 'DescriptorTable': 'table'}.get(
                call.group(1), 'root')
            if param[0] != want:
                fail(f'{where}: SetComputeRoot{call.group(1)} into parameter {index}, which is a {param[0]} parameter')
            elif call.group(1) == '32BitConstants':
                count, offset = resolve(args[1], texts), resolve(args[3], texts)
                if count is None or offset is None:
                    count = resolve(re.sub(r'sizeof\s*\(\s*\w+\s*\)\s*/\s*4', '0', args[1]), texts)
                    if count is None:
                        continue  # sizeof(T)/4: not resolvable without the type layout; the parameter type held
                if count + offset > param[3]:
                    fail(f'{where}: writes {count} root constants at {offset} into a parameter of {param[3]}')
        # The bytecode against the signature.
        for array in arrays:
            header = find_header(array)
            if header is None:
                fail(f'{rel(path)}: {array} is in no precompile/ header')
                continue
            try:
                found = dxil_bindings(header_bytes(header))
            except (ValueError, KeyError, struct.error) as error:
                fail(f'{rel(header)}: {array} is not a readable DXIL container ({error})')
                continue
            for binding in found:
                bindings += 1
                if not covered(binding, signature):
                    kind, space, lower, upper = binding
                    fail(f'{rel(path)}:{signature["line"]}: {array} binds {kind} {lower}..{upper} space {space}, '
                         'which the root signature does not cover')
            # 7. A typed UAV load of more than one 32-bit component (SFI0's TypedUAVLoadAdditionalFormats) reads a
            # format outside R32_FLOAT/UINT/SINT; past the guaranteed set (R16G16_*, R11G11B10, R10G10B10A2, ...)
            # each format is optional and only CheckFeatureSupport says whether the device loads it.
            if dxil_flags(header_bytes(header)) & SFI0_TYPED_UAV_LOAD_ADDITIONAL_FORMATS:
                typed_loads.append(array)
                if 'D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD' not in text:
                    fail(f'{rel(path)}: {array} loads typed UAVs beyond R32 (SFI0 TypedUAVLoadAdditionalFormats) and '
                         'the pass never checks D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD for its formats')
    return passes, bindings


# --- Upload / readback heaps, transitions, CBV placement -----------------------------------------------------------

def check_heaps_and_barriers(sources, fail):
    creations = transitions = cbvs = 0
    for path, raw in sources.items():
        text = strip_comments(raw)
        # (offset, name, heap type) of every heap-properties declaration; a call takes the nearest one before it,
        # since the same name (`heap`) is declared in several functions of one file.
        heaps = [(m.start(), m.group(1), m.group(2)) for m in re.finditer(
            r'\b(?:const\s+)?(?:auto|CD3DX12_HEAP_PROPERTIES|D3D12_HEAP_PROPERTIES)\s+(\w+)\s*'
            r'(?:=\s*CD3DX12_HEAP_PROPERTIES\s*|\s*)\(\s*D3D12_HEAP_TYPE_(\w+)', text)]
        heaps += [(m.start(), m.group(1), m.group(2))
                  for m in re.finditer(r'\b(\w+)\.Type\s*=\s*D3D12_HEAP_TYPE_(\w+)', text)]
        heaps.sort()

        def heap_of(name, offset):
            kinds = [kind for at, n, kind in heaps if n == name and at < offset]
            return kinds[-1] if kinds else None
        for call in re.finditer(r'\bCreateCommittedResource\s*\(', text):
            args, _ = split_args(text, call.end() - 1)
            if len(args) < 4:
                continue
            first = args[0].strip()
            inline = re.search(r'D3D12_HEAP_TYPE_(\w+)', first)
            heap = inline.group(1) if inline else heap_of(first.lstrip('&').strip(), call.start())
            state = re.sub(r'\s+', '', args[3])
            if heap in ('UPLOAD', 'READBACK'):
                creations += 1
                want = 'D3D12_RESOURCE_STATE_GENERIC_READ' if heap == 'UPLOAD' else 'D3D12_RESOURCE_STATE_COPY_DEST'
                if state != want:
                    fail(f'{rel(path)}:{line_of(text, call.start())}: a {heap} committed resource is created in '
                         f'{args[3]}, not {want.replace("D3D12_RESOURCE_STATE_", "")}')
        # CD3DX12_RESOURCE_BARRIER::Transition and every local helper whose name says transition or barrier: two
        # adjacent state arguments that are the same literal are a transition from a state to itself.
        for call in re.finditer(r'\b(\w*(?:Transition|Barrier)\w*)\s*\(', text):
            if call.group(1) in ('ResourceBarrier',):
                continue
            args, _ = split_args(text, call.end() - 1)
            states = [re.sub(r'\s+', '', a) for a in args]
            pairs = [(a, b) for a, b in zip(states, states[1:]) if a.startswith('D3D12_RESOURCE_STATE_')
                     and b.startswith('D3D12_RESOURCE_STATE_')]
            if pairs:
                transitions += 1
            for before, after in pairs:
                if before == after:
                    fail(f'{rel(path)}:{line_of(text, call.start())}: a transition from {before} to itself')
        for m in re.finditer(r'(\w+)\.Transition\.StateBefore\s*=\s*([\w|\s]+?);', text):
            after = re.search(rf'\b{m.group(1)}\.Transition\.StateAfter\s*=\s*([\w|\s]+?);',
                              text[m.end():m.end() + 600])
            transitions += 1
            if after and re.sub(r'\s+', '', after.group(1)) == re.sub(r'\s+', '', m.group(2)) and \
                    m.group(2).strip().startswith('D3D12_RESOURCE_STATE_'):
                fail(f'{rel(path)}:{line_of(text, m.start())}: a transition from {m.group(2).strip()} to itself')
        for m in re.finditer(r'\bBufferLocation\s*=\s*([^;]+);', text):
            cbvs += 1
            value = re.sub(r'\s+', '', m.group(1))
            if not re.fullmatch(r'[\w.\->\[\]]+->GetGPUVirtualAddress\(\)', value):
                fail(f'{rel(path)}:{line_of(text, m.start())}: a constant-buffer view at {m.group(1).strip()}: '
                     'anything but a resource\'s own GPU address needs its 256-byte placement shown')
        # The aggregate form, `D3D12_CONSTANT_BUFFER_VIEW_DESC v { address, size }`, which neither the line above
        # nor nr-invariants' `SizeInBytes = sizeof(T)` sees: the address as above, and the size a sizeof(T) of an
        # alignas(256) struct (nr-invariants check 5's rule) or a literal multiple of 256.
        for m in re.finditer(r'\bD3D12_CONSTANT_BUFFER_VIEW_DESC\s+\w+\s*\{([^{}]+)\}', text):
            args = [a.strip() for a in m.group(1).split(',')]
            if len(args) != 2:
                continue
            cbvs += 1
            where = f'{rel(path)}:{line_of(text, m.start())}'
            if not re.fullmatch(r'[\w.\->\[\]]+->GetGPUVirtualAddress\(\)', re.sub(r'\s+', '', args[0])):
                fail(f'{where}: a constant-buffer view at {args[0]}: anything but a resource\'s own GPU address needs '
                     'its 256-byte placement shown')
            size = re.fullmatch(r'sizeof\s*\(\s*(\w+)\s*\)', args[1])
            if size:
                name = size.group(1)
                # The struct's declaration from the sources handed in, or else from the pass's own headers on disk.
                texts = list(sources.values()) + [h.read_text(errors='replace') for h in sibling_headers(path)
                                                  if h not in sources]
                if not any(re.search(rf'\bstruct\s+alignas\(256\)\s+{name}\b', t) for t in texts):
                    fail(f'{where}: a constant-buffer view sized by {name}, which is not declared alignas(256)')
            elif not (args[1].isdigit() and int(args[1]) % 256 == 0):
                fail(f'{where}: a constant-buffer view of size {args[1]}, not shown to be a multiple of 256')
    return creations, transitions, cbvs


# --- Recorded one-way transitions are settled ------------------------------------------------------------------------

def check_settled_recordings(sources, fail):
    """8. ZeroGuides::RecordClear records a one-way UNORDERED_ACCESS -> rest-state transition once, and believes it
    happened only when told: ConfirmExecuted when the list ran, AbandonRecording when it was dropped. A caller that
    never says either leaves it believing a clear that never ran, and every later frame hands the guides on as being
    in the rest state while they are still in UNORDERED_ACCESS -- a StateBefore that is not the resource's state
    (D3D12_RESOURCE_TRANSITION_BARRIER), which the runtime trusts. So every object a file calls RecordClear on, the
    same file settles both ways."""
    sites = 0
    for path, raw in sources.items():
        text = strip_comments(raw)
        for m in re.finditer(r'\b(\w+)\s*(\.|->)\s*RecordClear\s*\(', text):
            sites += 1
            name = m.group(1)
            for settle in ('ConfirmExecuted', 'AbandonRecording'):
                if not re.search(rf'\b{name}\s*(\.|->)\s*{settle}\s*\(', text):
                    fail(f'{rel(path)}:{line_of(text, m.start())}: {name}.RecordClear is never answered with '
                         f'{name}.{settle}(); the guides\' state then describes a recording nobody ran')
    return sites


def check_module_words(sources, fail):
    """9. VkShaderModuleCreateInfo::pCode is an array of uint32_t (VUID-VkShaderModuleCreateInfo-pCode-parameter);
    create_header.py's arrays are unsigned char, aligned to one byte, so pCode is never one of them cast."""
    sites = 0
    for path, raw in sources.items():
        text = strip_comments(raw)
        arrays = set(re.findall(r'\bunsigned\s+char\s+(\w+)\s*\[', text))
        for m in re.finditer(r'\bpCode\s*=\s*([^;]+);', text):
            sites += 1
            value = m.group(1)
            cast = re.fullmatch(r'\s*reinterpret_cast<\s*const\s+uint32_t\s*\*\s*>\s*\((.+)\)\s*', value)
            if not cast:
                continue
            inner = cast.group(1).strip()
            if re.search(r'_spv\b|\.data\b(?!\(\))', inner) or inner in arrays:
                fail(f'{rel(path)}:{line_of(text, m.start())}: pCode casts {inner}, a byte array, to uint32_t*; copy '
                     'it into uint32_t storage first')
    return sites


# --- Open findings, held where they are -----------------------------------------------------------------------------
# Two findings of the audit (native-api-rules.md, "Open, for review") are mechanical but not fixed here, because the
# fix changes pixels or needs a shader rebuild. They are listed with their reason, so a new instance fails, and a
# listed one that is gone fails too until the list (and the note) are updated.

# 10. Typeless views that change the component type. Every NR pass views with translateTypeless (kViewTranslateTypeless)
# through float4 declarations; a typeless family mapped to a UINT/SINT view hands a float shader integer texels.
KNOWN_INTEGER_VIEWS = {
    'DXGI_FORMAT_R10G10B10A2_TYPELESS': 'upstream maps it to R10G10B10A2_UINT; an NR-owned UNORM mapping changes pixels '
                                        'where it applies (native-api-rules.md, open)',
}

# 11. Vulkan storage images declared with a format in modules whose slots take views of several formats. The NR and HDR
# modules write the proxy, the meter, the game's output and the HDR targets through one declaration each.
KNOWN_DECLARED_STORAGE = {
    'DlssNr_Shader_Vk.spv': 'gTarget/gKeep declared Rgba32f, bound to RGBA16F, R32_SFLOAT and the game\'s format; fix is '
                            'a rebuild with -fspv-use-unknown-image-format (native-api-rules.md, open)',
    'DlssNr_PresentHdr_Shader_Vk.spv': 'gOut declared Rgba32f, bound to RGBA16F and A2B10G10R10 (same fix)',
}
MULTI_FORMAT_MODULES = ('DlssNr_Shader_Vk.spv', 'DlssNr_PresentHdr_Shader_Vk.spv')


def check_integer_views(table_text, fail):
    found = {}
    pending = []
    for line in strip_comments(table_text).splitlines():
        case = re.match(r'\s*case\s+(DXGI_FORMAT_\w+)\s*:', line)
        if case:
            pending.append(case.group(1))
            continue
        ret = re.match(r'\s*return\s+(DXGI_FORMAT_\w+)\s*;', line)
        if ret:
            for name in pending:
                found[name] = ret.group(1)
            pending = []
    integer = {k: v for k, v in found.items() if k.endswith('_TYPELESS') and re.search(r'_(UINT|SINT)$', v)}
    for name, view in integer.items():
        if name not in KNOWN_INTEGER_VIEWS:
            fail(f'TranslateTypelessFormats maps {name} to {view}: an integer view for float shaders')
    for name in KNOWN_INTEGER_VIEWS:
        if name not in integer:
            fail(f'{name} no longer maps to an integer view: take it off KNOWN_INTEGER_VIEWS and native-api-rules.md')
    return len(found)


def check_declared_storage(modules, fail):
    for path, blob in modules.items():
        if path.name not in MULTI_FORMAT_MODULES:
            continue
        ins = spirv(blob)
        declared = [w for op, w in ins if op == OP_TYPE_IMAGE and w[6] == 2 and w[7] != 0]  # Sampled 2, a format
        if declared and path.name not in KNOWN_DECLARED_STORAGE:
            fail(f'{rel(path)}: a storage image declared with a format, in a module whose slots take several formats')
        if not declared and path.name in KNOWN_DECLARED_STORAGE:
            fail(f'{rel(path)}: no storage image declares a format any more: take it off KNOWN_DECLARED_STORAGE and '
                 'native-api-rules.md')


# --- SPIR-V ---------------------------------------------------------------------------------------------------------

OP_NAME, OP_MEMBER_DECORATE, OP_DECORATE = 5, 72, 71
OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_MATRIX, OP_TYPE_IMAGE = 21, 22, 23, 24, 25
OP_TYPE_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_CONSTANT, OP_VARIABLE = 28, 30, 32, 43, 59
OP_LOAD, OP_IMAGE_READ, OP_IMAGE_WRITE, OP_CAPABILITY = 61, 98, 99, 17
SC_PUSH_CONSTANT, DEC_OFFSET, DEC_ARRAY_STRIDE = 9, 35, 6
CAP_READ_WITHOUT_FORMAT, CAP_WRITE_WITHOUT_FORMAT = 55, 56


def spirv(blob):
    words = struct.unpack(f'<{len(blob) // 4}I', blob[:len(blob) // 4 * 4])
    if not words or words[0] != 0x07230203:
        raise ValueError('not SPIR-V')
    at, out = 5, []
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 0xFFFF
        if count == 0:
            raise ValueError('zero-length instruction')
        out.append((op, words[at + 1:at + count]))
        at += count
    return out


def spirv_facts(blob):
    """(push-constant blocks [(name, size, offsets)], unknown-format storage images [(id, read, written)], caps)."""
    ins = spirv(blob)
    types, consts, offsets, strides, names, pointers, variables, caps = {}, {}, {}, {}, {}, {}, {}, set()
    for op, w in ins:
        if op == OP_CAPABILITY:
            caps.add(w[0])
        elif op == OP_NAME:
            names[w[0]] = bytes(b for word in w[1:] for b in struct.pack('<I', word)).split(b'\0')[0].decode()
        elif op == OP_MEMBER_DECORATE and w[2] == DEC_OFFSET:
            offsets.setdefault(w[0], {})[w[1]] = w[3]
        elif op == OP_DECORATE and w[1] == DEC_ARRAY_STRIDE:
            strides[w[0]] = w[2]
        elif op in (OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_MATRIX, OP_TYPE_IMAGE, OP_TYPE_ARRAY,
                    OP_TYPE_STRUCT):
            types[w[0]] = (op, w[1:])
        elif op == OP_TYPE_POINTER:
            pointers[w[0]] = (w[1], w[2])
        elif op == OP_CONSTANT:
            consts[w[1]] = w[2]
        elif op == OP_VARIABLE:
            variables[w[1]] = (w[0], w[2])

    def size(type_id):
        op, w = types[type_id]
        if op in (OP_TYPE_INT, OP_TYPE_FLOAT):
            return w[0] // 8
        if op == OP_TYPE_VECTOR:
            return size(w[0]) * w[1]
        if op == OP_TYPE_MATRIX:
            return size(w[0]) * w[1]
        if op == OP_TYPE_ARRAY:
            return strides.get(type_id, size(w[0])) * consts[w[1]]
        if op == OP_TYPE_STRUCT:
            member_offsets = offsets.get(type_id, {})
            return max((member_offsets.get(i, 0) + size(m) for i, m in enumerate(w)), default=0)
        raise ValueError(f'size of type op {op}')

    blocks = []
    for var, (pointer, storage) in variables.items():
        if storage == SC_PUSH_CONSTANT:
            block = pointers[pointer][1]
            blocks.append((names.get(block, f'%{block}'), size(block), sorted(offsets.get(block, {}).values())))

    # Storage images (Sampled == 2) with format Unknown (0), and whether the module reads or writes one.
    unknown = {t for t, (op, w) in types.items() if op == OP_TYPE_IMAGE and w[5] == 2 and w[6] == 0}
    loaded = {}
    for op, w in ins:
        if op == OP_LOAD and w[0] in unknown:
            loaded[w[1]] = w[0]
    read = any(op == OP_IMAGE_READ and w[2] in loaded for op, w in ins)
    written = any(op == OP_IMAGE_WRITE and w[0] in loaded for op, w in ins)
    return blocks, (bool(unknown), read, written), caps


def check_spirv(modules, loaders, fail):
    """modules: {path: bytes}; loaders: {path: text} of the code that may dispatch them."""
    blocks_seen = 0
    for path, blob in modules.items():
        try:
            blocks, (unknown, read, written), caps = spirv_facts(blob)
        except (ValueError, KeyError, struct.error) as error:
            fail(f'{rel(path)}: not a readable SPIR-V module ({error})')
            continue
        for name, size, offs in blocks:
            blocks_seen += 1
            if size > 128:
                fail(f'{rel(path)}: push-constant block {name} is {size} bytes, over the 128 every device offers')
            if any(o % 4 for o in offs):
                fail(f'{rel(path)}: push-constant block {name} has a member offset that is not a multiple of 4')
        feature = []
        if read:
            feature.append('shaderStorageImageReadWithoutFormat')
        if written:
            feature.append('shaderStorageImageWriteWithoutFormat')
        if read and CAP_READ_WITHOUT_FORMAT not in caps or written and CAP_WRITE_WITHOUT_FORMAT not in caps:
            fail(f'{rel(path)}: a storage image of format Unknown is accessed without the matching capability')
        stem = path.name.replace('_Shader_Vk.spv', '').replace('.spv', '')
        for want in feature:
            users = [p for p, t in loaders.items() if re.search(rf'\b{re.escape(stem)}\w*_spv\b', t)]
            if not users:
                fail(f'{rel(path)}: needs {want} and no source loads it under a name this suite can match')
            for user in users:
                if want not in loaders[user]:
                    fail(f'{rel(user)}: dispatches {path.name}, which needs {want}, and never checks the feature')
    return blocks_seen


def check_spirv_val(modules, fail, notes):
    tool = shutil.which('spirv-val')
    if tool is None:
        notes.append('spirv-val not installed: module validation skipped')
        return 0
    for path in modules:
        result = subprocess.run([tool, '--target-env', 'vulkan1.1', str(path)], capture_output=True, text=True)
        if result.returncode != 0:
            fail(f'{rel(path)}: spirv-val --target-env vulkan1.1: {(result.stdout + result.stderr).strip()[:300]}')
    return len(modules)


def rel(path):
    try:
        return str(Path(path).relative_to(root))
    except ValueError:
        return str(path)


# --- the live tree --------------------------------------------------------------------------------------------------

def tree_sources(dirs):
    files = {}
    for base in dirs:
        for pattern in ('*.cpp', '*.h'):
            for path in sorted(base.glob(pattern)):
                files[path] = path.read_text(errors='replace')
    return files


def run_tree():
    failures, notes = [], []
    fail = failures.append
    pass_sources = {p: t for p, t in tree_sources(PASS_DIRS).items() if p.suffix == '.cpp'}
    typed_loads.clear()  # the self-test's runs added theirs
    passes, bindings = check_root_signatures(pass_sources, fail)
    if passes < 12 or bindings < 60:
        fail(f'only {passes} passes and {bindings} bindings read; the parser no longer matches the code')
    else:
        print(f'PASS 1-2: {bindings} DXIL bindings of {passes} passes lie in their root signatures; costs, tables '
              'and root-argument writes hold')
    if not typed_loads:
        fail('no DXIL with TypedUAVLoadAdditionalFormats found; the synthesized-motion estimator has four')
    elif not any('UAV_TYPED_LOAD' in f for f in failures):
        print(f'PASS 7: {len(typed_loads)} shaders loading typed UAVs beyond R32 are dispatched by passes that check '
              'the format')

    host = {p: t for p, t in tree_sources(HOST_DIRS).items()}
    creations, transitions, cbvs = check_heaps_and_barriers(host, fail)
    if creations < 10 or transitions < 20 or cbvs < 8:
        fail(f'only {creations} upload/readback creations, {transitions} transitions and {cbvs} CBVs read; the '
             'parser no longer matches the code')
    else:
        print(f'PASS 3-5: {creations} upload/readback resources in their fixed state, {transitions} transitions '
              f'between two states, {cbvs} CBVs at their buffer\'s own address')

    before = len(failures)
    settled = check_settled_recordings(host, fail)
    if settled < 2:
        fail(f'only {settled} RecordClear sites read; the scan no longer matches the code')
    elif len(failures) == before:
        print(f'PASS 8: {settled} zero-guide clear recordings are each settled both ways by their caller')

    before = len(failures)
    words = check_module_words(host, fail)
    if words < 2:
        fail(f'only {words} pCode sites read; the scan no longer matches the code')
    elif len(failures) == before:
        print(f'PASS 9: {words} shader modules are created from uint32_t storage')

    modules = {p: p.read_bytes() for base in PASS_DIRS for p in sorted((base / 'precompile').glob('*.spv'))}
    before = len(failures)
    mapped = check_integer_views((root / 'OptiScaler/shaders/Shader_Dx12.cpp').read_text(errors='replace'), fail)
    check_declared_storage(modules, fail)
    if mapped < 10:
        fail(f'only {mapped} TranslateTypelessFormats entries read; the parser no longer matches the code')
    elif len(failures) == before:
        print(f'PASS 10-11: the open findings are where the audit left them ({len(KNOWN_INTEGER_VIEWS)} integer view '
              f'mapping, {len(KNOWN_DECLARED_STORAGE)} modules with declared storage formats), and no new one')
    blocks = check_spirv(modules, host, fail)
    validated = check_spirv_val(modules, fail, notes)
    if len(modules) < 10:
        fail(f'only {len(modules)} SPIR-V modules found; the scan no longer matches the tree')
    else:
        print(f'PASS 6: {len(modules)} SPIR-V modules, {blocks} push-constant blocks within 128 bytes, no unchecked '
              f'format-less storage image' + (f', {validated} valid for Vulkan 1.1' if validated else ''))
    return failures, notes


# --- seeded violations ----------------------------------------------------------------------------------------------

def self_test():
    """Each check must fail on a violation planted in a copy of a real pass, and pass on the copy unchanged."""
    problems = []
    cadence = nr / 'shaders/dlssnr/DlssNr_Cadence_Dx12.cpp'
    synth = nr / 'shaders/synth_motion/SynthMotion_Dx12.cpp'
    cadence_text, synth_text = cadence.read_text(), synth.read_text()

    def outcome(sources):
        found = []
        check_root_signatures(sources, found.append)
        check_heaps_and_barriers(sources, found.append)
        return found

    def expect(label, sources, needle):
        found = outcome(sources)
        if not any(needle in f for f in found):
            problems.append(f'{label}: expected a failure containing "{needle}", got {found or "none"}')

    clean = outcome({cadence: cadence_text, synth: synth_text})
    if clean:
        problems.append(f'unchanged copies fail: {clean}')
    # 1. One SRV fewer in the root signature than the cadence shader binds.
    expect('binding outside', {cadence: cadence_text.replace('SetupRootSignature(InDevice, kSrvCount, kUavCount, 1)',
                                                             'SetupRootSignature(InDevice, kSrvCount - 1, kUavCount, 1)')},
           'which the root signature does not cover')
    # 2. Root constants past the parameter, a table written as constants, an expensive signature, a mixed table.
    expect('constants overflow', {synth: synth_text.replace('SetComputeRoot32BitConstants(2, 8, constants, 0)',
                                                            'SetComputeRoot32BitConstants(2, 8, constants, 4)')},
           'root constants at 4 into a parameter of 8')
    expect('wrong parameter kind', {synth: synth_text.replace('SetComputeRoot32BitConstants(2, 8, constants, 0)',
                                                              'SetComputeRoot32BitConstants(0, 8, constants, 0)')},
           'which is a table parameter')
    expect('cost', {synth: synth_text.replace('parameters[3].InitAsConstants(8, 1);',
                                              'parameters[3].InitAsConstants(60, 1);')}, 'DWORDs, over the 64')
    expect('mixed table', {cadence: cadence_text.replace('SetupRootSignature(InDevice, kSrvCount, kUavCount, 1)',
                                                         'SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 1)')},
           'mixes SAMPLER ranges')
    # 3. An upload buffer created in COMMON.
    expect('upload state', {cadence: cadence_text.replace(
        'D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,\n                                                        '
        'IID_PPV_ARGS(&_constantBuffers[i])',
        'D3D12_RESOURCE_STATE_COMMON, nullptr,\n                                                        '
        'IID_PPV_ARGS(&_constantBuffers[i])')}, 'UPLOAD committed resource is created in')
    # 4. A transition to the same state.
    expect('same-state transition', {cadence: cadence_text + '\nvoid Seed(ID3D12Resource* r) { auto b = '
                                     'CD3DX12_RESOURCE_BARRIER::Transition(r, D3D12_RESOURCE_STATE_COPY_DEST, '
                                     'D3D12_RESOURCE_STATE_COPY_DEST); }\n'}, 'to itself')
    # 5. A CBV at an offset.
    expect('cbv offset', {cadence: cadence_text.replace('cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();',
                                                        'cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress() + 48;')},
           '256-byte placement')
    # 7. The format check taken out of the estimator that loads R16G16_SINT flow.
    expect('typed uav load', {synth: synth_text.replace('D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD', 'D3D12_FORMAT_SUPPORT2_NONE')},
           'never checks D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD')
    # 5b. An aggregate CBV sized by a struct that is not alignas(256) (the guide resample's 48 bytes, 13f10d60).
    cadence_header = cadence.with_suffix('.h')
    header_text = cadence_header.read_text()
    expect('cbv aggregate size', {cadence: cadence_text, cadence_header: header_text.replace(
        'struct alignas(256) Constants', 'struct Constants')}, 'which is not declared alignas(256)')
    # 8. The present host's clear, never abandoned.
    host_cpp = nr / 'dlssnr/DlssNr_PresentHost.cpp'
    found = []
    check_settled_recordings({host_cpp: host_cpp.read_text().replace('_guides.AbandonRecording()', '_guides.Forget()')},
                             found.append)
    if not any('never answered with _guides.AbandonRecording' in f for f in found):
        problems.append(f'unsettled clear: expected a failure, got {found or "none"}')
    found = []
    check_settled_recordings({host_cpp: host_cpp.read_text()}, found.append)
    if found:
        problems.append(f'the present host\'s settled clear fails: {found}')
    # 9. The estimator's modules cast straight from their byte arrays, as before 2026-10-23.
    vk = nr / 'shaders/synth_motion/SynthMotion_Vk.cpp'
    found = []
    check_module_words({vk: vk.read_text().replace('moduleInfo.pCode = code.data();',
                                                   'moduleInfo.pCode = reinterpret_cast<const uint32_t*>(kBlobs[pass].data);')},
                       found.append)
    if not any('a byte array' in f for f in found):
        problems.append(f'byte-aligned pCode: expected a failure, got {found or "none"}')
    # 10. A second integer view, and the known one fixed.
    table = (root / 'OptiScaler/shaders/Shader_Dx12.cpp').read_text()
    found = []
    check_integer_views(table.replace('return DXGI_FORMAT_R16G16_FLOAT;', 'return DXGI_FORMAT_R16G16_UINT;'), found.append)
    if not any('R16G16_TYPELESS to DXGI_FORMAT_R16G16_UINT' in f for f in found):
        problems.append(f'new integer view: expected a failure, got {found or "none"}')
    found = []
    check_integer_views(table.replace('return DXGI_FORMAT_R10G10B10A2_UINT;', 'return DXGI_FORMAT_R10G10B10A2_UNORM;'),
                        found.append)
    if not any('no longer maps' in f for f in found):
        problems.append(f'fixed integer view: expected a failure, got {found or "none"}')
    # 6. SPIR-V: an oversized push-constant block, and a format-less storage write nobody checks.
    pc = root / 'retoque/shaders/synth_motion/precompile/SynthMotion_Expand_Shader_Vk.spv'
    blob = bytearray(pc.read_bytes())
    found = []
    words = list(struct.unpack(f'<{len(blob) // 4}I', bytes(blob)))
    at = 5
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 0xFFFF
        if op == OP_MEMBER_DECORATE and words[at + 3] == DEC_OFFSET and words[at + 2] == 2:
            words[at + 4] = 200  # member 2 moved to offset 200
            break
        at += count
    check_spirv({pc: struct.pack(f'<{len(words)}I', *words)}, {}, found.append)
    if not any('over the 128' in f for f in found):
        problems.append(f'push-constant size: expected a failure, got {found or "none"}')
    nr_vk = root / 'retoque/shaders/dlssnr/precompile/DlssNr_Shader_Vk.spv'
    words = list(struct.unpack(f'<{len(nr_vk.read_bytes()) // 4}I', nr_vk.read_bytes()))
    at, patched = 5, False
    while at < len(words):
        count, op = words[at] >> 16, words[at] & 0xFFFF
        if op == OP_TYPE_IMAGE and words[at + 7] == 2 and words[at + 8] != 0:
            words[at + 8] = 0  # the output storage image loses its format
            patched = True
        at += count
    seeded = struct.pack(f'<{len(words)}I', *words)
    found = []
    check_spirv({nr_vk: seeded}, {Path('loader.cpp'): 'DlssNr_Shader_Vk.h dlssnr_spv'}, found.append)
    if not patched or not any('without the matching capability' in f for f in found):
        problems.append(f'format-less storage image: expected a failure, got {found or "none"}')
    found = []
    check_spirv({Path('DlssNr_Shader_Vk.spv'): seeded}, {Path('loader.cpp'): 'DlssNr_Shader_Vk_spv'}, found.append)
    if not any('never checks the feature' in f for f in found):
        problems.append(f'unchecked feature: expected a failure, got {found or "none"}')
    # 11. The NR module with its storage images made format-less: the list must be updated.
    found = []
    check_declared_storage({Path('DlssNr_Shader_Vk.spv'): seeded}, found.append)
    if not any('no storage image declares a format' in f for f in found):
        problems.append(f'fixed declared storage: expected a failure, got {found or "none"}')
    return problems


if __name__ == '__main__':
    problems = self_test()
    for problem in problems:
        print(f'FAIL (self-test): {problem}')
    if not problems:
        print('PASS 0: every check fails on its seeded violation and passes on the unchanged copies')
    failures, notes = run_tree()
    for note in notes:
        print(f'NOTE: {note}')
    for failure in failures:
        print(f'FAIL: {failure}')
    sys.exit(1 if failures or problems else 0)
