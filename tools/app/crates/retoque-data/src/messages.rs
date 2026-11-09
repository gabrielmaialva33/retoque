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
