#!/usr/bin/env python3
"""Independent, targeted mutation checks. Local edits restored even on a failed test."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
CORE = ROOT / 'crates/retoque-core/src/legacy_probe/mod.rs'
CLI = ROOT / 'crates/retoque-cli/src/legacy_probe.rs'
RECOMMEND = ROOT / 'crates/retoque-data/src/recommend.rs'
PE = ROOT / 'crates/retoque-core/src/pe_version.rs'
PLATFORM = ROOT / 'crates/retoque-core/src/legacy_probe/platform.rs'
# Each mutant must compile and fail the named behavioral test, not merely an unrelated check.
MUTANTS = [
    ('capture-unknown', CORE, [('&& r.capture_api_available == Some(true)', '&& true')], 'capture_availability_is_not_inferred_from_the_requested_os'),
    ('missing-selection', PLATFORM, [('!selected_requested && p.image_path.is_some()', 'p.image_path.is_some()')], 'unreadable_explicit_selection_is_not_an_implicit_process_match'),
    ('blocking-deadline', CLI, [('.recv_timeout(deadline.saturating_duration_since(Instant::now()))', '.recv()')], 'blocking_os_work_returns_unknown_at_the_cli_deadline'),
    ('unknown-machine', PE, [('(_, 0x10b | 0x20b) => Machine::Unknown,', '(_, 0x10b | 0x20b) => Machine::X86,')], 'unknown_pe_machine_is_never_x86'),
    ('unknown-window-mode', CORE, [('window_mode_observation: Fact::Unknown,', 'window_mode_observation: Fact::Observed,')], 'access_denied_and_unknown_mode_remain_unknown'),
    ('module-is-not-support', CORE, [('if condition { "candidate" } else { "unknown" }', 'if condition { "supported" } else { "unknown" }')], 'mixed_apis_do_not_choose_a_renderer'),
    ('off', CORE, [('let request = request?;', 'if request.is_none() { source.snapshot(&Request::default(), Instant::now() + TIME_LIMIT); }\n    let request = request?;')], 'default_off_does_no_work'),
    ('stale', CORE, [('} else if !raw.process.stable {', '} else if false {')], 'stale_identity_discards_modules_and_architecture'),
    ('association', CORE, [('if request.pid.is_some() && !raw.process.matches_executable {', 'if false {'), ('&& raw.process.matches_executable', '&& true')], 'mismatch_is_a_named_refusal'),
    ('module-coverage', CORE, [('&& raw.process.modules_complete', '&& true')], 'incomplete_inventory_cannot_authorize_a_decision'),
    ('import-coverage', CORE, [('&& raw.executable.imports_complete', '&& true')], 'incomplete_inventory_cannot_authorize_a_decision'),
    ('hash-identity', CORE, [('&& raw.executable.sha256.is_some()', '&& true')], 'incomplete_inventory_cannot_authorize_a_decision'),
    ('launcher-refusal', CORE, [('&& !request.launcher_only', '&& true')], 'launcher_only_is_a_named_refusal'),
    ('process-bitness', CORE, [('process_architecture: raw.process.architecture,', 'process_architecture: Architecture::X64,')], 'unknown_process_never_inherits_executable_architecture'),
    ('deadline', CORE, [('if raw.timed_out || elapsed >= TIME_LIMIT {', 'if false {')], 'time_and_inventory_budgets_are_fail_closed'),
    ('inventory-cap', CORE, [('if raw.process.modules.len() > INVENTORY_LIMIT {', 'if false {')], 'time_and_inventory_budgets_are_fail_closed'),
    ('privacy', CORE, [('    #[serde(skip)]\n    imports:', '    imports:')], 'privacy_only_allows_known_basenames_and_digests'),
    ('elf-truncation', CORE, [('if b.len() < size {', 'if false {')], 'elf_headers_are_not_guessed_from_magic_or_class'),
    ('geometry-coverage', PLATFORM, [('complete && selected.len() == 1', 'selected.len() == 1')], 'fake_window_inventory_refuses_partial_ambiguous_and_invalid_geometry'),
    ('output-cap', CLI, [('if out.len() + 1 > legacy_probe::OUTPUT_LIMIT {', 'if false {')], 'output_limit_rejects_an_oversized_report'),
]

MUTANTS += [
    ('short-translation-conflict', CLI, [('if source == "probe.short.loaded" {', 'if false {')], 'short_loaded_translation_does_not_replace_the_imported_api'),
    ('short-vulkan', RECOMMEND, [('("probe.short.vulkan", Some(settings::NR_VULKAN_PRESENT))', '("probe.short.unknown_route", None)')], 'short_x64_vulkan'),
    ('short-d3d12', RECOMMEND, [('("probe.short.d3d12", Some(settings::NR_AT_PRESENT))', '("probe.short.unknown_route", None)')], 'short_x64_d3d12'),
    ('short-d3d11', RECOMMEND, [('("probe.short.d3d11", Some(settings::NR_AT_PRESENT))', '("probe.short.unknown_route", None)')], 'short_x64_d3d11'),
    ('short-d3d10', RECOMMEND, [('("probe.short.d3d10", None)', '("probe.short.unknown_route", None)')], 'short_d3d10_only'),
    ('short-opengl', RECOMMEND, [('("probe.short.opengl", None)', '("probe.short.unknown_route", None)')], 'short_opengl_only'),
    ('short-x86', RECOMMEND, [('if architecture == Architecture::X86 {', 'if false {')], 'short_x86_any_api'),
    ('short-elf', RECOMMEND, [('if format == Format::Elf {', 'if false {')], 'short_elf'),
    ('short-unknown', RECOMMEND, [('if format != Format::Pe {', 'if false {')], 'short_unknown'),
    ('log-newest', CORE, [('b.0.cmp(&a.0).then_with(|| a.1.cmp(&b.1))', 'a.0.cmp(&b.0).then_with(|| a.1.cmp(&b.1))')], 'automatic_logs_are_newest_first_and_explicit_log_wins'),
    ('log-names', CORE, [('log_name(n, "OptiScaler")', '{ let _ = n; true }')], 'automatic_logs_never_follow_other_folders_names_or_links'),
    ('log-shared-bytes', CORE, [('let evidence = read_log_budget(&path, deadline, &mut remaining);', 'let evidence = read_log_budget(&path, deadline, &mut (READ_LIMIT + 1));')], 'automatic_log_read_and_file_budgets_are_shared'),
    ('log-file-cap', CORE, [('if count >= 16 || remaining == 0', 'if count >= 32 || remaining == 0')], 'automatic_log_read_and_file_budgets_are_shared'),
    ('log-entry-cap', CORE, [('if count >= INVENTORY_LIMIT || Instant::now() >= deadline', 'if count >= INVENTORY_LIMIT * 2 || Instant::now() >= deadline')], 'automatic_log_directory_budget_is_fail_closed'),
    ('short-history', CLI, [('source = "probe.short.past";', 'source = "probe.short.loaded";')], 'short_evidence_is_ranked_and_past_never_becomes_current'),
    ('short-privacy', CLI, [('let mut evidence = format!("API: {api_name} ({})", t(source, lang));', 'let mut evidence = format!("API: {api_name} ({}) {}", t(source, lang), r.executable.sha256.as_deref().unwrap_or(""));')], 'short_privacy_bounds_and_detail_switch'),
    ('short-default', CLI, [('} else if !details {', '} else if false {')], 'short_privacy_bounds_and_detail_switch'),
]

def cargo(crate, test):
    result = subprocess.run(['cargo', 'test', '-j', '4', '-p', crate, test, '--', '--nocapture'], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return result

for name, path, replacements, test in MUTANTS:
    original = path.read_text()
    changed = original
    for old, new in replacements:
        if changed.count(old) != 1:
            raise SystemExit(f'{name}: source anchor drift')
        changed = changed.replace(old, new, 1)
    crate = 'retoque-cli' if path in (CLI, RECOMMEND) else 'retoque-core'
    try:
        path.write_text(changed)
        result = cargo(crate, test)
        if result.returncode != 101 or f'{test} ... FAILED' not in result.stdout or 'test result: FAILED' not in result.stdout:
            print(result.stdout[-6000:])
            raise SystemExit(f'{name}: survived or did not fail its own behavioral test')
        print(f'KILLED {name}: {test}', flush=True)
    finally:
        path.write_text(original)
result = cargo('retoque-core', 'legacy_probe')
if result.returncode:
    print(result.stdout[-6000:])
    raise SystemExit('restored baseline failed')
result = cargo('retoque-cli', 'legacy_probe')
if result.returncode:
    print(result.stdout[-6000:])
    raise SystemExit('restored CLI baseline failed')
print(f'PASS: {len(MUTANTS)} mutants killed by their named tests; restored baseline passes')
