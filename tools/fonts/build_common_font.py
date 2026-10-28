#!/usr/bin/env python3
"""Build one device font from optional language sets, entirely offline.

Requires fontTools and an OFL licensed Noto Sans CJK source face. The default
preserves existing coverage; --languages builds a smaller device-specific
subset. New outlines are converted to TrueType, with a compact cmap. Missing
source glyphs can be merged from an optional build-time supplement; no runtime
fallback face/cache is introduced.
"""
import argparse
import ast
import copy
from contextlib import ExitStack
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import tempfile

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
from fontTools import subset


LANGUAGES = ('en', 'latin', 'greek', 'cyrillic', 'zh-Hans', 'zh-Hant', 'ja', 'ko')
IME_DATA = Path(__file__).resolve().parents[2] / 'deps/pxa-system/tools/ime/data'


def system_ui_characters():
    """Keep fixed system labels usable even without a complete Chinese set.

    These are a few UI characters, not a second language repertoire. Read the
    actual catalog and bitmap-label source so adding a label cannot silently
    make a regional font unusable. No UI code is executed by the builder.
    """
    sdk = IME_DATA.parents[2]
    source = (sdk / 'ui/reference/lvgl/src/reference_lvgl.c').read_text(encoding='utf-8')
    catalog = source.split('reference_zh_resources[] = {', 1)[1].split('};', 1)[0]
    result = {ord(c) for c in catalog if ord(c) > 127}
    labels = ast.parse((sdk / 'tools/fonts/generate_ime_labels.py').read_text(encoding='utf-8'))
    assignment = next(node for node in labels.body if isinstance(node, ast.Assign) and
                      any(isinstance(t, ast.Name) and t.id == 'labels' for t in node.targets))
    result.update(map(ord, ast.literal_eval(assignment.value)))
    return result


def language_characters(languages):
    """Explicit profiles retain UI symbols, not unrelated writing systems.

    Chinese lists are the same independently validated sources as the IME.
    Japanese uses JIS X 0208/0201; Korean covers modern Hangul and Jamo.
    A corpus can add application-specific characters to any profile.
    """
    result = set()
    for language in languages:
        if language not in LANGUAGES:
            raise ValueError(f'Unknown language: {language}')
        if language in ('zh-Hans', 'zh-Hant'):
            filename = 'tgh2013.tsv' if language == 'zh-Hans' else 'traditional-readings.tsv'
            column = 1 if language == 'zh-Hans' else 0
            for line in (IME_DATA / filename).read_text(encoding='utf-8').splitlines():
                if line and not line.startswith('#'):
                    result.add(ord(line.split('\t')[column]))
        elif language == 'ja':
            for lead in range(0xa1, 0xff):
                for trail in range(0xa1, 0xff):
                    try:
                        result.update(map(ord, bytes((lead, trail)).decode('euc_jp')))
                    except UnicodeDecodeError:
                        pass
            result.update(range(0xff61, 0xffa0))
        elif language == 'ko':
            result.update(range(0x1100, 0x1200))
            result.update(range(0x3131, 0x318f))
            result.update(range(0xac00, 0xd7a4))
        else:
            lo, hi = {'en': (0x20, 0x7f), 'latin': (0x20, 0x250),
                      'greek': (0x370, 0x400), 'cyrillic': (0x400, 0x530)}[language]
            result.update(range(lo, hi))
            if language in ('latin', 'greek', 'cyrillic'):
                result.update(range(0x300, 0x370))
    return result


def ui_character(cp):
    # Keep existing UI punctuation, mathematical symbols, arrows and icons.
    # Never infer Chinese coverage from broad Han ranges in a trimmed font.
    return (0x20 <= cp <= 0x7e or 0xa0 <= cp <= 0xbf or cp in (0xd7, 0xf7) or
            0x2000 <= cp <= 0x2bff or 0x3000 <= cp <= 0x303f or
            0xe000 <= cp <= 0xf8ff or 0xfe00 <= cp <= 0xfe6f or
            0xff00 <= cp <= 0xff65 or 0xfff0 <= cp <= 0xffff)


def cjk_character(cp):
    return (0x2e80 <= cp <= 0xa4cf or 0xac00 <= cp <= 0xd7af or
            0xf900 <= cp <= 0xfaff or 0x20000 <= cp <= 0x323af)


def font_region(font):
    for name in font['name'].names:
        if name.nameID not in (1, 16):
            continue
        match = re.search(r'(?:^|[ -])(SC|CN|TC|HK|JP|KR)(?:$|[ -])', name.toUnicode())
        if match:
            return 'SC' if match[1] == 'CN' else match[1]
    return None


def setup_unicode_map(builder, mapping):
    """One complete Unicode charmap, including supplementary-plane characters.

    FreeType and Pillow select UCS-4 when present. Keeping a second BMP table
    duplicates mappings and allocates an extra charmap without new coverage.
    BMP-only fonts retain the compact format 4 representation.
    """
    builder.setupCharacterMap(mapping)
    format = 12 if any(cp > 0xffff for cp in mapping) else 4
    table = next(t for t in builder.font['cmap'].tables if t.platformID == 3 and t.format == format)
    if format == 4:
        compact = CompactCmap4(4)
        compact.platformID = table.platformID
        compact.platEncID = table.platEncID
        compact.language = 0
        compact.cmap = table.cmap
        table = compact
    builder.font['cmap'].tables = [table]


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
    if coverage == 'none':
        return set()
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


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(128 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def build(base_path, source_path, face, output, corpora, coverage='common', fallback_path=None, languages=None):
    # Allow updating the base in place, but never clobber source fonts/corpora.
    for path in (source_path, fallback_path, *corpora):
        if path is not None and path.resolve() == output.resolve():
            raise ValueError('Output would overwrite a source font or corpus')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='.'+output.name+'.', suffix='.ttf',
                                     dir=output.parent, delete=False) as staged:
        temporary = Path(staged.name)
    try:
        report = _build(base_path, source_path, face, temporary, corpora,
                        coverage, fallback_path, languages)
        # NamedTemporaryFile starts at 0600. Keep an existing asset's access
        # mode, or inherit the base's, so atomic generation stays distributable.
        temporary.chmod((output if output.exists() else base_path).stat().st_mode & 0o777)
        # _build closes all input/output readers before replacement, including
        # an in-place base. This also works on Windows, which locks open files.
        os.replace(temporary, output)
        return report
    finally:
        temporary.unlink(missing_ok=True)


def _build(base_path, source_path, face, output, corpora, coverage='common', fallback_path=None, languages=None):
    with ExitStack() as resources:
        base = resources.enter_context(TTFont(base_path, recalcTimestamp=False))
        source = resources.enter_context(TTFont(source_path, fontNumber=face, recalcTimestamp=False))
        names = [n.toUnicode() for n in source['name'].names if n.nameID == 1]
        region = 'SC'
        if languages and 'zh-Hans' not in languages:
            region = next((r for lang, r in [('zh-Hant','TC'), ('ja','JP'), ('ko','KR')]
                           if lang in languages), 'SC')
        if not any('Sans' in n and (' '+region in n or region == 'SC' and ' CN' in n) for n in names):
            raise ValueError(f'Select the Noto Sans CJK {region} source face')
        if source['head'].unitsPerEm != base['head'].unitsPerEm:
            raise ValueError('Source and base unitsPerEm must match')
        old_map = base.getBestCmap()
        base_region = font_region(base)
        source_map = source.getBestCmap()
        ui = system_ui_characters() if languages is not None else set()
        requested = (language_characters(languages) | ui | {cp for cp in old_map if ui_character(cp)}
                     if languages is not None else set(old_map) | big5_characters(coverage))
        # Optional language ranges only request glyphs actually present in Noto.
        # Chinese and application corpora below remain strict: a missing glyph is
        # an error, never silently removed from a promised character repertoire.
        omitted = set()
        if languages is not None:
            optional = language_characters([lang for lang in languages if not lang.startswith('zh-')])
            omitted = optional - set(old_map) - set(source_map)
            requested -= omitted
        for corpus in corpora:
            requested.update(ord(c) for c in corpus.read_text(encoding='utf-8')
                             if not c.isspace() and ord(c) != 0xfeff)
        fallback = resources.enter_context(TTFont(fallback_path, recalcTimestamp=False)) if fallback_path else None
        fallback_map = fallback.getBestCmap() if fallback else {}
        missing = requested - set(old_map) - set(source_map) - set(fallback_map)
        if missing:
            raise ValueError('Source lacks requested characters: ' +
                             ' '.join(f'U+{c:04X}' for c in sorted(missing)))
        if languages is not None:
            keep = requested & set(old_map)
            if base_region != region:
                keep -= {cp for cp in keep if cjk_character(cp) and cp in source_map}
            options = subset.Options()
            options.notdef_outline = True
            options.glyph_names = True
            options.recalc_timestamp = False
            subsetter = subset.Subsetter(options=options)
            subsetter.populate(unicodes=keep)
            subsetter.subset(base)
        retained_map = base.getBestCmap() or {}
        glyphs = {name: copy.deepcopy(base['glyf'][name]) for name in base.getGlyphOrder()}
        metrics = dict(base['hmtx'].metrics)
        cmap = dict(retained_map)
        source_glyphs = source.getGlyphSet()
        if glyphs['.notdef'].numberOfContours == 0:
            pen = TTGlyphPen(None)
            source_glyphs['.notdef'].draw(Cu2QuPen(pen,max_err=1.0,reverse_direction=True))
            glyphs['.notdef'] = pen.glyph()
        for cp in sorted(requested - set(retained_map)):
            name = f'cjk{cp:05X}'
            pen = TTGlyphPen(None)
            if cp in source_map:
                source_glyphs[source_map[cp]].draw(Cu2QuPen(pen, max_err=1.0, reverse_direction=True))
                metrics[name] = source['hmtx'][source_map[cp]]
            else:
                scale = base['head'].unitsPerEm / fallback['head'].unitsPerEm
                fallback_glyphs = fallback.getGlyphSet()
                outline = DecomposingRecordingPen(fallback_glyphs)
                fallback_glyphs[fallback_map[cp]].draw(outline)
                outline.replay(TransformPen(
                    Cu2QuPen(pen, max_err=1.0, reverse_direction=True),
                    (scale, 0, 0, scale, 0, 0)))
                metrics[name] = tuple(round(n * scale) for n in fallback['hmtx'][fallback_map[cp]])
            glyphs[name] = pen.glyph()
            cmap[cp] = name
        # Exact duplicates only: preserve outlines, instructions, and advances.
        # Sharing them reduces both Flash glyph data and FreeType's loca table.
        canonical = {}
        aliases = {}
        for name, glyph in glyphs.items():
            # Glyph ID zero always means missing in Unicode charmaps. A real
            # character with the same outline must retain a nonzero glyph ID.
            if name == '.notdef' or glyph.isComposite():
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
        setup_unicode_map(fb, cmap)
        fb.setupGlyf(glyphs)
        fb.setupHorizontalMetrics(metrics)
        fb.font['hhea'] = copy.deepcopy(base['hhea'])
        fb.font['OS/2'] = copy.deepcopy(base['OS/2'])
        fb.setupNameTable(dict(familyName=f'PXA Sans {region}', styleName='Regular',
                              fullName=f'PXA Sans {region} Regular', psName=f'PXASans{region}-Regular'),
                          mac=False)
        fb.font['OS/2'].recalcUnicodeRanges(fb.font)
        fb.font['OS/2'].usFirstCharIndex = min(cmap)
        fb.font['OS/2'].usLastCharIndex = min(max(cmap), 0xffff)
        fb.setupPost(keepGlyphNames=False)
        fb.font['head'].created = base['head'].created
        fb.font['head'].modified = base['head'].modified
        fb.font.recalcTimestamp = False
        output.parent.mkdir(parents=True, exist_ok=True)
        fb.save(output)
        check = resources.enter_context(TTFont(output, recalcTimestamp=False))
        actual = check.getBestCmap()
        if actual.keys() != cmap.keys():
            raise AssertionError('Compiled cmap coverage mismatch')
        # Coverage equality rejects spurious gap mappings; verify glyph IDs too,
        # including supplementary-plane characters rather than only the BMP.
        for cp, name in actual.items():
            if check.getGlyphID(name) != fb.font.getGlyphID(cmap[cp]):
                raise AssertionError(f'Compiled glyph mapping mismatch: U+{cp:04X}')
        return dict(base_sha256=file_sha256(base_path),
                    source_sha256=file_sha256(source_path),
                    source_face=face, base_codepoints=len(old_map),
                    fallback_source_sha256=file_sha256(fallback_path) if fallback_path else None,
                    fallback_codepoints=[f'U+{cp:04X}' for cp in sorted(requested-set(old_map)-set(source_map))],
                    languages=languages, source_region=region, base_region=base_region,
                    unicode_cmap_format=check['cmap'].tables[0].format,
                    fixed_ui_codepoints=len(ui),
                    omitted_optional_range_codepoints=[f'U+{cp:04X}' for cp in sorted(omitted)],
                    traditional_coverage=coverage if languages is None else None,
                    codepoints=len(cmap), glyphs=check['maxp'].numGlyphs,
                    added_codepoints=len(set(cmap)-set(old_map)),
                    removed_codepoints=len(set(old_map)-set(cmap)),
                    bytes=output.stat().st_size,
                    sha256=file_sha256(output),
                    tables={k:check.reader.tables[k].length for k in check.reader.tables},
                    corpora=[dict(name=str(p),sha256=file_sha256(p)) for p in corpora])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--face', type=int, help='TTC face; automatically selects SC/TC/JP/KR for --languages')
    parser.add_argument('--languages', help='Comma-separated optional sets: '+', '.join(LANGUAGES)+'. Omit to preserve existing coverage.')
    parser.add_argument('--fallback-source', type=Path, help='Optional OFL font for missing source glyphs')
    parser.add_argument('--corpus', type=Path, action='append', default=[])
    parser.add_argument('--traditional', choices=('none','common','all'), default='common',
                        help='Without --languages, none preserves the base and adds only corpus characters')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    languages = tuple(dict.fromkeys(args.languages.split(','))) if args.languages is not None else None
    if languages is not None and (not languages or any(lang not in LANGUAGES for lang in languages)):
        parser.error('Select at least one supported --languages value')
    face = args.face
    if face is None:
        face = 2 if languages is None or 'zh-Hans' in languages else next(
            (index for lang,index in [('zh-Hant',3),('ja',0),('ko',1)] if lang in languages), 2)
    if args.report.resolve() in {p.resolve() for p in
            (args.base, args.source, args.output, *args.corpus, *([args.fallback_source] if args.fallback_source else []))}:
        parser.error('--report must not overwrite a font or corpus')
    report = build(args.base,args.source,face,args.output,args.corpus,args.traditional,args.fallback_source,languages)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))


if __name__ == '__main__':
    main()
