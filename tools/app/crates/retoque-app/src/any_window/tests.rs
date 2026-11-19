use super::{protocol::*, screen::Screen};
use crate::{
    i18n::{Lang, t},
    theme,
};
use std::io::Cursor;
#[cfg(unix)]
use std::time::Duration;

fn settings() -> Settings {
    Settings {
        model: std::env::current_dir().unwrap().join("synthetic-model.dll"),
        ..Default::default()
    }
}
fn event(text: &str) -> Event {
    read_event(&mut Cursor::new(format!("{text}\n"))).unwrap().unwrap()
}
const STARTED: &str =
    r#"{"event":"started","protocol":1,"nr":true,"output":"preview","disengage_hotkey":"Escape in preview"}"#;
#[test]
fn settings_round_trip_and_launch_contract() {
    let mut s = settings();
    s.strength = 63;
    s.scale = 37;
    s.motion = true;
    s.compare = 2;
    let ini = retoque_core::ini::Ini::parse(&s.ini().unwrap());
    for (key, value) in [
        ("TransferStrength", "0.63"),
        ("WorkingScale", "0.37"),
        ("SynthMotion", "true"),
        ("UiProtection", "true"),
        ("Compare", "2"),
        ("HookMethod", "2"),
    ] {
        assert_eq!(ini.get("DlssNr", key), Some(value));
    }
    assert_eq!(s.arguments(), ["--run", "--countdown", "5"]);
    s.output = Output::Overlay;
    s.source = Source::Window("0x1AB".into());
    assert_eq!(s.arguments(), ["--run", "--window", "0x1AB", "--overlay"]);
    s.source = Source::Window("0x1 --no-nr".into());
    assert!(s.validate().is_err());
    s = settings();
    s.model = std::env::current_dir().unwrap().join("bad\nEnabled=false");
    assert!(s.ini().is_err());
    assert_eq!(command(Command::Stop, 9), "{\"cmd\":\"stop\",\"rev\":9}\n");
}
#[test]
fn bounded_parser() {
    for bytes in [
        b"garbage\n".to_vec(),
        STARTED.as_bytes().to_vec(),
        vec![b'x'; MAX_LINE + 1],
        b"{\"event\":\"unknown\"}\n".to_vec(),
        b"{\"event\":\"window\",\"hwnd\":\"bad\",\"pid\":1,\"title\":\"test\"}\n".to_vec(),
    ] {
        assert!(read_event(&mut Cursor::new(bytes)).is_err());
    }
}
#[test]
fn requested_applied_readiness_and_restart_latch() {
    let mut state = State::default();
    state.begin(Some(settings()));
    assert!(state.applied.is_none());
    state.accept(event(STARTED));
    assert_eq!(state.phase, Phase::Capturing);
    assert_eq!(state.applied, state.requested);
    state.accept(event(r#"{"event":"nr","running":true,"status":"present"}"#));
    assert_eq!(state.phase, Phase::Running);
    state.accept(event(r#"{"event":"status","rev":999,"state":"running"}"#));
    assert_eq!(state.confirmed_revision, None);
    state.accept(event(r#"{"event":"status","rev":1,"state":"running"}"#));
    assert_eq!(state.confirmed_revision, Some(1));
    state.phase = Phase::Stopping;
    state.accept(event(STARTED));
    assert_eq!(state.phase, Phase::Stopping);
    state.accept(event(r#"{"event":"restart-required","reason":"device lost"}"#));
    state.accept(event(r#"{"event":"stopped","reason":"done","exit":0}"#));
    state.exited(Some(0), true);
    assert_eq!(state.phase, Phase::RestartRequired);
}
#[test]
fn phase_three_refusals_have_specific_remedies() {
    for (reason, expected) in [
        ("cursor-capture-control-unavailable", Problem::Cursor),
        ("exclusive-fullscreen", Problem::Exclusive),
        ("source-topmost", Problem::Topmost),
        ("Ctrl+Alt+F10 is unavailable", Problem::Hotkey),
    ] {
        assert_eq!(Problem::reason("overlay", reason), expected);
        for lang in [Lang::En, Lang::Pt] {
            assert!(!t(lang, expected.key()).starts_with("any."));
        }
    }
}
#[cfg(unix)]
mod processes {
    use super::*;
    use crate::any_window::process::{Child, Limits, Session, Spawned};
    use std::{io, time::Instant};
    struct Fake(std::process::Child);
    impl Child for Fake {
        fn try_wait(&mut self) -> io::Result<Option<i32>> {
            self.0.try_wait().map(|s| s.map(|s| s.code().unwrap_or(-1)))
        }
        fn kill_wait(&mut self) -> io::Result<i32> {
            let _ = self.0.kill();
            Ok(self.0.wait()?.code().unwrap_or(-1))
        }
    }
    impl Drop for Fake {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    pub(super) fn fake(script: String, timeout: Duration) -> Session {
        Session::start(
            move |_| {
                let mut child = std::process::Command::new("python3")
                    .args(["-u", "-c", &script])
                    .stdin(std::process::Stdio::piped())
                    .stdout(std::process::Stdio::piped())
                    .stderr(std::process::Stdio::null())
                    .spawn()?;
                let input = Box::new(child.stdin.take().unwrap());
                let output = Box::new(child.stdout.take().unwrap());
                Ok(Spawned {
                    child: Box::new(Fake(child)),
                    input,
                    output,
                    folder: None,
                })
            },
            1,
            Limits {
                start: timeout,
                stop: Duration::from_millis(100),
            },
        )
    }
    fn finish(session: &Session, state: &mut State) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while !session.done() {
            session.poll(state);
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
        session.poll(state);
    }
    #[test]
    fn normal_crash_garbage_overflow_restart_slow_and_stop_during_start() {
        for (script, expected) in [
            (
                format!("print('{STARTED}')\nprint('{{\"event\":\"stopped\",\"reason\":\"done\",\"exit\":0}}')"),
                Phase::Stopped,
            ),
            ("raise SystemExit(9)".into(), Phase::Failed),
            ("print('garbage')".into(), Phase::Failed),
            (format!("print('x' * {})", MAX_LINE + 1), Phase::Failed),
            (
                "print('{\"event\":\"restart-required\",\"reason\":\"lost\"}')\nraise SystemExit(7)".into(),
                Phase::RestartRequired,
            ),
            ("import time; time.sleep(20)".into(), Phase::Failed),
        ] {
            let session = fake(script, Duration::from_millis(300));
            let mut state = State::default();
            state.begin(Some(settings()));
            finish(&session, &mut state);
            assert_eq!(state.phase, expected, "{}", state.detail);
        }
        let session = fake("import time; time.sleep(20)".into(), Duration::from_secs(30));
        let mut state = State::default();
        state.begin(Some(settings()));
        session.stop();
        state.phase = Phase::Stopping;
        finish(&session, &mut state);
        assert_eq!(state.phase, Phase::Stopped);
    }
    #[test]
    fn stop_command_is_sent_and_acknowledged() {
        let session = fake(
            format!(
                "import sys,json\nprint('{STARTED}')\nfor line in sys.stdin:\n if json.loads(line)['cmd']=='stop':\n  print('{{\"event\":\"stopped\",\"reason\":\"stop command\",\"exit\":0}}'); break"
            ),
            Duration::from_secs(5),
        );
        let mut state = State::default();
        state.begin(Some(settings()));
        let deadline = Instant::now() + Duration::from_secs(3);
        while state.phase == Phase::Starting {
            session.poll(&mut state);
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
        session.stop();
        finish(&session, &mut state);
        assert_eq!(state.phase, Phase::Stopped);
        assert_eq!(state.detail, "stop command");
    }
    #[test]
    fn slow_success_and_stop_after_countdown_begins() {
        let session = fake(
            format!(
                "import time,sys,json\nprint('{{\"event\":\"countdown\",\"seconds\":5}}')\ntime.sleep(0.15)\nprint('{STARTED}')\nfor line in sys.stdin:\n if json.loads(line)['cmd']=='stop':\n  print('{{\"event\":\"stopped\",\"reason\":\"stop command\",\"exit\":0}}'); break"
            ),
            Duration::from_secs(3),
        );
        let mut state = State::default();
        state.begin(Some(settings()));
        let deadline = Instant::now() + Duration::from_secs(3);
        while state.countdown.is_none() {
            session.poll(&mut state);
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
        assert!(state.applied.is_none());
        while state.applied.is_none() {
            session.poll(&mut state);
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
        session.stop();
        finish(&session, &mut state);
        assert_eq!(state.phase, Phase::Stopped);

        let session = fake(
            "import time\nprint('{\"event\":\"countdown\",\"seconds\":5}')\ntime.sleep(20)".into(),
            Duration::from_secs(30),
        );
        let mut state = State::default();
        state.begin(Some(settings()));
        let deadline = Instant::now() + Duration::from_secs(3);
        while state.countdown.is_none() {
            session.poll(&mut state);
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
        session.stop();
        state.phase = Phase::Stopping;
        finish(&session, &mut state);
        assert_eq!(state.phase, Phase::Stopped);
        assert!(state.applied.is_none());
    }

    #[test]
    fn flooded_queue_is_bounded_and_catalog_count_is_checked() {
        let session = fake(
            "for i in range(100000): print('{\"event\":\"frame\"}')".into(),
            Duration::from_secs(5),
        );
        let mut state = State::default();
        state.begin(Some(settings()));
        finish(&session, &mut state);
        assert_eq!(state.phase, Phase::Failed);
        let mut state = State::default();
        state.begin(None);
        state.accept(event(r#"{"event":"catalog","windows":1}"#));
        assert_eq!(state.phase, Phase::Failed);
    }
}
fn model() -> retoque_data::models::Model {
    retoque_data::models::Model {
        path: settings().model,
        label: "Synthetic model".into(),
        sha256: "a".repeat(64),
        version: None,
        size: 1,
        compatibility: Default::default(),
    }
}
#[cfg(feature = "visual-tests")]
fn screen(lang: Lang, phase: Phase) -> egui_kittest::Harness<'static, Screen> {
    screen_appearance(lang, phase, theme::Appearance::Dark)
}
fn screen_appearance(
    lang: Lang,
    phase: Phase,
    appearance: theme::Appearance,
) -> egui_kittest::Harness<'static, Screen> {
    let mut screen = Screen {
        available: true,
        draft: settings(),
        ..Default::default()
    };
    screen.state.phase = phase;
    screen.open = true;
    if phase == Phase::Failed {
        screen.state.problem = Some(Problem::Cursor);
    }
    if phase == Phase::RestartRequired {
        screen.state.problem = Some(Problem::Restart);
    }
    #[cfg(unix)]
    if matches!(
        phase,
        Phase::Starting | Phase::Running | Phase::Paused | Phase::Stopping | Phase::Capturing | Phase::Catalog
    ) {
        screen.session = Some(processes::fake(
            "import time; time.sleep(20)".into(),
            Duration::from_secs(30),
        ));
        screen.state.overlay = phase == Phase::Running;
        if screen.state.overlay {
            screen.draft.output = Output::Overlay;
        }
    }
    egui_kittest::Harness::builder()
        .with_theme(if appearance == theme::Appearance::Light {
            egui::Theme::Light
        } else {
            egui::Theme::Dark
        })
        .with_size(egui::vec2(1024., 900.))
        .build_ui_state(
            move |ui, state: &mut Screen| {
                theme::apply(ui.ctx());
                appearance.apply(ui.ctx());
                assert_eq!(
                    theme::palette(ui).BG,
                    if appearance == theme::Appearance::Light {
                        theme::LIGHT.BG
                    } else {
                        theme::DARK.BG
                    }
                );
                egui::CentralPanel::default().show(ui, |ui| {
                    let _ = state.draw(ui, lang, &[model()], false);
                });
                state.review(ui.ctx(), lang, &[model()]);
            },
            screen,
        )
}
#[test]
fn screen_states_in_english_and_portuguese() {
    use egui_kittest::kittest::Queryable;
    for appearance in [theme::Appearance::Dark, theme::Appearance::Light] {
        for lang in [Lang::En, Lang::Pt] {
            for phase in [
                Phase::Idle,
                Phase::Catalog,
                Phase::Starting,
                Phase::Capturing,
                Phase::Running,
                Phase::Paused,
                Phase::Stopping,
                Phase::Stopped,
                Phase::Failed,
                Phase::RestartRequired,
            ] {
                let mut h = screen_appearance(lang, phase, appearance);
                h.run_steps(4);
                h.get_by_label(t(lang, "any.title"));
                assert!(h.get_all_by_label(t(lang, phase.key())).next().is_some());
            }
            let mut h = screen_appearance(lang, Phase::Idle, appearance);
            h.run_steps(4);
            h.get_by_label(t(lang, "any.start_review")).click();
            h.run_steps(4);
            assert!(h.state().review.is_some());
            h.get_by_label(t(lang, "any.review_body"));
            // Exercise the controller helpers in the portable harness too.
            h.state_mut().stop();
            h.state_mut().poll();
            assert!(h.state().open);
        }
    }
}
#[cfg(feature = "visual-tests")]
#[test]
#[ignore = "offscreen synthetic renders; requires RETOQUE_ANY_SHOTS"]
fn render_states() {
    let out = std::path::PathBuf::from(std::env::var("RETOQUE_ANY_SHOTS").unwrap());
    std::fs::create_dir_all(&out).unwrap();
    for lang in [Lang::En, Lang::Pt] {
        for (name, phase) in [
            ("idle", Phase::Idle),
            ("catalog", Phase::Catalog),
            ("starting", Phase::Starting),
            ("capturing", Phase::Capturing),
            ("paused", Phase::Paused),
            ("stopping", Phase::Stopping),
            ("stopped", Phase::Stopped),
            ("running", Phase::Running),
            ("refused", Phase::Failed),
            ("restart", Phase::RestartRequired),
        ] {
            let mut h = screen(lang, phase);
            h.run_steps(4);
            h.render()
                .unwrap()
                .save(out.join(format!("nr-anywhere-{}-{name}.png", lang.code())))
                .unwrap();
        }
    }
}

#[cfg(not(windows))]
#[test]
fn linux_product_has_no_window_capture_entry() {
    use egui_kittest::kittest::Queryable;
    let mut app = crate::app_tests::shell_tests::app();
    app.lang = Lang::Pt;
    let mut h = egui_kittest::Harness::builder()
        .with_size(egui::vec2(1280., 800.))
        .build_ui_state(
            |ui, app: &mut crate::app::RetoqueApp| {
                theme::apply(ui.ctx());
                app.window(ui);
            },
            app,
        );
    h.run_steps(3);
    assert!(h.query_by_label(t(Lang::Pt, "any.title")).is_none());
}
