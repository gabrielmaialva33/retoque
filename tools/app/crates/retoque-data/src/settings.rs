//! The `OptiScaler.ini` keys the app edits. Every key here must exist in the shipped OptiScaler.ini (a test
//! checks it); values are written through `retoque_core::ini::Ini::set`, never appended beside an existing key.

#[derive(Clone, Debug, PartialEq)]
pub enum Kind {
    Bool,
    /// One of the listed values; `auto` included when the ini accepts it.
    Choice(&'static [&'static str]),
    Float {
        min: f32,
        max: f32,
    },
    Int {
        min: i64,
        max: i64,
    },
    Text,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Setting {
    pub section: &'static str,
    pub key: &'static str,
    pub kind: Kind,
    /// Short label and help, English and pt-BR.
    pub label_en: &'static str,
    pub label_pt: &'static str,
    pub help_en: &'static str,
    pub help_pt: &'static str,
}

impl Setting {
    /// Human labels for serialized choices. None means an unknown/custom INI value: preserve it unchanged.
    pub fn choice_labels(&self, value: &str) -> Option<(&'static str, &'static str)> {
        Some(match (self.section, self.key, value) {
            (_, _, "auto") => ("Automatic", "Automático"),
            (_, _, "true") => ("On", "Ligado"),
            (_, _, "false") => ("Off", "Desligado"),
            ("DlssNr", "Stage", "0") => ("After the upscaler", "Depois da ampliação"),
            ("DlssNr", "Stage", "1") => (
                "Before the upscaler (experimental)",
                "Antes da ampliação (experimental)",
            ),
            ("DlssNr", "HookMethod", "0") => ("Automatic route", "Rota automática"),
            ("DlssNr", "HookMethod", "1") => ("Upscaled image", "Imagem ampliada"),
            ("DlssNr", "HookMethod", "2") => ("At presentation", "Na apresentação"),
            ("DlssNr", "Transfer", "0") => ("Classic", "Clássica"),
            ("DlssNr", "Transfer", "1") => ("Residual", "Residual"),
            ("DlssNr", "Transfer", "2") => ("Residual + DLSS", "Residual + DLSS"),
            ("DlssNr", "Transfer", "3") => ("Sharp residual", "Residual nítido"),
            ("DlssNr", "Compare", "0") => ("Off", "Desligado"),
            ("DlssNr", "Compare", "1") => ("Side by side", "Lado a lado"),
            ("DlssNr", "Compare", "2") => ("Wipe", "Divisão móvel"),
            ("Upscalers", _, "fsr21") => ("FSR 2.1", "FSR 2.1"),
            ("Upscalers", _, "fsr22") => ("FSR 2.2", "FSR 2.2"),
            ("Upscalers", _, "fsr31") => ("FSR 3.1", "FSR 3.1"),
            ("Upscalers", _, "ffx") => ("FidelityFX", "FidelityFX"),
            ("Upscalers", _, "xess") => ("Intel XeSS", "Intel XeSS"),
            ("Upscalers", _, "dlss") => ("NVIDIA DLSS", "NVIDIA DLSS"),
            ("Upscalers", _, "fsr21_12") => ("FSR 2.1 (D3D12 bridge)", "FSR 2.1 (ponte D3D12)"),
            ("Upscalers", _, "fsr22_12") => ("FSR 2.2 (D3D12 bridge)", "FSR 2.2 (ponte D3D12)"),
            ("Upscalers", _, "ffx_12") => ("FidelityFX (D3D12 bridge)", "FidelityFX (ponte D3D12)"),
            ("Upscalers", _, "xess_12") => ("Intel XeSS (D3D12 bridge)", "Intel XeSS (ponte D3D12)"),
            ("Upscalers", _, "dlss_12") => ("NVIDIA DLSS (D3D12 bridge)", "NVIDIA DLSS (ponte D3D12)"),
            ("FrameGen", _, "nofg" | "None") => ("None", "Nenhuma"),
            ("FrameGen", _, "dlssg") => ("DLSS Frame Generation", "Geração de quadros DLSS"),
            ("FrameGen", _, "nvngxfg") => ("NVIDIA NGX", "NVIDIA NGX"),
            ("FrameGen", _, "fsrfg") => ("FSR Frame Generation", "Geração de quadros FSR"),
            ("FrameGen", _, "fsrfg30") => ("FSR 3.0 Frame Generation", "Geração de quadros FSR 3.0"),
            ("FrameGen", _, "xefg") => ("Intel Xe Frame Generation", "Geração de quadros Intel Xe"),
            ("FrameGen", _, "upscaler") => ("From the upscaler", "A partir do ampliador"),
            ("FrameGen", _, "synthesized") => ("Synthesized (experimental)", "Sintetizada (experimental)"),
            ("FrameGen", _, "Nukems") => ("Nukem's replacement", "Substituto do Nukem"),
            ("FrameGen", _, "Arturs") => ("Artur's replacement", "Substituto do Artur"),
            ("FrameGen", _, "FFX") => ("FidelityFX replacement", "Substituto FidelityFX"),
            ("FrameGen", _, "Combo") => ("Combined replacements", "Substitutos combinados"),
            ("LSFG", "Multiplier", "2") => ("2× frames", "2× quadros"),
            ("LSFG", "Multiplier", "3") => ("3× frames", "3× quadros"),
            ("LSFG", "Multiplier", "4") => ("4× frames", "4× quadros"),
            _ => return None,
        })
    }

    /// Validate before building an edit plan. Numeric settings accept finite values in the runtime range or auto.
    /// Keeping this beside the schema lets the GUI reject text fallback input without duplicating its rules.
    pub fn accepts(&self, value: &str) -> bool {
        let value = value.trim();
        if value.eq_ignore_ascii_case("auto") {
            return true;
        }
        match self.kind {
            Kind::Bool => matches!(value, "true" | "false"),
            Kind::Choice(choices) => choices.contains(&value),
            Kind::Float { min, max } => value
                .parse::<f32>()
                .is_ok_and(|number| number.is_finite() && (min..=max).contains(&number)),
            Kind::Int { min, max } => value.parse::<i64>().is_ok_and(|number| (min..=max).contains(&number)),
            Kind::Text => !value.is_empty() && !value.contains(['\r', '\n', '\0']),
        }
    }
}

const AUTO_BOOL: Kind = Kind::Choice(&["auto", "true", "false"]);

macro_rules! setting {
    ($section:literal, $key:literal, $kind:expr, $en:literal, $pt:literal, $help_en:literal, $help_pt:literal) => {
        Setting {
            section: $section,
            key: $key,
            kind: $kind,
            label_en: $en,
            label_pt: $pt,
            help_en: $help_en,
            help_pt: $help_pt,
        }
    };
}

/// Numeric and text settings also accept `auto` to restore the shipped automatic behavior. A schema entry
/// describes a setting, not runtime availability: the UI must gate controls against the game's capabilities.
/// LSFG is documented as study-build-only and requires user-owned assets; its presence here proves no support.
pub fn all() -> &'static [Setting] {
    &[
        setting!(
            "DlssNr",
            "Enabled",
            AUTO_BOOL,
            "Neural Rendering",
            "Renderização neural",
            "Needs a user-supplied NR model. Auto is off.",
            "Requer um modelo NR fornecido pelo usuário. Automático deixa desligado."
        ),
        setting!(
            "DlssNr",
            "Stage",
            Kind::Choice(&["auto", "0", "1"]),
            "NR stage",
            "Etapa do NR",
            "Before upscaling is experimental. Vulkan needs a restart; an unavailable stage does not switch automatically.",
            "Antes da ampliação é experimental. Vulkan exige reinício; uma etapa indisponível não muda automaticamente."
        ),
        setting!(
            "DlssNr",
            "WorkingScale",
            Kind::Float { min: 0.25, max: 2.0 },
            "Model scale",
            "Escala do modelo",
            "Auto selects by GPU; 0.25–2.0 is accepted. Above 1 supersamples; routes without real motion clamp to 1.",
            "Auto escolhe pela GPU; aceita de 0,25 a 2,0. Acima de 1 amplia a resolução; rotas sem movimento real limitam a 1."
        ),
        setting!(
            "DlssNr",
            "ModelFile",
            Kind::Text,
            "Model file",
            "Arquivo do modelo",
            "Auto searches for nvngx_dlssnr.dll. A missing named file disables NR. Restart required; incompatible with UseProxy.",
            "Auto procura nvngx_dlssnr.dll. Arquivo indicado ausente desativa NR. Exige reinício; incompatível com UseProxy."
        ),
        setting!(
            "DlssNr",
            "GameProfiles",
            AUTO_BOOL,
            "Game profiles",
            "Perfis de jogos",
            "Apply known game settings at startup only where the INI says auto.",
            "Aplica ajustes conhecidos ao iniciar, apenas onde o INI contém auto."
        ),
        setting!(
            "DlssNr",
            "HookMethod",
            Kind::Choice(&["auto", "0", "1", "2"]),
            "NR placement",
            "Posição do NR",
            "Choose where NR receives the picture. Native Vulkan ignores this. Some routes need a restart.",
            "Escolha onde NR recebe a imagem. Vulkan nativo ignora este ajuste. Algumas rotas exigem reinício."
        ),
        setting!(
            "DlssNr",
            "ApplyModel",
            AUTO_BOOL,
            "Apply model",
            "Aplicar modelo",
            "Off shows the clean picture while NR keeps running.",
            "Desativado mostra a imagem original enquanto NR continua executando."
        ),
        setting!(
            "DlssNr",
            "HighlightRecovery",
            AUTO_BOOL,
            "Highlight recovery",
            "Recuperar altas luzes",
            "Preserve bright colors at some cost to model detail. Linear HDR only.",
            "Preserva cores brilhantes com alguma perda de detalhe do modelo. Apenas HDR linear."
        ),
        setting!(
            "DlssNr",
            "Transfer",
            Kind::Choice(&["auto", "0", "1", "2", "3"]),
            "Model enlargement",
            "Ampliação do modelo",
            "Enlarges the model image below full resolution. Residual + DLSS is available only on supported routes.",
            "Amplia a imagem do modelo abaixo da resolução total. Residual + DLSS depende de uma rota compatível."
        ),
        setting!(
            "DlssNr",
            "Compare",
            Kind::Choice(&["auto", "0", "1", "2"]),
            "Compare pictures",
            "Comparar imagens",
            "Compare the original and edited pictures in the game.",
            "Compare a imagem original e a editada dentro do jogo."
        ),
        setting!(
            "DlssNr",
            "CompareSplit",
            Kind::Float { min: 0.0, max: 1.0 },
            "Wipe position",
            "Posição da divisão",
            "Fraction of picture width; only acts with Compare=2. Auto is 0.5.",
            "Fração da largura da imagem; só atua com Compare=2. Auto é 0.5."
        ),
        setting!(
            "DlssNr",
            "CompareSwap",
            AUTO_BOOL,
            "Swap comparison sides",
            "Trocar lados da comparação",
            "Swap clean and edited pictures while comparing.",
            "Troca a imagem original e a editada durante a comparação."
        ),
        setting!(
            "Upscalers",
            "Dx11Upscaler",
            Kind::Choice(&[
                "auto", "fsr22", "fsr31", "xess", "xess_12", "fsr21_12", "fsr22_12", "ffx_12", "dlss", "dlss_12"
            ]),
            "D3D11 upscaler",
            "Ampliador D3D11",
            "The _12 options use the D3D12 bridge; dlss_12 can carry NR. Native XeSS requires Arc.",
            "Opções _12 usam a ponte D3D12; dlss_12 pode executar NR. XeSS nativo requer Arc."
        ),
        setting!(
            "Upscalers",
            "Dx12Upscaler",
            Kind::Choice(&["auto", "xess", "fsr21", "fsr22", "ffx", "dlss"]),
            "D3D12 upscaler",
            "Ampliador D3D12",
            "Auto selects a backend for the GPU. Custom folder names remain editable in the INI.",
            "Auto escolhe um backend para a GPU. Nomes de pastas personalizadas podem ser editados no INI."
        ),
        setting!(
            "Upscalers",
            "VulkanUpscaler",
            Kind::Choice(&["auto", "fsr21", "fsr22", "ffx", "xess", "fsr21_12", "ffx_12", "dlss"]),
            "Vulkan upscaler",
            "Ampliador Vulkan",
            "The _12 options use the D3D12 bridge. Custom folder names remain editable in the INI.",
            "Opções _12 usam a ponte D3D12. Nomes de pastas personalizadas podem ser editados no INI."
        ),
        setting!(
            "FrameGen",
            "Enabled",
            AUTO_BOOL,
            "Frame generation",
            "Geração de quadros",
            "Auto is off, except DLSSG input follows the game's setting.",
            "Auto desativa, exceto quando a entrada DLSSG acompanha o ajuste do jogo."
        ),
        setting!(
            "FrameGen",
            "FGInput",
            Kind::Choice(&[
                "auto",
                "nofg",
                "dlssg",
                "nvngxfg",
                "fsrfg",
                "upscaler",
                "fsrfg30",
                "xefg",
                "synthesized"
            ]),
            "FG input",
            "Entrada da geração",
            "Choose the game's available source. Synthesized input is experimental D3D11/D3D12.",
            "Escolha a fonte disponível no jogo. Entrada synthesized é experimental em D3D11/D3D12."
        ),
        setting!(
            "FrameGen",
            "FGOutput",
            Kind::Choice(&["auto", "nofg", "fsrfg", "xefg", "dlssg"]),
            "FG output",
            "Saída da geração",
            "Requires the chosen provider's libraries and supported hardware.",
            "Requer as bibliotecas do provedor escolhido e hardware compatível."
        ),
        setting!(
            "FrameGen",
            "FGNvngxReplacement",
            Kind::Choice(&["auto", "None", "Nukems", "Arturs", "FFX", "Combo"]),
            "DLSSG replacement",
            "Substituto DLSSG",
            "Only acts with nvngxfg input or dlssg output. Requires the replacement libraries.",
            "Só atua com entrada nvngxfg ou saída dlssg. Requer bibliotecas do substituto."
        ),
        setting!(
            "LSFG",
            "Enabled",
            AUTO_BOOL,
            "LSFG (study build)",
            "LSFG (build de estudo)",
            "Requires fsrfg output and user-owned shaders. Unsupported frames fall back to FFX.",
            "Requer saída fsrfg e shaders do usuário. Quadros não suportados usam FFX."
        ),
        setting!(
            "LSFG",
            "FlowScale",
            Kind::Float { min: 0.25, max: 1.0 },
            "LSFG flow scale",
            "Escala do fluxo LSFG",
            "Lower motion-pyramid resolution costs less. Auto is 1.",
            "Menor resolução do fluxo custa menos. Auto é 1."
        ),
        setting!(
            "LSFG",
            "PerformanceMode",
            AUTO_BOOL,
            "LSFG performance mode",
            "Modo de desempenho LSFG",
            "Fewer feature channels: cheaper, softer motion.",
            "Menos canais: menor custo, movimento menos definido."
        ),
        setting!(
            "LSFG",
            "ShaderDir",
            Kind::Text,
            "LSFG shader folder",
            "Pasta de shaders LSFG",
            "Precompiled shaders from your own copy; none are shipped. Auto leaves this unset.",
            "Shaders compilados da sua própria cópia; nenhum é distribuído. Auto deixa sem pasta."
        ),
        setting!(
            "LSFG",
            "DllPath",
            Kind::Text,
            "Lossless Scaling location",
            "Local do Lossless Scaling",
            "lsfg-vk.dll, Lossless.dll or its folder. Auto searches Steam and Wine paths.",
            "lsfg-vk.dll, Lossless.dll ou sua pasta. Auto procura nas pastas Steam e Wine."
        ),
        setting!(
            "LSFG",
            "Multiplier",
            Kind::Choice(&["auto", "2", "3", "4"]),
            "LSFG multiplier",
            "Multiplicador LSFG",
            "Frames shown per real frame. Switching from 2 to 3/4 needs a restart.",
            "Quadros exibidos por quadro real. Mudar de 2 para 3/4 exige reinício."
        ),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::{HashMap, HashSet};

    #[test]
    fn every_choice_has_bilingual_labels_without_changing_serialized_values() {
        for setting in all() {
            if let Kind::Choice(values) = setting.kind {
                for value in values {
                    let (en, pt) = setting.choice_labels(value).expect("choice needs both labels");
                    assert!(!en.is_empty() && !pt.is_empty());
                    assert!(setting.accepts(value));
                }
            }
        }
        let stage = all().iter().find(|s| s.key == "Stage").unwrap();
        assert_eq!(
            stage.choice_labels("1"),
            Some((
                "Before the upscaler (experimental)",
                "Antes da ampliação (experimental)"
            ))
        );
        assert_eq!(stage.choice_labels("custom"), None);
    }

    #[test]
    fn review_working_scale_is_bounded_float_not_free_text() {
        let setting = all()
            .iter()
            .find(|s| s.section == "DlssNr" && s.key == "WorkingScale")
            .unwrap();
        assert_eq!(setting.kind, Kind::Float { min: 0.25, max: 2.0 });
    }

    #[test]
    fn working_scale_rejects_invalid_input_and_round_trips_finite_values_or_auto() {
        let setting = all().iter().find(|s| s.key == "WorkingScale").unwrap();
        for invalid in [
            "abc", "50%", "NaN", "inf", "-inf", "1e50", "0", "0.249", "2.001", "", "1.0junk",
        ] {
            assert!(!setting.accepts(invalid), "accepted {invalid}");
        }
        for value in ["auto", "0.25", "0.5", "1", "1.75", "2.0"] {
            assert!(setting.accepts(value));
            let mut ini = retoque_core::ini::Ini::parse(b"[DlssNr]\nWorkingScale=auto\n");
            ini.set(setting.section, setting.key, value);
            let text = ini.to_bytes();
            let round_trip = retoque_core::ini::Ini::parse(&text);
            assert_eq!(round_trip.get(setting.section, setting.key), Some(value));
        }
    }

    #[test]
    fn curated_keys_and_choice_defaults_match_shipped_ini() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../../OptiScaler.ini");
        let text = std::fs::read_to_string(path).unwrap();
        let mut section = "";
        let mut values = HashMap::new();
        for line in text.lines().map(str::trim) {
            if let Some(s) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
                section = s;
            } else if !line.starts_with(';')
                && !line.starts_with('#')
                && let Some((key, value)) = line.split_once('=')
            {
                values.insert((section, key.trim()), value.trim());
            }
        }
        let mut seen = HashSet::new();
        for setting in all() {
            assert!(seen.insert((setting.section, setting.key)), "duplicate setting");
            let default = values
                .get(&(setting.section, setting.key))
                .expect("setting must exist in shipped INI");
            if let Kind::Choice(choices) = setting.kind {
                assert!(
                    choices.contains(default),
                    "{} {} default {} not in {:?}",
                    setting.section,
                    setting.key,
                    default,
                    choices
                );
                assert!(choices.contains(&"auto"));
            }
            assert!(!setting.label_en.is_empty() && !setting.label_pt.is_empty());
            assert!(!setting.help_en.is_empty() && !setting.help_pt.is_empty());
        }
        assert_eq!(all().len(), 25);
    }
}
