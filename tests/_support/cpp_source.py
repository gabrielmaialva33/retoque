"""Small source-slice helpers: unique definitions, braces outside comments/literals.

For host contracts on named production functions, never a C++ compiler substitute.
"""
import re


def function(source, name):
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source, flags=re.S)
    pattern = r'^[\w:<>,*& \t]+\b' + re.escape(name) + r'\([^;{}]*\)\s*(?:const\s*)?\{'
    matches = list(re.finditer(pattern, masked, re.M))
    assert len(matches) == 1, f'expected one definition of {name}, found {len(matches)}'
    start = matches[0].start()
    opening = matches[0].end() - 1
    depth = 1
    for end in range(opening + 1, len(masked)):
        depth += (masked[end] == '{') - (masked[end] == '}')
        if not depth:
            return source[start:end + 1]
    raise AssertionError(f'unterminated function: {name}')


def mutate_function(source, name, old, new):
    """Change exactly one anchor in exactly one function; other routes stay intact."""
    original = function(source, name)
    assert original.count(old) == 1, f'{name}: mutation anchor is not unique: {old!r}'
    return source.replace(original, original.replace(old, new, 1), 1)
