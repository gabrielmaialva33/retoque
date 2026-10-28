"""Host-only checks of the registered Vulkan scale case; no Wine, model or GPU execution."""
import configparser
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('vulkan_scale_runner', ROOT / 'tests/vulkan-present-host/run.py')
vulkan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vulkan)


def route_evidence(working='1600x900'):
    """Synthetic facts and the needed lifecycle lines from the measured --route shape, with normalized handles.

    The swapchain is 632x446; the game's upscaler output is separately 1280x720 (nr.cpp's outW/outH). Exact
    readbacks during the route's active phases prove ownership, not the pixels of that off-screen output.
    """
    phase_rows = [
        ('cold-build', 27, 0, 23, 4, 2.52829),
        ('cold', 18, 0, 16, 0, 3.12898),
        ('on', 30, 30, 28, 28, 0),
        ('off-waiting', 85, 0, 83, 83, 0),
        ('off-handing-back', 33, 0, 31, 13, 1.84605),
        ('off-back', 24, 0, 22, 0, 3.23914),
        ('on-again', 30, 30, 28, 28, 0),
        ('off-again-waiting', 85, 0, 83, 83, 0),
        ('off-again-handing-back', 33, 0, 31, 13, 1.77739),
        ('off-again-back', 24, 0, 22, 0, 3.16576),
        ('on-at-exit', 10, 10, 8, 8, 0),
    ]
    facts = {'status': 'PASS', 'validation_active': True, 'validation_errors': 0, 'first_validation': '',
             'model_messages': 6, 'model_vuids': 'VUID-vkCmdDraw-None-09600', 'device': '0x100',
             'wdx12_probed': False,
             'swapchains': [{'name': 'route', 'handle': '0x200', 'width': 632, 'height': 446, 'presents': 399,
                             'readbacks': 395, 'exact': 268, 'sdr': True}],
             'phases': [dict(zip(('name', 'presents', 'evaluates', 'readbacks', 'exact', 'mae_mean'), row))
                        for row in phase_rows]}
    host = 'DLSS-NR Vulkan present host: '
    device = host + 'device 0x100 #4: '
    route = 'DLSS-NR Vulkan: '
    lines = []
    for cycle in range(3):
        lines.extend([
            host + "the model initialised on device 0x100 (cold: no application NGX initialisation; the driver's "
                   'NGX core brought up on it)',
            host + "the model's feature is up at 632x446 (frame 632x446)",
            route + 'this route waits: the Vulkan present host holds the model; it stands down for this route at '
                    'its next present',
            device + 'the model stood down and was freed',
            route + 'this route has the model again (the present host stood down)',
            route + f'feature up at {working} (frame 1280x720)',
        ])
        if cycle == 0:
            lines.append(route + 'running natively at 1280x720, guides 640x360')
        if cycle < 2:
            lines.extend([
                device + "the after-upscale route has not asked for the model for 10 s: taking it back, once the "
                         "device's 2 queue(s) drained",
                route + "the after-upscale route's model was freed for the hand-back (its feature at 1280x720, "
                        'its images, pass and parameter block); NGX stays initialised on the device',
                device + "the model is the present host's again (the drain took 94 ms)",
            ])
    lines.extend([
        route + "the after-upscale route's objects on the device freed before it goes",
        host + 'NGX shut down on the device before it goes (returned 1)',
        host + 'device 0x100 #4 destroyed; freed: 0 swapchain(s) the application left, 0 retained present '
               'semaphore(s), 0 slot(s) kept from earlier swapchains',
    ])
    return facts, '\n'.join(lines)


class ScaleCaseStaging(unittest.TestCase):
    def test_scale_case_is_selectable_by_the_existing_route_runner(self):
        self.assertIn('nr-route-scale125', vulkan.ROUTE_CASES)

    def test_scale_case_stages_the_complete_route_with_only_the_scale_override(self):
        with tempfile.TemporaryDirectory() as value:
            root = Path(value)
            out = root / 'artifacts'
            out.mkdir()
            dll = root / 'OptiScaler.dll'
            dll.write_bytes(b'controlled dll')
            (out / 'vk-present-host-nr.exe').write_bytes(b'controlled harness')
            files = {'built': root / 'nvngx.dll_dlssnr.dll', 'model': root / 'nvngx_dlssnr.dll',
                     'proton': root / 'proton'}
            files['built'].write_bytes(b'controlled forwarder')
            files['model'].write_bytes(b'controlled model')

            def launch(command, *, cwd, env, stdin, stdout, stderr, timeout):
                self.assertEqual(command, [str(files['proton']), 'run',
                                          str(out / 'cases/nr-route-scale125/vk-present-host-nr.exe'), '--route'])
                self.assertEqual(env['VK_INSTANCE_LAYERS'], 'VK_LAYER_KHRONOS_validation')
                self.assertEqual(env['VK_LAYER_ENABLES'],
                                 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT')
                (cwd / 'result.json').write_text(json.dumps({'status': 'PASS', 'validation_active': True,
                                                           'validation_errors': 0}))
                (cwd / 'OptiScaler.log').write_text('controlled route log\n')
                return subprocess.CompletedProcess(command, 0)

            # Keep the real staging/configuration/JSON path; replace only the external process boundary and its
            # workstation guard. The returned facts do not qualify a GPU run.
            with patch.object(vulkan, 'OUT', out), patch.object(vulkan, 'no_game'), \
                    patch.object(vulkan.subprocess, 'run', side_effect=launch):
                facts, log = vulkan.execute_route('nr-route-scale125', dll, [], files)
            self.assertEqual(facts['status'], 'PASS')
            self.assertEqual(log, 'controlled route log\n')
            work = out / 'cases/nr-route-scale125'
            ini = configparser.ConfigParser()
            ini.read(work / 'OptiScaler.ini')
            self.assertEqual(dict(ini['DlssNr']),
                             {'enabled': 'true', 'vulkanpresenthost': 'true', 'workingscale': '1.25'})
            self.assertEqual(dict(ini['Menu']), {'overlaymenu': 'true', 'shortcutkey': '0x2D'})
            self.assertEqual(dict(ini['Upscalers']), {'vulkanupscaler': 'fsr22'})
            self.assertTrue((work / 'optiscaler_skip_vulkan_hooks').exists())
            self.assertEqual((work / 'nvngx_dlssnr.dll').resolve(), files['model'])


class ScaleEvidence(unittest.TestCase):
    def assert_rejected(self, facts, log):
        try:
            problems, _ = vulkan.check_route('nr-route-scale125', facts, log)
        except (KeyError, TypeError, ValueError, AttributeError) as error:
            self.fail(f'incomplete evidence must return a rejection, not raise {error!r}')
        self.assertTrue(problems, 'invalid scale evidence was accepted')

    def test_registered_scale_case_accepts_three_route_builds_and_all_eleven_phases(self):
        facts, log = route_evidence()
        problems, summary = vulkan.check_route('nr-route-scale125', facts, log)
        self.assertEqual(problems, [])
        self.assertIn("3 of the route's", summary)
        self.assertIn('WorkingScale=1.25', summary)
        self.assertIn('frame 1280x720, working 1600x900', summary)
        self.assertIn('70 successful harness upscaler evaluates', summary)

    def test_existing_quiet_route_keeps_its_native_size_contract(self):
        facts, log = route_evidence('1280x720')
        self.assertEqual(vulkan.check_route('nr-route-quiet', facts, log)[0], [])
        self.assertTrue(vulkan.check_route('nr-route-quiet', facts, log.replace('feature up at 1280x720',
                                                                           'feature up at 1600x900'))[0])

    def test_wrong_scale_frame_or_working_extent_cannot_replace_a_route_build(self):
        facts, log = route_evidence()
        feature = 'DLSS-NR Vulkan: feature up at 1600x900 (frame 1280x720)'
        for replacement in ('DLSS-NR Vulkan: feature up at 1280x720 (frame 1280x720)',
                            'DLSS-NR Vulkan: feature up at 1920x1080 (frame 1280x720)',
                            'DLSS-NR Vulkan: feature up at 1600x899 (frame 1280x720)',
                            'DLSS-NR Vulkan: feature up at 1600x900 (frame 1280x721)',
                            'DLSS-NR Vulkan: feature up at 1600x900',
                            "DLSS-NR Vulkan present host: the model's feature is up at 1600x900 (frame 1280x720)",
                            ''):
            with self.subTest(replacement=replacement):
                self.assert_rejected(facts, log.replace(feature, replacement, 1))

    def test_malformed_or_extra_build_evidence_is_not_a_matching_substring(self):
        facts, log = route_evidence()
        feature = 'DLSS-NR Vulkan: feature up at 1600x900 (frame 1280x720)'
        for replacement in (feature + '0', feature + ' trailing unparsed evidence',
                            feature + '\nDLSS-NR Vulkan: feature up at 1280x720 (frame 1280x720)'):
            with self.subTest(replacement=replacement):
                self.assert_rejected(facts, log.replace(feature, replacement, 1))

    def test_each_build_must_belong_to_its_own_active_route_period(self):
        facts, log = route_evidence()
        feature = 'DLSS-NR Vulkan: feature up at 1600x900 (frame 1280x720)'
        # The number and dimensions stay correct, but the first build is before the route owns the model.
        self.assert_rejected(facts, feature + '\n' + log.replace(feature + '\n', '', 1))
        # Three valid-looking lines in the first period must not stand in for builds in the other two periods.
        without_builds = log.replace(feature + '\n', '')
        anchor = 'DLSS-NR Vulkan: running natively at 1280x720, guides 640x360'
        self.assert_rejected(facts, without_builds.replace(anchor, (feature + '\n') * 3 + anchor))

    def test_native_run_announcement_must_follow_the_first_route_build(self):
        facts, log = route_evidence()
        announced = 'DLSS-NR Vulkan: running natively at 1280x720, guides 640x360'
        for kind, altered in (('missing', log.replace(announced, '')),
                              ('wrong guides', log.replace('guides 640x360', 'guides 960x540')),
                              ('before build', announced + '\n' + log.replace(announced + '\n', ''))):
            with self.subTest(kind=kind):
                self.assert_rejected(facts, altered)

    def test_real_up_leg_fallback_disqualifies_the_supersampling_path(self):
        facts, log = route_evidence()
        fallback = 'DLSS-NR Vulkan supersample: upscaler unavailable, falling back to box enlarge.'
        self.assert_rejected(facts, log + '\n' + fallback)

    def test_probe_size_refusal_does_not_qualify_the_requested_path(self):
        facts, log = route_evidence()
        self.assert_rejected(facts, log + '\nDLSS-NR Vulkan did not run: 64x64 (working 80x80) is below '
                             '128 pixels; the model is not built that small')

    def test_active_evaluations_and_exact_readbacks_must_be_complete(self):
        facts, log = route_evidence()
        for phase_name, change in (('on', {'evaluates': 29}),
                                   ('on-again', {'presents': 29}),
                                   ('on-at-exit', {'readbacks': 7, 'exact': 7}),
                                   ('on', {'readbacks': 27, 'exact': 27}),
                                   ('on-again', {'exact': 27}),
                                   ('off-waiting', {'evaluates': 1})):
            broken = copy.deepcopy(facts)
            next(p for p in broken['phases'] if p['name'] == phase_name).update(change)
            with self.subTest(phase=phase_name, change=change):
                self.assert_rejected(broken, log)
        broken = copy.deepcopy(facts)
        del next(p for p in broken['phases'] if p['name'] == 'on')['evaluates']
        self.assert_rejected(broken, log)

    def test_all_eleven_phases_and_the_existing_cold_and_handback_gates_remain_required(self):
        facts, log = route_evidence()
        for phase_name in ('cold-build', 'cold', 'off-back', 'off-again-back'):
            broken = copy.deepcopy(facts)
            broken['phases'] = [p for p in broken['phases'] if p['name'] != phase_name]
            with self.subTest(missing_phase=phase_name):
                self.assert_rejected(broken, log)
        for phase_name in ('cold', 'off-back', 'off-again-back'):
            broken = copy.deepcopy(facts)
            phase = next(p for p in broken['phases'] if p['name'] == phase_name)
            phase['exact'] = phase['readbacks']
            phase['mae_mean'] = 0
            with self.subTest(unchanged_phase=phase_name):
                self.assert_rejected(broken, log)
        broken = copy.deepcopy(facts)
        broken['phases'].append(copy.deepcopy(broken['phases'][-1]))
        self.assert_rejected(broken, log)

    def test_host_validation_and_model_attribution_must_be_present_and_consistent(self):
        facts, log = route_evidence()
        for field, value in (('status', 'FAIL'), ('validation_active', False), ('validation_active', 'true'),
                             ('validation_errors', 1), ('validation_errors', False),
                             ('first_validation', 'VUID-vkCmdDispatch-OpImageWrite-08795'),
                             ('model_messages', -1), ('model_vuids', None)):
            with self.subTest(field=field, value=value):
                self.assert_rejected({**facts, field: value}, log)
        for field in ('status', 'validation_active', 'validation_errors', 'first_validation',
                      'model_messages', 'model_vuids'):
            broken = copy.deepcopy(facts)
            del broken[field]
            with self.subTest(missing=field):
                self.assert_rejected(broken, log)

    def test_08795_cannot_hide_in_host_or_model_attribution_or_the_log(self):
        facts, log = route_evidence()
        forbidden = 'VUID-vkCmdDispatch-OpImageWrite-08795'
        self.assert_rejected({**facts, 'validation_errors': 1, 'first_validation': forbidden}, log)
        self.assert_rejected({**facts, 'model_vuids': forbidden}, log)
        self.assert_rejected({**facts, 'model_vuids': facts['model_vuids'] + ',' + forbidden}, log)
        self.assert_rejected(facts, log + '\nMODEL ' + forbidden + ' | controlled validation error')

    def test_known_model_vuid_is_still_bounded_by_the_six_actual_feature_builds(self):
        facts, log = route_evidence()
        self.assertEqual(vulkan.check_route('nr-route-scale125', facts, log)[0], [])
        self.assert_rejected({**facts, 'model_messages': 7}, log)
        self.assert_rejected({**facts, 'model_vuids': 'VUID-unexpected-model-error'}, log)

    def test_positive_model_reports_require_identified_well_formed_admitted_vuids(self):
        facts, log = route_evidence()
        known = 'VUID-vkCmdDraw-None-09600'
        for attribution in ('', ',,,', known + ',', ',' + known, known + ',' + known):
            with self.subTest(attribution=attribution):
                self.assert_rejected({**facts, 'model_vuids': attribution}, log)

    def test_zero_model_reports_require_empty_attribution(self):
        facts, log = route_evidence()
        facts['model_messages'] = 0
        self.assertEqual(vulkan.check_route('nr-route-scale125', {**facts, 'model_vuids': ''}, log)[0], [])
        for attribution in ('VUID-vkCmdDraw-None-09600', ',,,'):
            with self.subTest(attribution=attribution):
                self.assert_rejected({**facts, 'model_vuids': attribution}, log)

    def test_phase_counters_must_describe_possible_observations(self):
        facts, log = route_evidence()
        for phase_name, change in (('cold', {'presents': 0}),
                                   ('off-waiting', {'presents': 0}),
                                   ('cold-build', {'exact': 999999}),
                                   ('off-again-handing-back', {'readbacks': 34}),
                                   ('on', {'presents': 29}),
                                   ('cold-build', {'presents': -1}),
                                   ('cold-build', {'evaluates': -1}),
                                   ('cold-build', {'readbacks': -1}),
                                   ('cold-build', {'exact': -1})):
            broken = copy.deepcopy(facts)
            next(p for p in broken['phases'] if p['name'] == phase_name).update(change)
            with self.subTest(phase=phase_name, change=change):
                self.assert_rejected(broken, log)

    def test_phase_mae_must_be_finite_and_nonnegative(self):
        facts, log = route_evidence()
        for phase_name, value in (('cold', float('inf')), ('off-waiting', float('nan')),
                                   ('cold-build', float('-inf')), ('on', -0.01),
                                   ('off-handing-back', -0.01)):
            broken = copy.deepcopy(facts)
            next(p for p in broken['phases'] if p['name'] == phase_name)['mae_mean'] = value
            with self.subTest(phase=phase_name, mae=value):
                self.assert_rejected(broken, log)

    def test_variable_waiting_phase_counts_do_not_gain_a_throughput_floor(self):
        facts, log = route_evidence()
        for phase_name in ('off-waiting', 'off-again-waiting'):
            next(p for p in facts['phases'] if p['name'] == phase_name).update(presents=3, readbacks=1, exact=1)
        # Each shortened waiting phase has 82 fewer observations; keep the aggregate fixture coherent too.
        facts['swapchains'][0].update(presents=235, readbacks=231, exact=104)
        self.assertEqual(vulkan.check_route('nr-route-scale125', facts, log)[0], [])


if __name__ == '__main__':
    unittest.main()
