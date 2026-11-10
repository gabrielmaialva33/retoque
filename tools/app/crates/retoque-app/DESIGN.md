# Retoque app: design rules

The app is for people who play games, not for engineers. One visual system, one word per idea, and actions that
say what they do. These rules apply to every screen; code review should hold new UI to them.

## Building blocks

Use these instead of building a look by hand.

| Need | Use |
|---|---|
| A card, and its title | `widgets::card`, `widgets::card_title` |
| A heading over a group of cards | `widgets::section_header` |
| A collapsible part | `widgets::collapsible(…, Fold::Section)`; evidence, hashes and hosts use `Fold::Details` |
| The single main action of a view | `theme::primary` |
| A confirm that deletes or uninstalls | `theme::danger` |
| Any other button | `ui.button` (32 px, never `small_button`) |
| Closing a notice | `widgets::close_button` |
| Work in progress | `widgets::busy` (always with a sentence; a bar when the total is known) |
| A download or change the user must agree to | `widgets::consent_modal` (`consent_modal_when` to hold the confirm while a write runs); the confirm label from `format::download_label` ("Baixar (115 MB)") |
| Deleting something the app keeps | `widgets::delete_modal` (a `danger` "Apagar") |
| Any other modal | `egui::Modal` with `widgets::modal_frame()` |
| A failure | `error::card` on a page, `error::body` inside a dialog, `error::summary` in a bar; all use `AppError::user_text` and fold the raw text under Details |
| Information or a warning that is not a failure | `AppError::info` / `AppError::warning`, shown with `error::notice` (MUTED / WARN, never BAD) |
| A repeated button (one per card or row) | `widgets::name_after` so screen readers hear "{action}: {item}" |
| A setting changed but not yet applied; back to automatic | `widgets::change_bar`; `widgets::reset_button` |
| A size | `format::size_text` (KB/MB/GB, decimal comma in Portuguese) |
| Status tiles in a row | `widgets::feature_tile` / `stat_tile_width` sized by `widgets::row_width` |

Sizes and colours come from `theme`: `TITLE` (17), `SECTION` (20) and `DISPLAY` (22) beside the Body (14.5) and
Small (13) text styles, and `CARD_GAP` (12) between cards. Colours are `ACCENT`, `GOOD`, `WARN`, `BAD`, `MUTED`,
`TEXT`, `TEXT_STRONG`, `ON_ACCENT`, `ON_IMAGE` and `CANVAS`; no raw `Color32::from_rgb` outside `theme.rs`.

## Rules

- **One primary per view.** The accent fill marks the one thing the user most likely came to do. Every other
  action is a plain button.
- **"…" means a dialog or review opens.** A label ends in "…" in both languages if clicking it opens a review,
  dialog or file picker, and in neither otherwise. A review's title is the label without "…".
- **The confirm and the result repeat the verb.** "Instalar e otimizar…" leads to a review confirmed with
  "Instalar e otimizar" and a toast "Retoque instalado e otimizado". Never "Confirmar" or "OK".
- **Nothing internal reaches the user.** No Rust `Debug` output, job names, exit codes, INI syntax, hashes or
  English OS text in the main view. Technical detail goes under `Fold::Details` or into the app log.
- **Information is not an error.** Progress and "nothing to do" are `MUTED`, something left out is `WARN`, only a
  failure is `BAD`. Every error says what to do next.
- **Status never relies on colour alone.** A coloured dot or bar always has a word beside it, and screen readers
  get the state in the widget's label ("Cyberpunk 2077, atualizado").
- **Repeated buttons say what they act on** for screen readers: `"{action}: {item}"`.
- **Deleting needs a confirmation.** Anything that removes files asks first, with a `danger` confirm.
- **Offers the app makes on its own** are dismissed with "Agora não"; actions the user started are dismissed with
  "Cancelar"; information dialogs close with "Fechar".

## Glossary

One term per idea. Portuguese is the default language; English is the second.

| Idea | pt-BR | en | Never |
|---|---|---|---|
| Put Retoque into a game, tuned | Instalar e otimizar… | Install and optimize… | "Otimizar automaticamente" as the main action |
| Tune an installed game | Otimizar… | Optimize… | "Aplicar recomendação" as the main action |
| Newer Retoque files over an install | Atualizar o Retoque… | Update Retoque… | "Revisar atualização do jogo" |
| Adopt an install made by the script | Registrar instalação antiga… | Register old install… | "recibo", "receipt" |
| Take Retoque out of a game | Desinstalar… | Uninstall… | "Remover" for this |
| Take an entry out of a list | Remover da lista | Remove from list | "Tirar da lista" |
| Delete the app's stored copy | Apagar do app… | Delete from app… | "Remover do catálogo" |
| Put original files back | Restaurar | Restore | |
| Open a change list | Revisar… | Review… | "Ver o que … faz" |
| Pending edits | alterações | changes | "mudanças" |
| Per-game options | Ajustes | Settings | "INI" in UI text |
| Default value | Automático | Automatic | "auto", "Padrão do jogo" |
| The nvngx_dlssnr.dll file | modelo NR | NR model | "DLL do modelo" in prose |
| Models and components the app keeps | Seus modelos / Seus componentes | Your models / Your components | "Catálogo do aplicativo", "catalogue" |
| The feature | Renderização neural (NR) | Neural Rendering (NR) | "Neural Rendering" in Portuguese |
| Frame multiplication | geração de quadros | frame generation | bare "FG" |
| Retoque's own files | arquivos do Retoque | Retoque files | "payload", "pacote" |
| A published version | versão (oficial) | (official) release | "release" in Portuguese |
| A build known only by its hash | compilação local | local build | "versão {hash}" |
| Fetch from the internet | Baixar (tamanho) | Download (size) | "Obter", "Get" |
| Graphics card | placa de vídeo | GPU | "sm_86", "alvo CUDA" in prose |
| Works on this card | compatível com sua placa de vídeo | compatible with your GPU | "alvo CUDA corresponde" |
| Health check | Diagnóstico do Retoque | Retoque health check | "Retoque Doctor" |
| File for support | arquivo de diagnóstico | diagnostic file | |
| Launch override | opção de inicialização | launch option | "override", "WINEDLLOVERRIDES" in prose |
| Loader DLL name (advanced) | nome do arquivo de carregamento | loader file name | "proxy", "Carregar como" |
| Interrupted install | instalação incompleta | incomplete install | "interrompida", "precisa de limpeza" |

English text uses US spelling (catalog, optimize, center).
