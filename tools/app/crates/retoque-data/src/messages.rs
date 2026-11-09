//! Shared diagnostic text for CLI and future GUI consumers; no GUI dependency.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Language {
    English,
    Portuguese,
}
pub fn text(key: &str, language: Language) -> &str {
    TEXT.iter()
        .find(|entry| entry.0 == key)
        .map_or(key, |entry| match language {
            Language::English => entry.1,
            Language::Portuguese => entry.2,
        })
}
pub const TEXT: &[(&str, &str, &str)] = &[
    ("rs.undo_addon", "Undo add-on changes", "Desfazer alterações do add-on"),
    (
        "rs.error.restored",
        "original already restored",
        "original já restaurado",
    ),
    (
        "rs.error.0",
        "ReShade size or SHA-256 mismatch",
        "Tamanho ou SHA-256 do ReShade divergente",
    ),
    (
        "rs.error.1",
        "ReShade input exceeds size cap",
        "Entrada do ReShade excede o limite de tamanho",
    ),
    (
        "rs.error.2",
        "ReShade input is not a PE executable",
        "A entrada do ReShade não é um executável PE",
    ),
    (
        "rs.error.3",
        "ReShade setup has no appended ZIP",
        "O instalador do ReShade não contém um ZIP anexado",
    ),
    (
        "rs.error.4",
        "ReShade ZIP has too many entries",
        "O ZIP do ReShade contém entradas demais",
    ),
    (
        "rs.error.5",
        "ambiguous or linked ReShade64.dll",
        "ReShade64.dll ambíguo ou um link",
    ),
    (
        "rs.error.6",
        "ReShade64.dll missing from setup",
        "ReShade64.dll ausente no instalador",
    ),
    (
        "rs.error.7",
        "ReShade DLL exceeds size cap",
        "A DLL do ReShade excede o limite de tamanho",
    ),
    ("rs.error.8", "invalid ReShade DLL", "DLL do ReShade inválida"),
    (
        "rs.error.9",
        "ReShade file exceeds size cap",
        "O arquivo do ReShade excede o limite de tamanho",
    ),
    (
        "rs.error.10",
        "invalid ReShade catalog id",
        "Identificador do catálogo ReShade inválido",
    ),
    (
        "rs.error.11",
        "ReShade catalog integrity failure",
        "Falha na integridade do catálogo ReShade",
    ),
    (
        "rs.error.12",
        "unrecognized ReShade DLL; require an x64 DLL with ReShade identification",
        "DLL do ReShade não reconhecida; é necessária uma DLL x64 com identificação ReShade",
    ),
    (
        "rs.error.13",
        "ReShade cache disappeared",
        "O cache do ReShade desapareceu",
    ),
    (
        "rs.error.14",
        "invalid ReShade filename",
        "Nome de arquivo ReShade inválido",
    ),
    (
        "rs.error.15",
        "ReShade download requires confirmation",
        "O download do ReShade exige confirmação",
    ),
    (
        "rs.error.16",
        "ReShade input is a link or not a regular file",
        "A entrada do ReShade é um link ou não é um arquivo regular",
    ),
    (
        "rs.error.17",
        "ReShade input exceeds size limit",
        "A entrada do ReShade excede o limite de tamanho",
    ),
    (
        "rs.error.18",
        "ambiguous ReShade filename casing",
        "Nomes de arquivo ReShade ambíguos por diferenças de maiúsculas",
    ),
    (
        "rs.error.19",
        "AddonPath uses an unresolved Wine drive or network path",
        "AddonPath usa uma unidade Wine não resolvida ou caminho de rede",
    ),
    ("rs.error.20", "AddonPath is a link", "AddonPath é um link"),
    (
        "rs.error.21",
        "add-on scan limit exceeded",
        "Limite de busca de add-ons excedido",
    ),
    (
        "rs.error.22",
        "non UTF-8 add-on filename",
        "Nome de add-on não codificado em UTF-8",
    ),
    (
        "rs.error.23",
        "ambiguous add-on filename casing",
        "Nomes de add-on ambíguos por diferenças de maiúsculas",
    ),
    (
        "rs.error.24",
        "add-on is a link or not a regular file",
        "O add-on é um link ou não é um arquivo regular",
    ),
    (
        "rs.error.25",
        "add-on inspection exceeds size budget",
        "A inspeção dos add-ons excede o limite de tamanho",
    ),
    (
        "rs.error.26",
        "ReShade state is not unchanged receipt-owned data",
        "O estado do ReShade foi alterado ou não pertence ao recibo",
    ),
    ("rs.error.27", "invalid ReShade state", "Estado do ReShade inválido"),
    (
        "rs.error.28",
        "ReShade undo history exceeds limit",
        "Histórico de reversão do ReShade excede o limite",
    ),
    (
        "rs.error.29",
        "conflicting ReShade/add-on destination casing",
        "Conflito de maiúsculas no destino ReShade/add-on",
    ),
    (
        "rs.error.30",
        "ReShade/add-on input is not an x64 PE DLL",
        "A entrada ReShade/add-on não é uma DLL PE x64",
    ),
    (
        "rs.error.31",
        "Special K is present; it owns ReShade loading",
        "Special K está presente e controla o carregamento do ReShade",
    ),
    (
        "rs.error.32",
        "another ReShade loader is present; use the existing keep_reshade migration first",
        "Outro carregador ReShade está presente; use primeiro a migração existente keep_reshade",
    ),
    (
        "rs.error.33",
        "ReShade requires the proxy and executable in the same directory",
        "ReShade exige o proxy e o executável no mesmo diretório",
    ),
    (
        "rs.error.34",
        "install Retoque before its ReShade component",
        "Instale o Retoque antes de seu componente ReShade",
    ),
    (
        "rs.error.35",
        "OptiScaler.ini is missing",
        "OptiScaler.ini está ausente",
    ),
    (
        "rs.error.36",
        "invalid lifecycle INI path",
        "Caminho de INI de reversão inválido",
    ),
    (
        "rs.error.37",
        "invalid lifecycle edit identity",
        "Identidade da edição de reversão inválida",
    ),
    (
        "rs.error.38",
        "LoadReshade changed since the reviewed install; preserve the undo state",
        "LoadReshade mudou desde a instalação revisada; preserve o estado de reversão",
    ),
    (
        "rs.error.39",
        "component was not imported by Retoque",
        "O componente não foi importado pelo Retoque",
    ),
    (
        "rs.error.40",
        "component state identity mismatch",
        "Identidade do estado do componente divergente",
    ),
    (
        "rs.error.41",
        "imported file changed; no files were removed",
        "O arquivo importado mudou; nenhum arquivo foi removido",
    ),
    (
        "rs.error.42",
        "ReShade backup changed; preserve the receipt",
        "O backup do ReShade mudou; preserve o recibo",
    ),
    (
        "rs.error.43",
        "select an .addon64 or .addon file with a safe filename",
        "Selecione um arquivo .addon64 ou .addon com nome seguro",
    ),
    (
        "rs.error.44",
        "AddonPath differs from the game folder; importing here would not load the add-on",
        "AddonPath difere da pasta do jogo; importar aqui não carregaria o add-on",
    ),
    (
        "rs.error.45",
        "invalid add-on filename",
        "Nome de arquivo do add-on inválido",
    ),
    (
        "rs.error.46",
        "add-on is not in ReShade's AddonPath",
        "O add-on não está no AddonPath do ReShade",
    ),
    (
        "rs.error.47",
        "ReShade build is limited or unknown; full add-on support is not established",
        "O build do ReShade é limitado ou desconhecido; o suporte completo a add-ons não está confirmado",
    ),
    (
        "rs.error.48",
        "Conservative INI undo: other edits are preserved; restoration is not byte-for-byte",
        "Reversão conservadora do INI: outras edições são preservadas; a restauração não é byte a byte",
    ),
    (
        "rs.error.49",
        "identical existing file; preserve its original ownership",
        "Arquivo existente idêntico; preservar sua propriedade original",
    ),
    ("rs.error.50", "invalid lifecycle INI undo", "Reversão do INI inválida"),
    (
        "rs.error.51",
        "unknown ReShade/add-on option",
        "Opção ReShade/add-on desconhecida",
    ),
    (
        "rs.error.52",
        "too many ReShade/add-on arguments",
        "Argumentos ReShade/add-on demais",
    ),
    ("rs.error.53", "addon add needs a file", "addon add exige um arquivo"),
    (
        "rs.error.54",
        "add-on filename required",
        "É necessário informar o nome do add-on",
    ),
    (
        "rs.error.55",
        "unknown ReShade/add-on action",
        "Ação ReShade/add-on desconhecida",
    ),
    ("rs.title", "ReShade and add-ons", "ReShade e add-ons"),
    (
        "rs.description",
        "Retoque keeps the game proxy and loads ReShade64.dll.",
        "O Retoque mantém o proxy do jogo e carrega ReShade64.dll.",
    ),
    ("rs.enabled", "Enabled", "Ativado"),
    ("rs.disabled", "Disabled", "Desativado"),
    ("rs.full", "Full add-on support", "Suporte completo a add-ons"),
    (
        "rs.limited",
        "Limited build: external add-ons cannot load",
        "Build limitado: add-ons externos não carregam",
    ),
    ("rs.unknown", "Unknown ReShade build", "Build do ReShade desconhecido"),
    ("rs.missing", "ReShade is missing", "ReShade ausente"),
    (
        "rs.recommend",
        "Install ReShade with add-on support",
        "Instalar ReShade com suporte a add-ons",
    ),
    (
        "rs.recommend_why",
        "Add-ons are present, but full ReShade support is missing or unconfirmed. Review installation separately.",
        "Há add-ons, mas o suporte completo do ReShade está ausente ou não confirmado. Revise a instalação separadamente.",
    ),
    (
        "rs.static",
        "Static evidence, medium confidence. Restart the game after changes; loading and compatibility remain untested.",
        "Evidência estática, confiança média. Reinicie o jogo após alterações; carregamento e compatibilidade ainda não foram testados.",
    ),
    ("rs.addons", "Add-ons", "Add-ons"),
    ("rs.build", "ReShade build", "Build do ReShade"),
    (
        "rs.install",
        "Review ReShade installation",
        "Revisar instalação do ReShade",
    ),
    ("rs.uninstall", "Remove ReShade component", "Remover componente ReShade"),
    (
        "rs.import",
        "Import local ReShade DLL or setup",
        "Importar DLL ou instalador local do ReShade",
    ),
    ("rs.add", "Import add-on", "Importar add-on"),
    ("rs.remove_addon", "Remove imported add-on", "Remover add-on importado"),
    (
        "rs.review",
        "Review ReShade / add-on changes",
        "Revisar alterações do ReShade / add-on",
    ),
    (
        "rs.drop_addon",
        "Copy this add-on to the selected game through a reviewed plan.",
        "Copiar este add-on para o jogo selecionado mediante revisão do plano.",
    ),
    (
        "rs.select_game",
        "Select a game before importing an add-on.",
        "Selecione um jogo antes de importar um add-on.",
    ),
    ("rs.nr_off", "Neural Rendering: off", "Neural Rendering: desligado"),
    (
        "rs.nr_addon",
        "Neural Rendering: by the add-on RenoDX DLSS5",
        "Neural Rendering: pelo add-on RenoDX DLSS5",
    ),
    (
        "rs.nr_retoque",
        "Neural Rendering: by Retoque",
        "Neural Rendering: pelo Retoque",
    ),
    (
        "rs.input_title",
        "ReShade's menu takes the mouse from the Retoque menu",
        "O menu do ReShade tira o mouse do menu do Retoque",
    ),
    (
        "rs.input_why",
        "With ReShade's input processing on \"block all\" (its default), while its overlay is open the Retoque menu gets no mouse or keyboard. Setting it to block only while the cursor is over ReShade's own windows lets both menus be used together.",
        "Com o processamento de entrada do ReShade em \"bloquear tudo\" (o padrão dele), enquanto o menu dele está aberto o menu do Retoque não recebe mouse nem teclado. Bloquear só quando o cursor está sobre as janelas do ReShade deixa os dois menus funcionarem juntos.",
    ),
    ("rs.input_mode", "ReShade.ini [INPUT]", "ReShade.ini [INPUT]"),
    (
        "rs.input_fix",
        "Let both menus use the mouse",
        "Deixar os dois menus usarem o mouse",
    ),
    (
        "rs.yield",
        "Let the NR add-on take over (default)",
        "Deixar o add-on NR assumir (padrão)",
    ),
    (
        "rs.ignore",
        "Keep Retoque NR (experts)",
        "Manter o NR do Retoque (avançado)",
    ),
    (
        "rs.consent",
        "Download ReShade 6.8.0 with full add-on support from reshade.me? The 4.3 MB official installer and extracted DLL have pinned SHA-256 hashes. BSD-3-Clause applies. Game changes require a separate review.",
        "Baixar ReShade 6.8.0 com suporte completo a add-ons de reshade.me? O instalador oficial de 4,3 MB e a DLL extraída têm hashes SHA-256 fixados. Aplica-se a licença BSD-3-Clause. Alterações no jogo exigem revisão separada.",
    ),
    (
        "rs.confirm_download",
        "Confirm official download",
        "Confirmar download oficial",
    ),
    ("rs.preparing", "Preparing ReShade…", "Preparando ReShade…"),
    (
        "rs.cli_confirm",
        "Type yes to confirm the download:",
        "Digite sim para confirmar o download:",
    ),
    (
        "rs.download_first",
        "Import or download ReShade first; dry-run does not download.",
        "Importe ou baixe o ReShade primeiro; dry-run não faz download.",
    ),
    (
        "rs.dry",
        "Dry run: no game or catalog changes.",
        "Simulação: nenhuma alteração no jogo ou catálogo.",
    ),
    (
        "rs.done",
        "Reviewed changes applied.",
        "Alterações revisadas aplicadas.",
    ),
    ("severity.warning", "Warning", "Aviso"),
    ("severity.error", "Error", "Erro"),
    (
        "doctor.journal_unreadable.why",
        "A pending journal exists but core refused to inspect this installation. Preserve it and review reconciliation; other checks are unavailable.",
        "Há um journal pendente, mas o núcleo recusou inspecionar a instalação. Preserve-o e revise a reconciliação; as demais verificações estão indisponíveis.",
    ),
    ("log.load.title", "NR model did not load", "Modelo NR não carregou"),
    (
        "log.load.why",
        "The direct loader could not load the selected model. Check the file and its dependencies; compatibility alone does not identify the cause.",
        "O carregador direto não carregou o modelo selecionado. Verifique o arquivo e suas dependências; a compatibilidade sozinha não identifica a causa.",
    ),
    (
        "doctor.healthy",
        "No findings in the available evidence; uninspected inputs remain unknown.",
        "Nenhum problema nas evidências disponíveis; entradas não inspecionadas permanecem desconhecidas.",
    ),
    (
        "doctor.review",
        "Review a fresh plan before any repair.",
        "Revise um novo plano antes de qualquer reparo.",
    ),
    (
        "doctor.unfinished.title",
        "Unfinished installation",
        "Instalação incompleta",
    ),
    (
        "doctor.unfinished.why",
        "The receipt or pending journal records unfinished work. Reconcile through the installer.",
        "O recibo ou journal pendente registra trabalho incompleto. Reconcilie pelo instalador.",
    ),
    (
        "doctor.version.title",
        "Proxy and forwarder versions differ",
        "Versões do proxy e forwarder diferentes",
    ),
    (
        "doctor.version.why",
        "Prepare an update of both files from the same release.",
        "Prepare a atualização dos dois arquivos com a mesma versão.",
    ),
    ("doctor.model_missing.title", "NR model missing", "Modelo NR ausente"),
    (
        "doctor.model_missing.why",
        "NR is enabled but its selected model is absent. Disable NR or select an available model.",
        "NR está ligado, mas o modelo selecionado está ausente. Desligue NR ou selecione um modelo disponível.",
    ),
    (
        "doctor.model_incompatible.title",
        "NR model incompatible with GPU",
        "Modelo NR incompatível com a GPU",
    ),
    (
        "doctor.model_incompatible.why",
        "Static CUDA targets do not match. A catalog alternative, when offered, is still untested in this game.",
        "Os alvos CUDA estáticos não correspondem. A alternativa do catálogo, quando oferecida, ainda não foi testada neste jogo.",
    ),
    (
        "doctor.lsfg_ready.title",
        "LSFG assets are not ready",
        "Arquivos do LSFG não estão prontos",
    ),
    (
        "doctor.lsfg_ready.why",
        "LSFG is enabled without the required assets or shader cache. Disable it until ready.",
        "LSFG está ligado sem os arquivos ou cache de shaders necessários. Desligue até estarem prontos.",
    ),
    (
        "doctor.lsfg_output.title",
        "LSFG output setting conflicts",
        "Saída incompatível com LSFG",
    ),
    (
        "doctor.lsfg_output.why",
        "LSFG requires FrameGen FGOutput=fsrfg. This setting alone does not prove readiness.",
        "LSFG exige FrameGen FGOutput=fsrfg. Esse ajuste sozinho não comprova que esteja pronto.",
    ),
    (
        "doctor.fg_conflict.title",
        "Conflicting frame generation providers",
        "Provedores de geração de quadros em conflito",
    ),
    (
        "doctor.fg_conflict.why",
        "sm86 is installed and LSFG is enabled. Review disabling LSFG before using sm86.",
        "sm86 está instalado e LSFG está ligado. Revise a desativação de LSFG antes de usar sm86.",
    ),
    (
        "doctor.launch.title",
        "Wine proxy override missing",
        "Override do proxy Wine ausente",
    ),
    (
        "doctor.launch.why",
        "The inspected launch configuration lacks the required override. Copy the shown option to the launcher.",
        "A configuração de execução inspecionada não tem o override necessário. Copie a opção mostrada para o launcher.",
    ),
    ("log.vulkan.title", "Vulkan device lost", "Dispositivo Vulkan perdido"),
    (
        "log.vulkan.why",
        "The NR resize drain reported device loss. Review disabling NR for a comparison run; this does not establish the cause.",
        "A drenagem de NR no redimensionamento registrou perda do dispositivo. Revise desligar NR para comparar; isso não estabelece a causa.",
    ),
    ("log.ngx.title", "NGX initialization failed", "Inicialização NGX falhou"),
    (
        "log.ngx.why",
        "The Vulkan model initialization returned failure. Inspect model and driver compatibility.",
        "A inicialização do modelo Vulkan retornou falha. Verifique a compatibilidade do modelo e driver.",
    ),
    (
        "log.model.title",
        "NR model operation failed",
        "Operação do modelo NR falhou",
    ),
    (
        "log.model.why",
        "CPU coverage records failed model operations. This alone does not identify a loading error or its cause.",
        "A cobertura de CPU registra operações do modelo com falha. Isso sozinho não identifica erro de carregamento nem sua causa.",
    ),
    (
        "log.model.incompatible",
        "Failed model operations coincide with a statically incompatible model. Review a compatible catalog model; causality remains unverified.",
        "Falhas do modelo coincidem com incompatibilidade estática. Revise um modelo compatível do catálogo; a causa ainda não foi comprovada.",
    ),
    (
        "log.pre.title",
        "NR pre-upscale fallback observed",
        "Fallback de NR antes do upscale observado",
    ),
    (
        "log.pre.why",
        "Pre-upscale coverage recorded fallback. Review Stage=0 to use the post-upscale route.",
        "A cobertura antes do upscale registrou fallback. Revise Stage=0 para usar a rota após o upscale.",
    ),
    (
        "log.nvngx.title",
        "Wine driver NGX file absent",
        "Arquivo NGX do driver Wine ausente",
    ),
    (
        "log.nvngx.why",
        "The driver probe did not initialize and inspection found no driver NGX file. Check the Wine driver installation; the probe alone is not proof.",
        "A sondagem do driver não inicializou e a inspeção não encontrou o arquivo NGX do driver. Verifique a instalação Wine do driver; a sondagem sozinha não é prova.",
    ),
    (
        "confidence.verified",
        "Verified in the last session",
        "Verificado na última sessão",
    ),
    (
        "confidence.static",
        "Static match, untested",
        "Correspondência estática, não testada",
    ),
    (
        "confidence.proton",
        "Experimental under Proton/Wine",
        "Experimental sob Proton/Wine",
    ),
    (
        "confidence.sm86_proton",
        "sm86 0.4.1 documents Windows 10/11 x64 and D3D12. Its Vulkan note describes limited coverage and community patches, not Proton qualification.",
        "sm86 0.4.1 documenta Windows 10/11 x64 e D3D12. A nota de Vulkan descreve cobertura limitada e patches da comunidade, sem homologar Proton.",
    ),
    (
        "evidence.installed",
        "Recognized installation",
        "Instalação reconhecida",
    ),
    ("evidence.tracked", "Tracked receipt", "Recibo rastreado"),
    ("evidence.runtime", "Runtime", "Ambiente de execução"),
    ("evidence.dlss", "DLSS DLL present", "DLL DLSS presente"),
    ("evidence.dlssg", "DLSSG DLL present", "DLL DLSSG presente"),
    ("evidence.sm86", "sm86 installed", "sm86 instalado"),
    ("evidence.sm86_legacy", "Legacy sm86", "sm86 legado"),
    ("evidence.sm86_docs", "sm86 documentation", "Documentação sm86"),
    ("evidence.lsfg", "LSFG enabled", "LSFG ligado"),
    ("evidence.gpu", "GPU identity", "Identidade da GPU"),
    ("evidence.api", "Last session API", "API da última sessão"),
    ("evidence.receipt", "Receipt status", "Estado do recibo"),
    ("evidence.pending", "Pending journal", "Journal pendente"),
    ("evidence.proxy_version", "Proxy version", "Versão do proxy"),
    ("evidence.forwarder_version", "Forwarder version", "Versão do forwarder"),
    ("evidence.nr_enabled", "NR enabled", "NR ligado"),
    (
        "evidence.model_present",
        "Selected model present",
        "Modelo selecionado presente",
    ),
    ("evidence.model_targets", "Model CUDA targets", "Alvos CUDA do modelo"),
    (
        "evidence.catalog_model",
        "Alternative model SHA256",
        "SHA256 do modelo alternativo",
    ),
    ("evidence.lsfg_ready", "LSFG readiness", "Disponibilidade LSFG"),
    (
        "evidence.fg_output",
        "Frame generation output",
        "Saída de geração de quadros",
    ),
    (
        "evidence.launch_option",
        "Required launch option",
        "Opção de execução necessária",
    ),
    (
        "evidence.override",
        "Launch override present",
        "Override de execução presente",
    ),
    ("evidence.log", "Redacted log line", "Linha de log anonimizada"),
    ("severity.info", "Information", "Informação"),
    (
        "doctor.dlss.title",
        "A newer DLSS is available",
        "Uma versão mais nova do DLSS está disponível",
    ),
    (
        "doctor.dlss.why",
        "NVIDIA DLSS 310.9.16 can replace older SR/RR/FG files after review. Game compatibility is not guaranteed.",
        "O NVIDIA DLSS 310.9.16 pode substituir arquivos SR/RR/FG antigos após revisão. A compatibilidade com o jogo não é garantida.",
    ),
    (
        "doctor.dlss_root.title",
        "DLSS root receipt needs recovery",
        "O recibo DLSS na raiz precisa de recuperação",
    ),
    (
        "doctor.dlss_root.why",
        "A pending or incomplete DLSS operation remains at the game installation root. Review restoration of both receipts.",
        "Há uma operação DLSS pendente ou incompleta na raiz do jogo. Revise a restauração dos dois recibos.",
    ),
    ("evidence.dlss_file_version", "Game DLSS version", "Versão DLSS do jogo"),
    (
        "evidence.community_model",
        "Community model SHA256",
        "SHA256 do modelo da comunidade",
    ),
];
/// Translate ReShade diagnostics without changing OS errors, paths or unrecognized evidence.
pub fn reshade_diagnostic(message: &str, language: Language) -> String {
    if language == Language::English {
        return message.into();
    }
    for (key, en, pt) in TEXT {
        if key.starts_with("rs.error.") && message.contains(en) {
            return message.replace(en, pt);
        }
    }
    message.into()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn all_diagnostic_keys_have_both_languages() {
        let mut keys = std::collections::HashSet::new();
        for &(key, en, pt) in TEXT {
            assert!(keys.insert(key));
            assert!(!en.is_empty() && !pt.is_empty());
        }
        for source in [
            include_str!("doctor.rs"),
            include_str!("logs.rs"),
            include_str!("recommend.rs"),
        ] {
            for key in source.split('"').filter(|s| {
                s.starts_with("doctor.")
                    || s.starts_with("log.")
                    || s.starts_with("evidence.")
                    || s.starts_with("confidence.")
                    || *s == "confidence.sm86_proton"
            }) {
                for lang in [Language::English, Language::Portuguese] {
                    assert_ne!(text(key, lang), key, "{key}");
                }
            }
        }
    }
}
