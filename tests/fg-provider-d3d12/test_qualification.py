#!/usr/bin/env python3
"""Host parser/config controls only: never compile, launch Wine, or signal processes."""
import configparser
import contextlib
import copy
import hashlib
import importlib.util
import io
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
           first_window_fallback=False):
    """Literal deltas for a synthetic evidence record; no hardware claim."""
    observed = multiplier if observed is None else observed
    maximum = multiplier-1 if maximum is None else maximum
    lines = [f'REQUEST multiplier={multiplier} generated={multiplier-1}',
             'ORACLE=real-output-plugin',
             'GPU vendor=0x10de device=0x2684 subsystem=0x000010de revision=161 '
             'luid=00000000:00001234 description=NVIDIA GeForce RTX 4090',
             r'MODULE role=output_plugin path=Z:\fixture\streamline\sl.dlss_g.dll',
             r'MODULE role=provider path=Z:\fixture\streamline\nvngx_dlssg.dll']
    frame, clock, total_actual = 0, 0, 0
    for phase in range(4):
        count = 48 if phase == 1 else 96
        lines.append(f'BASELINE phase={phase} us={clock}')
        actual, steady_actual = 0, 0
        for local in range(count):
            frame += 1
            clock += interval_us
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
        lines.append(f'PHASE phase={phase} enabled={int(phase != 1)} app={count} actual={actual} '
                     f'steady_app={count-32} steady_actual={steady_actual} ok={count-32} completed={count}')
    lines.append(f'PASS: FG-PROVIDER app=336 actual={total_actual} phases=4 resizes=1 oracle=real-plugin')
    return '\n'.join(lines)


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
