#!/usr/bin/env python3
"""Extend the existing SC system font with traditional Chinese, offline.

Requires fontTools and an OFL licensed Noto Sans CJK SC source face. Keeps
every existing character and its outline. New outlines are converted to
TrueType, with a compact cmap; no runtime fallback face/cache is introduced.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable


class CompactCmap4(CmapSubtable):
    """Minimum-size mix of delta segments and explicit glyph arrays, O(n).

    The usual cmap compiler splits at every absent codepoint. Sparse Hangul
    and CJK coverage then wastes much more RAM than small zero-filled gaps.
    FreeType loads this table; compacting it pays for the new glyph offsets.
    """
    def compile(self, font):
        if not self.cmap:
            return super().compile(font)
        values = sorted((cp, font.getGlyphID(g)) for cp, g in self.cmap.items())
        n = len(values)
        costs = [0] * (n + 1)
        choices = [None] * n
        best_array = None
        run = 0
        for i, (cp, gid) in enumerate(values):
            candidate = (costs[i] - 2 * cp, i)
            if best_array is None or candidate < best_array:
                best_array = candidate
            if i and (cp != values[i-1][0] + 1 or gid != values[i-1][1] + 1):
                run = i
            array_cost = best_array[0] + 2 * cp + 10
            delta_cost = costs[run] + 8
            if delta_cost <= array_cost:
                costs[i+1] = delta_cost
                choices[i] = (run, False)
            else:
                costs[i+1] = array_cost
                choices[i] = (best_array[1], True)
        segments = []
        i = n - 1
        while i >= 0:
            start, explicit = choices[i]
            segments.append((values[start:i+1], explicit))
            i = start - 1
        segments.reverse()
        count = len(segments) + 1
        starts, ends, deltas, offsets, glyphs = [], [], [], [], []
        for index, (items, explicit) in enumerate(segments):
            lo, hi = items[0][0], items[-1][0]
            starts.append(lo); ends.append(hi)
            if explicit:
                deltas.append(0)
                offsets.append(2 * (count - index + len(glyphs)))
                by_cp = dict(items)
                glyphs.extend(by_cp.get(cp, 0) for cp in range(lo, hi+1))
            else:
                deltas.append((items[0][1] - lo) & 0xffff)
                offsets.append(0)
        starts.append(0xffff); ends.append(0xffff)
        deltas.append(1); offsets.append(0)
        power = count.bit_length() - 1
        length = 16 + count * 8 + len(glyphs) * 2
        if length > 65535:
            raise ValueError('BMP cmap exceeds format 4 capacity')
        header = struct.pack('>7H', 4, length, 0, count*2,
                             (1 << power)*2, power, count*2-(1 << power)*2)
        words = ends + [0] + starts + deltas + offsets + glyphs
        return header + struct.pack('>' + 'H'*len(words), *words)


def big5_characters(coverage='common'):
    result = set()
    for lead in range(0xa1, 0xfa if coverage == 'all' else 0xc7):
        for trail in list(range(0x40, 0x7f)) + list(range(0xa1, 0xff)):
            if coverage == 'common' and lead == 0xc6 and trail > 0x7e:
                continue
            try:
                result.update(cp for cp in map(ord, bytes((lead, trail)).decode('big5'))
                              if 0x3400 <= cp <= 0x9fff or 0xf900 <= cp <= 0xfaff)
            except UnicodeDecodeError:
                pass
    return result


def build(base_path, source_path, face, output, corpora, coverage='common'):
    base = TTFont(base_path, recalcTimestamp=False)
    source = TTFont(source_path, fontNumber=face, recalcTimestamp=False)
    names = [n.toUnicode() for n in source['name'].names if n.nameID == 1]
    if not any('Sans' in n and (' SC' in n or ' CN' in n) for n in names):
        raise ValueError('Select the Chinese mainland sans-serif source face')
    if source['head'].unitsPerEm != base['head'].unitsPerEm:
        raise ValueError('Source and base unitsPerEm must match')
    old_map = base.getBestCmap()
    source_map = source.getBestCmap()
    requested = set(old_map) | big5_characters(coverage)
    for corpus in corpora:
        requested.update(ord(c) for c in corpus.read_text(encoding='utf-8')
                         if not c.isspace() and ord(c) != 0xfeff)
    missing = requested - set(old_map) - set(source_map)
    if missing:
        raise ValueError('Source lacks requested characters: ' +
                         ' '.join(f'U+{c:04X}' for c in sorted(missing)))
    glyphs = {name: copy.deepcopy(base['glyf'][name]) for name in base.getGlyphOrder()}
    metrics = dict(base['hmtx'].metrics)
    cmap = dict(old_map)
    source_glyphs = source.getGlyphSet()
    if glyphs['.notdef'].numberOfContours == 0:
        pen = TTGlyphPen(None)
        source_glyphs['.notdef'].draw(Cu2QuPen(pen,max_err=1.0,reverse_direction=True))
        glyphs['.notdef'] = pen.glyph()
    for cp in sorted(requested - set(old_map)):
        name = f'cjk{cp:05X}'
        pen = TTGlyphPen(None)
        source_glyphs[source_map[cp]].draw(Cu2QuPen(pen, max_err=1.0, reverse_direction=True))
        glyphs[name] = pen.glyph()
        metrics[name] = source['hmtx'][source_map[cp]]
        cmap[cp] = name
    # Exact duplicates only: preserve outlines, instructions, and advances.
    # Sharing them reduces both Flash glyph data and FreeType's loca table.
    canonical = {}
    aliases = {}
    for name, glyph in glyphs.items():
        if glyph.isComposite():
            continue
        key = (glyph.compile(base['glyf']), metrics[name])
        if key in canonical:
            aliases[name] = canonical[key]
        else:
            canonical[key] = name
    cmap = {cp:aliases.get(name,name) for cp,name in cmap.items()}
    referenced = {component.glyphName for glyph in glyphs.values()
                  if glyph.isComposite() for component in glyph.components}
    for name in aliases:
        if name not in referenced and name != '.notdef':
            del glyphs[name]; del metrics[name]
    order = ['.notdef']
    seen = set(order)
    # Assign the main Han characters before their radical/compatibility aliases.
    for cp in sorted(cmap,key=lambda cp:(not 0x4e00 <= cp <= 0x9fff,cp)):
        name = cmap[cp]
        if name not in seen:
            seen.add(name); order.append(name)
    # Preserve components referenced by the original glyphs.
    order.extend(name for name in base.getGlyphOrder() if name not in seen and name in glyphs)
    fb = FontBuilder(base['head'].unitsPerEm, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    # The original asset uses one Windows Unicode cmap. A duplicate platform-0
    # entry would allocate another FreeType charmap object for no new coverage.
    fb.font['cmap'].tables = [t for t in fb.font['cmap'].tables if t.platformID == 3]
    for index, old in enumerate(fb.font['cmap'].tables):
        if old.format == 4:
            table = CompactCmap4(4)
            table.platformID = old.platformID
            table.platEncID = old.platEncID
            table.language = 0
            table.cmap = old.cmap
            fb.font['cmap'].tables[index] = table
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.font['hhea'] = copy.deepcopy(base['hhea'])
    fb.font['OS/2'] = copy.deepcopy(base['OS/2'])
    fb.setupNameTable(dict(familyName='PXA Sans SC', styleName='Regular',
                          fullName='PXA Sans SC Regular', psName='PXASansSC-Regular'),
                      mac=False)
    fb.setupPost(keepGlyphNames=False)
    fb.font['head'].created = base['head'].created
    fb.font['head'].modified = base['head'].modified
    fb.font.recalcTimestamp = False
    output.parent.mkdir(parents=True, exist_ok=True)
    fb.save(output)
    check = TTFont(output)
    if check.getBestCmap().keys() != cmap.keys():
        raise AssertionError('Compiled cmap coverage mismatch')
    # Verify every mapping, including intentional unmapped gaps.
    for cp in range(0x10000):
        table = next(t for t in check['cmap'].tables if t.format == 4)
        name = table.cmap.get(cp)
        if cp in cmap:
            assert name and check.getGlyphID(name) == fb.font.getGlyphID(cmap[cp]), hex(cp)
        else:
            assert name is None or check.getGlyphID(name) == 0, hex(cp)
    return dict(base_sha256=hashlib.sha256(base_path.read_bytes()).hexdigest(),
                source_sha256=hashlib.sha256(source_path.read_bytes()).hexdigest(),
                source_face=face, base_codepoints=len(old_map),
                traditional_coverage=coverage,
                codepoints=len(cmap), glyphs=check['maxp'].numGlyphs,
                added_codepoints=len(cmap)-len(old_map),
                bytes=output.stat().st_size,
                sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                tables={k:check.reader.tables[k].length for k in check.reader.tables},
                corpora=[dict(name=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in corpora])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--face', type=int, default=2)
    parser.add_argument('--corpus', type=Path, action='append', default=[])
    parser.add_argument('--traditional', choices=('common','all'), default='common')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = build(args.base,args.source,args.face,args.output,args.corpus,args.traditional)
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps(report,ensure_ascii=False))


if __name__ == '__main__':
    main()
