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
            "Neural Rendering (NR)",
            "Renderização neural (NR)",
            "Needs an NR model (nvngx_dlssnr.dll) that you supply. Automatic: off.",
            "Precisa de um modelo NR (nvngx_dlssnr.dll) fornecido por você. No automático: desligado."
        ),
        setting!(
            "DlssNr",
            "Stage",
            Kind::Choice(&["auto", "0", "1"]),
            "NR stage",
            "Etapa do NR",
            "Before the upscaler is experimental. On Vulkan a change needs a game restart. If the chosen stage is not available, NR does not switch by itself.",
            "Antes da ampliação é experimental. No Vulkan a mudança exige reiniciar o jogo. Se a etapa escolhida não estiver disponível, o NR não troca sozinho."
        ),
        setting!(
            "DlssNr",
            "WorkingScale",
            Kind::Float { min: 0.25, max: 2.0 },
            "Model scale",
            "Escala do modelo",
            "Automatic: chosen for your graphics card. From 0.25 to 2.0; above 1 the model works at a higher resolution. Routes without real motion stay at 1 or below.",
            "No automático: escolhida pela sua placa de vídeo. De 0,25 a 2,0; acima de 1 o modelo trabalha numa resolução maior. Rotas sem movimento real ficam em 1 ou menos."
        ),
        setting!(
            "DlssNr",
            "Passes",
            Kind::Int { min: 1, max: 10 },
            "Model passes",
            "Passes do modelo",
            "How many times the model runs on each frame, each pass refining the previous one. D3D12 only; native Vulkan games use one. Automatic: 1.",
            "Quantas vezes o modelo roda em cada quadro, cada passe refinando o anterior. Só em D3D12; jogos em Vulkan nativo usam um. No automático: 1."
        ),
        setting!(
            "DlssNr",
            "ModelFile",
            Kind::Text,
            "Model file",
            "Arquivo do modelo",
            "Automatic: looks for nvngx_dlssnr.dll beside the game. If the chosen file is missing, NR turns off. Restart the game after changing it.",
            "No automático: procura o nvngx_dlssnr.dll junto do jogo. Se o arquivo escolhido não existir, o NR desliga. Reinicie o jogo depois de mudar."
        ),
        setting!(
            "DlssNr",
            "GameProfiles",
            AUTO_BOOL,
            "Game profiles",
            "Perfis de jogos",
            "Applies settings known to work for this game when it starts, only where a setting is left on Automatic.",
            "Aplica, ao abrir o jogo, ajustes que funcionam nele, só onde o ajuste está no automático."
        ),
        setting!(
            "DlssNr",
            "HookMethod",
            Kind::Choice(&["auto", "0", "1", "2"]),
            "NR placement",
            "Posição do NR",
            "Where NR receives the picture. Native Vulkan games ignore this. Some choices need a game restart.",
            "Onde o NR recebe a imagem. Jogos em Vulkan nativo ignoram este ajuste. Algumas opções exigem reiniciar o jogo."
        ),
        setting!(
            "DlssNr",
            "ApplyModel",
            AUTO_BOOL,
            "Apply model",
            "Aplicar modelo",
            "Off shows the original picture while NR keeps running, to compare.",
            "Desligado mostra a imagem original enquanto o NR continua rodando, para comparar."
        ),
        setting!(
            "DlssNr",
            "HighlightRecovery",
            AUTO_BOOL,
            "Highlight recovery",
            "Recuperar altas luzes",
            "Keeps bright colors, at some cost to the model's detail. Only for games with HDR output.",
            "Preserva cores brilhantes, com alguma perda de detalhe do modelo. Só em jogos com saída HDR."
        ),
        setting!(
            "DlssNr",
            "Transfer",
            Kind::Choice(&["auto", "0", "1", "2", "3"]),
            "Model enlargement",
            "Ampliação do modelo",
            "How the model's picture is enlarged when it works below full resolution. Residual + DLSS works only on some routes.",
            "Como a imagem do modelo é ampliada quando ele trabalha abaixo da resolução total. Residual + DLSS só funciona em algumas rotas."
        ),
        setting!(
            "DlssNr",
            "Compare",
            Kind::Choice(&["auto", "0", "1", "2"]),
            "Compare pictures",
            "Comparar imagens",
            "Shows the original and the edited picture together in the game.",
            "Mostra a imagem original e a editada juntas, dentro do jogo."
        ),
        setting!(
            "DlssNr",
            "CompareSplit",
            Kind::Float { min: 0.0, max: 1.0 },
            "Wipe position",
            "Posição da divisão",
            "Where the divider sits (0.5 = middle). Applies to the Wipe mode. Automatic: middle.",
            "Onde fica a divisória na tela (0,5 = meio). Vale para o modo Divisão móvel. No automático: no meio."
        ),
        setting!(
            "DlssNr",
            "CompareSwap",
            AUTO_BOOL,
            "Swap comparison sides",
            "Trocar lados da comparação",
            "Swaps the sides of the original and the edited picture while comparing.",
            "Troca de lado a imagem original e a editada durante a comparação."
        ),
        setting!(
            "Upscalers",
            "Dx11Upscaler",
            Kind::Choice(&[
                "auto", "fsr22", "fsr31", "xess", "xess_12", "fsr21_12", "fsr22_12", "ffx_12", "dlss", "dlss_12"
            ]),
            "D3D11 upscaler",
            "Ampliador D3D11",
            "Options marked \"D3D12 bridge\" run through D3D12; NVIDIA DLSS (D3D12 bridge) can also run NR. Intel XeSS without the bridge needs an Intel Arc card.",
            "Opções com \"ponte D3D12\" rodam pelo D3D12; NVIDIA DLSS (ponte D3D12) também pode rodar o NR. Intel XeSS sem a ponte precisa de uma placa Intel Arc."
        ),
        setting!(
            "Upscalers",
            "Dx12Upscaler",
            Kind::Choice(&["auto", "xess", "fsr21", "fsr22", "ffx", "dlss"]),
            "D3D12 upscaler",
            "Ampliador D3D12",
            "Automatic: chosen for your graphics card.",
            "No automático: escolhido pela sua placa de vídeo."
        ),
        setting!(
            "Upscalers",
            "VulkanUpscaler",
            Kind::Choice(&["auto", "fsr21", "fsr22", "ffx", "xess", "fsr21_12", "ffx_12", "dlss"]),
            "Vulkan upscaler",
            "Ampliador Vulkan",
            "Options marked \"D3D12 bridge\" run through D3D12. Automatic: chosen for your graphics card.",
            "Opções com \"ponte D3D12\" rodam pelo D3D12. No automático: escolhido pela sua placa de vídeo."
        ),
        setting!(
            "FrameGen",
            "Enabled",
            AUTO_BOOL,
            "Frame generation",
            "Geração de quadros",
            "Automatic: off, except when the game's own DLSS frame generation is the source; then it follows the game's setting.",
            "No automático: desligada, exceto quando a fonte é a geração de quadros DLSS do próprio jogo; aí segue o ajuste do jogo."
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
            "Which of the game's frame generation sources to use. Synthesized is experimental, D3D11 and D3D12 only.",
            "Qual fonte de geração de quadros do jogo usar. Sintetizada é experimental, só em D3D11 e D3D12."
        ),
        setting!(
            "FrameGen",
            "FGOutput",
            Kind::Choice(&["auto", "nofg", "fsrfg", "xefg", "dlssg"]),
            "FG output",
            "Saída da geração",
            "Needs the chosen provider's files and a graphics card that supports it.",
            "Precisa dos arquivos do provedor escolhido e de uma placa de vídeo compatível."
        ),
        setting!(
            "FrameGen",
            "FGNvngxReplacement",
            Kind::Choice(&["auto", "None", "Nukems", "Arturs", "FFX", "Combo"]),
            "DLSSG replacement",
            "Substituto DLSSG",
            "Only used with the NVIDIA NGX source or DLSS frame generation output. Needs the replacement's files.",
            "Só é usado com a fonte NVIDIA NGX ou a saída de geração de quadros DLSS. Precisa dos arquivos do substituto."
        ),
        setting!(
            "LSFG",
            "Enabled",
            AUTO_BOOL,
            "LSFG (experimental)",
            "LSFG (experimental)",
            "Needs FSR frame generation output and your own Lossless Scaling shaders. Frames LSFG cannot handle fall back to FidelityFX.",
            "Precisa da saída de geração de quadros FSR e dos shaders do seu Lossless Scaling. Quadros que o LSFG não suporta usam o FidelityFX."
        ),
        setting!(
            "LSFG",
            "FlowScale",
            Kind::Float { min: 0.25, max: 1.0 },
            "LSFG flow scale",
            "Escala do fluxo LSFG",
            "Lower values cost less and track motion less finely. Automatic: 1.",
            "Valores menores custam menos e acompanham o movimento com menos detalhe. No automático: 1."
        ),
        setting!(
            "LSFG",
            "PerformanceMode",
            AUTO_BOOL,
            "LSFG performance mode",
            "Modo de desempenho LSFG",
            "Cheaper, with softer motion.",
            "Custa menos, com movimento menos definido."
        ),
        setting!(
            "LSFG",
            "ShaderDir",
            Kind::Text,
            "LSFG shader folder",
            "Pasta de shaders LSFG",
            "Shaders prepared from your own copy of Lossless Scaling; none are shipped. Automatic: none set.",
            "Shaders preparados a partir da sua cópia do Lossless Scaling; nenhum é distribuído. No automático: nenhuma pasta."
        ),
        setting!(
            "LSFG",
            "DllPath",
            Kind::Text,
            "Lossless Scaling location",
            "Local do Lossless Scaling",
            "lsfg-vk.dll, Lossless.dll or its folder. Automatic: searched in your Steam and Wine folders.",
            "lsfg-vk.dll, Lossless.dll ou a pasta dele. No automático: procurado nas pastas da Steam e do Wine."
        ),
        setting!(
            "LSFG",
            "Multiplier",
            Kind::Choice(&["auto", "2", "3", "4"]),
            "LSFG multiplier",
            "Multiplicador LSFG",
            "Frames shown for each real frame. Going from 2 to 3 or 4 needs a game restart.",
            "Quadros exibidos para cada quadro real. Passar de 2 para 3 ou 4 exige reiniciar o jogo."
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
    fn passes_round_trip_one_to_ten_and_reject_the_rest() {
        let setting = all()
            .iter()
            .find(|s| s.section == "DlssNr" && s.key == "Passes")
            .unwrap();
        assert_eq!(setting.kind, Kind::Int { min: 1, max: 10 });
        for invalid in ["0", "11", "-1", "100", "1.5", "2x", "abc", "", "99999999999999999999"] {
            assert!(!setting.accepts(invalid), "accepted {invalid}");
        }
        for value in (1..=10).map(|n: i64| n.to_string()).chain(["auto".to_owned()]) {
            assert!(setting.accepts(&value));
            let mut ini = retoque_core::ini::Ini::parse(b"[DlssNr]\nPasses=auto\n");
            ini.set(setting.section, setting.key, &value);
            let round_trip = retoque_core::ini::Ini::parse(&ini.to_bytes());
            assert_eq!(round_trip.get(setting.section, setting.key), Some(value.as_str()));
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
        assert_eq!(all().len(), 26);
    }
}
