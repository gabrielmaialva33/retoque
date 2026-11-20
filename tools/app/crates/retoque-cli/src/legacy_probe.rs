//! CLI-only opt-in. An in-process worker bounds blocking OS metadata calls; no helper is launched.
use retoque_core::{
    Error,
    legacy_probe::{self, DesiredOutput, Fact, Mode, Report, Request, Runtime},
};
use retoque_data::messages::{self, Language};
use std::{
    path::{Path, PathBuf},
    sync::mpsc,
    time::Instant,
};

fn language(code: &str) -> Language {
    if code.starts_with("pt") {
        Language::Portuguese
    } else {
        Language::English
    }
}
fn t(key: &str, lang: Language) -> &str {
    messages::text(key, lang)
}
fn parse(args: impl Iterator<Item = String>) -> Result<(Request, bool, bool, Language), Error> {
    let mut lang = language(&std::env::var("RETOQUE_LANG").unwrap_or_default());
    let mut request = Request {
        classify_launcher: Some(is_launcher),
        ..Default::default()
    };
    let mut json = false;
    let mut details = false;
    let mut args = args.take(33).peekable();
    let mut count = 0;
    let mut seen = std::collections::BTreeSet::new();
    while let Some(arg) = args.next() {
        count += 1;
        if count > 16 || !seen.insert(arg.clone()) {
            return Err(Error::Other(t("probe.usage", lang).into()));
        }
        if arg == "--details" {
            details = true;
            continue;
        }
        if arg == "--json" {
            json = true;
            continue;
        }
        let value = args
            .next()
            .filter(|v| !v.is_empty() && v.len() <= 4096)
            .ok_or_else(|| Error::Other(t("probe.usage", lang).into()))?;
        match arg.as_str() {
            "--exe" => request.executable = Some(PathBuf::from(value)),
            "--pid" => {
                request.pid = Some(
                    value
                        .parse::<u32>()
                        .ok()
                        .filter(|p| *p > 0)
                        .ok_or_else(|| Error::Other(t("probe.usage", lang).into()))?,
                )
            }
            "--launcher-exe" => request.launcher = Some(PathBuf::from(value)),
            "--log" => request.log = Some(PathBuf::from(value)),
            "--lang" if value == "en" || value == "pt-BR" => lang = language(&value),
            "--runtime" => {
                request.runtime = match value.as_str() {
                    "windows" => Runtime::Windows,
                    "proton" => Runtime::Proton,
                    "linux" => Runtime::Linux,
                    _ => return Err(Error::Other(t("probe.usage", lang).into())),
                }
            }
            "--mode" => {
                request.mode = match value.as_str() {
                    "windowed" => Mode::Windowed,
                    "borderless" => Mode::Borderless,
                    "exclusive" => Mode::Exclusive,
                    _ => return Err(Error::Other(t("probe.usage", lang).into())),
                }
            }
            "--output" => {
                request.output = match value.as_str() {
                    "overlay" => DesiredOutput::Overlay,
                    "in-game" => DesiredOutput::InGame,
                    _ => return Err(Error::Other(t("probe.usage", lang).into())),
                }
            }
            _ => return Err(Error::Other(t("probe.usage", lang).into())),
        }
    }
    if request.executable.is_none() && request.pid.is_none() {
        return Err(Error::Other(t("probe.usage", lang).into()));
    }
    if let Some(exe) = &request.executable {
        request.launcher_only = is_launcher(exe);
    }
    Ok((request, json, details, lang))
}
fn is_launcher(exe: &Path) -> bool {
    let name = exe.file_name().unwrap_or_default().to_string_lossy();
    retoque_discovery::is_ignored_exe(&name, Path::new("")) || name.to_ascii_lowercase().contains("launcher")
}
pub fn run_os(args: impl Iterator<Item = std::ffi::OsString>) -> Result<(), Error> {
    let args = args
        .take(34)
        .map(|arg| arg.into_string())
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| {
            Error::Other(
                t(
                    "probe.usage",
                    language(&std::env::var("RETOQUE_LANG").unwrap_or_default()),
                )
                .into(),
            )
        })?;
    run(args.into_iter())
}
fn run(args: impl Iterator<Item = String>) -> Result<(), Error> {
    let (request, json, details, lang) = parse(args)?;
    let selected = request.clone();
    let report = bounded_snapshot(&request, lang, move || {
        legacy_probe::inspect(Some(&selected)).expect("explicit invocation")
    })?;
    let out = render(&report, json, details, lang)?;
    println!("{out}");
    Ok(())
}
fn bounded_snapshot(
    request: &Request,
    lang: Language,
    work: impl FnOnce() -> Report + Send + 'static,
) -> Result<Report, Error> {
    let deadline = Instant::now() + legacy_probe::TIME_LIMIT;
    let (send, receive) = mpsc::sync_channel(1);
    // Detached only if a kernel call stalls; CLI exit terminates this thread. No retries or sampling loop.
    std::thread::Builder::new()
        .name("legacy-metadata".into())
        .stack_size(512 * 1024)
        .spawn(move || {
            let _ = send.send(work());
        })
        .map_err(|_| Error::Other(t("probe.worker", lang).into()))?;
    Ok(receive
        .recv_timeout(deadline.saturating_duration_since(Instant::now()))
        .unwrap_or_else(|_| legacy_probe::timeout_report(request)))
}
fn fact(f: Fact, lang: Language) -> &'static str {
    t(
        if f == Fact::Observed {
            "probe.observed"
        } else {
            "probe.unknown"
        },
        lang,
    )
}
fn architecture(a: legacy_probe::Architecture, lang: Language) -> &'static str {
    match a {
        legacy_probe::Architecture::X86 => "x86",
        legacy_probe::Architecture::X64 => "x64",
        legacy_probe::Architecture::Arm64 => "ARM64",
        legacy_probe::Architecture::Unknown => t("probe.unknown", lang),
    }
}
fn format_name(f: legacy_probe::Format, lang: Language) -> &'static str {
    match f {
        legacy_probe::Format::Pe => "PE",
        legacy_probe::Format::Elf => "ELF",
        legacy_probe::Format::Unknown => t("probe.unknown", lang),
    }
}
// Only fixed, localized vocabulary enters the short answer, never inventory strings or log text.
fn short_answer(r: &Report, lang: Language) -> String {
    use legacy_probe::{Architecture, Format};
    let past = [
        ("vulkan", r.log.vulkan_upscaled, "probe.short.log_vk_upscaled"),
        ("vulkan", r.log.vulkan_present, "probe.short.log_vk_present"),
        ("vulkan", r.log.vulkan_creation, "probe.short.log_vk_created"),
        ("d3d11", r.log.d3d11_bridge_creation, "probe.short.log_dx11"),
        ("d3d12", r.log.d3d12_present, "probe.short.log_dx12"),
    ];
    let mut choices = Vec::new();
    let mut source = "probe.unknown";
    // A log never overrides observed file/process APIs. Modules do not prove the active renderer.
    for (field, key) in [(true, "probe.short.loaded"), (false, "probe.short.imported")] {
        choices = ["d3d9", "d3d10", "d3d11", "d3d12", "vulkan", "opengl", "wined3d"]
            .into_iter()
            .filter(|api| {
                r.apis
                    .iter()
                    .any(|a| a.api == *api && (if field { a.module } else { a.import }) == Fact::Observed)
            })
            .collect();
        if !choices.is_empty() {
            source = key;
            break;
        }
    }
    // A loaded translation backend is not proof of a native game API. Retain conflicting imports.
    if source == "probe.short.loaded" {
        for api in ["d3d9", "d3d10", "d3d11", "d3d12", "vulkan", "opengl", "wined3d"] {
            if r.apis.iter().any(|a| a.api == api && a.import == Fact::Observed) && !choices.contains(&api) {
                choices.push(api);
                source = "probe.short.mixed_sources";
            }
        }
    }
    if choices.is_empty() {
        for (api, seen, _) in past {
            if seen == Fact::Observed && !choices.contains(&api) {
                choices.push(api);
            }
        }
        if !choices.is_empty() {
            source = "probe.short.past";
        }
    }
    let api = (choices.len() == 1).then(|| choices[0]);
    let api_name = match api {
        Some("d3d9") => "D3D9",
        Some("d3d10") => "D3D10",
        Some("d3d11") => "D3D11",
        Some("d3d12") => "D3D12",
        Some("vulkan") => "Vulkan",
        Some("opengl") => "OpenGL",
        Some("wined3d") => "wined3d",
        _ if choices.len() > 1 => t("probe.short.mixed", lang),
        _ => t("probe.unknown", lang),
    };
    let mut evidence = format!("API: {api_name} ({})", t(source, lang));
    if !r.inspection_complete {
        evidence.push_str(&format!("; {}", t("probe.short.partial", lang)));
    }
    // One useful historical observation in the short answer; all markers remain in details/JSON.
    let history = past
        .iter()
        .find(|(seen_api, seen, _)| *seen == Fact::Observed && Some(*seen_api) == api)
        .or_else(|| past.iter().find(|(_, seen, _)| *seen == Fact::Observed));
    if let Some((_, _, key)) = history {
        evidence.push_str(&format!("; {}", t(key, lang)));
    }
    let launcher = r.issues.contains(&"probe.launcher_only");
    let (route, key) = retoque_data::recommend::metadata_route(
        if launcher { Format::Unknown } else { r.executable.format },
        r.executable.architecture,
        api,
    );
    let mut today = t(route, lang).to_string();
    if let Some((section, key, value)) = key {
        today.push_str(&format!(" ([{section}] {key}={value})."));
    }
    let next = if launcher || r.executable.format == Format::Unknown {
        "probe.short.next_exe"
    } else if r.executable.format == Format::Elf {
        "probe.short.next_elf"
    } else if matches!(route, "probe.short.x86" | "probe.short.d3d10" | "probe.short.opengl") {
        "probe.short.next_capture"
    } else if matches!(source, "probe.short.loaded" | "probe.short.mixed_sources")
        && r.process_identity == Fact::Observed
    {
        "probe.short.next_log"
    } else {
        "probe.short.next_pid"
    };
    let bits = match r.executable.architecture {
        Architecture::X86 => "probe.short.x86_bits",
        Architecture::X64 => "probe.short.x64_bits",
        Architecture::Arm64 => "probe.short.arm_bits",
        Architecture::Unknown => "probe.unknown",
    };
    [
        format!(
            "{}: {} / {}",
            t("probe.executable", lang),
            format_name(r.executable.format, lang),
            t(bits, lang)
        ),
        evidence,
        today,
        t(next, lang).to_string(),
    ]
    .join("\n")
}
fn render(r: &Report, json: bool, details: bool, lang: Language) -> Result<String, Error> {
    let out = if json {
        let mut value = serde_json::to_value(r).map_err(|_| Error::Other(t("probe.worker", lang).into()))?;
        value["explanation"] = t(r.renderer_decision, lang).into();
        for route in value["routes"].as_array_mut().expect("route schema") {
            route["explanation"] = t(route["reason"].as_str().expect("reason schema"), lang).into();
        }
        value["issue_explanations"] = r.issues.iter().map(|key| t(key, lang)).collect::<Vec<_>>().into();
        serde_json::to_string_pretty(&value).map_err(|_| Error::Other(t("probe.worker", lang).into()))?
    } else if !details {
        format!("{}\n{}", short_answer(r, lang), t("probe.short.hint", lang))
    } else {
        let mut lines = vec![
            short_answer(r, lang),
            t("probe.title", lang).into(),
            t(r.renderer_decision, lang).into(),
            t("probe.candidates", lang).into(),
        ];
        lines.push(format!(
            "{}: {} / {}; SHA-256: {}",
            t("probe.executable", lang),
            format_name(r.executable.format, lang),
            architecture(r.executable.architecture, lang),
            r.executable.sha256.as_deref().unwrap_or(t("probe.unknown", lang))
        ));
        lines.push(format!(
            "{}: {}; {}: {}",
            t("probe.process", lang),
            architecture(r.process_architecture, lang),
            t("probe.identity_label", lang),
            fact(r.process_identity, lang)
        ));
        if let Some(launcher) = &r.launcher {
            lines.push(format!(
                "{}: {}; SHA-256: {}",
                t("probe.launcher", lang),
                architecture(launcher.architecture, lang),
                launcher.sha256.as_deref().unwrap_or(t("probe.unknown", lang))
            ));
        }
        lines.push(format!(
            "{}: {} / {} / {} / {}",
            t("probe.requested", lang),
            r.host_os,
            t(
                match r.requested_runtime {
                    Runtime::Windows => "probe.windows",
                    Runtime::Proton => "probe.proton",
                    Runtime::Linux => "probe.linux",
                    Runtime::Unknown => "probe.unknown",
                },
                lang
            ),
            t(
                match r.requested_mode {
                    Mode::Windowed => "probe.windowed",
                    Mode::Borderless => "probe.borderless",
                    Mode::Exclusive => "probe.exclusive",
                    Mode::Unknown => "probe.unknown",
                },
                lang
            ),
            t(
                match r.desired_output {
                    DesiredOutput::Overlay => "probe.overlay",
                    DesiredOutput::InGame => "probe.in_game",
                    DesiredOutput::Unknown => "probe.unknown",
                },
                lang
            )
        ));
        lines.push(format!(
            "{}: {}",
            t("probe.geometry", lang),
            r.window_client_size
                .map(|[w, h]| format!("{w} x {h}"))
                .unwrap_or(t("probe.unknown", lang).into())
        ));
        lines.push(format!(
            "{}: {}; {}: {}; {}: {}",
            t("probe.complete", lang),
            t(if r.inspection_complete { "probe.yes" } else { "probe.no" }, lang),
            t("probe.imports", lang),
            t(
                if r.executable.imports_complete {
                    "probe.yes"
                } else {
                    "probe.no"
                },
                lang
            ),
            t("probe.modules", lang),
            t(if r.modules_complete { "probe.yes" } else { "probe.no" }, lang)
        ));
        lines.push(format!(
            "{}: {}",
            t("probe.capture", lang),
            t(
                match r.capture_api_available {
                    Some(true) => "probe.yes",
                    Some(false) => "probe.no",
                    None => "probe.unknown",
                },
                lang
            )
        ));
        for a in &r.apis {
            lines.push(format!(
                "{}: {} {}; {} {} ({})",
                a.api,
                t("probe.modules", lang),
                fact(a.module, lang),
                t("probe.imports", lang),
                fact(a.import, lang),
                a.observed_basenames.join(", ")
            ));
        }
        lines.push(format!(
            "{}: {}; SHA-256: {}",
            t("probe.log_complete", lang),
            t(if r.log.complete { "probe.yes" } else { "probe.no" }, lang),
            r.log.file_sha256.as_deref().unwrap_or(t("probe.unknown", lang))
        ));
        for (key, value) in [
            ("probe.log.vk_create", r.log.vulkan_creation),
            ("probe.log.vk_present", r.log.vulkan_present),
            ("probe.log.vk_upscaled", r.log.vulkan_upscaled),
            ("probe.log.bridge", r.log.d3d11_bridge_creation),
            ("probe.log.dx12", r.log.d3d12_present),
        ] {
            lines.push(format!("{}: {}", t(key, lang), fact(value, lang)));
        }
        for route in &r.routes {
            lines.push(format!(
                "{}: {}. {} {}",
                t(&format!("probe.name.{}", route.route), lang),
                t(&format!("probe.{}", route.state), lang),
                t(route.reason, lang),
                route.key.unwrap_or("")
            ));
        }
        for issue in &r.issues {
            lines.push(format!("- {}", t(issue, lang)));
        }
        lines.push(format!("{}: {} ms", t("probe.elapsed", lang), r.elapsed_ms));
        lines.join("\n")
    };
    if out.len() + 1 > legacy_probe::OUTPUT_LIMIT {
        return Err(Error::Other(t("probe.budget", lang).into()));
    }
    Ok(out)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn selection_is_required_and_bad_args_are_private() {
        for args in [
            vec![],
            vec!["--json"],
            vec!["--pid", "0"],
            vec!["--exe", "secret/path", "--wrong", "private-title"],
            vec!["--exe", "a", "--exe", "b"],
        ] {
            let err = parse(args.into_iter().map(str::to_string)).unwrap_err().to_string();
            assert!(!err.contains("secret") && !err.contains("private-title"));
        }
    }
    #[test]
    fn both_languages_and_json_are_bounded_and_private() {
        let r = legacy_probe::timeout_report(&Request::default());
        for lang in [Language::English, Language::Portuguese] {
            for json in [false, true] {
                let text = render(&r, json, false, lang).unwrap();
                assert!(text.len() < legacy_probe::OUTPUT_LIMIT);
                assert!(!text.contains("supported") && !text.contains("/home/"));
                if json {
                    assert!(serde_json::from_str::<serde_json::Value>(&text).is_ok());
                } else {
                    assert!(!text.contains("probe."));
                }
            }
        }
    }
    #[test]
    fn explicit_launcher_classification_reuses_discovery() {
        let (r, _, _, _) = parse(["--exe", "redprelauncher.exe"].into_iter().map(str::to_string)).unwrap();
        assert!(r.launcher_only);
    }
}

#[cfg(test)]
mod bound_tests {
    use super::*;
    #[test]
    fn output_limit_rejects_an_oversized_report() {
        let mut report = legacy_probe::timeout_report(&Request::default());
        report.issues = vec!["probe.title"; 5000];
        for json in [true, false] {
            assert!(render(&report, json, true, Language::English).is_err());
        }
    }
}

#[cfg(test)]
mod deadline_tests {
    use super::*;
    #[test]
    fn blocking_os_work_returns_unknown_at_the_cli_deadline() {
        let report = bounded_snapshot(&Request::default(), Language::English, || {
            std::thread::sleep(std::time::Duration::from_millis(2250));
            let mut late = legacy_probe::timeout_report(&Request::default());
            late.issues.clear(); // distinguishes the late worker from the timeout report
            late
        })
        .unwrap();
        assert!(report.issues.contains(&"probe.budget"));
        assert_eq!(report.process_identity, Fact::Unknown);
    }
}

#[cfg(all(test, unix))]
mod unicode_tests {
    use super::*;
    #[test]
    fn non_unicode_arguments_return_private_errors_without_inspection() {
        use std::os::unix::ffi::OsStringExt;
        let bad = std::ffi::OsString::from_vec(b"private/secret/\xff".to_vec());
        let error = run_os(["--exe".into(), bad].into_iter()).unwrap_err().to_string();
        assert!(!error.contains("private") && !error.contains("secret"));
    }
}

#[cfg(test)]
mod short_tests {
    use super::*;
    use legacy_probe::{ApiEvidence, Architecture, Format};
    fn sample(format: Format, architecture: Architecture, api: &'static str) -> Report {
        let mut r = legacy_probe::timeout_report(&Request::default());
        r.executable.format = format;
        r.executable.architecture = architecture;
        r.apis = vec![ApiEvidence {
            api,
            module: Fact::Unknown,
            import: Fact::Observed,
            observed_basenames: vec![],
        }];
        r
    }
    fn shape(r: &Report, route_en: &str, route_pt: &str, key: Option<&str>) {
        for (lang, route) in [(Language::English, route_en), (Language::Portuguese, route_pt)] {
            let out = render(r, false, false, lang).unwrap();
            let lines: Vec<_> = out.lines().collect();
            assert_eq!(lines.len(), 5, "{out}");
            assert!(lines[0].contains(format_name(r.executable.format, lang)));
            assert!(lines[1].starts_with("API: "));
            assert!(lines[1].contains(t("probe.short.partial", lang)));
            assert!(lines[2].contains(route), "{out}");
            if let Some(key) = key {
                assert!(lines[2].contains(key), "{out}");
            }
            assert!(lines[3].starts_with(if lang == Language::English {
                "Next:"
            } else {
                "Próximo:"
            }));
            assert!(lines[4].contains("--details") && lines[4].contains("--json"));
            for hidden in [
                "SHA-256",
                "probe.",
                "Decisão recusada",
                "Decision refused",
                "supported",
                " ms",
            ] {
                assert!(!out.contains(hidden), "{out}");
            }
            assert!(out.len() < 1600);
        }
    }
    #[test]
    fn short_x64_vulkan() {
        let r = sample(Format::Pe, Architecture::X64, "vulkan");
        shape(
            &r,
            "NR after DLSS/FSR/XeSS; without an upscaler, experimental Vulkan",
            "NR após DLSS/FSR/XeSS; sem ampliador, apresentação Vulkan experimental",
            Some("VulkanPresentHost=true"),
        );
        assert!(short_answer(&r, Language::English).starts_with("Selected executable: PE / x64 (64-bit)"));
        assert!(short_answer(&r, Language::English).contains("API: Vulkan (imported)"));
        assert!(short_answer(&r, Language::Portuguese).contains("--pid"));
    }
    #[test]
    fn short_x64_d3d12() {
        shape(
            &sample(Format::Pe, Architecture::X64, "d3d12"),
            "without an upscaler, D3D12 presentation",
            "sem ampliador, apresentação D3D12",
            Some("HookMethod=2"),
        );
    }
    #[test]
    fn short_x64_d3d11() {
        shape(
            &sample(Format::Pe, Architecture::X64, "d3d11"),
            "D3D11-to-D3D12 presentation bridge",
            "ponte de apresentação D3D11 para D3D12",
            Some("HookMethod=2"),
        );
    }
    #[test]
    fn short_x86_any_api() {
        for api in [
            "vulkan", "d3d12", "d3d11", "d3d10", "opengl", "d3d9", "wined3d", "unknown",
        ] {
            let r = sample(Format::Pe, Architecture::X86, api);
            shape(
                &r,
                "no in-game NR in a 32-bit game",
                "sem NR dentro de jogos de 32 bits",
                None,
            );
            let out = short_answer(&r, Language::English);
            assert!(out.contains("32-bit") && out.contains("external capture on Windows"));
            assert!(!out.contains("HookMethod") && !out.contains("VulkanPresentHost"));
        }
    }
    #[test]
    fn short_d3d10_only() {
        shape(
            &sample(Format::Pe, Architecture::X64, "d3d10"),
            "no in-game NR through D3D10; the alternative is experimental external capture on Windows",
            "sem NR dentro do jogo via D3D10; a alternativa é a captura externa experimental no Windows",
            None,
        );
    }
    #[test]
    fn short_opengl_only() {
        shape(
            &sample(Format::Pe, Architecture::X64, "opengl"),
            "no in-game NR through OpenGL; the alternative is experimental external capture on Windows",
            "sem NR dentro do jogo via OpenGL; a alternativa é a captura externa experimental no Windows",
            None,
        );
    }
    #[test]
    fn short_elf() {
        let r = sample(Format::Elf, Architecture::X64, "vulkan");
        shape(
            &r,
            "Linux executable has no in-game Retoque route",
            "executável Linux não tem rota do Retoque dentro do jogo",
            None,
        );
        assert!(short_answer(&r, Language::English).contains("--exe, together with --pid"));
    }
    #[test]
    fn short_unknown() {
        for arch in [Architecture::X64, Architecture::Unknown] {
            shape(
                &sample(Format::Unknown, arch, "vulkan"),
                "in-game NR route is still unknown",
                "rota de NR dentro do jogo ainda é desconhecida",
                None,
            );
        }
        let r = legacy_probe::timeout_report(&Request::default());
        assert!(short_answer(&r, Language::English).contains("API: unknown (unknown)"));
    }
    #[test]
    fn short_evidence_is_ranked_and_past_never_becomes_current() {
        let mut r = sample(Format::Pe, Architecture::X64, "vulkan");
        r.log.vulkan_upscaled = Fact::Observed;
        let out = short_answer(&r, Language::English);
        assert!(out.contains("API: Vulkan (imported)"));
        assert!(out.contains("Vulkan upscaler route recorded in a past log"));
        r.apis[0].import = Fact::Unknown;
        assert!(short_answer(&r, Language::English).contains("API: Vulkan (seen in a past log)"));
        r.apis[0].module = Fact::Observed;
        r.process_identity = Fact::Observed;
        let out = short_answer(&r, Language::English);
        assert!(out.contains("API: Vulkan (loaded now)") && out.contains("Next: inspect a new NR session log"));
        r.apis.push(ApiEvidence {
            api: "d3d12",
            module: Fact::Observed,
            import: Fact::Unknown,
            observed_basenames: vec![],
        });
        let out = short_answer(&r, Language::English);
        assert!(out.contains("multiple APIs (loaded now)") && !out.contains("VulkanPresentHost"));
        r.issues.push("probe.launcher_only");
        assert!(short_answer(&r, Language::English).contains("select the actual game executable"));
    }
    #[test]
    fn short_loaded_translation_does_not_replace_the_imported_api() {
        let mut r = sample(Format::Pe, Architecture::X64, "d3d11");
        r.apis.push(ApiEvidence {
            api: "vulkan",
            module: Fact::Observed,
            import: Fact::Unknown,
            observed_basenames: vec![],
        });
        r.process_identity = Fact::Observed;
        for lang in [Language::English, Language::Portuguese] {
            let out = short_answer(&r, lang);
            assert!(out.contains(t("probe.short.mixed", lang)));
            assert!(out.contains(t("probe.short.mixed_sources", lang)));
            assert!(out.contains(t("probe.short.unknown_route", lang)));
            assert!(out.contains("--log") && !out.contains("VulkanPresentHost"));
        }
    }
    #[test]
    fn short_privacy_bounds_and_detail_switch() {
        let (_, json, details, _) = parse(
            ["--exe", "game.exe", "--details", "--json"]
                .into_iter()
                .map(str::to_string),
        )
        .unwrap();
        assert!(json && details);
        let mut r = sample(Format::Pe, Architecture::X64, "vulkan");
        r.executable.sha256 = Some("private/title/0x123".into());
        r.log.file_sha256 = Some("private/title/0x123".into());
        r.apis[0].observed_basenames.push("private/title/0x123");
        r.issues = vec!["private/title/0x123"; 5000];
        for lang in [Language::English, Language::Portuguese] {
            let out = render(&r, false, false, lang).unwrap();
            assert_eq!(out.lines().count(), 5);
            assert!(!out.contains("private") && !out.contains("0x123"));
            assert!(out.len() < 1600);
        }
        let r = sample(Format::Pe, Architecture::X64, "vulkan");
        let details = render(&r, false, true, Language::English).unwrap();
        assert!(details.starts_with(&short_answer(&r, Language::English)) && details.contains("SHA-256"));
        let json: serde_json::Value =
            serde_json::from_str(&render(&r, true, true, Language::English).unwrap()).unwrap();
        assert!(json.get("renderer_decision").is_some() && json.get("routes").is_some());
        assert!(json.get("inspection_complete").is_some() && json.get("issues").is_some());
    }
}
