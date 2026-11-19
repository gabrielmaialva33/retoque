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
`TEXT`, `TEXT_STRONG`, `ON_ACCENT`, `ON_IMAGE` and `CANVAS`. Resolve these from `theme::palette(ui)`;
its dark and light palettes own every UI surface, widget fill and stroke. No raw colours outside `theme.rs`.
Use the local UI, not a process-global theme: art-backed headers have a dark local scope in both appearances.
Texture tint and image overlay ink/scrims are explicit image roles, independent of appearance; never tint the
actual comparison pixels. Text/surface pairs meet 4.5:1, control edges and focus rings 3:1 in both palettes.
Selected rows use strong text; image labels have a scrim and image focus/wipe rules have opposite outlines.

Preferences offers **Appearance / Aparência**: System, Dark, Light, applied immediately and saved. Dark remains
the default for new and pre-existing preferences; System follows OS theme events without reinstalling fonts.
The light theme idea came from the NeuRotic Hub comparison; palettes, text and implementation are independent.

## Rules

- **One primary per view.** The accent fill marks the one thing the user most likely came to do. Every other
  action is a plain button.
- **"…" means a dialog or review opens.** A label ends in "…" in every language if clicking it opens a review,
  dialog or file picker, and in none otherwise. A review's title is the label without "…".
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

One term per idea. Portuguese is the default language; English is the second and the source of the eight
translated packs (`src/i18n/*.tsv`, see the app README), each of which renders every row below with one term too.

| Idea | pt-BR | en | Never |
|---|---|---|---|
| Ask the game’s launcher to start it | Jogar | Play | “Jogo iniciado” before process detection |
| Games starred by the user | Favoritos | Favorites | “Fixados” |
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

## Game shortcuts

Play is a plain secondary beside optimization, without “…”, and only for a verified launcher identifier.
While a game-folder write runs, it is disabled with a visible reason; feedback says the launcher was asked.
A star on the page and cards is independently keyboard reachable, shows an outline/fill and focus ring,
and announces “Add to favorites: {game}” or “Remove from favorites: {game}” plus the row/card's favorite state.
Sidebar rows have no star control. On cards, paint the badge only for favorites or on hover/focus;
it remains reachable by Tab even when not painted. Favorites form the first filtered library group. Library cards/rows still open the game page; they have no Play.
The existing More actions menu holds game folder, settings file and captures folder, the latter two only if present.

## Responsive game header

The banner grows with its content at every zoom: its art and shade enclose the wrapped actions. Keep the game
name or logo, installation state, primary action, Play and store/runtime here. Component versions and the store's
raw identifier belong in game details. Before installation, the card explaining the primary action comes before
informational health-check notes; a failed change still comes first.
The five status tiles fill one row when there is room, otherwise two full-width rows of three and two. An extremely
narrow page uses a readable vertical stack instead of leaving a single orphan below a two-column grid.

The top bar keeps the Retoque mark at 28 points beside the word, in the same named home button. Reserve its
width when choosing which actions move into the overflow menu; the brand itself never goes into that menu.

## Library hierarchy

Rows and cover titles get up to two lines before ellipsis; their accessible names and hovers keep the full name
and installation state. A homogeneous visible list omits repeated store/runtime captions. A mixed list keeps
that context, and the game page always names its store/runtime.
Current and ordinary installed entries are quiet. Updates, incomplete or unregistered old installations, and
cached warning/error findings show a status word plus an emphasized edge; information alone is not attention.
“Needs attention” combines with search, “Installed only” and favorites, and keyboard navigation follows the same
filtered groups. Its help says that it includes problems already found, not a new inspection of every game.

### Page chrome

Game details name their contents: files, versions and folder. The settings explanation is one muted line;
its complete saved-versus-live explanation remains available on hover. Search and settings actions share
one row; secondary controls move into a named Options menu when they would squeeze search below 160 points.
Comparison controls with symbols or numeric shorthand have descriptive hover text and accessible names.

## Window capture

| Idea | pt-BR | en | Never |
|---|---|---|---|
| Windows capture screen | NR em qualquer janela | NR on any window | A Linux control for Windows capture |
| Output in another window | Prévia separada | Separate preview | Claiming NR is running when only capture started |
| Output over the captured window | Sobre a janela original | Over the source window | An overlay without its stop shortcut |
| Fraction used by the model | Escala de processamento | Processing scale | Output resolution or upscaling |
| Motion estimated from capture | Estimar movimento pela imagem | Estimate motion from the image | Native game motion vectors |

The screen has one primary action: Start… when idle, Stop while active. Start reviews the exact session choices
and private-folder preparation; its confirm is Start. Settings stay locked during a session because protocol 1
has no live settings command. Capture opened, NR running, paused, stopping, refused and restart required are
separate worded states. Raw host responses stay in Technical details. Linux ships neither this screen nor its entry.
