use retoque_core::{ComponentFiles, Error, Plan};
use retoque_data::{components, gpu, recommend};
use serde_json::{Value, json};
use std::path::Path;

fn data_error(e: std::io::Error) -> Error {
    Error::Other(e.to_string())
}
fn gpu_json(gpu: &gpu::Gpu) -> Value {
    json!({"name":gpu.name,"vendor":format!("{:?}",gpu.vendor),"arch":format!("{:?}",gpu.arch),
        "compute_cap":gpu.compute_cap,"driver":gpu.driver,"vram_mib":gpu.vram_mib})
}
fn action_json(action: &recommend::Action) -> Value {
    match action {
        recommend::Action::SetIni { section, key, value } => {
            json!({"kind":"set_ini","section":section,"key":key,"value":value})
        }
        recommend::Action::InstallComponent(kind) => {
            json!({"kind":"install_component","component":format!("{kind:?}")})
        }
        recommend::Action::RemoveComponent(kind) => json!({"kind":"remove_component","component":format!("{kind:?}")}),
    }
}
fn need_json(need: &recommend::Need) -> Value {
    match need {
        recommend::Need::Component(kind) => json!({"kind":"component","component":format!("{kind:?}")}),
        recommend::Need::LosslessScaling => json!({"kind":"lossless_scaling"}),
        recommend::Need::RetoqueInstalled => json!({"kind":"retoque_installed"}),
    }
}
pub(super) fn show_gpu(json: bool) {
    let gpus = gpu::detect();
    if json {
        println!(
            "{}",
            serde_json::to_string_pretty(&gpus.iter().map(gpu_json).collect::<Vec<_>>()).unwrap()
        );
    } else if gpus.is_empty() {
        println!("[=] No GPU information available.");
    } else {
        for g in gpus {
            println!(
                "[=] {} · {:?} · {} MiB · driver {}",
                g.name,
                g.arch,
                g.vram_mib.map_or_else(|| "unknown".into(), |v| v.to_string()),
                g.driver.as_deref().unwrap_or("unknown")
            );
        }
    }
}
pub(super) fn show_games(json: bool, home: Option<&Path>) {
    let gpus = gpu::detect();
    let (games, skipped) = retoque_discovery::discover(&retoque_discovery::Config {
        home: home.map(Path::to_owned),
        ..Default::default()
    });
    let result: Vec<_> = games.into_iter().map(|game| {
        let targets: Vec<_> = game.targets.iter().map(|target| {
            let status = retoque_core::status(&target.path);
            let components = retoque_core::installed_components(&target.path);
            let ini = retoque_core::read_ini(&target.path);
            let parsed_ini = ini.as_ref().ok().and_then(|value| value.as_ref()).map(|(_,bytes)| retoque_core::ini::Ini::parse(bytes));
            let facts = recommend::facts_for_with_ini(
                &game, target, status.as_ref().ok(),
                components.as_ref().map(Vec::as_slice).unwrap_or(&[]), parsed_ini.as_ref(),
            );
            // A corrupt/unreadable installation is not a clean slate for automatic recommendations.
            let errors: Vec<String> = [status.as_ref().err(), components.as_ref().err(), ini.as_ref().err()].into_iter().flatten().map(ToString::to_string).collect();
            let recommendations: Vec<_> = if errors.is_empty() { recommend::recommend(gpus.first(), &facts).into_iter().map(|r| json!({
                "id":r.id,"title_key":r.title_key,"why_key":r.why_key,"actions":r.actions.iter().map(action_json).collect::<Vec<_>>(),"needs":r.needs.as_ref().map(need_json)
            })).collect() } else { Vec::new() };
            json!({"path":target.path,"executables":target.executables,"dlss":target.dlss,"installed":facts.installed,
                "proxy":facts.proxy,"components":components.unwrap_or_default(),"lsfg_enabled":facts.lsfg_enabled,
                "has_dlss":facts.has_dlss,"has_dlssg":facts.has_dlssg,"sm86_installed":facts.sm86_installed,
                "sm86_legacy":facts.sm86_legacy,"untracked":facts.untracked,"loaders":target.loaders,
                "receipt_status":status.as_ref().ok().and_then(|s|s.receipt.as_ref()).map(|r|format!("{:?}",r.status)),
                "build_sha256":status.as_ref().ok().and_then(|s|s.build_sha256.as_ref()),"recommendations":recommendations,"errors":errors})
        }).collect();
        json!({"id":game.id,"name":game.name,"store":format!("{:?}",game.store),"runtime":format!("{:?}",game.runtime),"install_dir":game.install_dir,"registered":game.registered,"dlss_anywhere":game.dlss_anywhere,"targets":targets})
    }).collect();
    if json {
        println!(
            "{}",
            serde_json::to_string_pretty(
                &json!({"gpus":gpus.iter().map(gpu_json).collect::<Vec<_>>(),"games":result,"skipped":skipped})
            )
            .unwrap()
        );
    } else {
        for game in result {
            println!(
                "[=] {} ({})",
                game["name"].as_str().unwrap(),
                game["store"].as_str().unwrap()
            );
            for target in game["targets"].as_array().unwrap() {
                println!(
                    "    {} · installed={} · proxy={}",
                    target["path"].as_str().unwrap_or(""),
                    target["installed"],
                    target["proxy"]
                );
                for recommendation in target["recommendations"].as_array().unwrap() {
                    println!("      recommendation: {}", recommendation["id"].as_str().unwrap());
                }
                for error in target["errors"].as_array().unwrap() {
                    eprintln!("      error: {}", error.as_str().unwrap());
                }
            }
        }
        for warning in skipped {
            eprintln!("[!] {warning}");
        }
    }
}
pub(super) fn component_plan(game: &Path, source_or_id: &str, uninstall: bool) -> Result<Plan, Error> {
    let source = super::expand(source_or_id);
    if uninstall && !source.is_dir() {
        return retoque_core::plan_component_uninstall(game, source_or_id);
    }
    let component = components::inspect_dir(&source, "CLI component").map_err(data_error)?;
    if uninstall {
        retoque_core::plan_component_uninstall(game, &component.id)
    } else {
        retoque_core::plan_component_install(
            game,
            &ComponentFiles {
                id: component.id,
                source_root: component.root,
                files: component.files,
            },
        )
    }
}
