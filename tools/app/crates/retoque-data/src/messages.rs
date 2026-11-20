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
    (
        "probe.short.mixed_sources",
        "loaded now / imported",
        "carregada agora / importada",
    ),
    ("probe.short.x86_bits", "x86 (32-bit)", "x86 (32 bits)"),
    ("probe.short.x64_bits", "x64 (64-bit)", "x64 (64 bits)"),
    ("probe.short.arm_bits", "ARM64 (64-bit)", "ARM64 (64 bits)"),
    ("probe.short.imported", "imported", "importada"),
    ("probe.short.loaded", "loaded now", "carregada agora"),
    ("probe.short.past", "seen in a past log", "vista em log anterior"),
    ("probe.short.partial", "partial inspection", "inspeção parcial"),
    ("probe.short.mixed", "multiple APIs", "várias APIs"),
    (
        "probe.short.vulkan",
        "Today: NR after DLSS/FSR/XeSS; without an upscaler, experimental Vulkan presentation",
        "Hoje: NR após DLSS/FSR/XeSS; sem ampliador, apresentação Vulkan experimental",
    ),
    (
        "probe.short.d3d12",
        "Today: NR after DLSS/FSR/XeSS; without an upscaler, D3D12 presentation",
        "Hoje: NR após DLSS/FSR/XeSS; sem ampliador, apresentação D3D12",
    ),
    (
        "probe.short.d3d11",
        "Today: NR after DLSS/FSR/XeSS; without an upscaler, the D3D11-to-D3D12 presentation bridge",
        "Hoje: NR após DLSS/FSR/XeSS; sem ampliador, ponte de apresentação D3D11 para D3D12",
    ),
    (
        "probe.short.x86",
        "Today: no in-game NR in a 32-bit game; the alternative is experimental external capture on Windows.",
        "Hoje: sem NR dentro de jogos de 32 bits; a alternativa é a captura externa experimental no Windows.",
    ),
    (
        "probe.short.d3d10",
        "Today: no in-game NR through D3D10; the alternative is experimental external capture on Windows.",
        "Hoje: sem NR dentro do jogo via D3D10; a alternativa é a captura externa experimental no Windows.",
    ),
    (
        "probe.short.opengl",
        "Today: no in-game NR through OpenGL; the alternative is experimental external capture on Windows.",
        "Hoje: sem NR dentro do jogo via OpenGL; a alternativa é a captura externa experimental no Windows.",
    ),
    (
        "probe.short.elf",
        "Today: this Linux executable has no in-game Retoque route; check for a Windows edition to run with Proton.",
        "Hoje: este executável Linux não tem rota do Retoque dentro do jogo; verifique se há uma edição Windows para usar com Proton.",
    ),
    (
        "probe.short.unknown_route",
        "Today: the in-game NR route is still unknown; identify the game executable and API first.",
        "Hoje: a rota de NR dentro do jogo ainda é desconhecida; falta identificar o executável do jogo e a API.",
    ),
    (
        "probe.short.next_pid",
        "Next: rerun with --pid while the game is running.",
        "Próximo: rode de novo com --pid enquanto o jogo estiver aberto.",
    ),
    (
        "probe.short.next_log",
        "Next: inspect a new NR session log with --log to check which route ran.",
        "Próximo: use --log com o registro de uma nova sessão com NR para conferir a rota usada.",
    ),
    (
        "probe.short.next_exe",
        "Next: select the actual game executable with --exe.",
        "Próximo: escolha o executável do jogo com --exe.",
    ),
    (
        "probe.short.next_elf",
        "Next: if using Proton, select the Windows game executable with --exe, together with --pid.",
        "Próximo: se usa Proton, indique o executável Windows do jogo com --exe, junto de --pid.",
    ),
    (
        "probe.short.next_capture",
        "Next: check the experimental external-capture requirements on Windows.",
        "Próximo: confira os requisitos da captura externa experimental no Windows.",
    ),
    (
        "probe.short.hint",
        "More: --details for the full report; --json for tools.",
        "Mais: --details mostra o relatório completo; --json serve para ferramentas.",
    ),
    (
        "probe.short.log_vk_upscaled",
        "Vulkan upscaler route recorded in a past log",
        "rota Vulkan após ampliação registrada em log anterior",
    ),
    (
        "probe.short.log_vk_present",
        "Vulkan presentation route recorded in a past log",
        "rota de apresentação Vulkan registrada em log anterior",
    ),
    (
        "probe.short.log_vk_created",
        "Vulkan route created in a past log",
        "rota Vulkan criada em log anterior",
    ),
    (
        "probe.short.log_dx12",
        "D3D12 presentation route recorded in a past log",
        "rota de apresentação D3D12 registrada em log anterior",
    ),
    (
        "probe.short.log_dx11",
        "D3D11 bridge created in a past log",
        "ponte D3D11 criada em log anterior",
    ),
    (
        "probe.log_search_partial",
        "Local log search reached its entry, file, read or time limit; older evidence may be missing.",
        "A busca de logs na pasta atingiu seu limite; registros anteriores podem ter ficado de fora.",
    ),
    (
        "probe.log.vk_upscaled",
        "Historical Vulkan upscaler pass recording",
        "Registro anterior do passe Vulkan após ampliação",
    ),
    (
        "probe.log_complete",
        "Entire selected log inspected",
        "Log selecionado inspecionado por completo",
    ),
    (
        "probe.name.windows_capture",
        "Windows external capture",
        "Captura externa no Windows",
    ),
    (
        "probe.name.windows_d3d10",
        "Native Windows D3D10",
        "D3D10 nativo do Windows",
    ),
    ("probe.name.windows_opengl", "Windows OpenGL", "OpenGL no Windows"),
    (
        "probe.name.x86_in_process",
        "In-game x86 processing",
        "Processamento no jogo x86",
    ),
    ("probe.name.d3d10_dxvk", "D3D10 through DXVK", "D3D10 pelo DXVK"),
    ("probe.name.d3d_wined3d", "D3D through wined3d", "D3D pelo wined3d"),
    (
        "probe.name.native_linux_gl",
        "Native Linux OpenGL",
        "OpenGL nativo do Linux",
    ),
    ("probe.name.d3d11_bridge", "D3D11 bridge", "Ponte D3D11"),
    ("probe.name.d3d12_present", "D3D12 presentation", "Apresentação D3D12"),
    (
        "probe.name.vulkan_present",
        "Vulkan presentation",
        "Apresentação Vulkan",
    ),
    ("probe.windows", "Windows", "Windows"),
    ("probe.proton", "Proton", "Proton"),
    ("probe.linux", "native Linux", "Linux nativo"),
    ("probe.windowed", "windowed", "janela"),
    ("probe.borderless", "borderless", "sem bordas"),
    ("probe.exclusive", "exclusive fullscreen", "tela cheia exclusiva"),
    ("probe.overlay", "overlay", "sobreposição"),
    ("probe.in_game", "in-game return", "retorno ao jogo"),
    ("probe.geometry", "Observed client size", "Área interna observada"),
    (
        "probe.title",
        "Legacy API inspection — read only",
        "Inspeção de APIs antigas — somente leitura",
    ),
    (
        "probe.usage",
        "Use legacy-probe --exe FILE or --pid PID; optional --launcher-exe FILE, --log FILE, --details, --json, --lang en or pt-BR, --runtime windows/proton/linux, --mode windowed/borderless/exclusive, --output overlay/in-game.",
        "Use legacy-probe --exe ARQUIVO ou --pid PID; opcionais: --launcher-exe ARQUIVO, --log ARQUIVO, --details, --json, --lang en ou pt-BR, --runtime windows/proton/linux, --mode windowed/borderless/exclusive, --output overlay/in-game.",
    ),
    (
        "probe.worker",
        "Metadata inspection could not finish. Try again for the selected game.",
        "Não foi possível concluir a inspeção. Tente novamente para o jogo selecionado.",
    ),
    (
        "probe.budget",
        "Inspection budget exhausted; facts are partial or unknown. Inspect the executable and process separately.",
        "Limite da inspeção atingido; os dados são parciais ou desconhecidos. Inspecione o executável e o processo separadamente.",
    ),
    (
        "probe.no_process",
        "No process selected. File imports cannot identify the running renderer. Select the game PID separately from its launcher.",
        "Nenhum processo selecionado. As importações do arquivo não identificam o renderizador em execução. Selecione o PID do jogo separadamente do launcher.",
    ),
    (
        "probe.identity",
        "Process identity changed or could not be verified. Process facts were discarded; select the current game PID.",
        "A identidade do processo mudou ou não pôde ser verificada. Os dados do processo foram descartados; selecione o PID atual do jogo.",
    ),
    (
        "probe.mismatch",
        "The selected executable was not identified in the process. Choose the game executable and its own PID.",
        "O executável selecionado não foi identificado no processo. Escolha o executável do jogo e o PID dele.",
    ),
    (
        "probe.process_unreadable",
        "Process metadata was unavailable, denied or over budget. Evidence remains unknown; do not force access.",
        "Os metadados do processo estavam indisponíveis, sem acesso ou acima do limite. A evidência permanece desconhecida; não force o acesso.",
    ),
    (
        "probe.image_unknown",
        "Executable identity or architecture is incomplete. Choose a readable, regular game executable.",
        "A identidade ou arquitetura do executável está incompleta. Escolha um arquivo executável comum e legível do jogo.",
    ),
    (
        "probe.imports_partial",
        "Import coverage is incomplete: ELF dependencies and PE delay imports are not decoded. Missing APIs remain unknown.",
        "A leitura das importações está incompleta: dependências ELF e importações PE adiadas não são decodificadas. APIs ausentes permanecem desconhecidas.",
    ),
    (
        "probe.modules_partial",
        "Module coverage is incomplete. An API missing from this report may still be in use.",
        "A leitura dos módulos está incompleta. Uma API ausente deste relatório ainda pode estar em uso.",
    ),
    (
        "probe.mode_unknown",
        "Rendering mode was not declared. Specify windowed, borderless or exclusive; this is user input, not an observation.",
        "O modo de exibição não foi informado. Informe janela, sem bordas ou tela cheia exclusiva; isso é uma declaração, não uma observação.",
    ),
    (
        "probe.runtime_unknown",
        "Requested environment is unknown. Specify Windows, Proton or native Linux.",
        "O ambiente desejado é desconhecido. Informe Windows, Proton ou Linux nativo.",
    ),
    (
        "probe.output_unknown",
        "Desired output is unknown. Specify overlay or return into the game.",
        "O resultado desejado é desconhecido. Informe sobreposição ou retorno da imagem ao jogo.",
    ),
    (
        "probe.window_unknown",
        "Actual rendering mode is unknown. Geometry is reported only for one visible, non-minimized Windows window; no titles or pixels are collected.",
        "O modo real de exibição é desconhecido. A geometria só é informada quando há uma única janela Windows visível e não minimizada; nenhum título ou pixel é coletado.",
    ),
    (
        "probe.history",
        "Log observations are historical and cannot be bound to this process or executable hash. No log evidence does not prove no hook.",
        "As observações do log são históricas e não podem ser vinculadas a este processo ou hash do executável. A falta de evidência no log não prova ausência de hook.",
    ),
    (
        "probe.log_scope",
        "Log refused: explicitly select a regular file beside the game executable.",
        "Log recusado: selecione explicitamente um arquivo comum ao lado do executável do jogo.",
    ),
    (
        "probe.launcher_only",
        "Launcher or helper selected. Select the actual game executable/PID before deciding a renderer project.",
        "Launcher ou auxiliar selecionado. Selecione o executável/PID do jogo antes de decidir um projeto de renderização.",
    ),
    (
        "probe.incomplete",
        "Decision refused: incomplete target inspection. These are conditional route hypotheses only.",
        "Decisão recusada: inspeção do alvo incompleta. Estas são apenas hipóteses condicionais de rota.",
    ),
    (
        "probe.no_verdict",
        "Metadata inspected; renderer selection still needs game demand and runtime qualification.",
        "Metadados inspecionados; a escolha do renderizador ainda exige demanda do jogo e validação em execução.",
    ),
    (
        "probe.candidates",
        "Imports and modules are API candidates, never proof of active rendering. Keys below are references only; nothing is enabled.",
        "Importações e módulos são candidatos de API, nunca prova de renderização ativa. As chaves abaixo são apenas referências; nada foi ativado.",
    ),
    (
        "probe.route.capture",
        "Windows external capture is an experimental candidate for windowed/borderless overlay output, including x86, D3D10 and GL. No game key; use NR on any window only in separate qualification. No in-game return or Proton/Linux qualification.",
        "Captura externa no Windows é candidata experimental para sobreposição em janela/sem bordas, incluindo x86, D3D10 e GL. Sem chave no jogo; use NR em qualquer janela somente em validação separada. Não devolve quadros ao jogo nem tem validação em Proton/Linux.",
    ),
    (
        "probe.route.d3d10",
        "No native D3D10 adapter exists. On Windows consider external capture; on Proton first prove x64 DXVK presentation reachability.",
        "Não há adaptador D3D10 nativo. No Windows, considere captura externa; no Proton, primeiro comprove o acesso à apresentação DXVK x64.",
    ),
    (
        "probe.route.gl",
        "No Windows OpenGL in-game adapter exists. External Windows capture is a separate experimental option.",
        "Não existe adaptador OpenGL para processar dentro do jogo no Windows. A captura externa no Windows é uma opção experimental separada.",
    ),
    (
        "probe.route.x86",
        "The current proxy is x64 and cannot load into an x86 game. A 64-bit launcher or OS changes nothing. No x86 helper is provided.",
        "O arquivo de carregamento atual é x64 e não entra em jogos x86. Launcher ou sistema de 64 bits não mudam isso. Não há auxiliar x86 disponível.",
    ),
    (
        "probe.route.dxvk",
        "Only a verified x64 Windows game using D3D10 through DXVK is a candidate. D3D10 and Vulkan names do not prove DXVK or interception. Qualify existing Vulkan creation/present callbacks first.",
        "Somente um jogo Windows x64 verificado usando D3D10 pelo DXVK é candidato. Nomes D3D10 e Vulkan não comprovam DXVK nem interceptação. Valide primeiro os callbacks existentes de criação/apresentação Vulkan.",
    ),
    (
        "probe.route.wined3d",
        "D3D through wined3d uses OpenGL; it does not establish a visible Vulkan presentation route. No dedicated adapter exists.",
        "D3D pelo wined3d usa OpenGL; isso não comprova uma rota visível de apresentação Vulkan. Não há adaptador dedicado.",
    ),
    (
        "probe.route.native_gl",
        "A native Linux GL executable cannot load the Windows proxy. Native capture/transport needs a separate design; Wine presence changes nothing.",
        "Um executável GL nativo do Linux não carrega o arquivo Windows. Captura/transporte nativos exigem um projeto separado; a presença do Wine não muda isso.",
    ),
    (
        "probe.route.d3d11",
        "The existing D3D11 to D3D12 bridge is a conditional x64 candidate. Device/swapchain eligibility, model and actual present evidence still need verification.",
        "A ponte existente D3D11 para D3D12 é candidata condicional em x64. Elegibilidade do dispositivo/swapchain, modelo e evidência de apresentação ainda precisam ser verificadas.",
    ),
    (
        "probe.route.d3d12",
        "The existing D3D12 present route is a conditional x64 candidate. Imported or loaded D3D12 alone does not prove the game reaches it.",
        "A rota existente de apresentação D3D12 é candidata condicional em x64. D3D12 importado ou carregado não comprova que o jogo a alcança.",
    ),
    (
        "probe.route.vulkan",
        "The existing Windows Vulkan present host is a conditional x64 PE candidate. Native Linux ELF and x86 cannot load this proxy. Loader/queue/present coverage remains unproved.",
        "O host existente de apresentação Vulkan para Windows é candidato condicional para PE x64. ELF nativo do Linux e x86 não carregam esse arquivo. Cobertura do carregador/fila/apresentação permanece não comprovada.",
    ),
    ("probe.observed", "observed", "observado"),
    ("probe.unknown", "unknown", "desconhecido"),
    ("probe.candidate", "candidate", "candidato"),
    ("probe.unavailable", "unavailable", "indisponível"),
    ("probe.yes", "yes", "sim"),
    ("probe.no", "no", "não"),
    ("probe.executable", "Selected executable", "Executável selecionado"),
    (
        "probe.process",
        "Selected OS process architecture (Wine host on Linux)",
        "Arquitetura do processo do sistema selecionado (host Wine no Linux)",
    ),
    (
        "probe.identity_label",
        "Stable process identity",
        "Identidade estável do processo",
    ),
    (
        "probe.launcher",
        "Explicit launcher file",
        "Arquivo do launcher informado",
    ),
    (
        "probe.requested",
        "Host / declared environment / mode / output",
        "Sistema / ambiente declarado / modo / resultado",
    ),
    ("probe.complete", "Inspection complete", "Inspeção completa"),
    ("probe.imports", "imports", "importações"),
    ("probe.modules", "modules", "módulos"),
    (
        "probe.capture",
        "Capture API availability (no capture session)",
        "Disponibilidade da API de captura (sem sessão de captura)",
    ),
    ("probe.elapsed", "Elapsed wall time", "Tempo decorrido"),
    (
        "probe.log.vk_create",
        "Historical Vulkan device creation",
        "Criação histórica do dispositivo Vulkan",
    ),
    (
        "probe.log.vk_present",
        "Historical Vulkan first present",
        "Primeira apresentação Vulkan histórica",
    ),
    (
        "probe.log.bridge",
        "Historical D3D11 bridge creation",
        "Criação histórica da ponte D3D11",
    ),
    (
        "probe.log.dx12",
        "Historical D3D12 present boundary",
        "Ponto histórico de apresentação D3D12",
    ),
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
        "O estado do ReShade foi alterado ou não pertence ao registro de instalação",
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
        "O ReShade exige o arquivo de carregamento e o executável na mesma pasta",
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
        "O backup do ReShade mudou; preserve o registro de instalação",
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
        "A versão do ReShade é limitada ou desconhecida; o suporte completo a add-ons não está confirmado",
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
        "Retoque keeps the game's loader file and loads ReShade64.dll.",
        "O Retoque mantém o arquivo de carregamento do jogo e carrega o ReShade64.dll.",
    ),
    ("rs.enabled", "Enabled", "Ativado"),
    ("rs.disabled", "Disabled", "Desativado"),
    ("rs.full", "Full add-on support", "Suporte completo a add-ons"),
    (
        "rs.limited",
        "Limited ReShade version: external add-ons cannot load",
        "Versão limitada do ReShade: add-ons externos não carregam",
    ),
    (
        "rs.unknown",
        "Unknown ReShade version",
        "Versão do ReShade desconhecida",
    ),
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
        "Not yet tested in this game. Restart the game after changes.",
        "Ainda não testado neste jogo. Reinicie o jogo após alterações.",
    ),
    ("rs.addons", "Add-ons", "Add-ons"),
    ("rs.build", "ReShade version", "Versão do ReShade"),
    (
        "rs.install",
        "Review ReShade installation…",
        "Revisar instalação do ReShade…",
    ),
    ("rs.uninstall", "Remove ReShade component", "Remover componente ReShade"),
    (
        "rs.import",
        "Import local ReShade DLL or setup…",
        "Importar DLL ou instalador local do ReShade…",
    ),
    ("rs.add", "Import add-on…", "Importar add-on…"),
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
        "rs.drop_reshade",
        "Import this ReShade DLL or setup into the app? Installing it in a game has its own review.",
        "Importar esta DLL ou instalador do ReShade para o app? A instalação num jogo tem revisão própria.",
    ),
    (
        "rs.select_game",
        "Select a game before importing an add-on.",
        "Selecione um jogo antes de importar um add-on.",
    ),
    (
        "rs.nr_off",
        "Neural Rendering (NR): off",
        "Renderização neural (NR): desligada",
    ),
    (
        "rs.nr_addon",
        "Neural Rendering (NR): by the add-on RenoDX DLSS5",
        "Renderização neural (NR): pelo add-on RenoDX DLSS5",
    ),
    (
        "rs.nr_retoque",
        "Neural Rendering (NR): by Retoque",
        "Renderização neural (NR): pelo Retoque",
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
        "Let both menus use the mouse…",
        "Deixar os dois menus usarem o mouse…",
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
        "An unfinished install record exists, but Retoque could not inspect this installation. Keep it and review the repair; other checks are unavailable.",
        "Há um registro de instalação inacabado, mas o Retoque não conseguiu inspecionar a instalação. Preserve-o e revise o reparo; as demais verificações estão indisponíveis.",
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
        "The last install or uninstall stopped part-way. Review the repair to finish it.",
        "A última instalação ou desinstalação parou no meio. Revise o reparo para concluí-la.",
    ),
    (
        "doctor.version.title",
        "Retoque's files have different versions",
        "Os arquivos do Retoque têm versões diferentes",
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
        "NR model does not work on this graphics card",
        "O modelo NR não funciona nesta placa de vídeo",
    ),
    (
        "doctor.model_incompatible.why",
        "The model was not built for this graphics card. A suggested model from your models is still untested in this game.",
        "O modelo não foi feito para esta placa de vídeo. Um modelo sugerido dentre os seus ainda não foi testado neste jogo.",
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
        "LSFG needs FSR frame generation output (Settings › Frame generation › FG output). This setting alone does not prove it is ready.",
        "O LSFG precisa da saída de geração de quadros FSR (Ajustes › Geração de quadros › Saída da geração). Esse ajuste sozinho não comprova que esteja pronto.",
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
        "Launch option missing",
        "Falta a opção de inicialização",
    ),
    (
        "doctor.launch.why",
        "The game is not told to load Retoque yet. Copy the launch option shown in this tab into your launcher.",
        "O jogo ainda não foi avisado para carregar o Retoque. Copie a opção de inicialização mostrada nesta aba para o seu launcher.",
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
        "NR before upscaling fell back to the safe path",
        "O NR antes da ampliação voltou ao modo seguro",
    ),
    (
        "log.pre.why",
        "Set NR stage to After the upscaler in Settings to use the post-upscale route.",
        "Escolha Etapa do NR › Depois da ampliação em Ajustes para usar a rota após a ampliação.",
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
        "Not yet tested in this game",
        "Ainda não testado neste jogo",
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
    ("evidence.tracked", "Install record", "Registro de instalação"),
    ("evidence.runtime", "Runtime", "Ambiente de execução"),
    ("evidence.dlss", "DLSS DLL present", "DLL DLSS presente"),
    ("evidence.dlssg", "DLSSG DLL present", "DLL DLSSG presente"),
    ("evidence.sm86", "sm86 installed", "sm86 instalado"),
    ("evidence.sm86_legacy", "Legacy sm86", "sm86 legado"),
    ("evidence.sm86_docs", "sm86 documentation", "Documentação sm86"),
    ("evidence.field", "Field test", "Teste em jogo"),
    ("evidence.lsfg", "LSFG enabled", "LSFG ligado"),
    ("evidence.gpu", "GPU identity", "Identidade da GPU"),
    ("evidence.api", "Last session API", "API da última sessão"),
    (
        "evidence.receipt",
        "Install record status",
        "Estado do registro de instalação",
    ),
    ("evidence.pending", "Unfinished operation", "Operação inacabada"),
    (
        "evidence.proxy_version",
        "Loader file version",
        "Versão do arquivo de carregamento",
    ),
    (
        "evidence.forwarder_version",
        "NR component version",
        "Versão do componente NR",
    ),
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
        "Opção de inicialização necessária",
    ),
    (
        "evidence.override",
        "Launch option present",
        "Opção de inicialização presente",
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
        "DLSS install record needs recovery",
        "O registro de instalação do DLSS precisa de recuperação",
    ),
    (
        "doctor.dlss_root.why",
        "A pending or incomplete DLSS operation remains at the game installation root. Review restoring both install records.",
        "Há uma operação DLSS pendente ou incompleta na raiz do jogo. Revise a restauração dos dois registros de instalação.",
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
