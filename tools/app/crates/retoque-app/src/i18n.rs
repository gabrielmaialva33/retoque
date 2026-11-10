//! The app's text in English and Brazilian Portuguese. A key missing from the table shows the key itself, so a gap
//! is visible instead of silently empty.

use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Lang {
    En,
    Pt,
}

impl Lang {
    /// pt-BR when the system locale is Portuguese, English otherwise.
    pub fn from_system() -> Lang {
        // RETOQUE_LANG=en|pt forces the language (tests, screenshots); otherwise the system locale decides.
        if let Ok(forced) = std::env::var("RETOQUE_LANG") {
            return if forced.to_ascii_lowercase().starts_with("pt") {
                Lang::Pt
            } else {
                Lang::En
            };
        }
        match sys_locale::get_locale() {
            Some(l) if l.to_ascii_lowercase().starts_with("pt") => Lang::Pt,
            _ => Lang::En,
        }
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
        "Shader preparation failed:",
        "Falha ao preparar shaders:",
    ),
    ("lsfg.retry", "Retry preparation", "Tentar preparar novamente"),
    (
        "lsfg.download_prompt",
        "Preparing shaders requires Microsoft DXC v1.8.2505.1 (up to 64 MiB). Download it from Microsoft’s official GitHub release into the app data folder? The download is checked with SHA-256.",
        "Preparar shaders exige Microsoft DXC v1.8.2505.1 (até 64 MiB). Baixar da versão oficial da Microsoft no GitHub para a pasta de dados do app? O download é verificado com SHA-256.",
    ),
    (
        "lsfg.download_confirm",
        "Download compiler and prepare shaders",
        "Baixar compilador e preparar shaders",
    ),
    (
        "sm86.error.0",
        "ambiguous sm86 release asset",
        "Mais de um arquivo corresponde à release sm86.",
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
        "O arquivo documentado da release sm86 não foi encontrado.",
    ),
    (
        "sm86.error.6",
        "incomplete sm86 release",
        "Faltam arquivos obrigatórios da release sm86.",
    ),
    (
        "sm86.error.7",
        "invalid official sm86 release metadata",
        "Os metadados da release oficial sm86 são inválidos.",
    ),
    (
        "sm86.error.8",
        "invalid release record",
        "O registro local da release é inválido.",
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
        "O redirecionamento da release não informa o destino.",
    ),
    (
        "sm86.error.13",
        "non-official sm86 URL",
        "O endereço não pertence à release oficial sm86.",
    ),
    (
        "sm86.error.14",
        "protected binary in kit archive",
        "O pacote do kit contém um binário protegido que não pode ser incluído.",
    ),
    (
        "sm86.error.15",
        "refused sm86 release redirect",
        "O destino do redirecionamento da release sm86 não é permitido.",
    ),
    (
        "sm86.error.16",
        "release API redirect refused",
        "A API de releases tentou um redirecionamento não permitido.",
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
        "Os metadados da release excedem o limite de tamanho.",
    ),
    (
        "sm86.error.20",
        "release record exceeds limit",
        "O registro local da release excede o limite de tamanho.",
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
        "Não foi possível consultar ou baixar a release sm86 por HTTPS.",
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
        "O servidor da release retornou um status HTTP inesperado.",
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
        "Esta release sm86 não é suportada.",
    ),
    (
        "note.sm86_upgrade",
        "Upgrade sm86 0.3.x: back up and remove the old proxy and INI before copying the complete 0.4.x runtime.",
        "Atualizar sm86 0.3.x: guardar backup e remover o proxy e o INI antigos antes de copiar o runtime 0.4.x completo.",
    ),
    (
        "note.sm86_install",
        "Install catalogued dlssg-sm86; record its identity and files in .retoque/components.txt. The game installer makes no network requests.",
        "Instalar dlssg-sm86 do catálogo e registrar sua identidade e arquivos em .retoque/components.txt. O instalador do jogo não faz requisições de rede.",
    ),
    (
        "sm86.consent_title",
        "Get official frame generation components",
        "Obter componentes oficiais de geração de quadros",
    ),
    (
        "sm86.check_consent",
        "Contact the official GitHub release API to compare the latest version with your local catalog?",
        "Consultar a API oficial de releases do GitHub para comparar a versão mais recente com seu catálogo local?",
    ),
    (
        "sm86.download_consent",
        "Download dlssg_for_sm86 from its official GitHub release and verify its hash? Files stay in app data until you confirm a game review. Up to 256 MiB download and 1 GiB unpacked. No telemetry.",
        "Baixar dlssg_for_sm86 da release oficial no GitHub e verificar seu hash? Os arquivos ficam nos dados do app até você confirmar a revisão do jogo. Até 256 MiB de download e 1 GiB descompactado. Sem telemetria.",
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
    ("sm86.confirm", "Confirm", "Confirmar"),
    ("sm86.later", "Later", "Agora não"),
    (
        "sm86.download_help",
        "Get the official release once and reuse it across games. Game installation always has a separate review.",
        "Obtenha a release oficial uma vez e reutilize-a nos jogos. A instalação no jogo sempre tem uma revisão separada.",
    ),
    ("sm86.download", "Get official release…", "Obter release oficial…"),
    ("sm86.check", "Check for update…", "Verificar atualização…"),
    (
        "sm86.progress",
        "Preparing official components",
        "Preparando componentes oficiais",
    ),
    (
        "sm86.current",
        "This release is available in your catalog.",
        "Esta release está disponível no seu catálogo.",
    ),
    (
        "sm86.new",
        "This release is not yet available in your catalog.",
        "Esta release ainda não está disponível no seu catálogo.",
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
        "Lossless Scaling was not found, so LSFG shaders cannot be prepared. Choose its folder in the LSFG card. No changes were applied.",
        "O Lossless Scaling não foi encontrado, então os shaders do LSFG não podem ser preparados. Escolha a pasta dele no cartão do LSFG. Nenhuma alteração foi aplicada.",
    ),
    (
        "optimize.lsfg_preparing",
        "Preparing the LSFG shaders first (app data only). The review opens when they are ready.",
        "Preparando os shaders do LSFG primeiro (só nos dados do app). A revisão abre quando estiverem prontos.",
    ),
    (
        "optimize.lsfg_failed",
        "LSFG shader preparation failed; see the LSFG card. No changes were applied.",
        "A preparação dos shaders do LSFG falhou; veja o cartão do LSFG. Nenhuma alteração foi aplicada.",
    ),
    (
        "optimize.experimental_omitted",
        "Experimental sm86 was left out. Select it explicitly above to include it in a review.",
        "O sm86 experimental ficou de fora. Selecione-o explicitamente acima para incluí-lo em uma revisão.",
    ),
    (
        "optimize.nothing",
        "No applicable changes with the current choices.",
        "Nenhuma alteração aplicável com as escolhas atuais.",
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
    ("doctor.review", "Review repair…", "Revisar reparo…"),
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
        "This NR model has no compatible kernels for the detected GPU. Choose another model.",
        "Este modelo NR não tem kernels compatíveis com a GPU detectada. Escolha outro modelo.",
    ),
    (
        "mdl.arch_match",
        "CUDA target matches this GPU. Runtime operation is not verified.",
        "O alvo CUDA corresponde à GPU. O funcionamento não foi verificado em execução.",
    ),
    (
        "mdl.compat_unknown",
        "NR model compatibility is unknown. This file or GPU has not been identified reliably.",
        "Compatibilidade do modelo NR desconhecida. Este arquivo ou GPU não foi identificado com segurança.",
    ),
    ("mdl.archs", "CUDA targets found:", "Alvos CUDA encontrados:"),
    (
        "mdl.suggest",
        "Catalog model with matching CUDA targets:",
        "Modelo do catálogo com alvos CUDA compatíveis:",
    ),
    ("mdl.use_suggested", "Review model change", "Revisar troca de modelo"),
    ("ver.unknown", "unknown", "desconhecida"),
    (
        "ver.mismatch",
        "OptiScaler and the NR forwarder have different file versions. Update them together; installation is still allowed.",
        "OptiScaler e o forwarder NR têm versões de arquivo diferentes. Atualize os dois juntos; a instalação continua permitida.",
    ),
    ("welcome.title", "Your games, with Retoque", "Seus jogos, com Retoque"),
    (
        "welcome.body",
        "Install Retoque, tune each game, choose your NR model and understand its logs.",
        "Instale o Retoque, ajuste cada jogo, escolha seu modelo NR e consulte os logs.",
    ),
    ("welcome.found", "games found", "jogos encontrados"),
    ("welcome.installed", "with Retoque", "com Retoque"),
    ("set.nr", "Neural Rendering", "Renderização neural"),
    ("set.upscalers", "Upscalers", "Ampliadores"),
    ("set.framegen", "Frame generation", "Geração de quadros"),
    ("set.lsfg", "LSFG (study build)", "LSFG (versão de estudo)"),
    (
        "set.no_match",
        "No setting matches this filter.",
        "Nenhum ajuste corresponde a este filtro.",
    ),
    (
        "set.lsfg_note",
        "Study build only. Requires FSR FG output and your own Lossless Scaling files; unavailable features cannot run.",
        "Somente na versão de estudo. Requer saída FSR FG e seus próprios arquivos do Lossless Scaling; recursos indisponíveis não funcionam.",
    ),
    ("mdl.default", "Default search", "Busca padrão"),
    ("mdl.named", "Selected file", "Arquivo selecionado"),
    ("mdl.empty_title", "Bring your own model", "Adicione seu modelo"),
    ("mdl.version", "Version", "Versão"),
    ("mdl.dll_filter", "Model DLL", "DLL do modelo"),
    ("unit.mib", "MiB", "MiB"),
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
    ("st.build", "build", "versão"),
    ("inst.notes()", "Notes", "Observações"),
    ("dismiss", "×", "×"),
    ("dismiss.help", "Dismiss message", "Fechar mensagem"),
    ("brand", "Retoque", "Retoque"),
    ("lang.en", "English", "English"),
    ("lang.pt", "Português (BR)", "Português (BR)"),
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
    ("kind.install", "install", "instalação"),
    ("kind.uninstall", "uninstall", "remoção"),
    ("kind.settings", "settings", "ajustes"),
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
        "job.failed",
        "The operation could not finish",
        "Não foi possível concluir a operação",
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
        "O nome {path} não é aceito como proxy pelo OptiScaler",
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
    ("app.lang", "Language", "Idioma"),
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
    ("game.none", "Choose a game on the left.", "Escolha um jogo à esquerda."),
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
    ("tab.install", "Install", "Retoque"),
    ("tab.settings", "Settings", "Ajustes"),
    ("tab.model", "NR model", "Modelo NR"),
    ("tab.compare", "Before / after", "Antes e depois"),
    ("tab.logs", "Logs", "Logs"),
    (
        "set.invalid",
        "OptiScaler does not accept this value; fix it to review the changes.",
        "O OptiScaler não aceita este valor; corrija para revisar as mudanças.",
    ),
    ("st.installed", "Retoque installed", "Retoque instalado"),
    ("st.partial", "Install stopped part-way", "Instalação interrompida"),
    (
        "st.uninstall_incomplete",
        "Uninstall stopped part-way",
        "Remoção interrompida",
    ),
    ("st.not_installed", "Not installed", "Não instalado"),
    (
        "st.untracked",
        "OptiScaler present without a receipt",
        "OptiScaler presente sem recibo",
    ),
    ("st.nr_on", "NR on", "NR ligado"),
    ("st.nr_off", "NR off", "NR desligado"),
    (
        "st.model_missing",
        "no nvngx_dlssnr.dll beside the game",
        "sem nvngx_dlssnr.dll junto do jogo",
    ),
    ("st.model_present", "model beside the game", "modelo junto do jogo"),
    (
        "st.running",
        "The game is running. Close it first.",
        "O jogo está aberto. Feche-o antes.",
    ),
    ("inst.proxy", "Load as", "Carregar como"),
    ("inst.enable_nr", "Turn Neural Rendering on", "Ligar o Neural Rendering"),
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
        "Allow a second OptiScaler proxy",
        "Permitir um segundo proxy do OptiScaler",
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
    ("inst.uninstall", "Uninstall", "Remover"),
    ("inst.plan", "What will be done", "O que será feito"),
    ("inst.nothing", "Nothing to change.", "Nada a mudar."),
    ("inst.refused", "Nothing will be written:", "Nada será gravado:"),
    ("inst.working", "Working…", "Trabalhando…"),
    ("inst.launch", "Steam launch option", "Opção de inicialização da Steam"),
    (
        "inst.launch_help",
        "Under Proton the game must be told to load this DLL: paste this into the game's launch options in Steam.",
        "No Proton o jogo precisa ser avisado para carregar esta DLL: cole isto nas opções de inicialização do jogo na Steam.",
    ),
    ("inst.copy", "Copy", "Copiar"),
    ("inst.copied", "Copied", "Copiado"),
    (
        "ref.running",
        "The game is running (exit 3):",
        "O jogo está aberto (saída 3):",
    ),
    (
        "ref.foreign",
        "A file at one of our names is not ours (exit 4):",
        "Um arquivo com um dos nossos nomes não é nosso (saída 4):",
    ),
    (
        "ref.second",
        "Another proxy already holds OptiScaler (exit 5):",
        "Outro proxy já tem o OptiScaler (saída 5):",
    ),
    (
        "ref.receipt",
        "The receipt cannot be used:",
        "O recibo não pode ser usado:",
    ),
    ("ref.usage", "Cannot do this:", "Não dá para fazer isto:"),
    (
        "set.none",
        "Install Retoque first: there is no OptiScaler.ini here.",
        "Instale o Retoque antes: não há OptiScaler.ini aqui.",
    ),
    ("set.save", "Review changes", "Revisar mudanças"),
    ("set.revert", "Undo changes", "Desfazer mudanças"),
    ("set.changed", "changed", "alterado(s)"),
    (
        "set.missing",
        "not in this ini yet: saving adds it",
        "ainda não está neste ini: salvar acrescenta",
    ),
    ("mdl.current", "This game uses", "Modelo deste jogo"),
    (
        "mdl.auto",
        "the default search (nvngx_dlssnr.dll beside OptiScaler, then beside the game)",
        "a busca padrão (nvngx_dlssnr.dll junto do OptiScaler, depois junto do jogo)",
    ),
    (
        "mdl.missing_file",
        "This file does not exist: NR will be unavailable in this game.",
        "Este arquivo não existe: o NR ficará indisponível neste jogo.",
    ),
    ("mdl.catalog", "Your models", "Catálogo do aplicativo"),
    ("mdl.import", "Import a model…", "Importar um modelo…"),
    ("mdl.use", "Use in this game", "Usar neste jogo"),
    ("mdl.in_use", "In use", "Em uso"),
    ("mdl.use_auto", "Use the default search", "Usar a busca padrão"),
    ("mdl.remove", "Remove from the list", "Tirar da lista"),
    (
        "mdl.empty",
        "No models yet. Import the nvngx_dlssnr.dll files you have; they are copied into the app's folder and never shared.",
        "Nenhum modelo ainda. Importe os nvngx_dlssnr.dll que você tem; eles são copiados para a pasta do app e nunca compartilhados.",
    ),
    (
        "mdl.needs_ini",
        "Install Retoque first: the choice is saved in OptiScaler.ini.",
        "Instale o Retoque antes: a escolha fica no OptiScaler.ini.",
    ),
    (
        "log.none",
        "No OptiScaler log here yet: play the game once with Retoque installed.",
        "Ainda não há log do OptiScaler aqui: jogue uma vez com o Retoque instalado.",
    ),
    ("log.dlss", "DLSS", "DLSS"),
    ("log.api", "API", "API"),
    ("log.route", "NR route", "Rota do NR"),
    ("log.nr_init", "NR started", "NR iniciou"),
    ("log.gpu", "NR GPU time", "Tempo de GPU do NR"),
    ("log.errors", "Errors", "Erros"),
    ("log.warnings", "Warnings", "Avisos"),
    ("log.lines", "Lines worth reading", "Linhas que importam"),
    ("log.bundle", "Make a diagnostic file", "Gerar diagnóstico…"),
    (
        "log.bundle_done",
        "Written (home folder and user name removed):",
        "Gravado (pasta pessoal e nome de usuário removidos):",
    ),
    ("log.open_folder", "Open folder", "Abrir pasta"),
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
        "Deletes the catalog's copy. A game that names this file loses NR until it gets another model.",
        "Apaga a cópia do catálogo. Um jogo que aponta para este arquivo fica sem NR até receber outro modelo.",
    ),
    (
        "app.tagline",
        "Neural Rendering for your games",
        "Neural Rendering nos seus jogos",
    ),
    ("lib.group_installed", "With Retoque", "Com Retoque"),
    ("lib.group_other", "Other games", "Outros jogos"),
    ("lib.state_installed", "Retoque", "Retoque"),
    ("card.install", "Install", "Instalação"),
    ("card.install_options", "Install options", "Opções de instalação"),
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
        "Só saem arquivos que o Retoque, o OptiScaler, o sm86, o ReShade ou os caches deles colocaram ali. Qualquer outra coisa, como um save, fica, e a pasta dela também. Nada muda antes da sua confirmação.",
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
    (
        "sess.drops_advice",
        "The real frame rate dropped more than 20% in {percent}% of the session. It is usually the game loading new areas or too little free RAM: close other programs and lower texture quality.",
        "Os quadros reais caíram mais de 20% em {percent}% da sessão. Costuma ser o jogo carregando áreas novas ou pouca RAM livre: feche outros programas e baixe a qualidade das texturas.",
    ),
    (
        "sess.nr_off",
        "Neural Rendering was switched off during the session (its key in the game).",
        "O Neural Rendering foi desligado durante a sessão (pela tecla dele no jogo).",
    ),
    ("card.launch", "Steam launch option", "Opção de inicialização da Steam"),
    ("yes", "yes", "sim"),
    ("no", "no", "não"),
    ("error", "Error", "Erro"),
    ("cancel", "Cancel", "Cancelar"),
    // Shared words of the design system (widgets::busy, consent_modal and info dialogs).
    ("progress.of", "of", "de"),
    ("not_now", "Not now", "Agora não"),
    ("close", "Close", "Fechar"),
    ("download", "Download", "Baixar"),
    // Before / after viewer (compare.rs).
    ("canvas", "Comparison image", "Imagem de comparação"),
    ("title", "Before / after", "Antes / depois"),
    ("captures", "Captures", "Capturas"),
    ("refresh", "Refresh", "Atualizar"),
    ("loading", "Loading capture…", "Carregando captura…"),
    ("scanning", "Looking for captures…", "Buscando capturas…"),
    ("empty", "No complete captures yet", "Nenhuma captura completa ainda"),
    (
        "empty_help",
        "Run the game with NR enabled. In Retoque's in-game menu, open Compare and press “Capture original and processed”. Return here and refresh. Both images must finish saving beside the game's proxy, in retoque-captures.",
        "Abra o jogo com o NR ativado. No menu do Retoque dentro do jogo, abra Comparação e clique em “Capturar original e processado”. Volte aqui e atualize. As duas imagens precisam terminar de salvar na pasta retoque-captures, ao lado da DLL do Retoque.",
    ),
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
    (
        "interaction",
        "Wheel: zoom · Drag: pan · Drag divider: compare · Space: original in Flip",
        "Roda: zoom · Arraste: mover · Arraste o divisor: comparar · Espaço: original em Alternar",
    ),
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
    ("provenance", "Capture provenance", "Dados da captura"),
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
    ("window", "Retoque comparison", "Comparação do Retoque"),
    ("exposure", "Exposure", "Exposição"),
    ("exposure_reset", "Reset exposure", "Redefinir exposição"),
    (
        "exposure_help",
        "0 EV uses the automatic or recorded exposure. Each stop doubles or halves the light, equally on both images. The preview updates when you release the slider.",
        "0 EV usa a exposição automática ou registrada. Cada ponto dobra ou reduz pela metade a luz nas duas imagens. A prévia atualiza ao soltar o controle.",
    ),
    ("exposure_updating", "Updating preview…", "Atualizando prévia…"),
    ("thumbnail_loading", "Loading thumbnail…", "Carregando miniatura…"),
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
        "Revisar mudanças (Ctrl+Enter); nada será gravado antes da confirmação.",
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
        "Model catalogue updated",
        "Catálogo de modelos atualizado",
    ),
    ("shell.checking", "Checking installation…", "Verificando instalação…"),
    ("shell.update_available", "Update available", "Atualização disponível"),
    ("shell.up_to_date", "Up to date", "Atualizado"),
    (
        "shell.partial",
        "Installation needs cleanup",
        "Instalação precisa de limpeza",
    ),
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
    (
        "shell.review_remove",
        "Review Retoque removal",
        "Revisar remoção do Retoque",
    ),
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
    (
        "shell.home_help",
        "Add a game folder or scan your launchers to get started.",
        "Adicione a pasta de um jogo ou busque os jogos dos seus launchers para começar.",
    ),
    ("shell.reinstall", "Reinstall…", "Reinstalar…"),
    ("shell.resume", "Finish cleanup…", "Concluir limpeza…"),
    ("shell.update", "Update Retoque…", "Atualizar o Retoque…"),
    ("shell.install", "Install Retoque…", "Instalar Retoque…"),
    ("shell.open_folder", "Open game folder", "Abrir pasta do jogo"),
    ("shell.pending", "unsaved changes", "alterações não salvas"),
    ("shell.preparing", "Preparing your review…", "Preparando a revisão…"),
    ("shell.installed_done", "Retoque installed", "Retoque instalado"),
    ("shell.updated_done", "Retoque updated", "Retoque atualizado"),
    ("shell.removed_done", "Retoque uninstalled", "Retoque desinstalado"),
    ("shell.settings_done", "Settings applied", "Ajustes aplicados"),
    // Worker B: settings, current model and session summary.
    ("set.advanced", "Advanced settings", "Ajustes avançados"),
    ("set.game_default", "Game default", "Padrão do jogo"),
    ("set.model_link", "Choose in NR model…", "Escolher em Modelo NR…"),
    ("set.custom", "Custom value", "Valor personalizado"),
    ("set.automatic", "Automatic", "Automático"),
    (
        "tab.install_first",
        "Install Retoque to set up this game",
        "Instale o Retoque para configurar este jogo",
    ),
    ("tab.review_install", "Review installation…", "Revisar instalação…"),
    ("tab.loading", "Reading game settings…", "Lendo os ajustes do jogo…"),
    ("mdl.loading", "Reading model version…", "Lendo a versão do modelo…"),
    ("mdl.beside", "Found beside the game", "Encontrado na pasta do jogo"),
    (
        "mdl.from_catalog",
        "From your app catalogue",
        "Do catálogo do aplicativo",
    ),
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
        "Your app catalogue is empty",
        "O catálogo do aplicativo está vazio",
    ),
    (
        "mdl.optional",
        "This game already has a model. Importing another is optional.",
        "Este jogo já tem um modelo. Importar outro é opcional.",
    ),
    (
        "mdl.drop",
        "Drop a model DLL here, or choose a file",
        "Solte a DLL do modelo aqui ou escolha um arquivo",
    ),
    (
        "mdl.import_review",
        "Import this file into your catalogue? The game folder stays unchanged.",
        "Importar este arquivo para o catálogo? A pasta do jogo permanece como está.",
    ),
    (
        "mdl.confirm_import",
        "Import into catalogue",
        "Importar para o catálogo",
    ),
    ("mdl.used_games", "games using this model", "jogos usando este modelo"),
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
    ("log.error_count", "errors", "erros"),
    (
        "log.hint_errors",
        "Review the errors below. Make a diagnostic file if you need help.",
        "Confira os erros abaixo. Gere um diagnóstico se precisar de ajuda.",
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
        "cmp.empty.capture",
        "In Compare, choose “Capture original and processed”. Wait for both files to finish, then refresh here.",
        "Em Comparar, escolha “Capturar original e processado”. Aguarde as duas imagens terminarem de salvar e atualize aqui.",
    ),
    ("cmp.empty.refresh", "Look for captures", "Buscar capturas"),
    ("cmp.strip", "Capture filmstrip", "Faixa de capturas"),
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
    ("tile.fg", "Frame generation", "Geração de quadros"),
    ("tile.on", "On", "Ligado"),
    ("tile.off", "Off", "Desligado"),
    ("tile.unknown", "Unknown", "Desconhecido"),
    ("tile.old_install", "Old install", "Instalação antiga"),
    ("tile.st_current", "Up to date", "Atualizado"),
    ("tile.st_update", "New version", "Nova versão"),
    ("tile.st_installed", "Installed", "Instalado"),
    ("tile.st_partial", "Incomplete", "Incompleto"),
    ("tile.st_untracked", "No receipt", "Sem recibo"),
    ("tile.st_missing", "Not installed", "Não instalado"),
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
        "Optimised for your GPU",
        "Ajustes para sua placa de vídeo",
    ),
    ("auto.checking", "Checking this game…", "Conferindo este jogo…"),
    (
        "auto.none",
        "No automatic changes recommended for this GPU and game.",
        "Nenhuma mudança automática recomendada para esta placa e este jogo.",
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
    ("auto.off", "Off", "Desligado"),
    ("auto.x2", "x2", "x2"),
    ("auto.x3", "x3", "x3"),
    ("auto.x4", "x4", "x4"),
    (
        "auto.ls_ready",
        "Lossless Scaling shaders ready",
        "Shaders do Lossless Scaling prontos",
    ),
    (
        "auto.ls_shaders",
        "Lossless Scaling needs a shader cache",
        "Falta preparar os shaders do Lossless Scaling",
    ),
    (
        "auto.ls_missing",
        "Lossless Scaling not found",
        "Lossless Scaling não encontrado",
    ),
    (
        "auto.ls_checking",
        "Checking Lossless Scaling…",
        "Conferindo o Lossless Scaling…",
    ),
    (
        "auto.choose_lossless",
        "Choose Lossless Scaling folder…",
        "Escolher pasta do Lossless Scaling…",
    ),
    (
        "auto.ls_conflict",
        "Remove the installed sm86 component in the NR model tab before enabling LSFG.",
        "Remova o componente sm86 instalado na aba Modelo NR antes de ligar o LSFG.",
    ),
    (
        "auto.components",
        "Frame generation components",
        "Componentes de geração de quadros",
    ),
    (
        "auto.import_help",
        "Import your own dlssg_for_sm86 release folder (version.dll, dlssg_sm86.ini and dlssg_sm86/). No runtime is downloaded or included with Retoque.",
        "Importe sua própria pasta do dlssg_for_sm86 (version.dll, dlssg_sm86.ini e dlssg_sm86/). O Retoque não baixa nem distribui esse runtime.",
    ),
    ("auto.import", "Import sm86 folder…", "Importar pasta sm86…"),
    (
        "auto.no_component",
        "No sm86 component in your catalogue",
        "Nenhum componente sm86 no catálogo",
    ),
    (
        "auto.component_installed",
        "Component installed in this game",
        "Componente instalado neste jogo",
    ),
    (
        "auto.component_install",
        "Review component installation",
        "Revisar instalação do componente",
    ),
    (
        "auto.component_remove",
        "Review component removal",
        "Revisar remoção do componente",
    ),
    (
        "auto.catalog_remove",
        "Remove from component catalogue",
        "Remover do catálogo de componentes",
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
        "DLSS frame generation x4",
        "Geração de quadros DLSS x4",
    ),
    (
        "rec.sm86_mfg.why",
        "sm86 is installed and this game has DLSS-G: ask for three generated frames per real one (x4) and keep Retoque's own frame generation out of the way. You can lower it in Settings.",
        "O sm86 está instalado e este jogo tem DLSS-G: pede três quadros gerados para cada real (x4) e tira a geração de quadros do Retoque do caminho. Dá para baixar em Ajustes.",
    ),
    (
        "rec.rtx30_nr.title",
        "Neural Rendering tuned for RTX 30",
        "Neural Rendering ajustado para RTX 30",
    ),
    (
        "rec.rtx30_nr.why",
        "Full detail in the centre of the screen at the same cost (peripheral compression), and a stronger effect (1.5). Measured on an RTX 3060; you can change both in Settings.",
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
        "Retoque is here without a receipt (installed by an older script). Reinstalling through the app records every file, so it can be updated and removed safely later.",
        "O Retoque está aqui sem recibo (foi instalado por um script antigo). Reinstalar pelo app registra cada arquivo, para atualizar e remover com segurança depois.",
    ),
    (
        "rec.sm86_update.title",
        "Update DLSS-G for RTX 30 (dlssg_for_sm86)",
        "Atualizar o DLSS-G para RTX 30 (dlssg_for_sm86)",
    ),
    (
        "rec.sm86_update.why",
        "This game has an older dlssg_for_sm86 (0.3.x). The version in your catalogue replaces it; the old files are kept as a backup.",
        "Este jogo tem um dlssg_for_sm86 antigo (0.3.x). A versão do seu catálogo o substitui; os arquivos antigos ficam guardados como cópia.",
    ),
    (
        "auto.shader_hint",
        "The app prepares shaders automatically in its data folder. If the compiler is missing, confirm its official download.",
        "O app prepara os shaders automaticamente na pasta de dados. Se o compilador estiver ausente, confirme o download oficial.",
    ),
    // Discovery folders / worker t117
    ("store.xbox", "Xbox", "Xbox"),
    ("folders.title", "Your folders", "Suas pastas"),
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
    ("folders.remove", "Remove", "Remover"),
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
    ("palette.language", "Language", "Idioma"),
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
    ("set.reset", "Reset to auto", "Restaurar automático"),
    (
        "set.source_auto",
        "Source: auto (game default)",
        "Origem: auto (padrão do jogo)",
    ),
    ("set.source_ini", "Source: INI", "Origem: INI"),
    (
        "set.source_draft",
        "Pending draft; review to apply",
        "Rascunho pendente; revise para aplicar",
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
        "Undo refused: this INI value changed since the recorded edit. Review the current settings:",
        "Desfazer recusado: este valor do INI mudou após a alteração registrada. Revise os ajustes atuais:",
    ),
    (
        "undo.save_failed",
        "Settings applied, but undo history could not be saved",
        "Ajustes aplicados, mas não foi possível salvar o histórico",
    ),
    (
        "undo.auto_note",
        "Undo is reviewed before applying. Previously absent keys are removed.",
        "O desfazer é revisado antes de aplicar. Chaves antes ausentes são removidas.",
    ),
    (
        "drop.hover",
        "Drop to review model import, component import or game addition",
        "Solte para revisar a importação de modelo, componente ou adição de jogo",
    ),
    ("drop.title", "Review dropped item", "Revisar item solto"),
    (
        "drop.model",
        "Import this NR model into the app catalog?",
        "Importar este modelo NR para o catálogo do aplicativo?",
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
    ("drop.confirm", "Confirm", "Confirmar"),
    ("doctor.title", "Retoque Doctor", "Retoque Doctor"),
    ("doctor.prepare", "Prepare repair…", "Preparar reparo…"),
    ("doctor.evidence", "Evidence", "Evidências"),
    ("doctor.badge", "Health findings", "Problemas detectados"),
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
        "Remove Retoque and restore original DLSS…",
        "Remover Retoque e restaurar DLSS original…",
    ),
    (
        "dlss.restore_only",
        "Restore original DLSS…",
        "Restaurar DLSS original…",
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
        "Get DLSS from the DLSS card first, then review the update.",
        "Obtenha o DLSS no cartão DLSS primeiro; depois revise a atualização.",
    ),
    (
        "dlss.consent",
        "Download SR, RR and FG directly from NVIDIA (115 MB)? Only the kinds already in the game will be replaced after a separate review. NVIDIA license applies.",
        "Baixar SR, RR e FG diretamente da NVIDIA (115 MB)? Apenas os tipos já presentes no jogo serão substituídos após outra revisão. Aplica-se a licença NVIDIA.",
    ),
    (
        "dlss.caution",
        "DLL changes may be incompatible with a game or its anti-cheat. Check the game policy before updating. Originals are backed up for restoration.",
        "Trocas de DLL podem ser incompatíveis com o jogo ou seu anticheat. Confira a política do jogo antes de atualizar. Os originais terão backup para restauração.",
    ),
    ("dlss.confirm_download", "Confirm download", "Confirmar download"),
    ("dlss.license", "Read NVIDIA license", "Ler licença NVIDIA"),
    (
        "dlss.error.0",
        "DLSS root receipt belongs to another installation",
        "O recibo na raiz pertence a outra instalação",
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
        "Restaure o recibo DLSS anterior antes de mudar sua raiz",
    ),
    (
        "dlss.error.5",
        "DLSS root marker is not receipt-owned",
        "O marcador DLSS da raiz não pertence ao recibo",
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
        "Query GitHub Releases (gabrielmaialva33/retoque) to check for a newer Retoque payload?",
        "Consultar o GitHub Releases (gabrielmaialva33/retoque) para verificar se há uma versão mais nova do Retoque?",
    ),
    (
        "payload.download_consent",
        "Download and verify the latest Retoque payload (OptiScaler.dll, forwarder and INI) from gabrielmaialva33/retoque into app data? Game folders are not modified until you review an installation.",
        "Baixar e verificar o pacote mais recente do Retoque (OptiScaler.dll, forwarder e INI) de gabrielmaialva33/retoque na pasta de dados do app? Nenhum jogo será alterado antes da revisão da instalação.",
    ),
    (
        "payload.hosts_note",
        "Allowed HTTPS hosts: api.github.com, github.com/gabrielmaialva33/retoque and release-assets.githubusercontent.com. Verified with SHA256SUMS and the GitHub SHA-256 digest.",
        "Hosts HTTPS permitidos: api.github.com, github.com/gabrielmaialva33/retoque e release-assets.githubusercontent.com. Verificado com SHA256SUMS e digest SHA-256 do GitHub.",
    ),
    ("payload.confirm", "Continue", "Continuar"),
    ("payload.later", "Not now", "Agora não"),
    (
        "payload.progress",
        "Downloading Retoque payload…",
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
        "O SHA-256 do pacote não corresponde ao digest publicado pelo GitHub.",
    ),
    (
        "payload.error.1",
        "payload SHA-256 does not match SHA256SUMS",
        "O SHA-256 do pacote não corresponde ao arquivo SHA256SUMS.",
    ),
    (
        "payload.error.2",
        "SHA256SUMS digest mismatch",
        "O SHA-256 do arquivo SHA256SUMS não corresponde ao digest publicado.",
    ),
    (
        "payload.error.3",
        "SHA256SUMS does not list payload archive",
        "O arquivo SHA256SUMS não contém o pacote de payload.",
    ),
    (
        "payload.error.4",
        "forbidden file in payload archive",
        "Arquivo proibido encontrado no pacote de payload.",
    ),
    (
        "payload.error.5",
        "unexpected extra file in payload archive",
        "Arquivo extra não permitido encontrado no pacote de payload.",
    ),
    (
        "payload.error.6",
        "OptiScaler.dll and nvngx.dll_dlssnr.dll file versions do not match",
        "As versões de arquivo do OptiScaler.dll e do nvngx.dll_dlssnr.dll não coincidem.",
    ),
    (
        "payload.error.7",
        "incomplete payload release archive",
        "Faltam arquivos obrigatórios no pacote de payload.",
    ),
    (
        "payload.error.8",
        "invalid official payload release metadata",
        "Os metadados da release oficial do Retoque são inválidos.",
    ),
    (
        "payload.error.9",
        "payload HTTPS request failed",
        "A requisição HTTPS da release do Retoque falhou.",
    ),
    (
        "payload.error.10",
        "non-official payload URL",
        "URL fora do repositório oficial do Retoque.",
    ),
    (
        "payload.error.11",
        "refused payload release redirect",
        "Redirecionamento da release do Retoque recusado.",
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
        "Get a compatible NR model",
        "Obter um modelo NR compatível",
    ),
    (
        "nr_fetch.consent_body",
        "Download this archive over HTTPS into the app catalog, verify its size and SHA-256, and extract only nvngx_dlssnr.dll? Game settings are reviewed separately before any write.",
        "Baixar este pacote por HTTPS para o catálogo do aplicativo, verificar o tamanho e o SHA-256 e extrair apenas nvngx_dlssnr.dll? Os ajustes do jogo são revisados separadamente antes de qualquer gravação.",
    ),
    ("nr_fetch.confirm", "Confirm download", "Confirmar download"),
    ("nr_fetch.later", "Later", "Agora não"),
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
        "Fetch catalog.json from ApolloF/DLSS5-Updater-catalog on GitHub (up to 8 MiB)? Only entries on RankFTW/rhi-repo releases with a SHA-256 are accepted; unprobed entries are never assumed compatible.",
        "Baixar catalog.json de ApolloF/DLSS5-Updater-catalog no GitHub (até 8 MiB)? Apenas entradas em releases de RankFTW/rhi-repo com SHA-256 são aceitas; entradas não sondadas nunca são assumidas como compatíveis.",
    ),
    ("nr_fetch.confirm_refresh", "Refresh catalog", "Atualizar catálogo"),
    (
        "nr_fetch.downloading",
        "Downloading community NR model",
        "Baixando modelo NR da comunidade",
    ),
    (
        "nr_fetch.no_compatible_gpu",
        "No compatible model can be picked automatically because the GPU or its CUDA architecture is unknown.",
        "Nenhum modelo compatível pode ser escolhido automaticamente porque a GPU ou sua arquitetura CUDA é desconhecida.",
    ),
    (
        "nr_fetch.unavailable",
        "No compatible NR model entry is available for this GPU. No changes were applied.",
        "Nenhum modelo NR compatível está disponível para esta GPU. Nenhuma alteração foi aplicada.",
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
        "O endereço não pertence às releases de RankFTW/rhi-repo.",
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
    for (k, en, pt) in TEXT {
        if *k == key {
            return match lang {
                Lang::En => en,
                Lang::Pt => pt,
            };
        }
    }
    retoque_data::messages::text(
        key,
        match lang {
            Lang::En => retoque_data::messages::Language::English,
            Lang::Pt => retoque_data::messages::Language::Portuguese,
        },
    )
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
}

/// Translate known engine messages without changing filenames, configuration values or original log records.
/// Unknown OS/driver/receipt diagnostics remain verbatim so a report retains the actual failure evidence.
pub fn diagnostic(lang: Lang, message: &str) -> String {
    crate::applog::shown_error(message);
    let translated = retoque_data::messages::reshade_diagnostic(
        message,
        match lang {
            Lang::En => retoque_data::messages::Language::English,
            Lang::Pt => retoque_data::messages::Language::Portuguese,
        },
    );
    if translated != message {
        return translated;
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
