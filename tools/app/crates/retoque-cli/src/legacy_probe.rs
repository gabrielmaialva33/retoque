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
fn parse(args: impl Iterator<Item = String>) -> Result<(Request, bool, Language), Error> {
    let mut lang = language(&std::env::var("RETOQUE_LANG").unwrap_or_default());
    let mut request = Request {
        classify_launcher: Some(is_launcher),
        ..Default::default()
    };
    let mut json = false;
    let mut args = args.take(33).peekable();
    let mut count = 0;
    let mut seen = std::collections::BTreeSet::new();
    while let Some(arg) = args.next() {
        count += 1;
        if count > 16 || !seen.insert(arg.clone()) {
            return Err(Error::Other(t("probe.usage", lang).into()));
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
    Ok((request, json, lang))
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
    let (request, json, lang) = parse(args)?;
    let selected = request.clone();
    let report = bounded_snapshot(&request, lang, move || {
        legacy_probe::inspect(Some(&selected)).expect("explicit invocation")
    })?;
    let out = render(&report, json, lang)?;
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
fn render(r: &Report, json: bool, lang: Language) -> Result<String, Error> {
    let out = if json {
        let mut value = serde_json::to_value(r).map_err(|_| Error::Other(t("probe.worker", lang).into()))?;
        value["explanation"] = t(r.renderer_decision, lang).into();
        for route in value["routes"].as_array_mut().expect("route schema") {
            route["explanation"] = t(route["reason"].as_str().expect("reason schema"), lang).into();
        }
        value["issue_explanations"] = r.issues.iter().map(|key| t(key, lang)).collect::<Vec<_>>().into();
        serde_json::to_string_pretty(&value).map_err(|_| Error::Other(t("probe.worker", lang).into()))?
    } else {
        let mut lines = vec![
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
                let text = render(&r, json, lang).unwrap();
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
        let (r, _, _) = parse(["--exe", "redprelauncher.exe"].into_iter().map(str::to_string)).unwrap();
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
            assert!(render(&report, json, Language::English).is_err());
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
