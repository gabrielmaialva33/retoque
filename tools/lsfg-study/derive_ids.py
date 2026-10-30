#!/usr/bin/env python3
"""Re-derive LsfgShaderLibrary's resource-id -> role table from the user's own lsfg-vk.dll, without an id table.

    derive_ids.py [path/to/lsfg-vk.dll] [--json]

The DLL is read in memory. Only interface facts are printed (binding numbers and classes, descriptor counts,
image formats, workgroup sizes, instruction counts); no shader bytes are written anywhere.

Roles come from two lsfg-vk revisions that are not CC BY-NC-ND:
  - GPL-3.0-or-later, PancakeTAS/lsfg-vk 8b0da2661c6f3473a7fccc8ba643880050e71642:
    lsfg-vk-backend/src/lsfgvk.cpp (chain wiring, levels, samplers), src/shaderchains/*.cpp (inputs, outputs,
    dispatch tiles, image formats), src/extraction/shader_registry.cpp (per-role sampled/storage image counts for
    quality and performance);
  - MIT, lsfg-vk v1.0.0 (7113d7d0): src/extract/extract.cpp (chain names alpha[0..3], beta[0..4], gamma[0..4],
    delta[0..9]; p_ performance variants; mipmaps and generate without performance variants).
Both name shaders by Lossless.dll resource ids, which are not lsfg-vk.dll's. The ids here come only from the DLL:
which binding each module writes and reads (dataflow), its descriptor counts, samplers and image formats, its
Float16 capability, and how many image writes and reads it has. Retoque's `epsilon0..4` is lsfg-vk's `delta[5..9]`.

Exit 0 when the derived table equals retoque/lsfg/LsfgShaderLibrary.cpp, 1 when it differs, 2 when the DLL does not
have the structure the roles describe (a Lossless Scaling update can change it).
"""

import argparse
import json
from pathlib import Path
import re
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import convert  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
LIBRARY_CPP = REPO / "retoque" / "lsfg" / "LsfgShaderLibrary.cpp"

# shader_registry.cpp (8b0da266), SHADER(id, sampled, storage, buffers, samplers): (sampled, storage) per role,
# quality then performance. Only the direction quality vs performance is used, never the absolute counts: lsfg-vk.dll
# packs images into arrays, so its descriptor counts differ from Lossless.dll's.
GPL_COUNTS = {
    "alpha0": ((1, 2), (1, 1)), "alpha1": ((2, 2), (1, 1)), "alpha2": ((2, 4), (1, 2)), "alpha3": ((4, 4), (2, 2)),
    "beta0": ((12, 2), (6, 2)), "beta1": ((2, 2), (2, 2)), "beta2": ((2, 2), (2, 2)), "beta3": ((2, 2), (2, 2)),
    "beta4": ((2, 6), (2, 6)),
    "gamma0": ((9, 3), (5, 3)), "gamma1": ((3, 4), (3, 2)), "gamma2": ((4, 4), (2, 2)), "gamma3": ((4, 4), (2, 2)),
    "gamma4": ((6, 1), (4, 1)),
    "delta0": ((9, 3), (5, 3)), "delta1": ((3, 4), (3, 2)), "delta2": ((4, 4), (2, 2)), "delta3": ((4, 4), (2, 2)),
    "delta4": ((6, 1), (4, 1)),
    "epsilon0": ((10, 2), (6, 1)), "epsilon1": ((2, 2), (1, 1)), "epsilon2": ((2, 2), (1, 1)),
    "epsilon3": ((2, 2), (1, 1)), "epsilon4": ((3, 1), (2, 1)),
}
GPL_NAME = {f"epsilon{i}": f"delta[{5 + i}]" for i in range(5)}

# Samplers per role (shaderchains/*.cpp): bnb = clamp to border, transparent black; bnw = clamp to border, opaque
# white; eab = clamp to edge, compare always (lsfgvk.cpp createCtx).
BNW_ROLES = {"beta0", "gamma0", "delta0", "epsilon0"}
EAB_ROLES = {"generate", "gamma0", "gamma4", "delta0", "delta4", "epsilon0", "epsilon4"}

# Dispatch tile per role (add_shift_extent(extent, a, i): 1 << i pixels): mipmaps 64, generate 16, beta[4] 32, else 8.
TILES = {"mipmaps": 64, "generate": 16, "beta4": 32}

SPV_FORMATS = {0: "unknown", 1: "rgba32f", 2: "rgba16f", 4: "rgba8", 10: "rgba16", 15: "r8"}
OP_IMAGE_READS = range(87, 99)  # OpImageSample*, OpImageFetch, OpImageGather, OpImageDrefGather, OpImageRead
OP_IMAGE_WRITE = 99


class DeriveError(Exception):
    pass


def reflect(spv: bytes) -> dict:
    """Interface facts of one SPIR-V module."""
    if len(spv) < 20 or len(spv) % 4 or spv[:4] != bytes.fromhex("03022307"):
        raise DeriveError("not a SPIR-V module")
    words = struct.unpack_from(f"<{len(spv) // 4}I", spv)
    decorations, types, constants, pointers, variables = {}, {}, {}, {}, {}
    local, writes, reads = None, 0, 0
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        a = words[i + 1 : i + count]
        if op == 71 and len(a) >= 3:  # OpDecorate
            decorations.setdefault(a[0], {})[a[1]] = a[2]
        elif op == 16 and len(a) >= 5 and a[1] == 17:  # OpExecutionMode LocalSize
            local = tuple(a[2:5])
        elif op == 43 and len(a) >= 3:  # OpConstant
            constants[a[1]] = a[2]
        elif op == 25:  # OpTypeImage
            types[a[0]] = ("image", a[6], a[7])
        elif op == 26:
            types[a[0]] = ("sampler",)
        elif op == 27:
            types[a[0]] = ("sampled-image",)
        elif op == 28:
            types[a[0]] = ("array", a[1], a[2])
        elif op == 30:
            types[a[0]] = ("struct",)
        elif op == 32:
            pointers[a[0]] = a[2]
        elif op == 59:
            variables[a[1]] = a[0]
        elif op == OP_IMAGE_WRITE:
            writes += 1
        elif op in OP_IMAGE_READS:
            reads += 1
        i += count if count else 1

    bindings = {}
    for var, pointer in variables.items():
        binding = decorations.get(var, {}).get(33)  # Binding; push constants have none
        if binding is None or pointer not in pointers:
            continue
        kind, length = types.get(pointers[pointer], ("?",)), 1
        if kind[0] == "array":
            length = constants.get(kind[2], 0)
            kind = types.get(kind[1], ("?",))
        if kind[0] == "image":
            storage = kind[1] == 2
            bindings[binding] = ("u" if storage else "t", length, SPV_FORMATS.get(kind[2], str(kind[2])) if storage else "")
        elif kind[0] == "sampled-image":
            bindings[binding] = ("t", length, "")
        elif kind[0] == "sampler":
            bindings[binding] = ("s", length, "")
        elif kind[0] == "struct":
            bindings[binding] = ("b", length, "")
        else:
            bindings[binding] = ("?", length, "")
    return {
        "bindings": dict(sorted(bindings.items())),
        "local": local,
        "writes": writes,
        "reads": reads,
        "float16": convert.declares_float16(spv),
    }


def _sign(x: int) -> int:
    return (x > 0) - (x < 0)


def derive(modules: dict[int, bytes]) -> dict:
    """{rid: {"role", "fp16", "performance", "evidence"}} from interface facts and the roles above."""
    facts = {rid: reflect(blob) for rid, blob in modules.items()}

    # Dataflow on binding signatures: every variant of a role declares the same bindings.
    def key(rid):
        return tuple((b, c, n) for b, (c, n, _) in facts[rid]["bindings"].items())

    groups = {}
    for rid in sorted(facts):
        groups.setdefault(key(rid), []).append(rid)

    def classes(k, cls):
        return {b for b, c, _ in k if c == cls}

    def count(k, binding):
        return next(n for b, _, n in k if b == binding)

    uav = {}
    for k in groups:
        u = classes(k, "u")
        if len(u) != 1:
            raise DeriveError(f"modules {groups[k]} write {len(u)} bindings, expected 1")
        uav[k] = next(iter(u))
    writer = {u: k for k, u in uav.items()}
    if len(writer) != len(uav):
        raise DeriveError("two binding signatures write the same binding")
    external = {b for k in groups for b in classes(k, "t") if b - 1 not in writer}

    def readers(k):
        return [r for r in groups if uav[k] + 1 in classes(r, "t")]

    def unique(candidates, what):
        if len(candidates) != 1:
            raise DeriveError(f"{what}: {len(candidates)} candidates")
        return candidates[0]

    roles = {}  # signature -> role
    mipmaps = unique([k for k in groups if classes(k, "t") <= external and readers(k)], "mipmaps")
    generate = unique([k for k in groups if not readers(k) and classes(k, "t") & external], "generate")
    roles[mipmaps], roles[generate] = "mipmaps", "generate"

    def chain(name, head, length):
        seq = [head]
        for _ in range(length - 1):
            seq.append(unique([r for r in readers(seq[-1]) if r not in roles and r not in seq and r not in heads],
                              f"{name}{len(seq)}"))
        for i, k in enumerate(seq):
            roles[k] = f"{name}{i}"
        return seq

    heads = []
    alpha = chain("alpha", unique(readers(mipmaps), "alpha0"), 4)  # Alpha0 (alpha[0..2]) + Alpha1 (alpha[3])
    alpha_out = uav[alpha[-1]]
    after_alpha = [r for r in readers(alpha[-1]) if r not in roles]
    beta_head = unique([r for r in after_alpha if classes(r, "t") == {alpha_out + 1}], "beta0 (reads only alpha)")
    heads = [r for r in after_alpha if r != beta_head]
    if len(heads) != 3:
        raise DeriveError(f"{len(heads)} chains start from alpha's output besides beta, expected gamma, delta, delta[5]")
    beta = chain("beta", beta_head, 5)
    chains = {h: chain(f"head{n}_", h, 5) for n, h in enumerate(heads)}
    for k in [k for k, r in roles.items() if r.startswith("head")]:
        del roles[k]

    finals = {h: uav[seq[-1]] for h, seq in chains.items()}
    epsilon = unique([h for h in heads if all(finals[o] + 1 in classes(h, "t") for o in heads if o != h)],
                     "delta[5] (reads alpha, gamma and delta)")
    rest = [h for h in heads if h != epsilon]
    # gamma[0] and delta[0] take their own previous level (lsfgvk.cpp: gamma1.at(j - 1), delta1.at(j - 5).getImage0());
    # delta[5..9] takes it at its last step (delta1.cpp: additionalInput2 into delta[9]).
    for h, first in [(h, h) for h in rest] + [(epsilon, chains[epsilon][-1])]:
        if finals[h] + 1 not in classes(first, "t"):
            raise DeriveError(f"chain {groups[h]} does not read its own previous level")
    levels = {h: count(h, uav[h]) for h in rest}
    gamma = unique([h for h in rest if levels[h] == 7], "gamma (7 levels)")
    delta = unique([h for h in rest if levels[h] == 3], "delta (levels 4..6)")
    for name, h in (("gamma", gamma), ("delta", delta), ("epsilon", epsilon)):
        for i, k in enumerate(chains[h]):
            roles[k] = f"{name}{i}"

    # Checks against the GPL wiring the roles above did not use.
    by_role = {r: k for k, r in roles.items()}
    beta_out = uav[by_role["beta4"]] + 1
    checks = [
        (count(by_role["mipmaps"], uav[mipmaps]) == 7, "mipmaps writes 7 levels"),
        (count(by_role["beta4"], uav[by_role["beta4"]]) == 6, "beta4 writes 6 levels"),
        (count(epsilon, uav[epsilon]) == 3, "delta[5] runs at levels 4..6"),
        (beta_out in classes(by_role["gamma4"], "t") and beta_out in classes(by_role["delta4"], "t"),
         "gamma4 and delta4 read beta's output"),
        (beta_out not in classes(by_role["epsilon4"], "t"), "delta[9] does not read beta's output"),
        ({finals[gamma] + 1, finals[delta] + 1, finals[epsilon] + 1} <= classes(generate, "t"),
         "generate reads the gamma, delta and delta[5..9] results"),
        (len(by_role) == 26, "26 binding signatures: mipmaps, generate (8- and 16-bit share one), 24 core roles"),
    ]
    sampler_users = {}
    for k, r in roles.items():
        for b in classes(k, "s"):
            sampler_users.setdefault(b, set()).add(r)
    named = {frozenset(BNW_ROLES): "bnw", frozenset(EAB_ROLES): "eab"}
    sampler_kind = {b: named.get(frozenset(users), "bnb") for b, users in sampler_users.items()}
    checks.append((sorted(sampler_kind.values()) == ["bnb", "bnw", "eab"],
                   "three samplers, bnw and eab used by exactly the GPL roles"))
    for ok, what in checks:
        if not ok:
            raise DeriveError(f"structure check failed: {what}")

    # Variants: Float16 capability; quality writes/reads more where GPL's quality counts are larger.
    out = {}
    offsets = set()
    inferred = []
    for k, role in roles.items():
        rids = groups[k]
        if role == "mipmaps":
            out[rids[0]] = {"role": role, "fp16": False, "performance": False}
            continue
        if role == "generate":
            for rid in rids:
                fmt = facts[rid]["bindings"][uav[k]][2]
                out[rid] = {"role": {"rgba8": "generate_8bit", "rgba16": "generate_16bit"}.get(fmt, f"generate_{fmt}"),
                            "fp16": False, "performance": False}
            continue
        if len(rids) != 4:
            raise DeriveError(f"{role}: {len(rids)} variants, expected 4")
        (q_sampled, q_storage), (p_sampled, p_storage) = GPL_COUNTS[role]
        expect = (_sign(q_storage - p_storage), _sign(q_sampled - p_sampled))
        pairs = {}
        for fp16 in (False, True):
            pair = [r for r in rids if facts[r]["float16"] == fp16]
            if len(pair) != 2:
                raise DeriveError(f"{role}: {len(pair)} modules with Float16={fp16}, expected 2")
            a, b = pair
            seen = (_sign(facts[a]["writes"] - facts[b]["writes"]), _sign(facts[a]["reads"] - facts[b]["reads"]))
            if expect == (0, 0):
                if seen != (0, 0):
                    raise DeriveError(f"{role}: variants differ in image accesses where GPL's counts are equal")
                pairs[fp16] = None
            elif seen == expect:
                pairs[fp16] = (a, b)
            elif seen == (-expect[0], -expect[1]):
                pairs[fp16] = (b, a)
            else:
                raise DeriveError(f"{role}: image accesses {seen} contradict GPL's quality/performance {expect}")
        if pairs[False] is None or pairs[True] is None:
            inferred.append((role, rids))
            continue
        (q32, p32), (q16, p16) = pairs[False], pairs[True]
        offsets.add((p32 - q32, q16 - q32, p16 - q32))
        for rid, fp16, perf in ((q32, 0, 0), (p32, 0, 1), (q16, 1, 0), (p16, 1, 1)):
            out[rid] = {"role": role, "fp16": bool(fp16), "performance": bool(perf), "evidence": "fingerprint"}
    if len(offsets) != 1:
        raise DeriveError(f"fingerprinted roles disagree on the variant layout: {sorted(offsets)}")
    perf_off, fp16_off, both_off = next(iter(offsets))
    for role, rids in inferred:
        base = min(rids)
        layout = {base: (0, 0), base + perf_off: (0, 1), base + fp16_off: (1, 0), base + both_off: (1, 1)}
        if sorted(layout) != sorted(rids) or any(facts[r]["float16"] != bool(v[0]) for r, v in layout.items()):
            raise DeriveError(f"{role}: modules {rids} do not fit the variant layout of the fingerprinted roles")
        for rid, (fp16, perf) in layout.items():
            out[rid] = {"role": role, "fp16": bool(fp16), "performance": bool(perf), "evidence": "layout"}

    for k, role in roles.items():
        tile = TILES.get(role, 8)
        for rid in groups[k]:
            local = facts[rid]["local"]
            expected = (tile // 2, tile // 2, 1) if role == "mipmaps" else (tile, tile, 1)  # mipmaps: 2x2 per thread
            if local != expected:
                raise DeriveError(f"{role} ({rid}): workgroup {local}, expected {expected} for a {tile}-pixel tile")
            out[rid].setdefault("evidence", "unique")
            out[rid]["gpl"] = GPL_NAME.get(role, re.sub(r"(\d)$", r"[\1]", role))
            out[rid]["samplers"] = sorted(sampler_kind[b] for b in classes(k, "s"))
            out[rid]["local"] = list(local)
    return dict(sorted(out.items()))


def expected_from_source(path: Path = LIBRARY_CPP) -> dict:
    """{rid: (role, fp16, performance)} as LsfgShaderLibrary::ResolveResourceId resolves them."""
    text = path.read_text()
    base = dict(re.findall(r'shaderName == "(\w+)"\)\s*return (\d+);', text))
    block = re.search(r"kCoreShaders = \{(.*?)\};", text, re.S)
    variant = re.search(r"\(fp16 \? (\d+) : 0\) \+ \(performance \? (\d+) : 0\)", text)
    if not block or not variant:
        raise DeriveError(f"{path}: kCoreShaders or the variant rule not found")
    fp16_off, perf_off = int(variant[1]), int(variant[2])
    table = {int(rid): (name, False, False) for name, rid in base.items() if name != "generate"}
    for name, rid in re.findall(r'\{ "(\w+)", (\d+) \}', block[1]):
        for fp16 in (False, True):
            for perf in (False, True):
                table[int(rid) + fp16 * fp16_off + perf * perf_off] = (name, fp16, perf)
    return dict(sorted(table.items()))


def compare(derived: dict, expected: dict) -> list[str]:
    got = {rid: (v["role"], v["fp16"], v["performance"]) for rid, v in derived.items()}
    return [f"rid {rid}: derived {got.get(rid)}, LsfgShaderLibrary {expected.get(rid)}"
            for rid in sorted(set(got) | set(expected)) if got.get(rid) != expected.get(rid)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("dll", nargs="?", type=Path)
    parser.add_argument("--json", action="store_true", help="print the derived table as JSON")
    args = parser.parse_args()
    dll = args.dll or convert.find_default_dll()
    if dll is None or not Path(dll).is_file():
        print("lsfg-vk.dll not found", file=sys.stderr)
        return 2
    try:
        derived = derive(convert.extract_spv_from_pe(Path(dll).read_bytes()))
    except (DeriveError, ValueError, struct.error) as error:
        print(f"derivation failed: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(derived, indent=1))
    else:
        for rid, v in derived.items():
            variant = ("fp16" if v["fp16"] else "fp32") + (" perf" if v["performance"] else " qual")
            print(f"{rid:3} {v['role']:15} {v['gpl']:10} {variant}  {v['evidence']:11} samplers={','.join(v['samplers'])}")
    problems = compare(derived, expected_from_source())
    for line in problems:
        print(f"MISMATCH {line}")
    print("derived table equals LsfgShaderLibrary" if not problems else f"{len(problems)} mismatches")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
