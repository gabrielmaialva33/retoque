use retoque_core::{Error, reshade as core};
use retoque_data::{
    messages::{self, Language},
    reshade as provider,
};
use std::{
    io::{self, Write},
    path::PathBuf,
};
fn text(key: &str) -> String {
    let lang = std::env::var("RETOQUE_LANG")
        .or_else(|_| std::env::var("LANG"))
        .unwrap_or_default();
    messages::text(
        key,
        if lang.starts_with("pt") {
            Language::Portuguese
        } else {
            Language::English
        },
    )
    .into()
}
fn err(e: impl std::fmt::Display) -> Error {
    Error::Other(e.to_string())
}
pub fn run(command: &str, args: impl Iterator<Item = String>) -> Result<(), Error> {
    let mut positional = Vec::new();
    let mut dry = false;
    let mut consent = false;
    for arg in args {
        match arg.as_str() {
            "--dry-run" | "-n" => dry = true,
            "--yes-download" => consent = true,
            s if s.starts_with('-') => return Err(err("unknown ReShade/add-on option")),
            _ => positional.push(arg),
        }
    }
    if positional.len() < 2 {
        return Err(err(
            "reshade install|uninstall [--dry-run] <game-dir> [DLL|setup.exe]; addon list|add|remove|enable|disable [--dry-run] <game-dir> [file]",
        ));
    }
    let action = &positional[0];
    let game = crate::expand(&positional[1]);
    let source = positional.get(2).map(|s| crate::expand(s));
    if positional.len() > 3 {
        return Err(err("too many ReShade/add-on arguments"));
    }
    if command == "addon" && action == "list" {
        for item in core::addons(&game)? {
            println!(
                "{}\t{}\t{}\t{}\t{}",
                item.file,
                item.size,
                item.version.as_deref().unwrap_or("?"),
                item.product.as_deref().unwrap_or(""),
                text(if item.enabled { "rs.enabled" } else { "rs.disabled" })
            );
        }
        return Ok(());
    }
    let mut staged = None;
    let plan = match (command, action.as_str()) {
        ("reshade", "install") => {
            let dll: PathBuf = if let Some(source) = source {
                if source.extension().is_some_and(|e| e.eq_ignore_ascii_case("exe")) {
                    // Ephemeral preparation only: dry-run never publishes a cache or changes the game.
                    let (mut input, _) = retoque_discovery::io::open_regular_file(&source).map_err(err)?;
                    use std::io::Read;
                    let mut bytes = Vec::new();
                    std::io::Read::by_ref(&mut input)
                        .take(provider::MAX_INPUT as u64 + 1)
                        .read_to_end(&mut bytes)
                        .map_err(err)?;
                    let bytes = provider::extract(&bytes).map_err(err)?;
                    let temp = tempfile::tempdir().map_err(err)?;
                    let file = temp.path().join(core::DLL);
                    std::fs::write(&file, bytes).map_err(err)?;
                    staged = Some(temp);
                    file
                } else {
                    source
                }
            } else {
                let data = retoque_data::lsfg_prepare::data_dir().map_err(err)?;
                let cached = provider::cached(&data, provider::DLL_SHA).map_err(err)?;
                if let Some(c) = cached {
                    c.dll()
                } else {
                    if dry {
                        return Err(err(text("rs.download_first")));
                    }
                    if !consent {
                        println!("{}\n{}", text("rs.consent"), text("rs.cli_confirm"));
                        io::stdout().flush().map_err(err)?;
                        let mut answer = String::new();
                        io::stdin().read_line(&mut answer).map_err(err)?;
                        consent = matches!(answer.trim(), "yes" | "sim");
                    }
                    provider::download(&data, consent, &provider::OfficialFetcher, &mut |_| {})
                        .map_err(err)?
                        .dll()
                }
            };
            core::plan_install(&game, &game, &dll)?
        }
        ("reshade", "uninstall") if source.is_none() => core::plan_uninstall(&game)?,
        ("addon", "add") => core::plan_add(&game, &source.ok_or_else(|| err("addon add needs a file"))?)?,
        ("addon", "remove" | "enable" | "disable") => {
            let name = positional.get(2).ok_or_else(|| err("add-on filename required"))?;
            match action.as_str() {
                "remove" => core::plan_remove(&game, name)?,
                _ => core::plan_enabled(&game, name, action == "enable")?,
            }
        }
        _ => return Err(err("unknown ReShade/add-on action")),
    };
    for note in plan.notes() {
        println!("[=] {}", localized(Error::Other(note.clone())));
    }
    for step in plan.steps() {
        println!(
            "[plan] {}",
            crate::describe(step, matches!(action.as_str(), "uninstall" | "remove"))
        );
    }
    if !plan.refusals().is_empty() {
        return Err(Error::Refused(plan.refusals().to_vec()));
    }
    if dry {
        println!("{}", text("rs.dry"));
    } else {
        retoque_core::apply(&plan.into_verified()?, &mut |_| {})?;
        println!("{}", text("rs.done"));
    }
    drop(staged);
    Ok(())
}

pub fn localized(error: Error) -> Error {
    let pt = std::env::var("RETOQUE_LANG")
        .or_else(|_| std::env::var("LANG"))
        .unwrap_or_default()
        .starts_with("pt");
    let translate =
        |s: &str| messages::reshade_diagnostic(s, if pt { Language::Portuguese } else { Language::English });
    match error {
        Error::Other(s) => Error::Other(translate(&s)),
        Error::StalePlan(s) => Error::StalePlan(translate(&s)),
        Error::Refused(items) => Error::Refused(
            items
                .into_iter()
                .map(|r| match r {
                    retoque_core::Refusal::Usage(s) => retoque_core::Refusal::Usage(translate(&s)),
                    other => other,
                })
                .collect(),
        ),
        other => other,
    }
}
