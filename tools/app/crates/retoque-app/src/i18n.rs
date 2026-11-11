//! The app's text. English and Brazilian Portuguese live in the table below; eight more languages are packs in
//! `i18n/<code>.tsv` (one `key<TAB>text` per line, embedded at build time and parsed on first use). A key missing
//! from a pack shows in English; a key missing from the table shows the key itself, so a gap is visible instead of
//! silently empty. The packs are machine-translated drafts pending native review (see tools/app/README.md).

use std::collections::HashMap;
use std::sync::OnceLock;

use serde::{Deserialize, Serialize};

/// The app's languages. Saved by variant name ("En", "Pt", "ZhCn"), so a variant is never renamed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum Lang {
    En,
    Pt,
    De,
    Es,
    Fr,
    Ja,
    Ko,
    Pl,
    Ru,
    ZhCn,
}

impl Lang {
    /// Every language in the order the selector lists them: by native name, Latin, then Cyrillic, then CJK.
    pub const ALL: [Lang; 10] = [
        Lang::De,
        Lang::En,
        Lang::Es,
        Lang::Fr,
        Lang::Pl,
        Lang::Pt,
        Lang::Ru,
        Lang::Ja,
        Lang::Ko,
        Lang::ZhCn,
    ];

    /// The language's name in itself, as the selector shows it whatever language the app is in.
    pub fn native_name(self) -> &'static str {
        match self {
            Lang::En => "English",
            Lang::Pt => "Português (Brasil)",
            Lang::De => "Deutsch",
            Lang::Es => "Español",
            Lang::Fr => "Français",
            Lang::Ja => "日本語",
            Lang::Ko => "한국어",
            Lang::Pl => "Polski",
            Lang::Ru => "Русский",
            Lang::ZhCn => "简体中文",
        }
    }

    /// The BCP 47 tag, also the pack's file name.
    pub fn code(self) -> &'static str {
        match self {
            Lang::En => "en",
            Lang::Pt => "pt-BR",
            Lang::De => "de",
            Lang::Es => "es",
            Lang::Fr => "fr",
            Lang::Ja => "ja",
            Lang::Ko => "ko",
            Lang::Pl => "pl",
            Lang::Ru => "ru",
            Lang::ZhCn => "zh-CN",
        }
    }

    /// Text in Chinese, Japanese or Korean needs a system font with those glyphs (egui's own fonts have none).
    pub fn needs_cjk(self) -> bool {
        matches!(self, Lang::Ja | Lang::Ko | Lang::ZhCn)
    }

    /// A machine-translated pack that no native speaker has reviewed yet.
    pub fn is_draft(self) -> bool {
        !matches!(self, Lang::En | Lang::Pt)
    }

    /// Writes 4,0 rather than 4.0.
    pub fn decimal_comma(self) -> bool {
        matches!(self, Lang::Pt | Lang::De | Lang::Es | Lang::Fr | Lang::Pl | Lang::Ru)
    }

    /// The column of the built-in tables a language falls back to: Portuguese for pt-BR, English for the rest.
    fn table(self) -> Lang {
        if self == Lang::Pt { Lang::Pt } else { Lang::En }
    }

    /// The language for text that `retoque-data` writes itself (English for a language it does not have).
    pub fn data_language(self) -> retoque_data::messages::Language {
        match self.table() {
            Lang::Pt => retoque_data::messages::Language::Portuguese,
            _ => retoque_data::messages::Language::English,
        }
    }

    /// The supported language a locale names ("de-DE", "pt_BR.UTF-8", "zh-Hans-CN"), if any. Traditional Chinese
    /// is not Simplified Chinese, so it is not matched.
    pub fn from_locale(locale: &str) -> Option<Lang> {
        let tag = locale
            .split(['.', '@'])
            .next()
            .unwrap_or_default()
            .to_ascii_lowercase()
            .replace('_', "-");
        let mut parts = tag.split('-');
        Some(match parts.next()? {
            "en" => Lang::En,
            "pt" => Lang::Pt,
            "de" => Lang::De,
            "es" => Lang::Es,
            "fr" => Lang::Fr,
            "ja" => Lang::Ja,
            "ko" => Lang::Ko,
            "pl" => Lang::Pl,
            "ru" => Lang::Ru,
            "zh" if !parts.any(|p| matches!(p, "hant" | "tw" | "hk" | "mo")) => Lang::ZhCn,
            _ => return None,
        })
    }

    /// The system locale's language when the app has it, English otherwise. RETOQUE_LANG (a tag such as "de" or
    /// "pt-BR") forces one for tests and screenshots.
    pub fn from_system() -> Lang {
        if let Ok(forced) = std::env::var("RETOQUE_LANG") {
            return Lang::from_locale(&forced).unwrap_or(Lang::En);
        }
        sys_locale::get_locale()
            .and_then(|l| Lang::from_locale(&l))
            .unwrap_or(Lang::En)
    }
}

/// A language pack: key → text, borrowed from the embedded file.
type Pack = HashMap<&'static str, &'static str>;

/// The embedded pack of a language that is not in the built-in table.
fn pack_source(lang: Lang) -> Option<&'static str> {
    Some(match lang {
        Lang::En | Lang::Pt => return None,
        Lang::De => include_str!("i18n/de.tsv"),
        Lang::Es => include_str!("i18n/es.tsv"),
        Lang::Fr => include_str!("i18n/fr.tsv"),
        Lang::Ja => include_str!("i18n/ja.tsv"),
        Lang::Ko => include_str!("i18n/ko.tsv"),
        Lang::Pl => include_str!("i18n/pl.tsv"),
        Lang::Ru => include_str!("i18n/ru.tsv"),
        Lang::ZhCn => include_str!("i18n/zh-CN.tsv"),
    })
}

/// `key<TAB>text` lines; blank lines and lines starting with "#" are skipped (tests reject anything else).
fn parse_pack(source: &'static str) -> Pack {
    source
        .lines()
        .map(|line| line.trim_end_matches('\r'))
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .filter_map(|line| line.split_once('\t'))
        .collect()
}

/// Parsed once per language, on first use.
fn pack(lang: Lang) -> Option<&'static Pack> {
    static PACKS: [OnceLock<Pack>; Lang::ALL.len()] = [const { OnceLock::new() }; Lang::ALL.len()];
    let source = pack_source(lang)?;
    Some(PACKS[lang as usize].get_or_init(|| parse_pack(source)))
}

/// The CLDR plural category of a whole count: "one", "few", "many" or "other".
fn plural_category(lang: Lang, n: usize) -> &'static str {
    let (units, tens) = (n % 10, n % 100);
    let few = (2..=4).contains(&units) && !(12..=14).contains(&tens);
    match lang {
        Lang::Ja | Lang::Ko | Lang::ZhCn => "other",
        Lang::Fr if n <= 1 => "one",
        Lang::Pl if n == 1 => "one",
        Lang::Pl if few => "few",
        Lang::Pl => "many",
        Lang::Ru if units == 1 && tens != 11 => "one",
        Lang::Ru if few => "few",
        Lang::Ru => "many",
        _ if n == 1 => "one",
        _ => "other",
    }
}

/// The word that follows a count ("1 game", "3 games"). The table has a `_one` key and the other form (`x_other`,
/// or plain `x`); a pack whose language has more forms adds `x_few` and `x_many` (Polish "2 gry", "5 gier"), else
/// the other form serves.
pub fn plural(lang: Lang, n: usize, one: &'static str, other: &'static str) -> &'static str {
    let category = plural_category(lang, n);
    let base = other.strip_suffix("_other").unwrap_or(other);
    let extra = matches!(category, "few" | "many")
        .then(|| pack(lang).and_then(|p| p.get(format!("{base}_{category}").as_str()).copied()))
        .flatten();
    extra.unwrap_or_else(|| t(lang, if category == "one" { one } else { other }))
}

/// A per-game setting's label and help line.
pub fn setting_text(lang: Lang, setting: &retoque_data::settings::Setting) -> (&'static str, &'static str) {
    let builtin = match lang.table() {
        Lang::Pt => (setting.label_pt, setting.help_pt),
        _ => (setting.label_en, setting.help_en),
    };
    let Some(pack) = pack(lang) else {
        return builtin;
    };
    let key = format!("setting.{}.{}", setting.section, setting.key);
    (
        pack.get(key.as_str()).copied().unwrap_or(builtin.0),
        pack.get(format!("{key}.help").as_str()).copied().unwrap_or(builtin.1),
    )
}

/// A per-game setting's label alone.
pub fn setting_label(lang: Lang, setting: &retoque_data::settings::Setting) -> &'static str {
    setting_text(lang, setting).0
}

/// The pack key of a drop-down value, from its English label: "Before the upscaler (experimental)" →
/// "choice.before_the_upscaler_experimental".
fn choice_key(en: &str) -> String {
    let mut key = String::from("choice.");
    for word in en
        .to_ascii_lowercase()
        .split(|c: char| !c.is_ascii_alphanumeric())
        .filter(|w| !w.is_empty())
    {
        if !key.ends_with('.') {
            key.push('_');
        }
        key.push_str(word);
    }
    key
}

/// A drop-down value's label, given its English and Portuguese labels from `Setting::choice_labels`.
pub fn choice_text(lang: Lang, en: &'static str, pt: &'static str) -> &'static str {
    match lang {
        Lang::En => en,
        Lang::Pt => pt,
        _ => pack(lang)
            .and_then(|p| p.get(choice_key(en).as_str()).copied())
            .unwrap_or(en),
    }
}

// (key, English, Portuguese)
const TEXT: &[(&str, &str, &str)] = &[
    ("lsfg.found", "Lossless Scaling found", "Lossless Scaling encontrado"),
    (
        "lsfg.preparing",
        "Preparing shaders automatically…",
        "Preparando shaders automaticamente…",
    ),
    (
        "lsfg.downloading",
        "Downloading the official shader compiler…",
        "Baixando o compilador oficial de shaders…",
    ),
    ("lsfg.cancel", "Cancel preparation", "Cancelar preparação"),
    (
        "lsfg.cancelled",
        "Preparation cancelled. Your game was not changed.",
        "Preparação cancelada. Seu jogo não foi alterado.",
    ),
    (
        "lsfg.failed",
        "The LSFG shaders could not be prepared. Try again; if it keeps failing, make a diagnostic file.",
        "Não foi possível preparar os shaders do LSFG. Tente de novo; se continuar falhando, gere um arquivo de diagnóstico.",
    ),
    ("lsfg.retry", "Retry preparation", "Tentar preparar novamente"),
    (
        "lsfg.download_prompt",
        "Preparing the LSFG shaders needs Microsoft's shader compiler. Retoque downloads it from Microsoft's official release into the app's data, checks it and prepares the shaders. Your game does not change.",
        "Preparar os shaders do LSFG exige o compilador de shaders da Microsoft. O Retoque o baixa da versão oficial da Microsoft para os dados do app, o verifica e prepara os shaders. Seu jogo não muda.",
    ),
    (
        "lsfg.need_compiler",
        "The shaders need Microsoft's shader compiler, which is not downloaded yet.",
        "Os shaders precisam do compilador de shaders da Microsoft, que ainda não foi baixado.",
    ),
    (
        "lsfg.consent_title",
        "Download the shader compiler",
        "Baixar o compilador de shaders",
    ),
    (
        "lsfg.download_details",
        "Microsoft DXC v1.8.2505.1 from github.com/microsoft/DirectXShaderCompiler (release-assets.githubusercontent.com), checked with a fixed SHA-256.",
        "Microsoft DXC v1.8.2505.1 de github.com/microsoft/DirectXShaderCompiler (release-assets.githubusercontent.com), conferido com um SHA-256 fixo.",
    ),
    (
        "lsfg.download_confirm",
        "Download the compiler…",
        "Baixar o compilador…",
    ),
    (
        "sm86.error.0",
        "ambiguous sm86 release asset",
        "Mais de um arquivo corresponde à versão sm86.",
    ),
    (
        "sm86.error.1",
        "app data unavailable",
        "O diretório de dados do app está indisponível.",
    ),
    (
        "sm86.error.2",
        "archive exceeds unpacked size limit",
        "O pacote excede o limite de tamanho descompactado.",
    ),
    (
        "sm86.error.3",
        "archive link, encryption or special file refused",
        "O pacote contém um link, criptografia ou arquivo especial não permitido.",
    ),
    (
        "sm86.error.4",
        "archive size overflow",
        "O tamanho declarado do pacote é inválido.",
    ),
    (
        "sm86.error.5",
        "documented sm86 release asset missing",
        "O arquivo documentado da versão sm86 não foi encontrado.",
    ),
    (
        "sm86.error.6",
        "incomplete sm86 release",
        "Faltam arquivos obrigatórios da versão sm86.",
    ),
    (
        "sm86.error.7",
        "invalid official sm86 release metadata",
        "Os metadados da versão oficial sm86 são inválidos.",
    ),
    (
        "sm86.error.8",
        "invalid release record",
        "O registro local da versão é inválido.",
    ),
    (
        "sm86.error.9",
        "kit cache requires a GitHub digest",
        "O cache do kit exige um hash publicado pelo GitHub.",
    ),
    (
        "sm86.error.10",
        "kit cache requires a matching GitHub SHA-256 digest",
        "O cache do kit exige correspondência com o SHA-256 publicado pelo GitHub.",
    ),
    (
        "sm86.error.11",
        "kit metadata too large",
        "Os metadados do kit excedem o limite de tamanho.",
    ),
    (
        "sm86.error.12",
        "missing release redirect",
        "O redirecionamento da versão não informa o destino.",
    ),
    (
        "sm86.error.13",
        "non-official sm86 URL",
        "O endereço não pertence à versão oficial sm86.",
    ),
    (
        "sm86.error.14",
        "protected binary in kit archive",
        "O pacote do kit contém um binário protegido que não pode ser incluído.",
    ),
    (
        "sm86.error.15",
        "refused sm86 release redirect",
        "O destino do redirecionamento da versão sm86 não é permitido.",
    ),
    (
        "sm86.error.16",
        "release API redirect refused",
        "A API de versões tentou um redirecionamento não permitido.",
    ),
    (
        "sm86.error.17",
        "release SHA-256 mismatch",
        "O SHA-256 do download não corresponde ao publicado.",
    ),
    (
        "sm86.error.18",
        "release changed since first download",
        "O conteúdo desta versão mudou desde o primeiro download.",
    ),
    (
        "sm86.error.19",
        "release metadata exceeds size limit",
        "Os metadados da versão excedem o limite de tamanho.",
    ),
    (
        "sm86.error.20",
        "release record exceeds limit",
        "O registro local da versão excede o limite de tamanho.",
    ),
    (
        "sm86.error.21",
        "release response exceeds size limit",
        "A resposta do download excede o limite de tamanho.",
    ),
    (
        "sm86.error.22",
        "release size mismatch",
        "O tamanho do download não corresponde ao declarado.",
    ),
    (
        "sm86.error.23",
        "sm86 HTTPS request failed",
        "Não foi possível consultar ou baixar a versão sm86 por HTTPS.",
    ),
    (
        "sm86.error.24",
        "too many archive entries",
        "O pacote contém arquivos demais.",
    ),
    (
        "sm86.error.25",
        "too many release redirects",
        "O download excedeu o limite de redirecionamentos.",
    ),
    (
        "sm86.error.26",
        "truncated archive entry",
        "O pacote contém um arquivo truncado.",
    ),
    (
        "sm86.error.27",
        "unexpected release HTTP status",
        "O servidor da versão retornou um status HTTP inesperado.",
    ),
    (
        "sm86.error.28",
        "unsafe or colliding archive path",
        "O pacote contém um caminho inseguro ou nomes conflitantes.",
    ),
    (
        "sm86.error.29",
        "unsupported release digest",
        "O algoritmo de hash publicado não é suportado.",
    ),
    (
        "sm86.error.30",
        "unsupported sm86 release",
        "Esta versão sm86 não é suportada.",
    ),
    (
        "note.sm86_upgrade",
        "Upgrade sm86 0.3.x: back up and remove the old proxy and INI before copying the complete 0.4.x runtime.",
        "Atualizar sm86 0.3.x: guardar backup e remover o arquivo de carregamento e o INI antigos antes de copiar o runtime 0.4.x completo.",
    ),
    (
        "note.sm86_install",
        "Install catalogued dlssg-sm86; record its identity and files in .retoque/components.txt. The game installer makes no network requests.",
        "Instalar dlssg-sm86 do catálogo e registrar sua identidade e arquivos em .retoque/components.txt. O instalador do jogo não faz requisições de rede.",
    ),
    (
        "sm86.consent_title",
        "Download the official frame generation components",
        "Baixar os componentes oficiais de geração de quadros",
    ),
    (
        "sm86.check_title",
        "Check for a newer version",
        "Verificar se há versão mais nova",
    ),
    (
        "sm86.check_consent",
        "Ask GitHub for the latest version of the frame generation components and compare it with the one you have?",
        "Perguntar ao GitHub qual é a versão mais recente dos componentes de geração de quadros e comparar com a que você tem?",
    ),
    (
        "sm86.check_details",
        "Host: api.github.com (sdli1995/dlssg_for_sm86, latest release). Nothing is downloaded.",
        "Host: api.github.com (sdli1995/dlssg_for_sm86, versão mais recente). Nada é baixado.",
    ),
    (
        "sm86.download_consent",
        "Retoque downloads dlssg_for_sm86 from its official release and checks it. The files stay in the app until you confirm a game's review. No telemetry.",
        "O Retoque baixa o dlssg_for_sm86 da versão oficial e o verifica. Os arquivos ficam no app até você confirmar a revisão de um jogo. Sem telemetria.",
    ),
    (
        "sm86.download_details",
        "Hosts: api.github.com and github.com (sdli1995/dlssg_for_sm86). Checked against the release's SHA-256. At most 256 MB to download and 1 GB unpacked.",
        "Hosts: api.github.com e github.com (sdli1995/dlssg_for_sm86). Conferido com o SHA-256 da versão. No máximo 256 MB de download e 1 GB descompactado.",
    ),
    (
        "sm86.kit_available",
        "This installation includes a first-run cache. It will be verified and imported locally, without downloading.",
        "Esta instalação inclui um cache inicial. Ele será verificado e importado localmente, sem download.",
    ),
    (
        "sm86.license",
        "Third-party runtimes and kernels retain their own terms; the complete package is not MIT. License texts and notices are preserved in the app cache.",
        "Runtimes e kernels de terceiros mantêm seus próprios termos; o pacote completo não é MIT. Licenças e avisos são preservados no cache do app.",
    ),
    ("sm86.import", "Import", "Importar"),
    (
        "sm86.download_help",
        "Download the official release once and reuse it across games. Game installation always has a separate review.",
        "Baixe a versão oficial uma vez e use-a em todos os jogos. A instalação no jogo sempre tem uma revisão separada.",
    ),
    ("sm86.download", "Download official release…", "Baixar versão oficial…"),
    ("sm86.check", "Check for update…", "Verificar atualização…"),
    (
        "sm86.progress",
        "Preparing official components",
        "Preparando componentes oficiais",
    ),
    (
        "sm86.current",
        "This release is available in your catalog.",
        "Esta versão está disponível no seu catálogo.",
    ),
    (
        "sm86.new",
        "This release is not yet available in your catalog.",
        "Esta versão ainda não está disponível no seu catálogo.",
    ),
    (
        "sm86.ignored",
        "Archive entries ignored (not extracted)",
        "Entradas do arquivo ignoradas (não extraídas)",
    ),
    ("optimize.review", "Review optimization…", "Revisar otimização…"),
    (
        "optimize.experimental",
        "Include experimental sm86 under Proton/Wine in this review",
        "Incluir sm86 experimental em Proton/Wine nesta revisão",
    ),
    (
        "optimize.choose_lsfg",
        "Include LSFG in this review",
        "Incluir LSFG nesta revisão",
    ),
    (
        "optimize.lsfg_unavailable",
        "Lossless Scaling was not found, so LSFG shaders cannot be prepared. Choose its folder in Components › Frame generation. No changes were applied.",
        "O Lossless Scaling não foi encontrado, então os shaders do LSFG não podem ser preparados. Escolha a pasta dele em Componentes › Geração de quadros. Nenhuma alteração foi aplicada.",
    ),
    (
        "optimize.lsfg_preparing",
        "Preparing the LSFG shaders first (app data only). The review opens when they are ready.",
        "Preparando os shaders do LSFG primeiro (só nos dados do app). A revisão abre quando estiverem prontos.",
    ),
    (
        "optimize.lsfg_failed",
        "LSFG shader preparation failed; see Components › Frame generation. No changes were applied.",
        "A preparação dos shaders do LSFG falhou; veja Componentes › Geração de quadros. Nenhuma alteração foi aplicada.",
    ),
    (
        "optimize.experimental_omitted",
        "Experimental sm86 was left out. Select it explicitly above to include it in a review.",
        "O sm86 experimental ficou de fora. Selecione-o explicitamente acima para incluí-lo em uma revisão.",
    ),
    (
        "optimize.up_to_date",
        "{game} already has the recommended settings for your graphics card.",
        "{game} já está com os ajustes recomendados para sua placa de vídeo.",
    ),
    (
        "optimize.install_first",
        "Install or adopt Retoque before optimizing.",
        "Instale ou adote o Retoque antes de otimizar.",
    ),
    (
        "optimize.identity_missing",
        "The installed component identity is unavailable. Refresh the game before reviewing.",
        "A identidade do componente instalado está indisponível. Atualize o jogo antes de revisar.",
    ),
    (
        "optimize.conflict",
        "The proposed settings conflict. No changes were applied.",
        "As configurações propostas entram em conflito. Nenhuma alteração foi aplicada.",
    ),
    (
        "remedy.processes",
        "Show running processes",
        "Ver processos em execução",
    ),
    (
        "remedy.close_processes",
        "Close these programs, then try again",
        "Feche estes programas e tente de novo",
    ),
    ("remedy.retry", "Try again", "Tentar de novo"),
    ("remedy.replan", "Prepare a fresh review", "Preparar uma nova revisão"),
    (
        "remedy.foreign",
        "Choose keep or replace…",
        "Escolher manter ou substituir…",
    ),
    (
        "mdl.incompatible",
        "Does not work on this graphics card",
        "Não funciona nesta placa de vídeo",
    ),
    (
        "mdl.arch_match",
        "Compatible with your graphics card",
        "Compatível com sua placa de vídeo",
    ),
    (
        "mdl.compat_unknown",
        "Compatibility not confirmed",
        "Compatibilidade não confirmada",
    ),
    ("mdl.archs", "CUDA targets found:", "Alvos CUDA encontrados:"),
    (
        "mdl.suggest",
        "One of your models works on this card:",
        "Um dos seus modelos funciona nesta placa:",
    ),
    ("mdl.use_suggested", "Review model change…", "Revisar troca de modelo…"),
    ("ver.unknown", "unknown", "desconhecida"),
    (
        "ver.mismatch",
        "OptiScaler and the NR component have different file versions. Update them together; installation is still allowed.",
        "OptiScaler e o componente NR têm versões de arquivo diferentes. Atualize os dois juntos; a instalação continua permitida.",
    ),
    ("ver.forwarder", "NR component {v}", "Componente NR {v}"),
    ("welcome.title", "Your games, with Retoque", "Seus jogos, com Retoque"),
    ("welcome.installed", "with Retoque", "com Retoque"),
    ("set.nr", "Neural Rendering (NR)", "Renderização neural (NR)"),
    ("set.upscalers", "Upscalers", "Ampliadores"),
    ("set.framegen", "Frame generation", "Geração de quadros"),
    ("set.lsfg", "LSFG (experimental)", "LSFG (experimental)"),
    (
        "set.no_match",
        "No setting matches this filter.",
        "Nenhum ajuste corresponde a este filtro.",
    ),
    (
        "set.lsfg_note",
        "Experimental. Needs FSR frame generation and your own Lossless Scaling files; features that are not available cannot run.",
        "Experimental. Precisa da geração de quadros FSR e dos seus próprios arquivos do Lossless Scaling; recursos indisponíveis não funcionam.",
    ),
    ("mdl.version", "Version", "Versão"),
    ("mdl.dll_filter", "NR model (.dll)", "Modelo NR (.dll)"),
    ("unit.ms", "ms", "ms"),
    ("unknown", "—", "—"),
    (
        "log.route_unknown",
        "No route reported yet.",
        "Nenhuma rota informada ainda.",
    ),
    (
        "log.evidence_note",
        "Initialization records describe this session; they do not prove NR is still running. GPU time includes confirmed samples only.",
        "O início registrado vale para esta sessão; não prova que o NR continua ativo. O tempo de GPU considera apenas amostras confirmadas.",
    ),
    ("log.showing", "Showing", "Exibindo"),
    (
        "log.limited",
        "The summary is limited; the log file contains the full record.",
        "O resumo tem limites; consulte o arquivo de log para ver o registro completo.",
    ),
    ("inst.advanced", "Advanced", "Avançado"),
    ("st.build", "Local build", "Compilação local"),
    ("st.release", "Version {v}", "Versão {v}"),
    ("st.build_hash", "Build {hash}", "Compilação {hash}"),
    ("inst.notes()", "Notes", "Observações"),
    ("dismiss.help", "Dismiss message", "Fechar mensagem"),
    ("brand", "Retoque", "Retoque"),
    ("lang.ja", "Japanese", "Japonês"),
    ("lang.ko", "Korean", "Coreano"),
    ("lang.zh_cn", "Chinese (Simplified)", "Chinês (simplificado)"),
    ("step.copy", "Copy", "Copiar"),
    ("step.backup", "Back up", "Salvar cópia"),
    ("step.rename", "Rename", "Renomear"),
    ("step.ini", "Set", "Ajustar"),
    ("step.remove", "Remove", "Remover"),
    ("step.restore", "Restore", "Restaurar"),
    ("step.keep", "Keep", "Preservar"),
    ("value.unset", "unset", "não definido"),
    ("ref.unknown_file", "unknown file", "arquivo desconhecido"),
    ("ref.reshade", "ReShade", "ReShade"),
    ("store.steam", "Steam", "Steam"),
    ("store.heroic", "Heroic", "Heroic"),
    ("store.lutris", "Lutris", "Lutris"),
    ("store.epic", "Epic", "Epic"),
    ("store.gog", "GOG", "GOG"),
    ("store.manual", "Manual", "Adicionado manualmente"),
    ("runtime.windows", "Windows", "Windows"),
    ("runtime.proton", "Proton", "Proton"),
    ("runtime.wine", "Wine", "Wine"),
    ("runtime.linux", "Linux", "Linux"),
    (
        "error.io",
        "Could not read or write {file}.",
        "Não foi possível ler ou gravar {file}.",
    ),
    (
        "error.io_any",
        "Could not read or write a file.",
        "Não foi possível ler ou gravar um arquivo.",
    ),
    ("technical.details", "Technical details", "Detalhes técnicos"),
    (
        "note.resume",
        "The last deploy here stopped part-way; its records are kept and this one carries on from them",
        "A instalação anterior foi interrompida. Seus registros foram preservados e esta instalação continua de onde ela parou.",
    ),
    (
        "note.driver",
        "NVIDIA Wine _nvngx.dll not found; install your driver Wine NGX component or supply it beside the game.",
        "O _nvngx.dll do driver NVIDIA para Wine não foi encontrado. Instale o componente Wine NGX do driver ou coloque o arquivo junto do jogo.",
    ),
    (
        "note.model",
        "Supply your own nvngx_dlssnr.dll beside the game (or name another copy with [DlssNr] ModelFile); the model is not included.",
        "Coloque seu próprio nvngx_dlssnr.dll junto do jogo ou indique outra cópia em [DlssNr] ModelFile. O modelo não está incluído.",
    ),
    (
        "note.vk_skip",
        "Vulkan hooks: skipped via marker file",
        "Hooks Vulkan: desativados pelo arquivo marcador",
    ),
    (
        "note.vk_disabled",
        "Vulkan hooks: ACTIVE (marker present but disabled)",
        "Hooks Vulkan: ATIVOS (o marcador existe, mas está desativado)",
    ),
    (
        "note.vk_active",
        "Vulkan hooks: ACTIVE (no marker); set [Vulkan] SkipHooks=true if the device is lost",
        "Hooks Vulkan: ATIVOS (sem marcador). Se ocorrer perda do dispositivo, use [Vulkan] SkipHooks=true.",
    ),
    (
        "keep.same",
        "already this build, left as it is",
        "já corresponde a esta compilação; mantido como está",
    ),
    (
        "keep.model",
        "NVIDIA's model, never removed; left in place",
        "modelo da NVIDIA; preservado, nunca é removido",
    ),
    ("keep.gone", "already gone", "já foi removido"),
    (
        "keep.identical_component",
        "identical component file",
        "idêntico ao arquivo do componente",
    ),
    (
        "keep.user_settings",
        "your own settings file, kept as it is",
        "seu arquivo de configuração, mantido como está",
    ),
    (
        "keep.changed",
        "left in place, it changed since this script wrote it",
        "preservado porque foi alterado após a instalação",
    ),
    (
        "keep.no_backup",
        "not restored (model or missing backup)",
        "não restaurado: é um modelo ou a cópia de segurança não existe",
    ),
    (
        "keep.no_rename",
        "not renamed (model or already gone)",
        "não renomeado: é um modelo ou já foi removido",
    ),
    (
        "keep.rename_changed",
        "not renamed back, it changed since",
        "nome original não restaurado porque o arquivo foi alterado",
    ),
    (
        "ref.ini_missing",
        "OptiScaler.ini is missing; install or supply it before editing settings",
        "O OptiScaler.ini não existe. Instale o Retoque ou forneça o arquivo antes de editar os ajustes.",
    ),
    (
        "ref.ini_value",
        "INI edits require single-line section, key and value without structural separators",
        "A seção, a chave e o valor do INI devem ocupar uma única linha, sem separadores estruturais.",
    ),
    (
        "note.windows_denied",
        "Windows denies write access to {path}; apply will refuse until permissions are corrected",
        "O Windows negou acesso de gravação a {path}. Corrija as permissões para continuar.",
    ),
    (
        "note.second_proxy",
        "{path} also holds an OptiScaler build; deploying anyway (--allow-second-proxy)",
        "{path} também contém o OptiScaler. A instalação continuará com --allow-second-proxy.",
    ),
    (
        "keep.destination",
        "{path}: not restored, a file is still there",
        "{path}: não restaurado porque já existe um arquivo no destino",
    ),
    (
        "keep.blocked",
        "not renamed back, {path} is in the way",
        "nome original não restaurado: {path} ocupa o destino",
    ),
    (
        "ref.proxy",
        "{path} is not a name OptiScaler answers to",
        "O OptiScaler não aceita {path} como nome do arquivo de carregamento",
    ),
    ("app.refresh", "Rescan library", "Buscar jogos novamente"),
    ("app.payload", "Retoque files", "Arquivos do Retoque"),
    (
        "app.payload.none",
        "not next to the app: choose the folder with OptiScaler.dll to install or update",
        "não estão ao lado do app: escolha a pasta com o OptiScaler.dll para instalar ou atualizar",
    ),
    ("app.payload.choose", "Choose…", "Escolher…"),
    ("payload.missing", "not found", "não encontrados"),
    (
        "payload.files_note",
        "The app copies these files into a game when you install or update Retoque there.",
        "O app copia estes arquivos para o jogo quando você instala ou atualiza o Retoque nele.",
    ),
    ("payload.files_source", "Where they come from", "De onde vêm"),
    ("payload.files_updates", "Official releases", "Versões oficiais"),
    ("lib.search", "Search games", "Buscar jogos"),
    ("lib.installed_only", "Installed only", "Só instalados"),
    ("lib.add_folder", "Add folder…", "Adicionar pasta…"),
    ("lib.scanning", "Looking for games…", "Procurando jogos…"),
    (
        "lib.empty",
        "No games found. Add a game folder by hand.",
        "Nenhum jogo encontrado. Adicione a pasta de um jogo à mão.",
    ),
    ("lib.skipped", "Could not read", "Não foi possível ler"),
    ("game.root", "game folder", "pasta do jogo"),
    ("game.target", "Executable folder", "Pasta do executável"),
    (
        "game.no_target",
        "No executable folder found in this game.",
        "Nenhuma pasta com executável encontrada neste jogo.",
    ),
    (
        "game.native",
        "Native Linux build: Retoque works only with the Windows version under Proton or Wine.",
        "Versão nativa de Linux: o Retoque só funciona com a versão de Windows no Proton ou no Wine.",
    ),
    ("tab.install", "Overview", "Visão geral"),
    ("tab.settings", "Settings", "Ajustes"),
    ("tab.model", "Components", "Componentes"),
    // The Components tab's sections, in the order a game uses them.
    ("comp.nr", "NR model", "Modelo NR"),
    (
        "comp.sm86_name",
        "Frame generation for RTX 20/30 · version {version}",
        "Geração de quadros para RTX 20/30 · versão {version}",
    ),
    ("comp.dlss", "DLSS", "DLSS"),
    ("comp.fg", "Frame generation", "Geração de quadros"),
    ("tab.compare", "Before / after", "Antes e depois"),
    ("tab.logs", "Logs", "Logs"),
    (
        "set.invalid",
        "OptiScaler does not accept this value; fix it to review the changes.",
        "O OptiScaler não aceita este valor; corrija para revisar as alterações.",
    ),
    ("st.installed", "Retoque installed", "Retoque instalado"),
    ("st.installed_short", "Installed", "Instalado"),
    ("st.current", "Retoque up to date", "Retoque atualizado"),
    ("st.current_short", "Up to date", "Atualizado"),
    ("st.update", "Update available", "Atualização disponível"),
    ("st.update_short", "Update", "Atualizar"),
    ("st.partial", "Incomplete install", "Instalação incompleta"),
    ("st.partial_short", "Incomplete", "Incompleta"),
    ("st.not_installed", "Not installed", "Sem Retoque"),
    ("st.untracked", "Old install", "Instalação antiga"),
    ("st.untracked_short", "Old", "Antiga"),
    ("st.nr_on", "NR on", "NR ligado"),
    ("st.nr_off", "NR off", "NR desligado"),
    ("inst.proxy", "Loader file name", "Nome do arquivo de carregamento"),
    (
        "inst.proxy_help",
        "Change only if the game does not start with the default.",
        "Mude só se o jogo não abrir com o padrão.",
    ),
    ("inst.menu_language", "In-game menu language", "Idioma do menu no jogo"),
    (
        "inst.enable_nr",
        "Turn Neural Rendering (NR) on",
        "Ligar a renderização neural (NR)",
    ),
    (
        "inst.keep_reshade",
        "Keep ReShade (load it through OptiScaler)",
        "Manter o ReShade (carregado pelo OptiScaler)",
    ),
    (
        "inst.replace_foreign",
        "Replace a file that is not ours (kept as a backup)",
        "Substituir um arquivo que não é nosso (fica uma cópia)",
    ),
    (
        "inst.second_proxy",
        "Allow a second OptiScaler loader file",
        "Permitir um segundo arquivo de carregamento do OptiScaler",
    ),
    (
        "inst.copy_nvngx",
        "Copy the driver's _nvngx.dll for Wine",
        "Copiar o _nvngx.dll do driver para o Wine",
    ),
    ("inst.preview", "Preview install", "Ver o que a instalação faz"),
    ("inst.preview_uninstall", "Preview uninstall", "Ver o que a remoção faz"),
    ("inst.install", "Install", "Instalar"),
    ("inst.update", "Update", "Atualizar"),
    ("inst.uninstall", "Uninstall", "Desinstalar"),
    ("inst.plan", "What will be done", "O que será feito"),
    (
        "inst.plan_title",
        "What Retoque will do in this game",
        "O que o Retoque vai fazer neste jogo",
    ),
    (
        "inst.plan_install",
        "Install Retoque files in this game",
        "Instalar os arquivos do Retoque neste jogo",
    ),
    ("inst.nothing", "Nothing to change.", "Nada a mudar."),
    ("inst.working", "Working…", "Trabalhando…"),
    (
        "inst.failed_install",
        "The install did not finish",
        "A instalação não foi concluída",
    ),
    (
        "inst.failed_uninstall",
        "The uninstall did not finish",
        "A desinstalação não foi concluída",
    ),
    (
        "inst.failed_settings",
        "The settings were not saved",
        "Os ajustes não foram salvos",
    ),
    (
        "inst.launch_help",
        "Paste this in the game's Properties › Launch options.",
        "Cole isto em Propriedades › Opções de inicialização do jogo.",
    ),
    (
        "inst.launch_help_heroic",
        "In Heroic, open the game's settings › Advanced › Environment variables and add WINEDLLOVERRIDES with this value.",
        "No Heroic, abra as configurações do jogo › Avançado › Variáveis de ambiente e adicione WINEDLLOVERRIDES com este valor.",
    ),
    (
        "inst.launch_help_lutris",
        "In Lutris, open the game's Configure › System options › Environment variables and add WINEDLLOVERRIDES with this value.",
        "No Lutris, abra Configurar › Opções do sistema › Variáveis de ambiente do jogo e adicione WINEDLLOVERRIDES com este valor.",
    ),
    (
        "inst.launch_help_other",
        "The game must load Retoque when it starts: put this in your launcher's launch options, before the command that starts the game.",
        "O jogo precisa carregar o Retoque ao abrir: coloque isto nas opções de inicialização do seu iniciador, antes do comando que abre o jogo.",
    ),
    ("inst.launch_label", "Launch option", "Opção de inicialização"),
    ("inst.copy", "Copy", "Copiar"),
    ("inst.copied", "Copied", "Copiado"),
    (
        "ref.running",
        "The game is running. Close it and try again:",
        "O jogo está aberto. Feche-o e tente de novo:",
    ),
    (
        "ref.foreign",
        "A file with the same name was not placed by Retoque:",
        "Um arquivo com o mesmo nome não foi colocado pelo Retoque:",
    ),
    (
        "ref.second",
        "Another file already loads OptiScaler in this game:",
        "Outro arquivo já carrega o OptiScaler neste jogo:",
    ),
    (
        "ref.receipt",
        "The install record cannot be used:",
        "O registro de instalação não pode ser usado:",
    ),
    ("ref.usage", "Cannot do this:", "Não dá para fazer isto:"),
    ("set.save", "Review changes…", "Revisar alterações…"),
    ("mdl.current", "This game uses", "Modelo deste jogo"),
    (
        "mdl.missing_file",
        "This file does not exist: NR will be unavailable in this game.",
        "Este arquivo não existe: o NR ficará indisponível neste jogo.",
    ),
    ("mdl.catalog", "Your models", "Seus modelos"),
    ("mdl.import", "Import a model…", "Importar um modelo…"),
    ("mdl.use", "Use in this game…", "Usar neste jogo…"),
    ("mdl.in_use", "In use", "Em uso"),
    ("mdl.use_auto", "Use the default search…", "Usar a busca padrão…"),
    ("mdl.remove", "Delete from app…", "Apagar do app…"),
    (
        "mdl.remove_title",
        "Delete this model from the app?",
        "Apagar este modelo do app?",
    ),
    (
        "mdl.empty",
        "No models yet. Import the nvngx_dlssnr.dll files you have; they are copied into the app's folder and never shared.",
        "Nenhum modelo ainda. Importe os nvngx_dlssnr.dll que você tem; eles são copiados para a pasta do app e nunca compartilhados.",
    ),
    (
        "log.none",
        "No OptiScaler log here yet: play the game once with Retoque installed.",
        "Ainda não há log do OptiScaler aqui: jogue uma vez com o Retoque instalado.",
    ),
    ("log.dlss", "DLSS", "DLSS"),
    ("log.api", "API", "API"),
    ("log.gpu", "NR GPU time", "Tempo de GPU do NR"),
    ("log.errors", "Errors", "Erros"),
    ("log.warnings", "Warnings", "Avisos"),
    ("log.lines", "Lines worth reading", "Linhas que importam"),
    ("log.bundle", "Make a diagnostic file", "Gerar arquivo de diagnóstico"),
    (
        "log.bundle_done",
        "Written (home folder and user name removed):",
        "Gravado (pasta pessoal e nome de usuário removidos):",
    ),
    ("log.open_folder", "Open folder", "Abrir pasta"),
    ("prefs.open", "Preferences…", "Preferências…"),
    ("prefs.title", "Preferences", "Preferências"),
    ("prefs.installs", "New installs", "Novas instalações"),
    (
        "prefs.installs_help",
        "Where a game without Retoque starts. A game that already has it keeps its own choices.",
        "O ponto de partida para um jogo sem Retoque. Um jogo que já tem o Retoque mantém as escolhas dele.",
    ),
    (
        "prefs.menu_lang_same",
        "Same as the app ({language})",
        "O mesmo do app ({language})",
    ),
    (
        "prefs.menu_lang_help",
        "Applies to every game at its next install, update or Optimize. A language file of your own is left as it is. The translations are machine-made drafts.",
        "Vale para cada jogo na próxima instalação, atualização ou otimização. Um arquivo de idioma seu fica como está. As traduções são rascunhos feitos por máquina.",
    ),
    ("prefs.language", "Language", "Idioma"),
    (
        "prefs.language_help",
        "The app's text changes as soon as you choose.",
        "O texto do app muda assim que você escolhe.",
    ),
    (
        "prefs.language_draft",
        "This translation is a machine-made draft that a native speaker has not reviewed yet. Some wording may be off.",
        "Esta tradução é um rascunho automático que ainda não foi revisado por um falante nativo. Algumas frases podem soar estranhas.",
    ),
    (
        "prefs.language_no_font",
        "{languages}: this computer has no font with Chinese, Japanese and Korean characters (such as Noto Sans CJK). Install one and open Retoque again.",
        "{languages}: este computador não tem uma fonte com caracteres chineses, japoneses e coreanos (como a Noto Sans CJK). Instale uma e abra o Retoque de novo.",
    ),
    ("app_install.heading", "Retoque app", "App do Retoque"),
    (
        "app_install.install",
        "Install Retoque on this computer…",
        "Instalar o Retoque neste computador…",
    ),
    (
        "app_install.install_title",
        "Install Retoque on this computer",
        "Instalar o Retoque neste computador",
    ),
    ("app_install.install_confirm", "Install Retoque", "Instalar o Retoque"),
    (
        "app_install.install_body",
        "Create this program folder and copy the files listed below into it:",
        "Criar esta pasta do programa e copiar para ela os arquivos abaixo:",
    ),
    (
        "app_install.files",
        "Files (including their folders)",
        "Arquivos (incluindo suas pastas)",
    ),
    (
        "app_install.shortcuts",
        "Create Start Menu and Desktop shortcuts:",
        "Criar atalhos no menu Iniciar e na Área de Trabalho:",
    ),
    (
        "app_install.registration",
        "Register Retoque in Windows Apps & features for this user.",
        "Registrar o Retoque em Aplicativos e recursos do Windows para este usuário.",
    ),
    (
        "app_install.preserve",
        "Your app data and Retoque in your games are kept.",
        "Os dados do app e o Retoque nos seus jogos são preservados.",
    ),
    (
        "app_install.installed",
        "Retoque installed. Restart to use the installed copy.",
        "Retoque instalado. Reinicie para usar a cópia instalada.",
    ),
    (
        "app_install.check",
        "Check for app updates…",
        "Buscar atualizações do app…",
    ),
    (
        "app_install.check_title",
        "Check for app updates",
        "Buscar atualizações do app",
    ),
    (
        "app_install.check_body",
        "Contact our official GitHub releases to check for a newer Windows app? Nothing is downloaded at startup.",
        "Consultar nossas versões oficiais no GitHub para buscar um app Windows mais recente? Nada é baixado ao iniciar.",
    ),
    (
        "app_install.available",
        "App version {version} is available.",
        "A versão {version} do app está disponível.",
    ),
    ("app_install.update", "Update the app…", "Atualizar o app…"),
    ("app_install.update_title", "Update the app", "Atualizar o app"),
    (
        "app_install.download_body",
        "Download and verify the new app and its Retoque files from GitHub. You will review restarting before replacing the installed files.",
        "Baixar e verificar o novo app e seus arquivos do Retoque pelo GitHub. Você revisará o reinício antes da substituição dos arquivos instalados.",
    ),
    (
        "app_install.current",
        "The app is up to date.",
        "O app está atualizado.",
    ),
    (
        "app_install.no_release",
        "No Windows app release has been published yet.",
        "Nenhuma versão do app Windows foi publicada ainda.",
    ),
    (
        "app_install.ready",
        "App update downloaded and verified. Restart when you are ready.",
        "Atualização do app baixada e verificada. Reinicie quando quiser.",
    ),
    ("app_install.restart_offer", "Restart the app…", "Reiniciar o app…"),
    ("app_install.restart_title", "Restart the app", "Reiniciar o app"),
    ("app_install.restart_confirm", "Restart the app", "Reiniciar o app"),
    (
        "app_install.restart_body",
        "Restart the app to replace its installed files with the verified update. The previous app is kept until the new copy starts.",
        "Reiniciar o app para substituir seus arquivos instalados pela atualização verificada. O app anterior é mantido até a nova cópia iniciar.",
    ),
    (
        "app_install.busy",
        "Working on the app installation",
        "Preparando a instalação do app",
    ),
    (
        "app_install.uninstall_body",
        "Uninstall the Retoque app, its program folder, shortcuts and Apps & features entry? Your app data and Retoque in your games will be kept.",
        "Desinstalar o app do Retoque, sua pasta do programa, atalhos e registro em Aplicativos e recursos? Os dados do app e o Retoque nos seus jogos serão preservados.",
    ),
    (
        "app_install.failed",
        "The app could not be installed, updated or uninstalled. Close other Retoque windows and try again. Details:",
        "Não foi possível instalar, atualizar ou desinstalar o app. Feche outras janelas do Retoque e tente novamente. Detalhes:",
    ),
    ("prefs.data", "App data", "Dados do app"),
    ("prefs.data_usage", "{size} in this folder", "{size} nesta pasta"),
    ("prefs.data_measuring", "Measuring the folder…", "Medindo a pasta…"),
    (
        "prefs.clean",
        "Leftovers of interrupted downloads and imports are cleared on their own after a day.",
        "Sobras de downloads e importações interrompidos são apagadas sozinhas depois de um dia.",
    ),
    (
        "prefs.cleaned",
        "Cleared at startup: {n} leftover(s) of interrupted operations ({size}).",
        "Apagado ao abrir: {n} sobra(s) de operações interrompidas ({size}).",
    ),
    (
        "app.wait_write",
        "A write is still running: wait for it to finish before closing.",
        "Uma gravação ainda está em andamento: espere terminar antes de fechar.",
    ),
    (
        "inst.stale",
        "The options or the Retoque files changed after this preview: preview again.",
        "As opções ou os arquivos do Retoque mudaram depois desta prévia: veja de novo.",
    ),
    ("set.apply", "Apply", "Aplicar"),
    (
        "mdl.remove_help",
        "Deletes the copy the app keeps. A game that names this file loses NR until it gets another model.",
        "Apaga a cópia guardada no app. Um jogo que aponta para este arquivo fica sem NR até receber outro modelo.",
    ),
    (
        "app.tagline",
        "Neural Rendering for your games",
        "Renderização neural nos seus jogos",
    ),
    ("lib.group_installed", "With Retoque", "Com Retoque"),
    ("lib.group_other", "Other games", "Outros jogos"),
    ("card.install_options", "Install options", "Opções de instalação"),
    ("card.reinstall_options", "Reinstall options", "Opções de reinstalação"),
    (
        "left.title",
        "Left behind by uninstalled games",
        "Sobras de jogos desinstalados",
    ),
    (
        "left.why",
        "The store removed these games but not the files Retoque added to their folders.",
        "A loja removeu esses jogos, mas não os arquivos que o Retoque colocou nas pastas deles.",
    ),
    ("left.summary", "{count} folders · {size}", "{count} pastas · {size}"),
    ("left.review", "Review cleanup…", "Revisar limpeza…"),
    ("left.removing", "Removing…", "Removendo…"),
    (
        "left.review_title",
        "Clean up what uninstalled games left",
        "Limpar sobras de jogos desinstalados",
    ),
    (
        "left.review_note",
        "Only files Retoque, OptiScaler, sm86, ReShade or their caches put there are removed. Anything else, such as a save, stays, and so does its folder. Nothing changes until you confirm.",
        "Só saem arquivos que o Retoque, o OptiScaler, o sm86, o ReShade ou os caches deles colocaram ali. Qualquer outra coisa, como um jogo salvo, fica, e a pasta dela também. Nada muda antes da sua confirmação.",
    ),
    (
        "left.folder_line",
        "{files} files · {size}",
        "{files} arquivos · {size}",
    ),
    (
        "left.keeps",
        "Keeps {count} file(s) that are not Retoque's",
        "Mantém {count} arquivo(s) que não são do Retoque",
    ),
    ("left.confirm", "Remove leftovers ({size})", "Remover sobras ({size})"),
    (
        "left.done",
        "Leftovers removed: {size} freed",
        "Sobras removidas: {size} liberados",
    ),
    ("left.failed", "Cleanup stopped", "Limpeza interrompida"),
    ("rs.ready", "Ready to install:", "Pronto para instalar:"),
    ("rs.checking", "Checking ReShade…", "Verificando o ReShade…"),
    ("rs.consent_title", "Download ReShade 6.8.0", "Baixar o ReShade 6.8.0"),
    (
        "rs.consent_body",
        "Retoque downloads the official ReShade installer with add-on support from reshade.me and keeps only its DLL. Your game changes only after a separate review.",
        "O Retoque baixa o instalador oficial do ReShade com suporte a add-ons de reshade.me e guarda só a DLL dele. Seu jogo só muda depois de outra revisão.",
    ),
    (
        "rs.consent_details",
        "Host: reshade.me. The installer and the DLL taken from it have fixed SHA-256 hashes. License: BSD-3-Clause.",
        "Host: reshade.me. O instalador e a DLL tirada dele têm hashes SHA-256 fixos. Licença: BSD-3-Clause.",
    ),
    ("sess.title", "Last session", "Última sessão"),
    ("sess.driver", "driver", "driver"),
    ("sess.base", "Real frames", "Quadros reais"),
    ("sess.low", "Worst 5%", "Pior 5%"),
    ("sess.screen", "On screen", "Na tela"),
    ("sess.generation", "Frame generation", "Geração de quadros"),
    ("sess.generation_value", "x{x} · {percent}%", "x{x} · {percent}%"),
    ("sess.drops", "Drops", "Quedas"),
    ("sess.median", "median", "mediana"),
    (
        "sess.curve",
        "Real frames per second over the session",
        "Quadros reais por segundo ao longo da sessão",
    ),
    (
        "sess.short",
        "This session was too short to judge. Play a few minutes and come back.",
        "Sessão curta demais para avaliar. Jogue alguns minutos e volte aqui.",
    ),
    (
        "sess.steady",
        "Steady session: nothing to change.",
        "Sessão estável: nada a mudar.",
    ),
    (
        "sess.fake_nvapi",
        "This Retoque gave the game a stand-in NVAPI, so DLSS frame generation can disappear from the game's menu. Update Retoque in this game.",
        "Este Retoque entregou ao jogo um NVAPI substituto, e a geração de quadros DLSS pode sumir do menu do jogo. Atualize o Retoque neste jogo.",
    ),
    (
        "sess.low_base",
        "Frame generation multiplied only {base} real frames per second. Below about 40 it feels heavy and stutters: choose DLSS Balanced or Performance in the game.",
        "A geração de quadros multiplicou só {base} quadros reais por segundo. Abaixo de uns 40 ela fica pesada e engasga: escolha DLSS Balanceado ou Desempenho no jogo.",
    ),
    (
        "sess.low_base_x4",
        "x3 instead of x4 also feels steadier on a low base.",
        "x3 em vez de x4 também fica mais estável com a base baixa.",
    ),
    ("sess.use_x3", "Use x3…", "Usar x3…"),
    ("sess.use_x3_done", "Now using x3", "Agora usando x3"),
    (
        "sess.drops_advice",
        "The real frame rate dropped more than 20% in {percent}% of the session. It is usually the game loading new areas or too little free RAM: close other programs and lower texture quality.",
        "Os quadros reais caíram mais de 20% em {percent}% da sessão. Costuma ser o jogo carregando áreas novas ou pouca RAM livre: feche outros programas e baixe a qualidade das texturas.",
    ),
    (
        "sess.nr_off",
        "Neural Rendering was switched off during the session (its key in the game).",
        "A renderização neural (NR) foi desligada durante a sessão (pela tecla dela no jogo).",
    ),
    (
        "card.launch",
        "Last step: Steam launch option",
        "Último passo: opção de inicialização da Steam",
    ),
    (
        "card.launch_heroic",
        "Last step: environment variable in Heroic",
        "Último passo: variável de ambiente no Heroic",
    ),
    (
        "card.launch_lutris",
        "Last step: Lutris option",
        "Último passo: opção do Lutris",
    ),
    (
        "card.launch_other",
        "Last step: launch option",
        "Último passo: opção de inicialização",
    ),
    ("cancel", "Cancel", "Cancelar"),
    // Shared words of the design system (widgets::busy, consent_modal and info dialogs).
    ("progress.of", "of", "de"),
    ("not_now", "Not now", "Agora não"),
    ("close", "Close", "Fechar"),
    ("delete", "Delete", "Apagar"),
    ("download", "Download", "Baixar"),
    ("check_now", "Check", "Verificar"),
    ("size.up_to", "up to {size}", "até {size}"),
    // The top bar's spinner: what is running, in the user's words.
    (
        "busy.writing",
        "Applying changes to a game…",
        "Aplicando alterações em um jogo…",
    ),
    (
        "busy.reading",
        "Reading game information…",
        "Lendo informações dos jogos…",
    ),
    // Before / after viewer (compare.rs).
    ("canvas", "Comparison image", "Imagem de comparação"),
    ("captures", "Captures", "Capturas"),
    ("refresh", "Refresh", "Atualizar"),
    ("loading", "Loading capture…", "Carregando captura…"),
    ("scanning", "Looking for captures…", "Buscando capturas…"),
    (
        "scan_error",
        "Could not read the capture folder. Check that it exists and is readable, then refresh.",
        "Não foi possível ler a pasta de capturas. Confira se ela existe e se há permissão de leitura, depois atualize.",
    ),
    (
        "load_error",
        "Could not load this pair. A file may be incomplete, unsupported or have different dimensions. Refresh to retry.",
        "Não foi possível abrir este par. Um arquivo pode estar incompleto, ter formato incompatível ou dimensões diferentes. Atualize para tentar de novo.",
    ),
    ("wipe", "Wipe", "Divisor"),
    ("side", "Side by side", "Lado a lado"),
    ("flip", "Flip", "Alternar"),
    ("diff", "Difference", "Diferença"),
    ("fit", "Fit", "Ajustar à janela"),
    ("one", "1:1", "1:1"),
    ("original", "Original", "Original"),
    ("processed", "Retoque NR", "Retoque NR"),
    ("hold", "Hold to see original", "Segure para ver o original"),
    ("stats", "Display preview", "Prévia de exibição"),
    ("psnr", "PSNR", "PSNR"),
    ("changed", "Changed >5%", "Variação >5%"),
    ("mad", "Mean difference", "Diferença média"),
    (
        "stats_help",
        "RGB PSNR and mean absolute difference use 8-bit display values; changed pixels have an absolute luma-code difference above 5%. These are not raw HDR metrics or a quality score.",
        "PSNR RGB e diferença média absoluta usam os valores de exibição de 8 bits; pixels alterados têm diferença absoluta de luma acima de 5%. Não são métricas do HDR bruto nem uma nota de qualidade.",
    ),
    (
        "hdr",
        "HDR to SDR preview · ACES + sRGB · shared exposure",
        "Prévia HDR em SDR · ACES + sRGB · exposição compartilhada",
    ),
    (
        "sdr",
        "SDR capture · original display values",
        "Captura SDR · valores originais de exibição",
    ),
    (
        "hdr_note",
        "The game's own tonemapping and post-processing are not reproduced here.",
        "O tonemapping e o pós-processamento do jogo não são reproduzidos aqui.",
    ),
    (
        "diff_key",
        "Heatmap · fixed 8× gain · black = equal, blue, yellow, red = increasing difference",
        "Mapa de diferenças · ganho fixo de 8× · preto = igual, azul, amarelo, vermelho = diferença crescente",
    ),
    (
        "no_provenance",
        "No sidecar or embedded provenance found.",
        "Nenhum metadado encontrado no JSON ou na imagem.",
    ),
    ("model", "Model", "Modelo"),
    ("route", "Route", "Rota"),
    ("working", "Working size", "Resolução de processamento"),
    ("settings", "Settings", "Ajustes"),
    ("cmp.unknown", "Not recorded", "Não registrado"),
    ("loaded", "Loaded in", "Carregado em"),
    ("exposure", "Exposure", "Exposição"),
    ("exposure_reset", "Reset exposure", "Redefinir exposição"),
    (
        "exposure_help",
        "0 EV uses the automatic or recorded exposure. Each stop doubles or halves the light, equally on both images. The preview updates when you release the slider.",
        "0 EV usa a exposição automática ou registrada. Cada ponto dobra ou reduz pela metade a luz nas duas imagens. A prévia atualiza ao soltar o controle.",
    ),
    ("exposure_updating", "Updating preview…", "Atualizando prévia…"),
    ("thumbnail_error", "Thumbnail unavailable", "Miniatura indisponível"),
    ("shell.uninstall_open", "Uninstall…", "Desinstalar…"),
    ("shell.discard", "Discard", "Descartar"),
    (
        "shell.restart_hint",
        "Restart the game to use the changes.",
        "Reinicie o jogo para usar as alterações.",
    ),
    (
        "shell.library_checking",
        "Checking installed versions…",
        "Conferindo as versões instaladas…",
    ),
    ("shell.model_version", "model", "modelo"),
    (
        "shell.model_unknown",
        "Model found · version unknown",
        "Modelo encontrado · versão não identificada",
    ),
    (
        "shell.review_shortcut",
        "Review changes (Ctrl+Enter); nothing is written until confirmation.",
        "Revisar alterações (Ctrl+Enter); nada será gravado antes da confirmação.",
    ),
    ("shell.progress_install", "Installing…", "Instalando…"),
    ("shell.progress_update", "Updating…", "Atualizando…"),
    ("shell.progress_remove", "Removing…", "Removendo…"),
    ("shell.progress_save", "Saving…", "Salvando…"),
    // UX shell / worker A
    ("shell.home", "Back to the library", "Voltar à biblioteca"),
    ("shell.library_show", "Show game list", "Mostrar lista de jogos"),
    ("shell.library_hide", "Hide game list", "Ocultar lista de jogos"),
    ("shell.back_library", "‹ Library", "‹ Biblioteca"),
    ("shell.details", "Details", "Detalhes"),
    ("shell.bundle_done", "Diagnostic file created", "Diagnóstico criado"),
    (
        "shell.failed",
        "The action could not be completed",
        "Não foi possível concluir",
    ),
    (
        "shell.catalog_changed",
        "Model catalog updated",
        "Catálogo de modelos atualizado",
    ),
    ("shell.checking", "Checking installation…", "Verificando instalação…"),
    (
        "shell.review_update",
        "Review Retoque update",
        "Revisar atualização do Retoque",
    ),
    (
        "shell.review_install",
        "Review Retoque installation",
        "Revisar instalação do Retoque",
    ),
    ("shell.review_remove", "Review uninstall", "Revisar desinstalação"),
    (
        "shell.review_note",
        "Nothing changes until you confirm.",
        "Nada será alterado antes da sua confirmação.",
    ),
    (
        "shell.undo_record",
        "Retoque's record for undoing this",
        "Registro do Retoque para desfazer",
    ),
    (
        "shell.resolve",
        "Resolve the issue above, close this review and try again.",
        "Resolva o impedimento acima, feche esta revisão e tente novamente.",
    ),
    ("shell.add", "Add", "Adicionar"),
    ("shell.replace", "Replace or restore", "Substituir ou restaurar"),
    ("shell.keep", "Keep", "Manter"),
    ("shell.remove", "Remove", "Remover"),
    ("shell.settings", "Settings", "Ajustes"),
    ("shell.games_one", "game", "jogo"),
    ("shell.games_other", "games", "jogos"),
    ("shell.updates_one", "update available", "atualização disponível"),
    ("shell.updates_other", "updates available", "atualizações disponíveis"),
    ("update_all.open", "Update all…", "Atualizar todos…"),
    ("update_all.title", "Update all", "Atualizar todos"),
    (
        "update_all.note",
        "Review each game. Your existing settings are kept. Games are updated one at a time.",
        "Revise cada jogo. Seus ajustes atuais serão mantidos. Os jogos serão atualizados um de cada vez.",
    ),
    (
        "update_all.confirm_one",
        "Update {count} game",
        "Atualizar {count} jogo",
    ),
    ("update_all.confirm", "Update {count} games", "Atualizar {count} jogos"),
    ("update_all.ready", "Will update Retoque", "O Retoque será atualizado"),
    ("update_all.skipped", "Skipped", "Ignorado"),
    (
        "update_all.running",
        "Game running. Close it and review again.",
        "Jogo em execução. Feche-o e revise novamente.",
    ),
    (
        "update_all.partial",
        "Incomplete install. Open this game to finish cleanup first.",
        "Instalação incompleta. Abra este jogo para concluir a limpeza primeiro.",
    ),
    (
        "update_all.foreign",
        "Unregistered or foreign install. Review it on the game page first.",
        "Instalação não registrada ou de terceiros. Revise-a na página do jogo primeiro.",
    ),
    (
        "update_all.unknown",
        "Status unknown. Refresh the game and review again.",
        "Estado desconhecido. Atualize o estado do jogo e revise novamente.",
    ),
    ("update_all.current", "Already up to date.", "Já está atualizado."),
    (
        "update_all.refused",
        "Update refused. Open the details and review this game separately.",
        "Atualização recusada. Abra os detalhes e revise este jogo separadamente.",
    ),
    (
        "update_all.overlap",
        "Another entry updates this folder. Review duplicate or overlapping folders separately.",
        "Outra entrada atualiza esta pasta. Revise pastas duplicadas ou sobrepostas separadamente.",
    ),
    (
        "update_all.busy",
        "Another change is in progress. Wait and review again.",
        "Outra alteração está em andamento. Aguarde e revise novamente.",
    ),
    (
        "update_all.progress",
        "Updating games · {done} of {total} finished",
        "Atualizando jogos · {done} de {total} concluídos",
    ),
    (
        "update_all.done",
        "Update finished: {updated} updated · {skipped} skipped · {failed} failed",
        "Atualização concluída: {updated} atualizados · {skipped} ignorados · {failed} falharam",
    ),
    (
        "shell.home_help",
        "Add a game folder or scan your launchers to get started.",
        "Adicione a pasta de um jogo ou busque os jogos dos seus launchers para começar.",
    ),
    ("shell.reinstall", "Reinstall…", "Reinstalar…"),
    ("shell.resume", "Finish cleanup…", "Concluir limpeza…"),
    ("shell.update", "Update Retoque…", "Atualizar o Retoque…"),
    ("shell.open_folder", "Open game folder", "Abrir pasta do jogo"),
    ("shell.pending", "unsaved changes", "alterações não salvas"),
    ("shell.preparing", "Preparing your review…", "Preparando a revisão…"),
    ("shell.installed_done", "Retoque installed", "Retoque instalado"),
    ("shell.updated_done", "Retoque updated", "Retoque atualizado"),
    ("shell.removed_done", "Retoque uninstalled", "Retoque desinstalado"),
    ("shell.settings_done", "Settings applied", "Ajustes aplicados"),
    // Worker B: settings, current model and session summary.
    ("set.advanced", "Advanced settings", "Ajustes avançados"),
    (
        "set.model_link",
        "Choose the NR model in Components",
        "Escolher modelo NR em Componentes",
    ),
    ("set.custom", "Custom value", "Valor personalizado"),
    ("set.automatic", "Automatic", "Automático"),
    (
        "set.passes_cost",
        "Cost: {n}× the model's GPU time and {n} model instances in video memory.",
        "Custo: {n}× o tempo de GPU do modelo e {n} instâncias do modelo na memória de vídeo.",
    ),
    (
        "set.passes_ampere",
        "On an RTX 30 card, higher counts may be refused by its video memory budget.",
        "Numa placa RTX 30, contagens maiores podem ser recusadas pelo limite de memória de vídeo dela.",
    ),
    (
        "tab.install_first",
        "Install Retoque to set up this game",
        "Instale o Retoque para configurar este jogo",
    ),
    ("tab.review_install", "Review installation…", "Revisar instalação…"),
    ("tab.loading", "Reading game settings…", "Lendo os ajustes do jogo…"),
    ("mdl.loading", "Reading model version…", "Lendo a versão do modelo…"),
    ("mdl.reading_catalog", "Reading your models…", "Lendo seus modelos…"),
    ("mdl.beside", "Found beside the game", "Encontrado na pasta do jogo"),
    ("mdl.from_catalog", "From your models", "Dos seus modelos"),
    ("mdl.from_path", "From the configured file", "Do arquivo configurado"),
    (
        "mdl.version_unknown",
        "Version not identified",
        "Versão não identificada",
    ),
    (
        "mdl.unresolved",
        "This path cannot be verified on this system. Check it in the game.",
        "Este caminho não pode ser verificado neste sistema. Confira no jogo.",
    ),
    (
        "mdl.auto_unresolved",
        "No model beside the executable. The default search may find one in another folder.",
        "Não há modelo junto do executável. A busca padrão pode encontrar um em outra pasta.",
    ),
    (
        "mdl.next_start",
        "This is the file selected for the next start, not evidence of what a running game loaded.",
        "Este é o arquivo selecionado para a próxima abertura, não uma confirmação do que o jogo aberto carregou.",
    ),
    (
        "mdl.catalog_empty",
        "You have no models yet",
        "Você ainda não tem modelos",
    ),
    (
        "mdl.optional",
        "This game already has a model. Importing another is optional.",
        "Este jogo já tem um modelo. Importar outro é opcional.",
    ),
    (
        "mdl.drop",
        "Drop an NR model (nvngx_dlssnr.dll) here, or choose a file",
        "Solte aqui um modelo NR (nvngx_dlssnr.dll) ou escolha um arquivo",
    ),
    (
        "mdl.import_review",
        "Import this file into your models? The game folder stays unchanged.",
        "Importar este arquivo para seus modelos? A pasta do jogo continua como está.",
    ),
    ("mdl.confirm_import", "Import model", "Importar modelo"),
    (
        "mdl.used_games_none",
        "No game uses this model",
        "Nenhum jogo usa este modelo",
    ),
    (
        "mdl.used_games_one",
        "1 game using this model",
        "1 jogo usando este modelo",
    ),
    (
        "mdl.used_games",
        "{n} games using this model",
        "{n} jogos usando este modelo",
    ),
    ("mdl.refresh", "Check model again", "Verificar modelo novamente"),
    ("log.loading", "Reading log…", "Lendo registro…"),
    ("log.latest", "Latest session", "Última sessão"),
    ("log.history", "Session log", "Registro da sessão"),
    ("log.no_sample", "No confirmed sample", "Sem amostra confirmada"),
    ("log.details", "Technical details", "Detalhes técnicos"),
    (
        "log.no_evidence",
        "No NR activity confirmed in this log",
        "Sem atividade de NR confirmada neste registro",
    ),
    (
        "log.initialized_only",
        "NR initialized; evaluation is not confirmed",
        "NR iniciou; a execução não foi confirmada",
    ),
    ("log.ran", "NR evaluation recorded", "Execução de NR registrada"),
    ("log.stage_pre", "before the upscaler", "antes da ampliação"),
    ("log.stage_post", "after the upscaler", "depois da ampliação"),
    ("log.stage_present", "at presentation", "na apresentação"),
    ("log.on_api", "on", "em"),
    ("log.error_count_one", "error", "erro"),
    ("log.error_count", "errors", "erros"),
    (
        "log.hint_errors",
        "Review the errors below. Make a diagnostic file if you need help.",
        "Confira os erros abaixo. Gere um arquivo de diagnóstico se precisar de ajuda.",
    ),
    (
        "log.hint_inactive",
        "Check that NR is enabled and a model is available, then start the game again.",
        "Confira se NR está ligado e se há um modelo disponível; depois abra o jogo novamente.",
    ),
    (
        "log.hint_timing",
        "No confirmed GPU timing was recorded. This alone does not mean NR failed.",
        "Não há tempo de GPU confirmado no registro. Isso, por si só, não indica falha do NR.",
    ),
    (
        "log.no_log_bundle",
        "The diagnostic includes available settings and installation state even without a log.",
        "O diagnóstico inclui os ajustes disponíveis e o estado da instalação, mesmo sem registro.",
    ),
    ("log.repeat_one", "occurrence", "ocorrência"),
    ("log.repeat", "occurrences", "ocorrências"),
    ("log.retry", "Read again", "Ler novamente"),
    // Comparison UX (worker C): keep this block together for the integration merge.
    (
        "cmp.empty.no_shortcut",
        "The menu shortcut is disabled. Set Menu.ShortcutKey in OptiScaler.ini, then restart the game.",
        "O atalho do menu está desativado. Configure Menu.ShortcutKey no OptiScaler.ini e reinicie o jogo.",
    ),
    ("cmp.fit.short", "Fit", "Ajustar"),
    ("cmp.reset.short", "0", "0"),
    ("cmp.tools", "Viewer options", "Opções da comparação"),
    ("cmp.more", "…", "…"),
    ("cmp.expand", "Enlarge comparison", "Ampliar comparação"),
    ("cmp.close", "Close enlarged view", "Fechar comparação ampliada"),
    ("cmp.details", "Capture details", "Detalhes da captura"),
    ("cmp.shortcuts", "Keyboard shortcuts", "Atalhos de teclado"),
    (
        "cmp.keyboard",
        "Focus the image: 1–4 modes · F fit · Space original · arrows pan · Shift + left/right moves the divider · +/− zoom · Esc leaves enlarged view.",
        "Com foco na imagem: 1–4 modos · F ajustar · Espaço original · setas mover · Shift + esquerda/direita move o divisor · +/− zoom · Esc sai da ampliação.",
    ),
    ("cmp.divider", "Comparison position", "Posição da comparação"),
    ("cmp.zoom_in", "Zoom in", "Aproximar"),
    ("cmp.zoom_out", "Zoom out", "Afastar"),
    ("cmp.preview", "SDR preview", "Prévia SDR"),
    ("cmp.empty.title", "Your first comparison", "Sua primeira comparação"),
    (
        "cmp.empty.install",
        "Start in the {tab} tab to review installation, then open the game with NR enabled.",
        "Comece na aba {tab} para revisar a instalação. Depois, abra o jogo com o NR ativado.",
    ),
    (
        "cmp.empty.run",
        "Open the game with Retoque and NR enabled.",
        "Abra o jogo com o Retoque e o NR ativado.",
    ),
    (
        "cmp.empty.menu",
        "Press {key} to open Retoque's in-game menu.",
        "Pressione {key} para abrir o menu do Retoque no jogo.",
    ),
    (
        "cmp.empty.custom",
        "Open Retoque's in-game menu with your configured menu key.",
        "Abra o menu do Retoque no jogo com a tecla configurada para o menu.",
    ),
    (
        "cmp.empty.disabled",
        "The in-game menu is disabled in this game's configuration. Enable it before capturing.",
        "O menu no jogo está desativado na configuração deste jogo. Ative-o antes de capturar.",
    ),
    (
        "cmp.empty.capture_now",
        "Click Capture now. The game saves the frame before and after NR, and both show up here.",
        "Clique em Capturar agora. O jogo salva o quadro antes e depois do NR, e os dois aparecem aqui.",
    ),
    ("cmp.empty.or_menu", "Or from inside the game:", "Ou de dentro do jogo:"),
    (
        "cmp.empty.capture",
        "In the DLSS Neural Rendering panel, choose “Capture original and processed”.",
        "No painel Renderização neural DLSS, escolha “Capturar original e processado”.",
    ),
    ("cmp.empty.refresh", "Look for captures", "Buscar capturas"),
    ("cmp.capture_now", "Capture now", "Capturar agora"),
    (
        "cmp.capture_now_help",
        "Asks the running game for one capture of the same frame before and after NR, without opening its menu.",
        "Pede ao jogo aberto uma captura do mesmo quadro antes e depois do NR, sem abrir o menu dele.",
    ),
    (
        "cmp.capture_waiting",
        "Waiting for the game to save the capture…",
        "Aguardando o jogo salvar a captura…",
    ),
    (
        "cmp.capture_not_running",
        "Open the game first: the running game takes the capture.",
        "Abra o jogo antes: quem faz a captura é o jogo aberto.",
    ),
    (
        "cmp.capture_ignored",
        "The game did not take the request: NR may be off, or this game's route does not read it (Vulkan, native D3D11). Use the in-game menu below.",
        "O jogo não pegou o pedido: o NR pode estar desligado, ou a rota deste jogo não lê o pedido (Vulkan, D3D11 nativo). Use o menu do jogo, abaixo.",
    ),
    (
        "cmp.capture_late",
        "The game took the request, but no capture arrived. The in-game menu shows why.",
        "O jogo pegou o pedido, mas nenhuma captura chegou. O menu do jogo mostra o motivo.",
    ),
    ("cmp.capture_saved", "Capture saved.", "Captura salva."),
    (
        "cmp.capture_failed",
        "Could not ask the game for a capture: {error}",
        "Não deu para pedir a captura ao jogo: {error}",
    ),
    // Autotune / worker F
    ("auto.gpu_reading", "Detecting GPU…", "Detectando a placa de vídeo…"),
    ("auto.gpu_unknown", "GPU not detected", "Placa de vídeo não detectada"),
    (
        "shell.install_optimize",
        "Install and optimize…",
        "Instalar e otimizar…",
    ),
    (
        "shell.register_optimize",
        "Register and optimize…",
        "Registrar e otimizar…",
    ),
    ("shell.update_optimize", "Update and optimize…", "Atualizar e otimizar…"),
    ("shell.more", "More actions", "Mais ações"),
    (
        "shell.install_optimize_hint",
        "One review: Retoque, then what this game and your graphics card need. Nothing changes until you confirm.",
        "Uma revisão só: o Retoque e o que este jogo e sua placa precisam. Nada muda antes da sua confirmação.",
    ),
    ("shell.install_only", "Install only…", "Só instalar…"),
    ("shell.optimize", "Optimize…", "Otimizar…"),
    (
        "shell.optimize_hint",
        "Review what this game and your graphics card can use now.",
        "Revisar o que este jogo e sua placa podem usar agora.",
    ),
    (
        "shell.no_payload",
        "Retoque's files were not found. Click \"Retoque files\" at the bottom of the window.",
        "Os arquivos do Retoque não foram encontrados. Clique em \"Arquivos do Retoque\" no rodapé da janela.",
    ),
    (
        "auto.use_banner",
        "\"Install and optimize…\" at the top does all of this in one review.",
        "O botão \"Instalar e otimizar…\" lá em cima faz tudo isso numa revisão só.",
    ),
    ("tile.gpu", "Graphics card", "Placa de vídeo"),
    ("tile.nr", "Neural Rendering", "Renderização neural"),
    ("tile.dlss", "DLSS", "DLSS"),
    ("tile.fg", "Frame generation", "Geração de quadros"),
    ("tile.on", "On", "Ligado"),
    ("tile.off", "Off", "Desligado"),
    ("tile.unknown", "Unknown", "Desconhecido"),
    ("tile.model_missing", "Model missing", "Modelo ausente"),
    ("tile.fg_sm86", "sm86 for RTX 20/30", "sm86 para RTX 20/30"),
    ("tile.fg_game", "The game's own", "Do próprio jogo"),
    ("tile.fg_available", "Available", "Disponível"),
    (
        "tile.fg_needs_sm86",
        "Needs sm86 on this card",
        "Precisa do sm86 nesta placa",
    ),
    ("tile.dlss_none", "Not found", "Não encontrado"),
    ("tile.dlss_detail", "Files in the game", "Arquivos no jogo"),
    ("auto.turing", "Turing", "Turing"),
    ("auto.ampere", "Ampere", "Ampere"),
    ("auto.ada", "Ada", "Ada"),
    ("auto.blackwell", "Blackwell", "Blackwell"),
    ("auto.older", "Earlier NVIDIA GPU", "NVIDIA de geração anterior"),
    ("auto.other", "GPU", "GPU"),
    ("auto.unknown", "Unknown architecture", "Arquitetura desconhecida"),
    ("auto.gb", "GB", "GB"),
    ("auto.driver", "Driver", "Driver"),
    (
        "auto.title",
        "Optimized for your GPU",
        "Ajustes para sua placa de vídeo",
    ),
    ("auto.checking", "Checking this game…", "Conferindo este jogo…"),
    (
        "auto.reading_components",
        "Reading your components…",
        "Lendo seus componentes…",
    ),
    (
        "auto.none",
        "No automatic changes recommended for this GPU and game.",
        "Nenhuma alteração automática recomendada para esta placa e este jogo.",
    ),
    (
        "auto.need_install",
        "Install Retoque in this game first.",
        "Instale o Retoque neste jogo primeiro.",
    ),
    ("auto.apply", "Apply only this…", "Aplicar só isto…"),
    ("auto.confirm", "Apply", "Aplicar"),
    ("auto.done", "Changes applied", "Alterações aplicadas"),
    ("auto.done_optimize", "Optimization applied", "Otimização aplicada"),
    (
        "auto.done_install",
        "Retoque installed and optimized",
        "Retoque instalado e otimizado",
    ),
    (
        "auto.done_update",
        "Retoque updated and optimized",
        "Retoque atualizado e otimizado",
    ),
    (
        "auto.done_register",
        "Retoque registered and optimized",
        "Retoque registrado e otimizado",
    ),
    (
        "auto.partial",
        "Not fully applied; completed operations",
        "Aplicação incompleta; operações concluídas",
    ),
    (
        "auto.lsfg",
        "Lossless Scaling frame generation",
        "Geração de quadros com Lossless Scaling",
    ),
    (
        "auto.ls_help",
        "Select a multiplier to review the changes. Your Lossless Scaling files are referenced, never copied. Restart the game after applying.",
        "Escolha o multiplicador para revisar os ajustes. Seus arquivos do Lossless Scaling são usados no local, sem cópia. Reinicie o jogo após aplicar.",
    ),
    ("auto.off", "Off…", "Desligado…"),
    ("auto.x2", "x2…", "x2…"),
    ("auto.x3", "x3…", "x3…"),
    ("auto.x4", "x4…", "x4…"),
    (
        "auto.ls_ready",
        "Lossless Scaling shaders ready",
        "Shaders do Lossless Scaling prontos",
    ),
    (
        "auto.ls_missing",
        "Lossless Scaling not found",
        "Lossless Scaling não encontrado",
    ),
    (
        "auto.choose_lossless",
        "Choose Lossless Scaling folder…",
        "Escolher pasta do Lossless Scaling…",
    ),
    (
        "auto.ls_conflict",
        "Uninstall sm86 in Components › Frame generation before turning LSFG on.",
        "Desinstale o sm86 em Componentes › Geração de quadros antes de ligar o LSFG.",
    ),
    (
        "auto.components",
        "Frame generation components",
        "Componentes de geração de quadros",
    ),
    (
        "auto.no_component",
        "No sm86 component in your catalog",
        "Nenhum componente sm86 no catálogo",
    ),
    (
        "auto.component_installed",
        "Component installed in this game",
        "Componente instalado neste jogo",
    ),
    (
        "auto.component_install",
        "Review component installation…",
        "Revisar instalação do componente…",
    ),
    (
        "auto.component_remove",
        "Review component removal",
        "Revisar remoção do componente",
    ),
    (
        "auto.catalog_remove",
        "Delete component from app…",
        "Apagar componente do app…",
    ),
    (
        "auto.catalog_remove_title",
        "Delete this component from the app?",
        "Apagar este componente do app?",
    ),
    (
        "auto.catalog_remove_help",
        "Deletes the copy the app keeps. Games that have it keep their files; to install it again, import or download it again.",
        "Apaga a cópia guardada no app. Os jogos que já o têm continuam com os arquivos; para instalá-lo de novo, importe ou baixe outra vez.",
    ),
    (
        "auto.none_installed",
        "No managed component installed in this game.",
        "Nenhum componente gerenciado instalado neste jogo.",
    ),
    (
        "rec.sm86.title",
        "DLSS frame generation for your GPU",
        "Geração de quadros DLSS para sua placa",
    ),
    (
        "rec.sm86.why",
        "This game has DLSS-G. Your imported sm86 loader can provide frame generation on Turing or Ampere; LSFG will be turned off.",
        "Este jogo tem DLSS-G. O carregador sm86 importado pode fornecer geração de quadros em Turing ou Ampere; o LSFG será desligado.",
    ),
    (
        "rec.sm86_mfg.title",
        "DLSS frame generation through sm86",
        "Geração de quadros DLSS pelo sm86",
    ),
    (
        "rec.sm86_mfg.why",
        "sm86 is installed and this game has DLSS-G: keep Retoque's own frame generation out of the way and, unless the game already has a choice, ask for two generated frames per real one (x3), which stays steadier than x4 on an RTX 30. Below about 30 real frames per second, pick DLSS Balanced or Performance in the game. You can change it in Settings.",
        "O sm86 está instalado e este jogo tem DLSS-G: tira a geração de quadros do Retoque do caminho e, se o jogo ainda não tem uma escolha, pede dois quadros gerados para cada real (x3), que fica mais estável que x4 numa RTX 30. Abaixo de uns 30 quadros reais por segundo, escolha DLSS Balanceado ou Desempenho no jogo. Dá para mudar em Ajustes.",
    ),
    (
        "rec.rtx30_nr.title",
        "Neural Rendering (NR) tuned for RTX 30",
        "Renderização neural (NR) ajustada para RTX 30",
    ),
    (
        "rec.rtx30_nr.why",
        "Full detail in the center of the screen at the same cost (peripheral compression), and a stronger effect (1.5). Measured on an RTX 3060; you can change both in Settings.",
        "Detalhe total no centro da tela pelo mesmo custo (compressão periférica) e efeito mais forte (1,5). Medido numa RTX 3060; dá para mudar os dois em Ajustes.",
    ),
    ("rec.lsfg.title", "Lossless Scaling x2", "Lossless Scaling x2"),
    (
        "rec.lsfg.why",
        "This game has no detected DLSS-G. Use your Lossless Scaling installation to generate an extra frame for each real frame.",
        "Não foi detectado DLSS-G neste jogo. Use seu Lossless Scaling para gerar um quadro extra a cada quadro real.",
    ),
    (
        "rec.install.title",
        "Install Retoque first",
        "Instale o Retoque primeiro",
    ),
    (
        "rec.install.why",
        "GPU-specific settings become available after installing Retoque in this game.",
        "Os ajustes para sua placa ficam disponíveis depois de instalar o Retoque neste jogo.",
    ),
    (
        "rec.adopt.title",
        "Register this Retoque install",
        "Registrar esta instalação do Retoque",
    ),
    (
        "rec.adopt.why",
        "Retoque is here but not registered (installed by an older script). Reinstalling through the app records every file, so it can be updated and removed safely later.",
        "O Retoque está aqui sem registro (foi instalado por um script antigo). Reinstalar pelo app registra cada arquivo, para atualizar e remover com segurança depois.",
    ),
    (
        "rec.sm86_update.title",
        "Update DLSS-G for RTX 30 (dlssg_for_sm86)",
        "Atualizar o DLSS-G para RTX 30 (dlssg_for_sm86)",
    ),
    (
        "rec.sm86_update.why",
        "This game has an older dlssg_for_sm86 (0.3.x). The version in your catalog replaces it; the old files are kept as a backup.",
        "Este jogo tem um dlssg_for_sm86 antigo (0.3.x). A versão do seu catálogo o substitui; os arquivos antigos ficam guardados como cópia.",
    ),
    // Discovery folders / worker t117
    ("store.xbox", "Xbox", "Xbox"),
    ("folders.title", "Your folders", "Suas pastas"),
    (
        "folders.migrated_both",
        "Updated your folders: moved {moved} to folders with several games; ignored drive root {roots}.",
        "Suas pastas foram atualizadas: {moved} movida(s) para pastas com vários jogos; raiz de unidade {roots} ignorada.",
    ),
    (
        "folders.migrated_moved",
        "Updated your folders: moved {moved} to folders with several games.",
        "Suas pastas foram atualizadas: {moved} movida(s) para pastas com vários jogos.",
    ),
    (
        "folders.migrated_dropped",
        "Updated your folders: ignored drive root {roots}.",
        "Suas pastas foram atualizadas: raiz de unidade {roots} ignorada.",
    ),
    (
        "folders.question",
        "What does this folder contain?",
        "O que há nesta pasta?",
    ),
    ("folders.game", "One game", "Um jogo"),
    (
        "folders.library",
        "A folder with several games",
        "Uma pasta com vários jogos",
    ),
    (
        "folders.game_help",
        "Choose the folder of one game or emulator.",
        "Escolha a pasta de um jogo ou emulador.",
    ),
    (
        "folders.library_help",
        "Each child folder is checked for a game. Choose the parent that contains them.",
        "Cada subpasta será examinada em busca de um jogo. Escolha a pasta que reúne esses jogos.",
    ),
    (
        "folders.help",
        "These folders are checked when you rescan. Removing an entry only stops checking that folder; no files are deleted.",
        "Estas pastas entram na busca de jogos. Remover uma entrada apenas retira a pasta da busca; nenhum arquivo é apagado.",
    ),
    (
        "folders.empty",
        "No folders added yet.",
        "Você ainda não adicionou pastas.",
    ),
    (
        "folders.games",
        "Individual games and emulators",
        "Jogos e emuladores individuais",
    ),
    (
        "folders.libraries",
        "Folders with several games",
        "Pastas com vários jogos",
    ),
    ("folders.remove", "Remove from list", "Remover da lista"),
    (
        "folders.remove_help",
        "Remove from this list only. Files stay where they are.",
        "Remover apenas desta lista. Os arquivos ficam onde estão.",
    ),
    ("folders.close", "Close", "Fechar"),
    (
        "folders.unregistered",
        "Not in the store’s list",
        "Não consta na lista da loja",
    ),
    // Intuitive shell: append-only translation keys.
    ("palette.title", "Commands", "Comandos"),
    (
        "palette.search",
        "Search games, tabs, settings and actions",
        "Buscar jogos, abas, ajustes e ações",
    ),
    (
        "palette.keys",
        "Arrow keys select · Enter opens · Esc closes",
        "Setas selecionam · Enter abre · Esc fecha",
    ),
    ("palette.empty", "No matches", "Nenhum resultado"),
    ("palette.game", "Game", "Jogo"),
    ("palette.uninstall", "Uninstall Retoque…", "Desinstalar o Retoque…"),
    (
        "palette.review_settings",
        "Review settings changes…",
        "Revisar alterações dos ajustes…",
    ),
    ("palette.focus", "Focus current search", "Focar busca atual"),
    ("set.filter", "Filter settings", "Filtrar ajustes"),
    ("set.only_changed", "Only changed", "Somente alterados"),
    (
        "set.reset",
        "Back to automatic: {label}",
        "Voltar ao automático: {label}",
    ),
    (
        "set.source_draft",
        "Changed, not applied yet. Review to apply.",
        "Alterado, ainda não aplicado. Revise para aplicar.",
    ),
    (
        "undo.exact",
        "INI undo restores the original bytes; the recorded post-apply hash still matches.",
        "O desfazer restaura os bytes originais do INI; o hash registrado após aplicar ainda corresponde.",
    ),
    (
        "undo.conservative",
        "Conservative INI undo: the file changed or exact history is unavailable. Other bytes are preserved; restoration is not byte-for-byte.",
        "Desfazer conservador do INI: o arquivo mudou ou não há histórico exato. Os demais bytes são preservados; a restauração não é byte a byte.",
    ),
    (
        "undo.provenance_mismatch",
        "INI undo provenance does not match the requested edits",
        "Os metadados para desfazer o INI não correspondem aos ajustes solicitados",
    ),
    (
        "undo.invalid_provenance",
        "invalid INI undo provenance; no files were changed",
        "Metadados inválidos para desfazer o INI; nenhum arquivo foi alterado",
    ),
    (
        "undo.stale_provenance",
        "INI undo provenance no longer matches; plan again",
        "Os metadados para desfazer não correspondem mais ao INI; revise um novo plano",
    ),
    (
        "undo.stale_value",
        "INI value changed since planning; plan again",
        "O valor do INI mudou após a revisão; revise um novo plano",
    ),
    ("undo.title", "Undo last change…", "Desfazer última alteração…"),
    (
        "undo.empty",
        "No saved settings change to undo for this game.",
        "Nenhuma alteração salva para desfazer neste jogo.",
    ),
    (
        "undo.conflict",
        "Undo refused: this setting changed after the recorded edit. Review the current settings:",
        "Desfazer recusado: este ajuste mudou depois da alteração registrada. Revise os ajustes atuais:",
    ),
    (
        "undo.save_failed",
        "Settings applied, but undo history could not be saved",
        "Ajustes aplicados, mas não foi possível salvar o histórico",
    ),
    (
        "undo.auto_note",
        "Undo is reviewed before applying. Settings that were not in the file before are removed again.",
        "O desfazer é revisado antes de aplicar. Ajustes que não estavam no arquivo voltam a ser removidos.",
    ),
    (
        "drop.hover",
        "Drop to review model import, component import or game addition",
        "Solte para revisar a importação de modelo, componente ou adição de jogo",
    ),
    ("drop.title", "Review dropped item", "Revisar item solto"),
    (
        "drop.model",
        "Import this NR model into your models?",
        "Importar este modelo NR para seus modelos?",
    ),
    (
        "drop.component",
        "Import this sm86 component into the app catalog? Installation requires a separate review.",
        "Importar este componente sm86 para o catálogo? A instalação exige outra revisão.",
    ),
    (
        "drop.game",
        "Register this game folder in the library? No game files will be modified.",
        "Registrar esta pasta de jogo na biblioteca? Nenhum arquivo do jogo será alterado.",
    ),
    (
        "drop.unsupported",
        "Unsupported item. Choose an nvngx_dlssnr*.dll file or a game/component folder.",
        "Item não suportado. Escolha nvngx_dlssnr*.dll ou uma pasta de jogo/componente.",
    ),
    ("drop.import", "Import", "Importar"),
    ("drop.add_game", "Add to library", "Adicionar à biblioteca"),
    ("drop.checking", "Checking the file…", "Verificando o arquivo…"),
    ("doctor.title", "Retoque health check", "Diagnóstico do Retoque"),
    ("doctor.prepare", "Review fix…", "Revisar correção…"),
    ("doctor.evidence", "Evidence", "Evidências"),
    ("doctor.badge", "Health findings", "Problemas detectados"),
    ("doctor.issues_one", "1 issue", "1 problema"),
    ("doctor.issues", "{n} issues", "{n} problemas"),
    (
        "watch.manual",
        "Live updates unavailable. Refresh manually.",
        "Atualizações automáticas indisponíveis. Atualize manualmente.",
    ),
    ("watch.refresh", "Refresh selected game", "Atualizar jogo selecionado"),
    ("log.diagnoses", "Log diagnosis", "Diagnóstico do log"),
    ("dlss.title", "NVIDIA DLSS", "NVIDIA DLSS"),
    ("dlss.installed", "DLSS in this game", "DLSS neste jogo"),
    ("dlss.available", "available", "disponível"),
    ("dlss.current", "up to date", "atualizado"),
    ("dlss.update", "Update DLSS…", "Atualizar DLSS…"),
    (
        "dlss.restore",
        "Restore original DLSS and uninstall Retoque…",
        "Restaurar DLSS original e desinstalar o Retoque…",
    ),
    (
        "dlss.restore_only",
        "Restore original DLSS…",
        "Restaurar DLSS original…",
    ),
    (
        "dlss.restore_confirm",
        "Restore original DLSS",
        "Restaurar DLSS original",
    ),
    (
        "dlss.optimize",
        "Also update DLSS (optional)",
        "Atualizar DLSS também (opcional)",
    ),
    ("dlss.download", "Get DLSS 310.9.16…", "Obter DLSS 310.9.16…"),
    (
        "dlss.all_current",
        "Every DLSS file in this game is already this version or newer.",
        "Todos os arquivos DLSS deste jogo já estão nesta versão ou mais nova.",
    ),
    (
        "dlss.downloading",
        "Downloading verified NVIDIA DLSS",
        "Baixando NVIDIA DLSS com verificação",
    ),
    (
        "dlss.download_first",
        "Download DLSS in Components › DLSS first, then review the update.",
        "Baixe o DLSS em Componentes › DLSS primeiro; depois revise a atualização.",
    ),
    (
        "dlss.consent",
        "Retoque downloads Super Resolution, Ray Reconstruction and Frame Generation directly from NVIDIA. Only the kinds the game already has are replaced, after a separate review.",
        "O Retoque baixa Super Resolution, Ray Reconstruction e Frame Generation diretamente da NVIDIA. Só os tipos que o jogo já tem são trocados, depois de outra revisão.",
    ),
    (
        "dlss.consent_title",
        "Download NVIDIA DLSS 310.9.16",
        "Baixar o NVIDIA DLSS 310.9.16",
    ),
    (
        "dlss.consent_details",
        "Hosts: raw.githubusercontent.com and media.githubusercontent.com (NVIDIA/DLSS, v310.9.16). Each file is checked against a fixed size and SHA-256. The NVIDIA DLSS license applies.",
        "Hosts: raw.githubusercontent.com e media.githubusercontent.com (NVIDIA/DLSS, v310.9.16). Cada arquivo é conferido com tamanho e SHA-256 fixos. Vale a licença do NVIDIA DLSS.",
    ),
    (
        "dlss.caution",
        "DLL changes may be incompatible with a game or its anti-cheat. Check the game policy before updating. Originals are backed up for restoration.",
        "Trocas de DLL podem ser incompatíveis com o jogo ou seu anticheat. Confira a política do jogo antes de atualizar. Os originais terão backup para restauração.",
    ),
    ("dlss.license", "Read NVIDIA license", "Ler licença NVIDIA"),
    (
        "dlss.error.0",
        "DLSS root receipt belongs to another installation",
        "O registro de instalação na raiz pertence a outra instalação",
    ),
    (
        "dlss.error.1",
        "invalid DLSS game root or empty file list",
        "Raiz do jogo inválida ou lista DLSS vazia",
    ),
    (
        "dlss.error.2",
        "invalid or duplicate DLSS path",
        "Caminho DLSS inválido ou duplicado",
    ),
    (
        "dlss.error.3",
        "DLSS destination missing, linked or ambiguous",
        "Destino DLSS ausente, vinculado ou ambíguo",
    ),
    (
        "dlss.error.4",
        "restore previous DLSS receipt before changing its root",
        "Restaure o registro de instalação DLSS anterior antes de mudar sua raiz",
    ),
    (
        "dlss.error.5",
        "DLSS root marker is not receipt-owned",
        "O marcador DLSS da raiz não pertence ao registro de instalação",
    ),
    (
        "dlss.error.6",
        "invalid DLSS catalog marker",
        "Marcador do catálogo DLSS inválido",
    ),
    (
        "dlss.error.7",
        "non-Unicode DLSS path",
        "Caminho DLSS com codificação inválida",
    ),
    (
        "dlss.error.8",
        "DLSS catalog file missing or linked",
        "Arquivo do catálogo DLSS ausente ou vinculado",
    ),
    (
        "dlss.error.9",
        "DLSS file changed since previous update; restore or reconcile first",
        "A DLL mudou desde a atualização anterior; restaure ou reconcilie primeiro",
    ),
    (
        "dlss.error.10",
        "target outside DLSS game root",
        "Pasta-alvo fora da raiz do jogo",
    ),
    (
        "dlss.error.11",
        "DLSS size or SHA-256 mismatch",
        "Tamanho ou SHA-256 do DLSS não confere",
    ),
    ("dlss.error.12", "linked DLSS catalog", "Catálogo DLSS é um vínculo"),
    (
        "dlss.error.13",
        "invalid DLSS catalog directory",
        "Diretório do catálogo DLSS inválido",
    ),
    (
        "dlss.error.14",
        "invalid DLSS catalog file",
        "Arquivo do catálogo DLSS inválido",
    ),
    (
        "dlss.error.15",
        "DLSS download requires confirmation",
        "O download DLSS exige confirmação",
    ),
    (
        "dlss.error.16",
        "DLSS catalog appeared during download",
        "O catálogo DLSS apareceu durante o download",
    ),
    (
        "dlss.error.17",
        "DLSS catalog is not downloaded",
        "O catálogo DLSS ainda não foi baixado",
    ),
    (
        "dlss.error.18",
        "DLSS catalog disappeared",
        "O catálogo DLSS desapareceu",
    ),
    (
        "dlss.error.19",
        "DLSS scan depth exceeded",
        "Limite de profundidade da busca DLSS excedido",
    ),
    (
        "dlss.error.20",
        "DLSS scan entry limit exceeded",
        "Limite de arquivos da busca DLSS excedido",
    ),
    (
        "dlss.error.21",
        "non-Unicode DLSS scan path",
        "Caminho com codificação inválida na busca DLSS",
    ),
    ("dlss.error.22", "invalid DLSS path", "Caminho DLSS inválido"),
    (
        "dlss.error.23",
        "restore cannot be combined with optimization",
        "A restauração não pode ser combinada com otimização",
    ),
    (
        "dlss.error.24",
        "only same-root installs can be combined",
        "Somente instalações na mesma raiz podem ser combinadas",
    ),
    (
        "dlss.error.25",
        "overlapping install plans",
        "Planos de instalação alteram os mesmos caminhos",
    ),
    (
        "dlss.error.26",
        "install plans observe different files",
        "Os arquivos mudaram entre os planos de instalação",
    ),
    (
        "dlss.error.component_changed",
        "component changed since review",
        "O componente mudou desde a revisão",
    ),
    // Payload release fetch (app-payload-fetch): append-only translation keys.
    (
        "payload.source.beside",
        "Use files beside app",
        "Usar arquivos ao lado do app",
    ),
    ("payload.source.folder", "Use chosen folder", "Usar pasta escolhida"),
    (
        "payload.source.release",
        "Use downloaded release",
        "Usar versão baixada",
    ),
    ("payload.download", "Get release…", "Baixar versão…"),
    ("payload.download_now", "Download update…", "Baixar atualização…"),
    ("payload.check", "Check for updates…", "Verificar atualizações…"),
    (
        "payload.consent_title",
        "Official Retoque release",
        "Versão oficial do Retoque",
    ),
    (
        "payload.check_consent",
        "Ask GitHub whether there is a newer version of the Retoque files?",
        "Perguntar ao GitHub se há uma versão mais nova dos arquivos do Retoque?",
    ),
    (
        "payload.download_consent",
        "Retoque downloads its latest files from the official release and checks them. Your games do not change until you review an install.",
        "O Retoque baixa os arquivos mais recentes da versão oficial e os verifica. Seus jogos não mudam até você revisar uma instalação.",
    ),
    (
        "payload.hosts_note",
        "Allowed HTTPS hosts: api.github.com, github.com/gabrielmaialva33/retoque and release-assets.githubusercontent.com. Verified with SHA256SUMS and the GitHub SHA-256 digest.",
        "Hosts HTTPS permitidos: api.github.com, github.com/gabrielmaialva33/retoque e release-assets.githubusercontent.com. Verificado com SHA256SUMS e digest SHA-256 do GitHub.",
    ),
    (
        "payload.progress",
        "Downloading Retoque files…",
        "Baixando arquivos do Retoque…",
    ),
    (
        "payload.no_release",
        "No official release is published on GitHub yet; keeping local folder or files beside the app.",
        "Ainda não há versão oficial publicada no GitHub; mantendo a pasta escolhida ou os arquivos ao lado do app.",
    ),
    ("payload.new_release", "New release available", "Nova versão disponível"),
    ("payload.up_to_date", "Release up to date", "Versão atualizada"),
    (
        "payload.error.0",
        "payload SHA-256 does not match GitHub asset digest",
        "O SHA-256 dos arquivos do Retoque não corresponde ao digest publicado pelo GitHub.",
    ),
    (
        "payload.error.1",
        "payload SHA-256 does not match SHA256SUMS",
        "O SHA-256 dos arquivos do Retoque não corresponde ao arquivo SHA256SUMS.",
    ),
    (
        "payload.error.2",
        "SHA256SUMS digest mismatch",
        "O SHA-256 do arquivo SHA256SUMS não corresponde ao digest publicado.",
    ),
    (
        "payload.error.3",
        "SHA256SUMS does not list payload archive",
        "O arquivo SHA256SUMS não lista os arquivos do Retoque.",
    ),
    (
        "payload.error.4",
        "forbidden file in payload archive",
        "Há um arquivo proibido entre os arquivos do Retoque.",
    ),
    (
        "payload.error.5",
        "unexpected extra file in payload archive",
        "Há um arquivo extra não permitido entre os arquivos do Retoque.",
    ),
    (
        "payload.error.6",
        "OptiScaler.dll and nvngx.dll_dlssnr.dll file versions do not match",
        "As versões de arquivo do OptiScaler.dll e do nvngx.dll_dlssnr.dll não coincidem.",
    ),
    (
        "payload.error.7",
        "incomplete payload release archive",
        "Faltam arquivos obrigatórios do Retoque nesta versão.",
    ),
    (
        "payload.error.8",
        "invalid official payload release metadata",
        "Os metadados da versão oficial do Retoque são inválidos.",
    ),
    (
        "payload.error.9",
        "payload HTTPS request failed",
        "Não foi possível consultar ou baixar a versão do Retoque por HTTPS.",
    ),
    (
        "payload.error.10",
        "non-official payload URL",
        "URL fora do repositório oficial do Retoque.",
    ),
    (
        "payload.error.11",
        "refused payload release redirect",
        "Redirecionamento da versão do Retoque recusado.",
    ),
    (
        "payload.error.12",
        "payload release changed since first download",
        "O pacote desta versão mudou após o primeiro download.",
    ),
    // Community NR model fetch (app-nr-fetch): append-only translation keys.
    (
        "nr_fetch.title",
        "Community NR model (RankFTW/rhi-repo)",
        "Modelo NR da comunidade (RankFTW/rhi-repo)",
    ),
    (
        "nr_fetch.source_note",
        "Community redistribution from RankFTW/rhi-repo (listed by ApolloF/DLSS5-Updater-catalog), not an official NVIDIA download. Verified by size and SHA-256 before import.",
        "Redistribuição da comunidade em RankFTW/rhi-repo (listada por ApolloF/DLSS5-Updater-catalog), não é um download oficial da NVIDIA. Verificado por tamanho e SHA-256 antes da importação.",
    ),
    (
        "nr_fetch.get_compatible",
        "Get a compatible model…",
        "Obter um modelo compatível…",
    ),
    (
        "nr_fetch.consent_title",
        "Download a compatible NR model",
        "Baixar um modelo NR compatível",
    ),
    (
        "nr_fetch.consent_body",
        "This model comes from the community, not from NVIDIA. Retoque checks it and keeps only the model file. Your game's settings change only after a separate review.",
        "Este modelo vem da comunidade, não da NVIDIA. O Retoque o verifica e guarda só o arquivo do modelo. Os ajustes do seu jogo só mudam depois de outra revisão.",
    ),
    (
        "nr_fetch.refresh_catalog",
        "Refresh community catalog…",
        "Atualizar catálogo da comunidade…",
    ),
    (
        "nr_fetch.refresh_title",
        "Refresh community NR model catalog",
        "Atualizar catálogo de modelos NR da comunidade",
    ),
    (
        "nr_fetch.refresh_body",
        "Download the latest list of community NR models from GitHub?",
        "Baixar do GitHub a lista mais recente de modelos NR da comunidade?",
    ),
    (
        "nr_fetch.refresh_details",
        "catalog.json from ApolloF/DLSS5-Updater-catalog (raw.githubusercontent.com, at most 8 MB). Only entries on RankFTW/rhi-repo releases with a SHA-256 are accepted; untested entries are never assumed compatible.",
        "catalog.json de ApolloF/DLSS5-Updater-catalog (raw.githubusercontent.com, no máximo 8 MB). Só entradas em versões de RankFTW/rhi-repo com SHA-256 são aceitas; entradas não testadas nunca são tidas como compatíveis.",
    ),
    ("nr_fetch.confirm_refresh", "Refresh catalog", "Atualizar catálogo"),
    (
        "nr_fetch.downloading",
        "Downloading community NR model",
        "Baixando modelo NR da comunidade",
    ),
    (
        "nr_fetch.refreshing",
        "Downloading the community model list…",
        "Baixando a lista de modelos da comunidade…",
    ),
    (
        "nr_fetch.no_compatible_gpu",
        "No compatible model can be picked automatically because the graphics card was not identified.",
        "Nenhum modelo compatível pode ser escolhido automaticamente porque a placa de vídeo não foi identificada.",
    ),
    (
        "nr_fetch.unavailable",
        "No compatible NR model entry is available for this GPU. No changes were applied.",
        "Nenhum modelo NR compatível está disponível para esta placa de vídeo. Nenhuma alteração foi aplicada.",
    ),
    (
        "nr_fetch.available_variants",
        "Available community variants",
        "Variantes disponíveis na comunidade",
    ),
    ("nr_fetch.download_variant", "Download…", "Baixar…"),
    (
        "nr_fetch.error.0",
        "invalid or missing model sha256",
        "O SHA-256 do modelo no catálogo está ausente ou é inválido.",
    ),
    (
        "nr_fetch.error.1",
        "model catalog exceeds size limit",
        "O catálogo de modelos excede o limite de tamanho.",
    ),
    (
        "nr_fetch.error.2",
        "too many model catalog entries",
        "O catálogo de modelos contém entradas demais.",
    ),
    (
        "nr_fetch.error.3",
        "invalid model catalog metadata",
        "Os metadados do catálogo de modelos são inválidos.",
    ),
    (
        "nr_fetch.error.4",
        "non-rhi-repo model URL",
        "O endereço não pertence às versões publicadas de RankFTW/rhi-repo.",
    ),
    (
        "nr_fetch.error.5",
        "refused model redirect",
        "O destino do redirecionamento do modelo não é permitido.",
    ),
    (
        "nr_fetch.error.6",
        "model catalog redirect refused",
        "O catálogo de modelos tentou um redirecionamento não permitido.",
    ),
    (
        "nr_fetch.error.7",
        "missing model redirect",
        "O redirecionamento do modelo não informa o destino.",
    ),
    (
        "nr_fetch.error.8",
        "model HTTPS request failed",
        "Não foi possível consultar ou baixar o modelo por HTTPS.",
    ),
    (
        "nr_fetch.error.9",
        "unexpected model HTTP status",
        "O servidor do modelo retornou um status HTTP inesperado.",
    ),
    (
        "nr_fetch.error.10",
        "model response exceeds size limit",
        "A resposta do download do modelo excede o limite de tamanho.",
    ),
    (
        "nr_fetch.error.11",
        "too many model redirects",
        "O download do modelo excedeu o limite de redirecionamentos.",
    ),
    (
        "nr_fetch.error.12",
        "nvngx_dlssnr.dll missing from archive",
        "O arquivo nvngx_dlssnr.dll não foi encontrado no pacote.",
    ),
    (
        "nr_fetch.error.13",
        "multiple nvngx_dlssnr.dll entries in archive",
        "O pacote contém mais de um nvngx_dlssnr.dll.",
    ),
    (
        "nr_fetch.error.14",
        "model size mismatch",
        "O tamanho do modelo não corresponde ao declarado.",
    ),
    (
        "nr_fetch.error.15",
        "model SHA-256 mismatch",
        "O SHA-256 do modelo não corresponde ao esperado.",
    ),
    (
        "nr_fetch.error.16",
        "pinned model metadata mismatch",
        "Os metadados do catálogo conflitam com uma versão fixada conhecida.",
    ),
];

pub fn t(lang: Lang, key: &str) -> &str {
    text_in(pack(lang), lang, key)
}

/// A key's text in a pack, else in the built-in table for the language's fallback column.
fn text_in<'a>(pack: Option<&Pack>, lang: Lang, key: &'a str) -> &'a str {
    match pack.and_then(|p| p.get(key)) {
        Some(text) => text,
        None => builtin(lang.table(), key),
    }
}

/// The built-in table (English or Portuguese), then retoque-data's shared messages, then the key itself.
fn builtin(lang: Lang, key: &str) -> &str {
    for (k, en, pt) in TEXT {
        if *k == key {
            return if lang == Lang::Pt { pt } else { en };
        }
    }
    retoque_data::messages::text(key, lang.data_language())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_recommendation_key_has_both_languages() {
        // Recommendation texts are keys stored in retoque-data, not literal t() calls, so the source scan above
        // cannot see them.
        let source = include_str!("../../retoque-data/src/recommend.rs");
        for piece in source.split('"').skip(1).step_by(2) {
            if piece.starts_with("rec.") && (piece.ends_with(".title") || piece.ends_with(".why")) {
                assert!(
                    TEXT.iter().any(|(k, _, _)| *k == piece),
                    "untranslated recommendation key {piece}"
                );
            }
        }
    }

    #[test]
    fn keys_are_unique_and_filled() {
        let mut seen = std::collections::HashSet::new();
        for (k, en, pt) in TEXT {
            assert!(seen.insert(*k), "duplicate key {k}");
            assert!(!en.is_empty() && !pt.is_empty(), "empty text for {k}");
        }
    }

    #[test]
    fn ellipsis_is_in_both_languages_or_in_neither() {
        // A label ends in "…" when clicking it opens a review, dialog or file picker (DESIGN.md), so the promise
        // cannot depend on the language.
        for (key, en, pt) in TEXT.iter().chain(retoque_data::messages::TEXT) {
            assert_eq!(en.ends_with('…'), pt.ends_with('…'), "{key}: \"{en}\" / \"{pt}\"");
        }
    }
}

#[cfg(test)]
mod packs {
    use super::*;
    use retoque_data::settings::Kind;

    /// Every key a pack may translate, with its English text: the table, retoque-data's messages, the settings'
    /// labels and help lines, and the drop-down values a setting can show.
    fn english() -> HashMap<String, &'static str> {
        let mut keys: HashMap<String, &'static str> = TEXT
            .iter()
            .chain(retoque_data::messages::TEXT)
            .map(|(k, en, _)| (k.to_string(), *en))
            .collect();
        for s in retoque_data::settings::all() {
            keys.insert(format!("setting.{}.{}", s.section, s.key), s.label_en);
            keys.insert(format!("setting.{}.{}.help", s.section, s.key), s.help_en);
            let mut values = vec!["auto", "true", "false"];
            if let Kind::Choice(choices) = s.kind {
                values.extend(choices);
            }
            for value in values {
                if let Some((en, _)) = s.choice_labels(value) {
                    keys.insert(choice_key(en), en);
                }
            }
        }
        keys
    }

    /// The English a pack line is checked against: its own key, or for a `_few`/`_many` plural form the `_other`
    /// form beside an `_one` (only languages with those forms may add them).
    fn source_of<'a>(english: &'a HashMap<String, &'static str>, key: &str) -> Option<&'a str> {
        if let Some(en) = english.get(key) {
            return Some(en);
        }
        let base = key.strip_suffix("_few").or_else(|| key.strip_suffix("_many"))?;
        english.get(&format!("{base}_one"))?;
        english
            .get(&format!("{base}_other"))
            .or_else(|| english.get(base))
            .copied()
    }

    fn drafts() -> impl Iterator<Item = (Lang, &'static str)> {
        Lang::ALL.into_iter().filter_map(|l| pack_source(l).map(|s| (l, s)))
    }

    fn placeholders(text: &str) -> Vec<&str> {
        let mut found: Vec<&str> = text
            .match_indices('{')
            .filter_map(|(at, _)| {
                let end = at + text[at..].find('}')?;
                let inner = &text[at + 1..end];
                inner
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
                    .then(|| &text[at..=end])
            })
            .collect();
        found.sort_unstable();
        found
    }

    #[test]
    fn every_language_but_english_and_portuguese_is_a_pack() {
        assert_eq!(drafts().count(), 8);
        for lang in Lang::ALL {
            assert_eq!(pack_source(lang).is_some(), lang.is_draft(), "{lang:?}");
        }
    }

    #[test]
    fn packs_hold_known_keys_once_and_are_marked_as_drafts() {
        let english = english();
        for (lang, source) in drafts() {
            let header: String = source.lines().take_while(|l| l.starts_with('#')).collect();
            assert!(
                header.contains("MACHINE-TRANSLATED DRAFT") && header.contains(&format!("({})", lang.code())),
                "{lang:?}: the header must say the pack is an unreviewed machine translation"
            );
            let mut seen = std::collections::HashSet::new();
            for (number, line) in source.lines().enumerate() {
                let line = line.trim_end_matches('\r');
                if line.is_empty() || line.starts_with('#') {
                    continue;
                }
                let at = format!("{}.tsv line {}", lang.code(), number + 1);
                let (key, text) = line.split_once('\t').unwrap_or_else(|| panic!("{at}: no TAB"));
                assert!(!text.contains('\t'), "{at}: more than one TAB");
                assert!(source_of(&english, key).is_some(), "{at}: unknown key {key}");
                assert!(seen.insert(key), "{at}: duplicate key {key}");
                assert!(
                    !text.trim().is_empty() && text.trim() == text,
                    "{at}: empty or padded text"
                );
            }
        }
    }

    #[test]
    fn packs_keep_placeholders_and_the_dialog_ellipsis() {
        let english = english();
        for (lang, _) in drafts() {
            for (key, text) in pack(lang).unwrap() {
                let en = source_of(&english, key).unwrap();
                assert_eq!(placeholders(text), placeholders(en), "{lang:?} {key}: \"{text}\"");
                // DESIGN.md: "…" promises that a dialog opens, in every language or in none.
                assert_eq!(en.ends_with('…'), text.ends_with('…'), "{lang:?} {key}: \"{text}\"");
                assert!(!text.contains("..."), "{lang:?} {key}: \"...\" instead of \"…\"");
            }
        }
    }

    #[test]
    fn placeholder_scan_counts_every_occurrence() {
        assert_eq!(
            placeholders("{n} of {n}, {} and {size}"),
            vec!["{n}", "{n}", "{size}", "{}"]
        );
        assert!(placeholders("{ not one } {X}").is_empty());
    }

    #[test]
    fn a_key_missing_from_a_pack_shows_in_english() {
        let pack = parse_pack("# comment\r\n\nprefs.title\tEinstellungen\r\nmalformed line\n");
        assert_eq!(pack.len(), 1);
        assert_eq!(text_in(Some(&pack), Lang::De, "prefs.title"), "Einstellungen");
        assert_eq!(text_in(Some(&pack), Lang::De, "prefs.open"), "Preferences…");
        assert_eq!(text_in(Some(&pack), Lang::De, "no.such.key"), "no.such.key");
        // retoque-data's shared messages too.
        assert_eq!(
            text_in(Some(&pack), Lang::De, "rs.undo_addon"),
            t(Lang::En, "rs.undo_addon")
        );
        assert_eq!(t(Lang::Pt, "prefs.open"), "Preferências…");
    }

    #[test]
    fn settings_and_their_values_are_translated() {
        let stage = retoque_data::settings::all()
            .iter()
            .find(|s| s.section == "DlssNr" && s.key == "Stage")
            .unwrap();
        assert_eq!(setting_text(Lang::En, stage), (stage.label_en, stage.help_en));
        assert_eq!(setting_text(Lang::Pt, stage), (stage.label_pt, stage.help_pt));
        let (en, pt) = stage.choice_labels("1").unwrap();
        assert_eq!(choice_key(en), "choice.before_the_upscaler_experimental");
        assert_eq!(choice_text(Lang::Pt, en, pt), pt);
        for (lang, _) in drafts() {
            let (label, help) = setting_text(lang, stage);
            assert!(label != stage.label_en && help != stage.help_en, "{lang:?}");
            assert_ne!(choice_text(lang, en, pt), en, "{lang:?}");
        }
    }

    #[test]
    fn locales_choose_a_supported_language() {
        for (locale, lang) in [
            ("en-US", Some(Lang::En)),
            ("pt-BR", Some(Lang::Pt)),
            ("pt_PT.UTF-8", Some(Lang::Pt)),
            ("de-DE", Some(Lang::De)),
            ("de_AT.UTF-8@euro", Some(Lang::De)),
            ("es-419", Some(Lang::Es)),
            ("fr-CA", Some(Lang::Fr)),
            ("ja-JP", Some(Lang::Ja)),
            ("ko-KR", Some(Lang::Ko)),
            ("pl-PL", Some(Lang::Pl)),
            ("ru-RU", Some(Lang::Ru)),
            ("zh-CN", Some(Lang::ZhCn)),
            ("zh-Hans-SG", Some(Lang::ZhCn)),
            ("zh", Some(Lang::ZhCn)),
            ("zh-TW", None),
            ("zh-Hant-HK", None),
            ("it-IT", None),
            ("C", None),
            ("", None),
            ("deu", None),
        ] {
            assert_eq!(Lang::from_locale(locale), lang, "{locale}");
        }
        for lang in Lang::ALL {
            assert_eq!(Lang::from_locale(lang.code()), Some(lang));
        }
    }

    #[test]
    fn the_saved_choice_keeps_its_spelling() {
        // Older saves hold "En" or "Pt"; a renamed variant would silently reset someone's language.
        assert_eq!(serde_json::from_str::<Lang>("\"En\"").unwrap(), Lang::En);
        assert_eq!(serde_json::from_str::<Lang>("\"Pt\"").unwrap(), Lang::Pt);
        assert_eq!(serde_json::to_string(&Lang::ZhCn).unwrap(), "\"ZhCn\"");
        for lang in Lang::ALL {
            let json = serde_json::to_string(&lang).unwrap();
            assert_eq!(serde_json::from_str::<Lang>(&json).unwrap(), lang);
        }
        let mut names: Vec<_> = Lang::ALL.iter().map(|l| l.native_name()).collect();
        names.dedup();
        assert_eq!(names.len(), Lang::ALL.len());
        assert_eq!(Lang::Pt.native_name(), "Português (Brasil)");
    }

    #[test]
    fn counted_words_follow_each_language() {
        let category = |lang, n| plural_category(lang, n);
        assert_eq!(
            [1, 2, 5, 12, 22, 25].map(|n| category(Lang::Pl, n)),
            ["one", "few", "many", "many", "few", "many"]
        );
        assert_eq!(
            [1, 21, 11, 3, 14, 0].map(|n| category(Lang::Ru, n)),
            ["one", "one", "many", "few", "many", "many"]
        );
        assert_eq!([0, 1, 2].map(|n| category(Lang::Fr, n)), ["one", "one", "other"]);
        assert_eq!([0, 1, 2].map(|n| category(Lang::Pt, n)), ["other", "one", "other"]);
        assert_eq!([1, 2].map(|n| category(Lang::Ja, n)), ["other", "other"]);
        assert_eq!(plural(Lang::En, 1, "shell.games_one", "shell.games_other"), "game");
        assert_eq!(plural(Lang::Pt, 3, "shell.games_one", "shell.games_other"), "jogos");
        for lang in [Lang::Pl, Lang::Ru] {
            let few = plural(lang, 3, "shell.games_one", "shell.games_other");
            let many = plural(lang, 5, "shell.games_one", "shell.games_other");
            assert_ne!(few, many, "{lang:?} has its own form for 2–4");
        }
    }

    #[test]
    fn pack_text_draws_without_missing_glyphs() {
        // egui's own fonts cover Latin and Cyrillic; Chinese, Japanese and Korean come from a system font, so they
        // are checked only where one is installed.
        for (lang, _) in drafts() {
            if !crate::fonts::can_show(lang) {
                eprintln!("{lang:?}: no CJK system font here; not drawn");
                continue;
            }
            let ctx = egui::Context::default();
            crate::fonts::apply(&ctx, lang);
            ctx.set_missing_glyph_policy(egui::MissingGlyphPolicy::Panic);
            let mut texts: Vec<&str> = pack(lang).unwrap().values().copied().collect();
            texts.sort_unstable();
            for _ in 0..2 {
                let mut output = ctx.run_ui(Default::default(), |ui| {
                    for text in &texts {
                        ui.label(*text);
                    }
                });
                output.textures_delta.clear();
            }
        }
    }

    /// Run with `--ignored` to list what each pack still shows in English.
    #[test]
    #[ignore = "a report for translators: packs may lag behind the English table"]
    fn untranslated_keys() {
        let english = english();
        let mut missing = 0;
        for (lang, _) in drafts() {
            let pack = pack(lang).unwrap();
            let mut keys: Vec<_> = english.keys().filter(|k| !pack.contains_key(k.as_str())).collect();
            keys.sort();
            missing += keys.len();
            println!("{}: {} untranslated {keys:?}", lang.code(), keys.len());
        }
        assert_eq!(missing, 0);
    }
}

/// Translate known engine messages without changing filenames, configuration values or original log records.
/// Unknown OS/driver/receipt diagnostics remain verbatim so a report retains the actual failure evidence.
pub fn diagnostic(lang: Lang, message: &str) -> String {
    crate::applog::shown_error(message);
    if lang.is_draft() {
        // retoque-data only knows English and Portuguese: translate its ReShade reasons through the pack.
        if let Some((key, en, _)) = retoque_data::messages::TEXT
            .iter()
            .find(|(key, en, _)| key.starts_with("rs.error.") && message.contains(en))
        {
            return message.replace(en, t(lang, key));
        }
    } else {
        let translated = retoque_data::messages::reshade_diagnostic(message, lang.data_language());
        if translated != message {
            return translated;
        }
    }

    if let Some((key, en, _)) = TEXT
        .iter()
        .find(|(key, en, _)| key.starts_with("dlss.error.") && message.contains(en))
    {
        return message.replace(en, t(lang, key));
    }
    if let Some((key, _, _)) = TEXT.iter().find(|(key, en, _)| {
        (key.starts_with("sm86.error.") || key.starts_with("payload.error.") || key.starts_with("nr_fetch.error."))
            && *en == message
    }) {
        return t(lang, key).into();
    }
    for key in [
        "undo.exact",
        "undo.conservative",
        "undo.provenance_mismatch",
        "undo.invalid_provenance",
        "undo.stale_provenance",
        "undo.stale_value",
        "note.sm86_upgrade",
        "note.sm86_install",
        "note.resume",
        "note.driver",
        "note.model",
        "note.vk_skip",
        "note.vk_disabled",
        "note.vk_active",
        "keep.same",
        "keep.model",
        "keep.gone",
        "keep.identical_component",
        "keep.user_settings",
        "keep.changed",
        "keep.no_backup",
        "keep.no_rename",
        "keep.rename_changed",
        "ref.ini_missing",
        "ref.ini_value",
    ] {
        if message == t(Lang::En, key) {
            return t(lang, key).into();
        }
    }
    for key in [
        "note.windows_denied",
        "note.second_proxy",
        "keep.destination",
        "keep.blocked",
        "ref.proxy",
    ] {
        if let Some((prefix, suffix)) = t(Lang::En, key).split_once("{path}")
            && let Some(path) = message.strip_prefix(prefix).and_then(|s| s.strip_suffix(suffix))
        {
            return t(lang, key).replace("{path}", path);
        }
    }
    message.to_owned()
}

#[cfg(test)]
mod source_coverage {
    use super::*;

    fn sources(dir: &std::path::Path, out: &mut Vec<(String, String)>) {
        for entry in std::fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            if path.is_dir() {
                sources(&path, out);
            } else if path.extension().is_some_and(|e| e == "rs") {
                out.push((path.display().to_string(), std::fs::read_to_string(&path).unwrap()));
            }
        }
    }

    // Tokenize only enough Rust to find literal translation arguments, ignoring comments and strings containing
    // sample code. This avoids a regex dependency and handles multiline calls and qualified Lang expressions.
    fn literal_keys(source: &str) -> Vec<String> {
        let bytes = source.as_bytes();
        let mut tokens: Vec<(bool, String)> = Vec::new();
        let mut i = 0;
        while i < bytes.len() {
            if bytes[i..].starts_with(b"//") {
                i += bytes[i..].iter().position(|b| *b == b'\n').unwrap_or(bytes.len() - i);
            } else if bytes[i..].starts_with(b"/*") {
                i += 2;
                let mut depth = 1;
                while i < bytes.len() && depth > 0 {
                    if bytes[i..].starts_with(b"/*") {
                        depth += 1;
                        i += 2;
                    } else if bytes[i..].starts_with(b"*/") {
                        depth -= 1;
                        i += 2;
                    } else {
                        i += 1;
                    }
                }
            } else if bytes[i] == b'"' {
                i += 1;
                let start = i;
                while i < bytes.len() && bytes[i] != b'"' {
                    if bytes[i] == b'\\' {
                        i += 1;
                    }
                    i += 1;
                }
                tokens.push((true, source[start..i.min(bytes.len())].to_owned()));
                i += 1;
            } else if bytes[i] == b'\'' && bytes.get(i + 2) == Some(&b'\'') {
                i += 3;
            } else if bytes[i..].starts_with(b"'\\") && bytes.get(i + 3) == Some(&b'\'') {
                i += 4;
            } else if bytes[i].is_ascii_alphabetic() || bytes[i] == b'_' {
                let start = i;
                while i < bytes.len() && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_') {
                    i += 1;
                }
                tokens.push((false, source[start..i].to_owned()));
            } else {
                if !bytes[i].is_ascii_whitespace() {
                    tokens.push((false, char::from(bytes[i]).to_string()));
                }
                i += 1;
            }
        }
        let mut keys = Vec::new();
        for (at, (literal, name)) in tokens.iter().enumerate() {
            if *literal || !matches!(name.as_str(), "t" | "tr") || tokens.get(at + 1).is_none_or(|t| t.1 != "(") {
                continue;
            }
            let mut argument = at + 2;
            if name == "t" {
                let mut depth = 0;
                while let Some((_, token)) = tokens.get(argument) {
                    if token == "," && depth == 0 {
                        argument += 1;
                        break;
                    }
                    if token == ")" && depth == 0 {
                        break;
                    }
                    if token == "(" {
                        depth += 1;
                    }
                    if token == ")" {
                        depth -= 1;
                    }
                    argument += 1;
                }
            }
            if let Some((true, key)) = tokens.get(argument) {
                keys.push(key.clone());
            }
        }
        keys
    }

    #[test]
    fn every_literal_translation_call_has_both_languages() {
        let mut all_sources = Vec::new();
        sources(
            std::path::Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/src")),
            &mut all_sources,
        );
        assert!(all_sources.len() >= 13);
        let mut checked = 0;
        for (file, source) in &all_sources {
            for key in literal_keys(source) {
                let entry = TEXT
                    .iter()
                    .chain(retoque_data::messages::TEXT)
                    .find(|(k, _, _)| *k == key);
                let (_, en, pt) = entry.unwrap_or_else(|| panic!("{file}: missing translation key {key}"));
                assert!(!en.is_empty() && !pt.is_empty(), "{file}: incomplete translation {key}");
                checked += 1;
            }
        }
        assert!(
            checked > 80,
            "translation-call scanner unexpectedly stopped finding calls"
        );
    }

    #[test]
    fn every_key_is_used_somewhere_in_the_crate() {
        // Text nothing shows goes stale and gets revived with old wording. A key counts as used when the crate
        // quotes it, or quotes the prefix it is built from at run time ("sm86.error." in diagnostic()).
        // Recommendation keys are named in retoque-data.
        let mut all_sources = Vec::new();
        sources(
            std::path::Path::new(concat!(env!("CARGO_MANIFEST_DIR"), "/src")),
            &mut all_sources,
        );
        let mut code = String::from(include_str!("../../retoque-data/src/recommend.rs"));
        for (file, source) in &all_sources {
            if file.ends_with("i18n.rs") {
                // The table itself is not a use.
                let start = source.find("const TEXT").unwrap();
                let end = start + source[start..].find("\n];").unwrap();
                code.push_str(&source[..start]);
                code.push_str(&source[end..]);
            } else {
                code.push_str(source);
            }
        }
        let quoted = |text: &str| code.contains(&format!("\"{text}\""));
        let unused: Vec<_> = TEXT
            .iter()
            .map(|(key, _, _)| *key)
            .filter(|key| {
                !quoted(key)
                    && !key
                        .rsplit_once('.')
                        .is_some_and(|(prefix, _)| quoted(&format!("{prefix}.")))
            })
            .collect();
        assert!(unused.is_empty(), "translation keys nothing uses: {unused:?}");
    }

    #[test]
    fn scanner_finds_multiline_calls_and_ignores_examples() {
        let source = "// t(lang, \"ignored\")\n t(\n Lang::En,\n \"set.save\"); self.tr (\"set.apply\"); let s = \"tr(\\\"ignored\\\")\"; t(lang, \"missing.key\")";
        assert_eq!(literal_keys(source), vec!["set.save", "set.apply", "missing.key"]);
        assert!(TEXT.iter().all(|(key, _, _)| *key != "missing.key"));
    }

    #[test]
    fn diagnostics_translate_known_reasons_without_translating_paths() {
        assert_eq!(diagnostic(Lang::Pt, "already gone"), "já foi removido");
        assert_eq!(
            diagnostic(Lang::Pt, "dxgi.dll: not restored, a file is still there"),
            "dxgi.dll: não restaurado porque já existe um arquivo no destino"
        );
        assert_eq!(
            diagnostic(Lang::Pt, "driver result 0xBAD00001"),
            "driver result 0xBAD00001"
        );
    }
}
