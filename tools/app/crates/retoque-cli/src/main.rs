mod autotune;
mod doctor;

use retoque_core::{
    Error, Foreign, InstallOptions, Refusal, Step, apply, plan_install, plan_settings, plan_uninstall, status,
};
use std::path::{Path, PathBuf};

fn usage() {
    println!(
        "Usage: retoque install [--dry-run] [--source-dir DIR] [--enable-nr] [--keep-reshade]\n       [--replace-foreign] [--allow-second-proxy] [--no-nvngx] <game-dir> [proxy]\n       retoque uninstall [--dry-run] <game-dir>\n       retoque status [--json] <game-dir>\n       retoque doctor [--json] <game-dir>\n       retoque set [--dry-run] <game-dir> <Section> <Key> <Value>\n       retoque gpu [--json]\n       retoque games [--json] [--home DIR]\n       retoque component install|uninstall [--dry-run] <game-dir> <component-dir-or-id>"
    );
}
fn expand(text: &str) -> PathBuf {
    if (text == "~" || text.starts_with("~/"))
        && let Some(home) = std::env::var_os("HOME")
    {
        return PathBuf::from(home).join(text.strip_prefix("~/").unwrap_or(""));
    }
    PathBuf::from(text)
}
fn payload() -> Result<PathBuf, Error> {
    let exe = std::env::current_exe().map_err(|e| Error::Other(e.to_string()))?;
    if let Some(dir) = exe.parent()
        && dir.join("OptiScaler.dll").is_file()
    {
        return Ok(dir.to_owned());
    }
    let cwd = std::env::current_dir().map_err(|e| Error::Other(e.to_string()))?;
    if let Some(repo) = cwd
        .ancestors()
        .chain(exe.ancestors())
        .find(|p| p.join("OptiScaler.ini").is_file() && p.join("tools/deploy-to-game.sh").is_file())
    {
        return Ok(repo.join("x64/out"));
    }
    Err(Error::Other(
        "payload not found; pass --source-dir or put the release beside retoque".into(),
    ))
}
fn describe(step: &Step, uninstall: bool) -> String {
    match step {
        Step::Copy { from, to } => format!(
            "install {} as {to}",
            from.file_name().unwrap_or_default().to_string_lossy()
        ),
        Step::Backup { path, copy } => format!("back up {path} to {copy}"),
        Step::Rename { from, to } => {
            if uninstall {
                format!("rename {from} back to {to}")
            } else if to.to_ascii_lowercase().ends_with("reshade64.dll") {
                format!("rename {from} (ReShade) to {to}")
            } else {
                format!("rename {from} to {to}")
            }
        }
        Step::SetIni {
            file,
            section,
            key,
            value,
            old,
        } => format!(
            "set [{section}] {key}={value} in {file} (was {})",
            old.as_deref().unwrap_or("<absent>")
        ),
        Step::Remove { path } => format!("remove {path}"),
        Step::Restore { copy, to } => format!("restore {to} from {copy}"),
        Step::Keep { path, why } => format!("{path}: {why}"),
    }
}
fn refusal(r: &Refusal) -> String {
    match r {
        Refusal::Usage(s) | Refusal::Receipt(s) => s.clone(),
        Refusal::GameRunning { processes } => format!(
            "game files are in use:\n{}\n    Quit the game, wait for it to exit, then run this again.",
            processes
                .iter()
                .map(|(pid, name)| if *pid == 0 {
                    format!("      {name}")
                } else {
                    format!("      pid {pid} ({name})")
                })
                .collect::<Vec<_>>()
                .join("\n")
        ),
        Refusal::Foreign { path, what } => match what {
            Foreign::ReShade => format!("{path} is ReShade. Pass --keep-reshade or --replace-foreign."),
            Foreign::Unknown => format!(
                "{path} is not ours; not ReShade either. Pass --replace-foreign for a foreign file; links in parents and conflicting case names must be moved first."
            ),
        },
        Refusal::SecondProxy { name } => {
            format!("{name} already holds an OptiScaler build; pass --allow-second-proxy to deploy anyway.")
        }
    }
}
fn show_status(game: &Path, json: bool) -> Result<(), Error> {
    let state = status(game)?;
    if json {
        use retoque_core::receipt::{ReceiptStatus, Record};
        use serde_json::json;
        let receipt = state.receipt.as_ref().map(|r| json!({
            "version": 1,
            "status": match r.status { ReceiptStatus::Complete => "complete", ReceiptStatus::Partial => "partial", ReceiptStatus::UninstallIncomplete => "uninstall-incomplete" },
            "updated": r.updated,
            "records": r.records.iter().map(|record| match record {
                Record::Proxy { name } => json!({"kind":"proxy", "name":name}),
                Record::File { sha256,path } => json!({"kind":"file", "sha256":sha256, "path":path}),
                Record::Backup { sha256,path,copy } => json!({"kind":"backup", "sha256":sha256, "path":path, "copy":copy}),
                Record::Rename { sha256,from,to } => json!({"kind":"rename", "sha256":sha256, "from":from, "to":to}),
            }).collect::<Vec<_>>()
        }));
        println!("{}", serde_json::to_string_pretty(&json!({ "receipt":receipt, "optiscaler_proxies":state.optiscaler_proxies, "build_sha256":state.build_sha256, "nr_enabled":state.nr_enabled, "model_present":state.model_present, "launch_option":state.launch_option, "proxy_version":state.proxy_version, "forwarder_version":state.forwarder_version, "version_mismatch":state.version_mismatch() })).unwrap());
    } else {
        println!(
            "[=] Receipt: {}",
            state
                .receipt
                .as_ref()
                .map(|r| format!("{:?}", r.status))
                .unwrap_or_else(|| "not installed".into())
        );
        println!("[=] OptiScaler proxies: {}", state.optiscaler_proxies.join(", "));
        println!(
            "[=] Build sha256: {}",
            state.build_sha256.as_deref().unwrap_or("unknown")
        );
        println!(
            "[=] Neural Rendering: {}",
            state
                .nr_enabled
                .map(|v| v.to_string())
                .unwrap_or_else(|| "auto or unset".into())
        );
        println!(
            "[=] OptiScaler version: {}",
            state.proxy_version.as_deref().unwrap_or("unknown")
        );
        println!(
            "[=] NR forwarder version: {}",
            state.forwarder_version.as_deref().unwrap_or("unknown")
        );
        if state.version_mismatch() {
            println!("[!] OptiScaler and NR forwarder versions differ; update them together.");
        }
        println!("[=] Model present: {}", state.model_present);
        if let Some(option) = state.launch_option {
            println!("    {option}");
        }
    }
    Ok(())
}
fn run() -> Result<(), Error> {
    let mut args = std::env::args().skip(1).peekable();
    let Some(command) = args.next() else {
        usage();
        return Err(Error::Other("no command given".into()));
    };
    if command == "--help" || command == "-h" {
        usage();
        return Ok(());
    }
    let component_action = if command == "component" {
        let action = args
            .next()
            .ok_or_else(|| Error::Other("component needs install or uninstall".into()))?;
        if action != "install" && action != "uninstall" {
            return Err(Error::Other("component needs install or uninstall".into()));
        }
        Some(action)
    } else {
        None
    };
    if ![
        "install",
        "uninstall",
        "status",
        "doctor",
        "set",
        "component",
        "gpu",
        "games",
    ]
    .contains(&command.as_str())
    {
        return Err(Error::Other(format!("unknown command: {command}")));
    }
    let mut opts = InstallOptions {
        copy_nvngx: cfg!(target_os = "linux"),
        ..Default::default()
    };
    let mut positional = Vec::new();
    let mut dry = false;
    let mut json = false;
    let mut home = None;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            _ if command == "set" && positional.len() == 3 => positional.push(arg),
            "--help" | "-h" => {
                usage();
                return Ok(());
            }
            "--dry-run" | "-n" if ["install", "uninstall", "set", "component"].contains(&command.as_str()) => {
                dry = true
            }
            "--json" if ["status", "doctor", "gpu", "games"].contains(&command.as_str()) => json = true,
            "--home" if command == "games" => {
                home = Some(expand(
                    &args
                        .next()
                        .ok_or_else(|| Error::Other("--home needs a directory".into()))?,
                ))
            }
            "--enable-nr" if command == "install" => opts.enable_nr = true,
            "--keep-reshade" if command == "install" => opts.keep_reshade = true,
            "--replace-foreign" if command == "install" => opts.replace_foreign = true,
            "--allow-second-proxy" if command == "install" => opts.allow_second_proxy = true,
            "--no-nvngx" if command == "install" => opts.copy_nvngx = false,
            "--source-dir" if command == "install" => {
                opts.source_dir = expand(
                    &args
                        .next()
                        .ok_or_else(|| Error::Other("--source-dir needs a directory".into()))?,
                )
            }
            "--" => {
                positional.extend(args);
                break;
            }
            _ if arg.starts_with("--source-dir=") && command == "install" => opts.source_dir = expand(&arg[13..]),
            _ if arg.starts_with('-') => return Err(Error::Other(format!("unknown option: {arg}"))),
            _ => positional.push(arg),
        }
    }
    if match command.as_str() {
        "install" => !(1..=2).contains(&positional.len()),
        "set" => positional.len() != 4,
        "component" => positional.len() != 2,
        "gpu" | "games" => !positional.is_empty(),
        _ => positional.len() != 1,
    } {
        usage();
        return Err(Error::Other("wrong number of arguments".into()));
    }
    if command == "gpu" {
        autotune::show_gpu(json);
        return Ok(());
    }
    if command == "games" {
        autotune::show_games(json, home.as_deref());
        return Ok(());
    }
    opts.game_dir = expand(&positional[0]);
    if command == "doctor" {
        return doctor::run(&opts.game_dir, json);
    }
    if command == "status" {
        return show_status(&opts.game_dir, json);
    }
    let uninstall = command == "uninstall" || component_action.as_deref() == Some("uninstall");
    let plan = if command == "component" {
        autotune::component_plan(&opts.game_dir, &positional[1], uninstall)?
    } else if uninstall {
        plan_uninstall(&opts.game_dir)?
    } else if command == "set" {
        plan_settings(
            &opts.game_dir,
            &[(positional[1].clone(), positional[2].clone(), positional[3].clone())],
        )?
    } else {
        opts.proxy = positional.get(1).cloned().unwrap_or_else(|| "dxgi.dll".into());
        if opts.source_dir.as_os_str().is_empty() {
            opts.source_dir = payload()?;
        }
        plan_install(&opts)?
    };
    if !plan.refusals().is_empty() {
        return Err(Error::Refused(plan.refusals().to_vec()));
    }
    for note in plan.notes() {
        println!("[=] {note}");
    }
    if dry {
        for step in plan.steps() {
            println!("[plan] {}", describe(step, uninstall));
        }
        if plan.steps().is_empty() {
            println!("[plan] nothing to do");
        }
        println!("[=] Dry run: nothing was written.");
    } else {
        if command == "component" {
            for step in plan.steps() {
                println!("[plan] {}", describe(step, uninstall));
            }
        }
        apply(&plan.into_verified()?, &mut |done| {
            println!("[+] {}", describe(&done.step, uninstall))
        })?;
        println!(
            "[ok] {}",
            if uninstall {
                "Uninstalled what could be; the receipt lists anything left."
            } else if command == "set" {
                "Settings saved; install receipt left unchanged."
            } else {
                "Deployment completed successfully!"
            }
        );
        if command == "install"
            && let Some(option) = status(&opts.game_dir)?.launch_option
        {
            println!("    {option}");
        }
    }
    Ok(())
}
fn main() {
    if let Err(e) = run() {
        let code = if let Error::Refused(ref refusals) = e {
            for r in refusals {
                eprintln!("[-] Refusing: {}", refusal(r));
            }
            eprintln!("[-] Nothing was written.");
            refusals.first().map(Refusal::exit_code).unwrap_or(1)
        } else {
            eprintln!("[-] Error: {e}");
            1
        };
        std::process::exit(code);
    }
}
