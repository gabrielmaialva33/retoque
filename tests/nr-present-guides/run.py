#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

with tempfile.TemporaryDirectory(prefix="optiscaler-present-guides-") as tmp:
    tmp_path = pathlib.Path(tmp)
    bin_path = tmp_path / "test_bin"

    cmd = [
        "g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined",
        f"-I{ROOT / 'retoque'}",
        f"-I{ROOT / 'OptiScaler'}",
        str(HERE / "test.cpp"),
        "-o", str(bin_path)
    ]
    subprocess.run(cmd, check=True)
    res = subprocess.run([str(bin_path)], capture_output=True, text=True)
    assert res.returncode == 0, res.stdout + res.stderr
    print(res.stdout.strip())

    ring_hdr = (ROOT / "retoque/dlssnr/DlssNr_PresentGuideRing.h").read_text()
    mutants = [
        (
            "ignore-write-in-flight",
            "if (isWriteInFlight(_slots[i].writeToken))",
            "if ((void) isWriteInFlight, false)",
        ),
        (
            "ignore-shape-match",
            "if (isShapeMatch(i))",
            "if ((void) isShapeMatch, false)",
        ),
        (
            "ignore-empty-preference",
            "if (!_slots[i].recorded)",
            "if (false)",
        ),
    ]
    for name, old, new in mutants:
        assert old in ring_hdr, f"missing mutant target {name}"
        mut_dir = tmp_path / f"mut_{name}"
        (mut_dir / "dlssnr").mkdir(parents=True, exist_ok=True)
        (mut_dir / "dlssnr/DlssNr_PresentGuideRing.h").write_text(ring_hdr.replace(old, new, 1))
        mut_bin = mut_dir / "test_bin"
        mut_cmd = [
            "g++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
            f"-I{mut_dir}",
            f"-I{ROOT / 'retoque'}",
            f"-I{ROOT / 'OptiScaler'}",
            str(HERE / "test.cpp"),
            "-o", str(mut_bin),
        ]
        subprocess.run(mut_cmd, check=True)
        mut_res = subprocess.run([str(mut_bin)], capture_output=True, text=True)
        assert mut_res.returncode != 0, f"mutant {name} unexpectedly passed"

    model_hdr = (ROOT / "retoque/dlssnr/DlssNr_SubmissionModel.h").read_text()
    predicate = "IsSafe(*capture, completed) || IsComplete(*capture, completed)"
    for name, replacement in (
        ("completed-only-rejects-orphan", "IsComplete(*capture, completed)"),
        ("cancelled-ring-starves", "(capture->submitted || capture->orphaned) && (IsSafe(*capture, completed) || IsComplete(*capture, completed))"),
        ("allow-unsubmitted-unknown", "!capture->submitted || IsSafe(*capture, completed) || IsComplete(*capture, completed)"),
        ("allow-outstanding-write", "capture->submitted || IsSafe(*capture, completed) || IsComplete(*capture, completed)"),
    ):
        assert predicate in model_hdr
        mut_dir = tmp_path / name
        (mut_dir / "dlssnr").mkdir(parents=True)
        (mut_dir / "dlssnr/DlssNr_SubmissionModel.h").write_text(model_hdr.replace(predicate, replacement, 1))
        mut_bin = mut_dir / "test_bin"
        subprocess.run(["g++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                        f"-I{mut_dir}", f"-I{ROOT / 'retoque'}", f"-I{ROOT / 'OptiScaler'}",
                        str(HERE / "test.cpp"), "-o", str(mut_bin)], check=True)
        result = subprocess.run([str(mut_bin)], capture_output=True, text=True)
        assert result.returncode != 0 and "Assertion" in result.stderr, f"mutant {name} survived: {result.stderr}"
        print(f"PASS: {name}: compiled; assertion killed mutant")

# Ensure the tested pure model admission is the production capture predicate, with orphan evidence refreshed
# under the existing submission lock (called with g_nrMutex held, preserving lock order).
runtime = (ROOT / "retoque/dlssnr/DlssNr_Submission.cpp").read_text()
wrapper = runtime.split("bool CaptureReusable(const Usage& usage)", 1)[1].split("\n}", 1)[0]
assert wrapper.index("std::lock_guard lock(stateMutex);") < wrapper.index("RefreshOrphans(usage);")
assert "return Detail::IsCaptureReusable(usage, FenceValue);" in wrapper
capture = (ROOT / "retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp").read_text()
write_busy = capture.split("const auto isWriteInFlight =", 1)[1].split(";\n    };", 1)[0]
assert "return !DlssNr::Submission::CaptureReusable(writeUsage)" in write_busy
