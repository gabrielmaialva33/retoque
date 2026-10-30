"""Token-aware baseline comparison for whole present-guide function definitions."""
import hashlib
import json
import re

# Keep multi-character punctuators whole and preserve token boundaries in the digest.
TOKEN = re.compile(r'''//[^\n]*|/\*[\s\S]*?\*/|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*'|[A-Za-z_][A-Za-z_0-9]*|(?:\d[\w.]*)(?:[+-](?<=e[+-])\d+)?|<=>|>>=|<<=|->\*|\.\.\.|::|->|\+\+|--|&&|\|\||==|!=|<=|>=|<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=|##|[^\s]''')


def definition_pattern(name):
    # A return type is mandatory; a control statement such as if (f(...)) cannot match.
    return re.compile(r'^(?:static\s+)?[A-Za-z_][\w:<>*& \t]*?\s+' + re.escape(name) + r'\([^;{]*\)\s*\{', re.M)


def present_body(text, name):
    matches = list(definition_pattern(name).finditer(text))
    assert len(matches) == 1, f'expected one present-guide definition: {name}, found {len(matches)}'
    start = matches[0].start()
    brace = text.index('{', start)
    level, end = 1, brace + 1
    while level:
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def present_digest(body):
    body = re.sub(r'^static\s+', '', body)
    tokens = TOKEN.findall(body)
    return hashlib.sha256(json.dumps(tokens, ensure_ascii=False, separators=(',', ':')).encode()).hexdigest()
