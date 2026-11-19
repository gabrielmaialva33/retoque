#!/usr/bin/env python3
"""Prove the preview tool (pixels + repeatability), not runtime menu correctness."""
from pathlib import Path
import json
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/menu-preview'))
from run import build, run  # noqa: E402

SIMPLE = {'nr-off', 'd3d12-simple', 'model-missing', 'd3d12-simple-pt-BR'}
ADVISOR_HEIGHTS = {'advisor-eligible-pt-BR': 4200, 'advisor-running-pt-BR': 4200,
                   'advisor-done-pt-BR': 4200, 'advisor-rr-done-pt-BR': 4200,
                   'advisor-timing-off-pt-BR': 4200, 'advisor-hidden-off': 900}
NARROW = {'individual-passes-pt-BR-narrow': (420,4200), 'simple-comparison-pt-BR': (420,1400), 'model-missing-pt-BR-narrow': (420,1100), 'model-missing-narrow': (420,1100), 'present-hdr-pt-BR-narrow': (420,4200), 'native-vulkan-narrow': (480,4200)}
STATES = set(NARROW) | set(ADVISOR_HEIGHTS) | SIMPLE | {'d3d12-full', 'individual-passes', 'present-hdr', 'native-vulkan', 'comparison', 'present-hdr-pt-BR'}


def png_pixels(path, expected):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    offset, compressed, size = 8, bytearray(), None
    ended = False
    while offset < len(data):
        n = struct.unpack('>I', data[offset:offset+4])[0]
        kind = data[offset+4:offset+8]
        payload = data[offset+8:offset+8+n]
        assert len(payload) == n
        crc = struct.unpack('>I', data[offset+8+n:offset+12+n])[0]
        assert zlib.crc32(kind+payload) == crc, 'bad PNG CRC'
        if kind == b'IHDR':
            width,height,depth,color,compression,filtering,interlace = struct.unpack('>IIBBBBB',payload)
            assert (width,height) == expected
            assert (depth,color,compression,filtering,interlace) == (8,2,0,0,0)
            size=(width,height)
        if kind == b'IDAT':
            compressed.extend(payload)
        if kind == b'IEND':
            assert n == 0
            ended = True
        offset += 12+n
    assert ended and size and offset == len(data)
    width,height = size
    raw = zlib.decompress(compressed)
    stride=width*3+1
    assert len(raw) == stride*height
    assert all(raw[y*stride] == 0 for y in range(height))
    pixels = b''.join(raw[y*stride+1:(y+1)*stride] for y in range(height))
    assert len(set(zip(pixels[::3],pixels[1::3],pixels[2::3]))) > 50, 'blank/flat image'
    # Exclude the preview title/watermark and borders: glyph-like bright pixels must be in the panel.
    bright = sum(raw[y*stride+1+x*3] > 150 and raw[y*stride+2+x*3] > 150
                 for y in range(90,min(450,height)) for x in range(35,width-25))
    assert bright > 1200, 'no readable light text in the panel'
    return data


with tempfile.TemporaryDirectory(prefix='menu-preview-test-') as temporary:
    temp = Path(temporary)
    raster = temp/'raster'
    subprocess.run(['g++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I',str(ROOT/'tools/menu-preview'),'-I',str(ROOT/'retoque'),'-I',str(ROOT/'OptiScaler/include'),
                    str(ROOT/'tests/menu-preview/raster.cpp'),'-o',str(raster)],check=True)
    subprocess.run([str(raster)],check=True)
    binary = build(temp)
    states = subprocess.check_output([str(binary),'--list'],text=True).splitlines()
    assert set(states)==STATES and len(states)==len(STATES)
    for output in (temp/'first',temp/'second'):
        run(binary,output,states)
    images={}
    for state in states:
        one,two = temp/'first'/state,temp/'second'/state
        expected=NARROW.get(state,(560,ADVISOR_HEIGHTS.get(state,900 if state in SIMPLE else 4200)))
        images[state]=png_pixels(one.with_suffix('.png'),expected)
        assert images[state]==two.with_suffix('.png').read_bytes(), f'{state}: nondeterministic PNG'
        stats=json.loads(one.with_suffix('.json').read_text())
        assert stats['glyph_pixels']>3000 and stats['triangles']>500
        assert 100<stats['content_bottom']<expected[1]-35, f'{state}: panel clipped vertically'
        text=one.with_suffix('.txt').read_text()
        assert len(text)>200
        assert 'DLSS Neural Rendering' in text or 'Renderização neural DLSS' in text
        if state not in SIMPLE and state not in ADVISOR_HEIGHTS and not state.startswith('model-missing') and state != 'simple-comparison-pt-BR':
            assert 'Debug view' in text or 'Visualização de diagnóstico' in text
        if state in SIMPLE and not state.startswith('model-missing'):
            assert 'Requested passes' not in text and 'Passes solicitados' not in text
            assert 'Copy NR diagnostics' not in text and 'Copiar diagnóstico do NR' not in text
            assert 'NR cost advisor' not in text and 'Assistente de custo do NR' not in text
        if state == 'advisor-hidden-off':
            assert 'Assistente de custo do NR' not in text
        elif state in ADVISOR_HEIGHTS:
            assert 'Assistente de custo do NR' in text
        if state == 'advisor-eligible-pt-BR':
            assert 'Medir opções do NR' in text
        elif state == 'advisor-running-pt-BR':
            assert 'Estabilizando o histórico' in text and 'Cancelar teste' in text
        elif state in {'advisor-done-pt-BR', 'advisor-rr-done-pt-BR'}:
            assert 'Média ms' in text and 'Cauda ms' in text and 'Aplicar opção medida' in text
            assert ('RRWorkingScale' in text) == (state == 'advisor-rr-done-pt-BR')
        elif state == 'advisor-timing-off-pt-BR':
            assert 'Ativar medição da GPU' in text and 'Medir opções do NR' not in text
    for left,right in [('d3d12-simple','d3d12-simple-pt-BR'),('present-hdr','present-hdr-pt-BR'),
                       ('nr-off','d3d12-simple'),('d3d12-full','individual-passes'),('d3d12-full','comparison')]:
        assert images[left]!=images[right], f'{left}/{right}: fixtures unexpectedly identical'
    assert subprocess.run([str(binary),'not-a-state',str(temp/'bad'),str(ROOT)],capture_output=True).returncode != 0
    assert not (temp/'bad').exists()
print('PASS: 22 named states, advisor visibility and results, actual ImGui glyph pixels, valid PNGs, complete panel height and byte-identical reruns')
