"""Full-v1 ControlMask experiment schema (host test checks against the compiled C++ plan)."""
import re

VALUES = (0, 64, 128, 191, 255)  # round(v * 255): 0, 1/4, 1/2, 3/4, 1
ARMS = ('r8', 'r001', 'r111', 'rgb')


def trials():
    rows = []
    for setter in ('u64', 'resource'):
        for blend in range(4):
            prefix = f'{setter}-m{blend}'
            def add(arm, value=None):
                rgba = [0 if value is None else value, 0, 0, 255]
                if arm == 'r111':
                    rgba[1:3] = [255, 255]
                elif arm == 'rgb':
                    rgba[1:3] = [value, value]
                rows.append({'name': f'{prefix}-{arm}' + (f'-v{value}' if value is not None else ''),
                             'masked': value is not None, 'arm': arm, 'setter': setter,
                             'blend': blend, 'rgba': rgba})
            add('null-r0')
            add('null-r1')
            for arm in ARMS:
                for value in VALUES:
                    add(arm, value)
            add('null-after')
    return rows


def pairs():
    result = []
    for setter in ('u64', 'resource'):
        for blend in range(4):
            prefix = f'{setter}-m{blend}'
            base = f'{prefix}-null-r0'
            if setter == 'u64':
                result.append((base, f'resource-m{blend}-null-r0', 'null-setter'))
            result.extend([(base, f'{prefix}-null-r1', 'repeat'), (base, f'{prefix}-null-after', 'null-after')])
            for arm in ARMS:
                for value in VALUES:
                    name = f'{prefix}-{arm}-v{value}'
                    result.append((base, name, 'mask-vs-null'))
                    if setter == 'u64':
                        result.append((name, f'resource-m{blend}-{arm}-v{value}', 'setter'))
                    if arm != 'r8':
                        result.append((f'{prefix}-r8-v{value}', name, 'channels'))
    return result


def check_coverage(log, *, full=False):
    label, count = ('MASK-MATRIX', len(trials())) if full else ('MASK-AB', 24)
    expected = (f'{label} PASS: trials={count} attempts={count * 32} successes={count * 32} '
                f'presents={count * 32} controls={count} capture_pairs={count * 7}')
    lines = re.findall(r'^MASK-(?:AB|MATRIX) PASS:.*$', log, re.M)
    if lines != [expected]:
        raise ValueError(f'ZERO COVERAGE: incomplete or mismatched ControlMask run; expected {expected}')
