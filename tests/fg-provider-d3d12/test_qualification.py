#!/usr/bin/env python3
"""Host parser/config and portable C++ schedule controls; no Wine or GPU."""
import configparser
import contextlib
import copy
import hashlib
import importlib.util
import io
import shutil
from pathlib import Path
import unittest
import tempfile
import signal
import subprocess
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fg_provider_fixture', HERE/'run.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def record(multiplier=2, *, observed=None, maximum=None, interval_us=100_000,
           first_window_fallback=False, steady_seconds=0, active_count=96,
           warmup_interval_us=None, disabled_interval_us=None, duration_records=True):
    """Literal deltas for a synthetic evidence record; no hardware claim."""
    observed = multiplier if observed is None else observed
    maximum = multiplier-1 if maximum is None else maximum
    request = f'REQUEST multiplier={multiplier} generated={multiplier-1}'
    if steady_seconds:
        request += f' steady_seconds={steady_seconds}'
    lines = [request,
             'ORACLE=real-output-plugin',
             'GPU vendor=0x10de device=0x2684 subsystem=0x000010de revision=161 '
             'luid=00000000:00001234 description=NVIDIA GeForce RTX 4090',
             r'MODULE role=output_plugin path=Z:\fixture\streamline\sl.dlss_g.dll',
             r'MODULE role=provider path=Z:\fixture\streamline\nvngx_dlssg.dll']
    frame, clock, total_actual = 0, 0, 0
    for phase in range(4):
        count = 48 if phase == 1 else active_count
        lines.append(f'BASELINE phase={phase} us={clock}')
        actual, steady_actual, window_start, complete_windows = 0, 0, None, 0
        for local in range(count):
            frame += 1
            clock += (disabled_interval_us if phase == 1 and disabled_interval_us is not None else
                      warmup_interval_us if local < 32 and warmup_interval_us is not None else interval_us)
            if local == 31:
                steady_start = window_start = clock
            elif local >= 32 and clock-window_start >= 5_000_000:
                complete_windows += 1
                window_start = clock
            delta = 1 if phase == 1 else observed
            if first_window_fallback and phase == 0 and local >= 32:
                # Overall steady ratio is exactly4: 50*3 +13*8 +1*2=256.
                # The first complete five-second window is only3x.
                delta = 3 if local < 82 else 8 if local < 95 else 2
            actual += delta
            if local >= 32:
                steady_actual += delta
            lines.extend([f'FRAME phase={phase} app={frame} actual={delta} status=0 max={maximum}',
                          f'SAMPLE phase={phase} app={frame} us={clock}'])
        total_actual += actual
        if duration_records and (steady_seconds or duration_records == 'always'):
            lines.append(f'DURATION phase={phase} requested_us={0 if phase == 1 else steady_seconds*1_000_000} '
                         f'steady_start_us={steady_start} end_us={clock} complete_windows={complete_windows}')
        lines.append(f'PHASE phase={phase} enabled={int(phase != 1)} app={count} actual={actual} '
                     f'steady_app={count-32} steady_actual={steady_actual} ok={count-32} completed={count}')
    lines.append(f'PASS: FG-PROVIDER app={frame} actual={total_actual} phases=4 resizes=1 oracle=real-plugin')
    return '\n'.join(lines)



class DurationQualification(unittest.TestCase):
    # Catch omitted CLI validation and mistaken default duration activation.
    def test_cli_duration_is_explicit_bounded_integer(self):
        self.assertEqual(fixture.parse_args([]).steady_seconds, 0)
        for value in ('5', '15', '120'):
            self.assertEqual(fixture.parse_args(['--steady-seconds', value]).steady_seconds, int(value))
        for value in ('0', '-1', '4', '121', 'nan', 'inf', '-inf', '15.5', ''):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    fixture.parse_args(['--steady-seconds', value])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            fixture.parse_args(['--steady-seconds'])

    # Catch a timeout independent of requested runtime or unbounded growth.
    def test_timeout_includes_all_active_phases_and_bounded_startup(self):
        self.assertEqual(fixture.runtime_timeout(0), 240)
        self.assertEqual(fixture.runtime_timeout(15), 300)
        self.assertEqual(fixture.runtime_timeout(120), 615)

    def test_long_x2_qualification_gaps_propagate_to_status_and_exit(self):
        # An explicit qualification request must not inherit quick x2's override.
        valid = record(2, steady_seconds=15, active_count=182)
        missing_gpu = '\n'.join(line for line in valid.splitlines() if not line.startswith('GPU '))
        low_cap = record(2, steady_seconds=15, active_count=182, maximum=0)
        # First5s window is1.5x; steady totalstill has surplus and laterwindows2x.
        degraded = valid
        for frame in range(33, 58):
            degraded = degraded.replace(f'FRAME phase=0 app={frame} actual=2 ',
                                        f'FRAME phase=0 app={frame} actual=1 ')
        degraded = degraded.replace('app=182 actual=364 steady_app=150 steady_actual=300',
                                    'app=182 actual=339 steady_app=150 steady_actual=275', 1)
        degraded = degraded.replace('app=594 actual=1140', 'app=594 actual=1115')
        # The existing x2 regression gate already rejects cap0 as invalid runtime evidence.
        with self.assertRaises(RuntimeError):
            fixture.assess(low_cap, 2, steady_seconds=15)
        for text in (missing_gpu, degraded):
            with self.subTest(record=text[:90]):
                result = fixture.assess(text, 2, steady_seconds=15)
                self.assertEqual(result['runtime_regression'], 'PASS')
                self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')
                self.assertEqual(result['status'], 'INCONCLUSIVE')
                self.assertTrue(result['reason'])
                self.assertEqual(fixture.result_exit_code(result), 1)
        result = fixture.assess(valid, 2, steady_seconds=15)
        self.assertEqual(fixture.result_exit_code(result), 0)
        quick = fixture.assess(record(2, interval_us=8_000), 2)
        self.assertEqual(quick['qualification']['status'], 'INCONCLUSIVE')
        self.assertEqual(quick['status'], 'PASS')
        self.assertEqual(fixture.result_exit_code(quick), 0)

    def test_long_x2_runtime_identity_gaps_propagate_but_quick_compatibility_remains(self):
        complete = {'cpu': {'model': 'controlled CPU'},
                    'proton_launcher': {'sha256': 'a'*64},
                    'proton_version_file': {'text': 'controlled Proton version', 'sha256': 'b'*64},
                    'linux_nvidia_driver': {'text': 'controlled driver version', 'sha256': 'c'*64},
                    'loaded_module_files': {'provider': {'sha256': 'd'*64},
                                            'output_plugin': {'sha256': 'e'*64}}}
        for key in ('linux_nvidia_driver', 'loaded_module_files'):
            identity = copy.deepcopy(complete)
            identity[key] = None
            with self.subTest(key=key):
                long_result = fixture.finalize_assessment(
                    fixture.assess(record(2, steady_seconds=15, active_count=182),
                                   2, steady_seconds=15), identity)
                self.assertEqual(long_result['qualification']['status'], 'INCONCLUSIVE')
                self.assertEqual(long_result['status'], 'INCONCLUSIVE')
                self.assertTrue(long_result['reason'])
                self.assertEqual(fixture.result_exit_code(long_result), 1)
                quick = fixture.finalize_assessment(fixture.assess(record(2), 2), identity)
                self.assertEqual(quick['qualification']['status'], 'INCONCLUSIVE')
                self.assertEqual(quick['status'], 'PASS')
                self.assertEqual(fixture.result_exit_code(quick), 0)

    def test_launch_arguments_propagate_only_explicit_duration(self):
        self.assertEqual(fixture.runtime_command('proton', 'fg.exe', 2, 0),
                         ['proton', 'run', 'fg.exe', '2'])
        self.assertEqual(fixture.runtime_command('proton', 'fg.exe', 4, 15),
                         ['proton', 'run', 'fg.exe', '4', '--steady-seconds', '15'])

    # Catch fixed 336 frames accidentally retained in explicit duration mode.
    def test_long_record_requires_duration_and_three_complete_windows(self):
        result = fixture.assess(record(2, steady_seconds=15, active_count=182), 2, steady_seconds=15)
        self.assertEqual(result['qualification']['status'], 'PASS')
        self.assertEqual(result['requested_steady_seconds'], 15)
        self.assertEqual(result['qualification']['required_complete_windows_per_active_phase'], 3)
        self.assertEqual([p['steady_elapsed_us'] for p in result['phases']],
                         [15_000_000, 1_600_000, 15_000_000, 15_000_000])
        self.assertEqual(len(result['qualification']['complete_windows']), 9)
        self.assertEqual([p['requested_steady_us'] for p in result['phases']],
                         [15_000_000, 0, 15_000_000, 15_000_000])
        self.assertEqual([p['complete_steady_windows'] for p in result['phases']], [3, 0, 3, 3])

    # Catch runtime satisfied by warmup/disabled time or an unverified claim.
    def test_short_steady_duration_cannot_be_hidden_by_other_phase_time(self):
        for kwargs in ({}, {'warmup_interval_us': 1_000_000},
                       {'disabled_interval_us': 1_000_000}):
            with self.subTest(kwargs=kwargs), self.assertRaises(RuntimeError):
                fixture.assess(record(2, steady_seconds=15, **kwargs), 2, steady_seconds=15)

    def test_duration_protocol_missing_duplicate_or_conflicting_is_rejected(self):
        valid = record(2, steady_seconds=15, active_count=182)
        duration = next(line for line in valid.splitlines() if line.startswith('DURATION phase=0 '))
        request = 'REQUEST multiplier=2 generated=1 steady_seconds=15'
        broken_records = (valid.replace(request, 'REQUEST multiplier=2 generated=1'),
                          valid.replace(request, request.replace('15', '10')),
                          valid+'\n'+request, valid+'\n'+duration,
                          valid.replace(duration+'\n', ''),
                          valid.replace('complete_windows=3', 'complete_windows=4', 1),
                          valid.replace('end_us=18200000', 'end_us=18200001', 1),
                          valid.replace('steady_start_us=3200000', 'steady_start_us=0', 1),
                          valid.replace('requested_us=15000000', 'requested_us=0', 1),
                          valid.replace('DURATION phase=0', 'DURATION phase=4', 1),
                          valid.replace('complete_windows=3', 'complete_windows=nan', 1),
                          valid.replace('steady_start_us=3200000', 'steady_start_us=-1', 1))
        for broken in broken_records:
            with self.subTest(broken=broken[:100]), self.assertRaises(RuntimeError):
                fixture.assess(broken, 2, steady_seconds=15)
        short = record(2, steady_seconds=15).replace('complete_windows=1', 'complete_windows=3')
        with self.assertRaises(RuntimeError):
            fixture.assess(short, 2, steady_seconds=15)

    # Catch parser trusting frame count over a complete observed window count.
    def test_duration_with_two_complete_windows_cannot_claim_three(self):
        text = record(2, steady_seconds=15, active_count=96, interval_us=238_000)
        with self.assertRaises(RuntimeError):
            fixture.assess(text, 2, steady_seconds=15)

    def test_long_record_cannot_be_replayed_as_quick_or_without_samples(self):
        valid = record(2, steady_seconds=15, active_count=182)
        with self.assertRaises(RuntimeError):
            fixture.assess(valid, 2)
        for prefix in ('SAMPLE ', 'BASELINE '):
            with self.subTest(prefix=prefix), self.assertRaises(RuntimeError):
                fixture.assess('\n'.join(x for x in valid.splitlines() if not x.startswith(prefix)),
                               2, steady_seconds=15)

    def test_duration_boundaries_and_window_boundary_loss_are_supported(self):
        for seconds, count, interval, windows in ((5, 96, 100_000, 1),
                                                 (15, 98, 238_000, 3),
                                                 (120, 1232, 100_000, 24)):
            with self.subTest(seconds=seconds):
                result = fixture.assess(record(2, steady_seconds=seconds, active_count=count,
                                               interval_us=interval), 2, steady_seconds=seconds)
                self.assertEqual(result['qualification']['status'], 'PASS')
                self.assertEqual(len(result['qualification']['complete_windows']), windows*3)

    def test_duration_does_not_replace_minimum_active_frames(self):
        with self.assertRaises(RuntimeError):
            fixture.assess(record(2, steady_seconds=5, active_count=60, interval_us=1_000_000),
                           2, steady_seconds=5)

    def test_new_quick_protocol_remains_quick_and_honest(self):
        text = record(2, interval_us=8_000, duration_records='always').replace('generated=1', 'generated=1 steady_seconds=0', 1)
        result = fixture.assess(text, 2)
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')
        self.assertEqual([p['app'] for p in result['phases']], [96, 48, 96, 96])

    @unittest.skipUnless(shutil.which('g++'), 'native C++ compiler unavailable')
    def test_native_schedule_excludes_warmup_and_requires_whole_windows(self):
        # Actual harness helper; literal observations independently specify boundaries.
        source = r'''#include "duration_schedule.h"
#include <cassert>
#include <initializer_list>
int main() {
    unsigned seconds = 0;
    assert(ParseDurationSeconds("5", seconds) && seconds == 5);
    assert(ParseDurationSeconds("15", seconds) && seconds == 15);
    assert(ParseDurationSeconds("120", seconds) && seconds == 120);
    for (const char* bad : {"0", "4", "121", "nan", "inf", "-inf", "15.5", "",
                            "-4294967281", "18446744073709551631"})
        assert(!ParseDurationSeconds(bad, seconds));
    DurationSchedule quick(96, 0);
    assert(quick.NeedsMore(95)); assert(!quick.NeedsMore(96));
    DurationSchedule disabled(48, 0);
    assert(disabled.NeedsMore(47)); assert(!disabled.NeedsMore(48));
    DurationSchedule duration(96, 15);
    duration.Observe(32, 100000000);
    assert(duration.NeedsMore(96)); // Warmup never satisfies steady time.
    duration.Observe(33, 105100000);
    duration.Observe(34, 110200000);
    duration.Observe(96, 115000000);
    assert(duration.NeedsMore(96)); //15s elapsed, only two whole windows.
    duration.Observe(97, 115300000);
    assert(!duration.NeedsMore(97));
    assert(duration.CompleteWindows() == 3);
    assert(duration.SteadyStartUs() == 100000000);
    assert(duration.EndUs() == 115300000);
    DurationSchedule minimum(96, 5);
    minimum.Observe(32, 1); minimum.Observe(33, 5000001);
    assert(minimum.NeedsMore(33)); // duration does not bypass minimum frames.
}
'''
        with tempfile.TemporaryDirectory(prefix='fg-duration-test-') as directory:
            source_path = Path(directory)/'test.cpp'
            source_path.write_text(source)
            build = subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(HERE),
                                    str(source_path), '-o', str(Path(directory)/'test')],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(Path(directory)/'test')], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)

class Qualification(unittest.TestCase):
    def test_exact_counter_windows_for_each_requested_multiplier(self):
        for multiplier in (2, 3, 4):
            with self.subTest(multiplier=multiplier):
                result = fixture.assess(record(multiplier), multiplier)
                self.assertEqual(result['runtime_regression'], 'PASS')
                self.assertEqual(result['qualification']['status'], 'PASS')
                self.assertEqual(result['status'], 'PASS')
                self.assertEqual(result['requested_multiplier'], multiplier)
                self.assertIsNone(result['effective_configured_generated_frames'])
                self.assertEqual(result['provider_capability']['steady_min_generated_frames'], multiplier-1)

    def test_x4_request_with_actual_x2_is_not_qualified(self):
        result = fixture.assess(record(4, observed=2, maximum=1), 4)
        self.assertEqual(result['runtime_regression'], 'PASS')
        self.assertEqual(result['status'], 'INCONCLUSIVE')
        self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')
        self.assertEqual(result['observed_steady_ratios'][0]['ratio'], 2.0)

    def test_reported_cap_below_request_cannot_qualify(self):
        result = fixture.assess(record(4, maximum=1), 4)
        self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')
        self.assertEqual(result['status'], 'INCONCLUSIVE')

    def test_short_x2_retains_regression_compatibility(self):
        result = fixture.assess(record(2, interval_us=8_000), 2)
        self.assertEqual(result['runtime_regression'], 'PASS')
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')

    def test_short_x3_x4_runs_are_not_approved(self):
        for multiplier in (3, 4):
            with self.subTest(multiplier=multiplier):
                result = fixture.assess(record(multiplier, interval_us=8_000), multiplier)
                self.assertEqual(result['runtime_regression'], 'PASS')
                self.assertEqual(result['status'], 'INCONCLUSIVE')

    def test_phase_average_cannot_hide_a_degraded_window(self):
        result = fixture.assess(record(4, first_window_fallback=True), 4)
        self.assertEqual(result['observed_steady_ratios'][0]['ratio'], 4.0)
        self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')

    def test_missing_timing_or_identity_cannot_qualify(self):
        valid = record(4)
        for prefix in ('SAMPLE ', 'BASELINE ', 'GPU ', 'MODULE role=provider '):
            with self.subTest(prefix=prefix):
                text = '\n'.join(line for line in valid.splitlines() if not line.startswith(prefix))
                result = fixture.assess(text, 4)
                self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')

    def test_requested_or_counter_record_mutations_are_rejected(self):
        valid = record(4)
        for broken in (valid.replace('REQUEST multiplier=4 generated=3', ''),
                       valid.replace('REQUEST multiplier=4 generated=3', 'REQUEST multiplier=3 generated=2'),
                       valid.replace('FRAME phase=0 app=1 actual=4', 'FRAME phase=0 app=1 actual=40', 1),
                       valid.replace('SAMPLE phase=0 app=1 us=100000', 'SAMPLE phase=0 app=1 us=0', 1),
                       valid+'\nSAMPLE phase=0 app=1 us=100000'):
            with self.subTest(broken=broken[:90]):
                with self.assertRaises(RuntimeError):
                    fixture.assess(broken, 4)

    def test_fixture_ini_controls_own_instance_without_implicit_unlock(self):
        for multiplier in (2, 3, 4):
            with self.subTest(multiplier=multiplier):
                config = configparser.ConfigParser()
                config.read_string(fixture.build_ini(multiplier))
                self.assertEqual(config['DLSSG']['InterpolationCount'], str(multiplier-1))
                self.assertEqual(config['DLSSG']['OverrideInterpolationCount'], 'auto')
                self.assertEqual(config['DLSSG']['AdaMfgUnlock'], 'false')
                self.assertEqual(config['DLSSG']['ForceDMFG'], 'false')

    def test_ada_unlock_requires_explicit_fixture_option(self):
        config = configparser.ConfigParser()
        config.read_string(fixture.build_ini(4, ada_mfg_unlock=True))
        self.assertEqual(config['DLSSG']['AdaMfgUnlock'], 'true')

    def test_runtime_identity_hashes_observed_modules_and_selected_launcher(self):
        with tempfile.TemporaryDirectory(prefix='fg-identity-test-') as directory:
            root = Path(directory)
            proton, provider, plugin = root/'proton', root/'provider.dll', root/'plugin.dll'
            for path, value in ((proton, b'launcher'), (provider, b'provider'), (plugin, b'plugin')):
                path.write_bytes(value)
            paths = {'provider': 'Z:'+str(provider).replace('/', '\\'),
                     'output_plugin': 'Z:'+str(plugin).replace('/', '\\')}
            result = fixture.runtime_identity(proton, paths, root/'compat')
            self.assertEqual(result['proton_launcher']['sha256'], hashlib.sha256(b'launcher').hexdigest())
            self.assertEqual(result['loaded_module_files']['provider']['sha256'], hashlib.sha256(b'provider').hexdigest())
            self.assertEqual(result['loaded_module_files']['output_plugin']['sha256'], hashlib.sha256(b'plugin').hexdigest())
            self.assertIn('cpu', result)
            self.assertIn('system', result)

    def test_missing_driver_or_runtime_version_prevents_qualification(self):
        complete = {'cpu': {'model': 'controlled CPU'},
                    'proton_launcher': {'sha256': 'a'*64},
                    'proton_version_file': {'text': 'controlled Proton version', 'sha256': 'b'*64},
                    'linux_nvidia_driver': {'text': 'controlled driver version', 'sha256': 'c'*64},
                    'loaded_module_files': {'provider': {'sha256': 'd'*64},
                                            'output_plugin': {'sha256': 'e'*64}}}
        result = fixture.finalize_assessment(fixture.assess(record(4), 4), complete)
        self.assertEqual(result['qualification']['status'], 'PASS')
        for key in ('proton_version_file', 'linux_nvidia_driver'):
            for missing in (None, {'text': '', 'sha256': 'b'*64}, {'text': 'version', 'sha256': None}):
                with self.subTest(key=key, missing=missing):
                    identity = copy.deepcopy(complete)
                    identity[key] = missing
                    result = fixture.finalize_assessment(fixture.assess(record(4), 4), identity)
                    self.assertEqual(result['status'], 'INCONCLUSIVE')
                    self.assertEqual(result['runtime_regression'], 'PASS')
                    self.assertEqual(result['qualification']['status'], 'INCONCLUSIVE')
                    self.assertTrue(result['qualification']['reasons'])

    def test_execute_does_not_reap_leader_before_final_group_signal(self):
        events = []
        class Child:
            pid, returncode, waits = 123456, None, 0
            def wait(self, timeout=None):
                self.waits += 1
                if self.waits == 1:
                    raise subprocess.TimeoutExpired(['controlled'], timeout)
                if 'KILL' not in events:
                    raise AssertionError('leader reaped before final group signal')
                self.returncode = -9
                return -9
        def group_signal(_pid, number):
            events.append('TERM' if number == signal.SIGTERM else 'KILL')
        with tempfile.TemporaryDirectory(prefix='fg-execute-test-') as directory, \
                patch.object(fixture.subprocess, 'Popen', return_value=Child()), \
                patch.object(fixture.os, 'killpg', side_effect=group_signal), \
                patch.object(fixture.time, 'sleep'), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(subprocess.TimeoutExpired):
                fixture.execute(['controlled'], cwd=Path(directory), env={},
                                log=Path(directory)/'log', timeout=.1)

    def test_execute_reaped_leader_never_authorizes_group_signal(self):
        class Child:
            pid, returncode = 123456, None
            def wait(self, timeout=None):
                self.returncode = 0
                raise KeyboardInterrupt
        with tempfile.TemporaryDirectory(prefix='fg-execute-test-') as directory, \
                patch.object(fixture.subprocess, 'Popen', return_value=Child()), \
                patch.object(fixture.os, 'killpg', side_effect=AssertionError('reaped PGID is not owned')), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(KeyboardInterrupt):
                fixture.execute(['controlled'], cwd=Path(directory), env={},
                                log=Path(directory)/'log', timeout=.1)

    def test_execute_nondefault_sigchld_refuses_before_spawn(self):
        with tempfile.TemporaryDirectory(prefix='fg-execute-test-') as directory, \
                patch.object(fixture.signal, 'getsignal', return_value=signal.SIG_IGN), \
                patch.object(fixture.subprocess, 'Popen', side_effect=AssertionError('must not spawn')), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'default SIGCHLD'):
                fixture.execute(['controlled'], cwd=Path(directory), env={},
                                log=Path(directory)/'log', timeout=.1)


class Provenance(unittest.TestCase):
    @staticmethod
    def assets(config):
        return {'OptiScaler.ini': hashlib.sha256(config).hexdigest(),
                'OptiScaler.dll': 'a'*64, 'provider.dll': 'b'*64, 'fg-provider.exe': 'c'*64}

    def verify(self, final, *, initial=None, after=None):
        initial = fixture.build_ini(2).encode() if initial is None else initial
        return fixture.verify_staged_provenance(self.assets(initial),
                                                self.assets(final) if after is None else after,
                                                initial, final)

    def test_runtime_ini_rewrite_preserves_explicit_settings(self):
        initial = fixture.build_ini(2).encode()
        final = initial.replace(b'=', b' = ').replace(b'\n', b'\r\n')
        final += b'\r\n[XeFG]\r\nDepthInverted = false\r\nJitteredMV = auto\r\nHighResMV = true\r\n'
        result = self.verify(final, initial=initial)
        self.assertTrue(result['rewritten'])
        self.assertTrue(result['explicit_settings_unchanged'])
        self.assertEqual(result['launch_sha256'], hashlib.sha256(initial).hexdigest())
        self.assertEqual(result['final_sha256'], hashlib.sha256(final).hexdigest())
        self.assertEqual(result['runtime_added_settings']['xefg.highresmv'], 'true')

    def test_unchanged_ini_is_recorded_without_rewrite(self):
        initial = fixture.build_ini(2).encode()
        result = self.verify(initial)
        self.assertFalse(result['rewritten'])
        self.assertEqual(result['runtime_added_settings'], {})

    def test_changed_or_missing_owned_settings_are_rejected(self):
        initial = fixture.build_ini(2).encode()
        for final in (initial.replace(b'InterpolationCount=1', b'InterpolationCount=3'),
                      initial.replace(b'AdaMfgUnlock=false', b'AdaMfgUnlock=true'),
                      initial.replace(b'FGOutput=dlssg', b'FGOutput=DLSSG'),
                      initial.replace(b'FGOutput=dlssg\n', b'')):
            with self.subTest(final=final):
                with self.assertRaises(RuntimeError):
                    self.verify(final)

    def test_unexpected_added_configuration_is_rejected(self):
        initial = fixture.build_ini(2).encode()
        for suffix in (b'[Unexpected]\nOption=true\n',
                       b'[XeFG]\nHighResMV=false\n',
                       b'[DEFAULT]\nFGOutput=fsrfg\n'):
            with self.subTest(suffix=suffix):
                with self.assertRaises(RuntimeError):
                    self.verify(initial + suffix)

    def test_binary_mutation_or_missing_artifact_is_rejected(self):
        initial = fixture.build_ini(2).encode()
        for after in ({**self.assets(initial), 'provider.dll': 'f'*64},
                      {k: v for k, v in self.assets(initial).items() if k != 'fg-provider.exe'}):
            with self.subTest(after=after):
                with self.assertRaises(RuntimeError):
                    self.verify(initial, after=after)

    def test_snapshot_hash_mismatch_and_duplicate_section_are_rejected(self):
        initial = fixture.build_ini(2).encode()
        with self.assertRaises(RuntimeError):
            self.verify(initial, after={**self.assets(initial), 'OptiScaler.ini': 'f'*64})
        with self.assertRaises(RuntimeError):
            self.verify(initial + b'[dlssg]\nInterpolationCount=3\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
