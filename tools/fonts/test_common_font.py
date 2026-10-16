import io
import random
import unittest
import tempfile
import hashlib
import os
import subprocess
from pathlib import Path
from unittest.mock import patch
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
from build_common_font import CompactCmap4,big5_characters,language_characters,ui_character,system_ui_characters,setup_unicode_map,build


def fixture_font(path, region='SC', han_width=300):
    fb=FontBuilder(1000,isTTF=True)
    mapping={ord('A'):'latin',ord('骨'):'han',0xf060:'icon'}
    order=['.notdef',*mapping.values()];fb.setupGlyphOrder(order);fb.setupCharacterMap(mapping)
    glyphs={}
    for name in order:
        pen=TTGlyphPen(None);pen.moveTo((0,0));pen.lineTo((han_width if name=='han' else 300,0))
        pen.lineTo((0,600));pen.closePath();glyphs[name]=pen.glyph()
    fb.setupGlyf(glyphs);fb.setupHorizontalMetrics({name:(1000,0) for name in order})
    fb.setupHorizontalHeader(ascent=800,descent=-200)
    fb.setupNameTable(dict(familyName='Noto Sans CJK '+region,styleName='Regular'))
    fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
    fb.setupPost();fb.save(path)


class CmapTest(unittest.TestCase):
    def roundtrip(self, values):
        font=TTFont();names=['.notdef']+[f'gid{i:05d}' for i in range(1,257)]
        font.setGlyphOrder(names)
        maxp=newTable('maxp');maxp.tableVersion=0x5000;maxp.numGlyphs=len(names);font['maxp']=maxp
        mapping={cp:names[gid] for cp,gid in values.items()}
        cmap=newTable('cmap');cmap.tableVersion=0
        table=CompactCmap4(4);table.platformID=3;table.platEncID=1;table.language=0;table.cmap=mapping
        cmap.tables=[table];font['cmap']=cmap
        regular=CmapSubtable.newSubtable(4);regular.platformID=3;regular.platEncID=1
        regular.language=0;regular.cmap=mapping
        self.assertLessEqual(len(table.compile(font)),len(regular.compile(font)))
        data=io.BytesIO();font.save(data);data.seek(0);decoded=TTFont(data)
        actual=decoded.getBestCmap()
        for cp in range(65536):
            self.assertEqual(decoded.getGlyphID(actual[cp]) if cp in actual else 0,values.get(cp,0),hex(cp))

    def test_sparse_dense_aliases_and_bmp_edges(self):
        self.roundtrip({0:1,32:2,0x3400:3,0x3402:4,0x3404:5,0xfffd:6,0xfffe:7})
        self.roundtrip({100+i:1+i for i in range(256)})
        self.roundtrip({0x4e00+i*2:1+i%13 for i in range(300)})
        self.roundtrip({0x5000+i:1+(i*31)%256 for i in range(400)})

    def test_random_gaps_and_shared_glyphs(self):
        randomizer=random.Random(20261011)
        for _ in range(12):
            self.roundtrip({cp:randomizer.randrange(1,257) for cp in randomizer.sample(range(65535),500)})

    def test_one_complete_charmap_includes_bmp_and_supplementary_aliases(self):
        for supplementary in (False,True):
            fb=FontBuilder(1000,isTTF=True);fb.setupGlyphOrder(['.notdef','first','second'])
            mapping={32:'first',0x4e00:'second',0xfffe:'first'}
            if supplementary:mapping.update({0x10000:'first',0x2ffff:'second',0x10ffff:'first'})
            setup_unicode_map(fb,mapping)
            self.assertEqual(len(fb.font['cmap'].tables),1)
            self.assertEqual(fb.font['cmap'].tables[0].format,12 if supplementary else 4)
            data=io.BytesIO();fb.font.save(data);data.seek(0)
            with TTFont(data) as check:
                actual=check.getBestCmap()
                self.assertEqual(set(actual),set(mapping))
                for cp in mapping:self.assertEqual(check.getGlyphID(actual[cp]),fb.font.getGlyphID(mapping[cp]))

    def test_common_big5_includes_last_level_one_row(self):
        common=big5_characters('common')
        self.assertIn(ord(bytes.fromhex('c67e').decode('big5')),common)
        self.assertIn(ord('靈'),common)
        self.assertLess(len(common),len(big5_characters('all')))

    def test_language_repertoires_are_independent(self):
        sc=language_characters(('zh-Hans',));tc=language_characters(('zh-Hant',))
        self.assertEqual(len(sc),8105);self.assertEqual(len(tc),8833)
        self.assertIn(ord('啰'),sc);self.assertIn(ord('𠳐'),sc)
        self.assertIn(ord('爱'),sc);self.assertNotIn(ord('愛'),sc)
        self.assertIn(ord('愛'),tc);self.assertNotIn(ord('爱'),tc)
        self.assertIn(ord('あ'),language_characters(('ja',)))
        self.assertIn(ord('가'),language_characters(('ko',)))
        self.assertIn(ord('A'),language_characters(('en',)))
        self.assertNotIn(ord('é'),language_characters(('en',)))
        self.assertIn(ord('é'),language_characters(('latin',)))
        self.assertFalse(ui_character(ord('愛')));self.assertTrue(ui_character(0xf060))
        self.assertTrue({ord(c) for c in '设置键盘'} <= system_ui_characters())
        with self.assertRaises(ValueError):language_characters(('unknown',))

    def test_trimmed_font_removes_unused_glyphs_but_retains_icons(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);base=path/'base.ttf';output=path/'out.ttf'
            fb=FontBuilder(1000,isTTF=True)
            mapping={ord('A'):'latin',ord('爱'):'sc',ord('愛'):'tc',0xf060:'icon',0x2190:'arrow'}
            order=['.notdef',*mapping.values()];fb.setupGlyphOrder(order);fb.setupCharacterMap(mapping)
            pen=TTGlyphPen(None);pen.moveTo((0,0));pen.lineTo((300,0));pen.lineTo((0,600));pen.closePath()
            fb.setupGlyf({name:pen.glyph() for name in order})
            fb.setupHorizontalMetrics({name:(1000,0) for name in order})
            fb.setupHorizontalHeader(ascent=800,descent=-200)
            fb.setupNameTable(dict(familyName='Noto Sans CJK SC',styleName='Regular'))
            fb.setupOS2(sTypoAscender=800,sTypoDescender=-200,usWinAscent=800,usWinDescent=200)
            fb.setupPost();fb.save(base)
            base.chmod(0o640)
            mode=base.stat().st_mode & 0o777
            with patch('build_common_font.system_ui_characters',return_value=set()):
                report=build(base,base,0,output,[],languages=('en',))
            self.assertEqual(output.stat().st_mode & 0o777,mode)
            cmap=TTFont(output).getBestCmap()
            self.assertEqual(set(cmap),{ord('A'),0xf060,0x2190})
            self.assertEqual(report['removed_codepoints'],2)
            self.assertEqual(report['source_region'],'SC')

    def test_regional_base_cannot_override_selected_mainland_glyphs(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);base=path/'tc.ttf';source=path/'sc.ttf';output=path/'out.ttf'
            fixture_font(base,'TC',500);fixture_font(source,'SC',300)
            corpus=path/'sample.txt';corpus.write_text('骨',encoding='utf-8')
            with patch('build_common_font.system_ui_characters',return_value=set()):
                report=build(base,source,0,output,[corpus],languages=('en',))
            with TTFont(output) as result:
                glyph=result['glyf'][result.getBestCmap()[ord('骨')]]
                self.assertEqual(glyph.xMax,300)
            self.assertEqual(report['base_region'],'TC');self.assertEqual(report['source_region'],'SC')

    def test_failed_validation_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);base=path/'base.ttf';output=path/'out.ttf';fixture_font(base)
            output.write_bytes(b'existing asset')
            def corrupt(builder,mapping):
                setup_unicode_map(builder,mapping)
                builder.font['cmap'].tables[0].cmap=dict(builder.font['cmap'].tables[0].cmap)
                builder.font['cmap'].tables[0].cmap.pop(ord('A'))
            with patch('build_common_font.system_ui_characters',return_value=set()), \
                 patch('build_common_font.setup_unicode_map',side_effect=corrupt):
                with self.assertRaises(AssertionError):build(base,base,0,output,[],languages=('en',))
            self.assertEqual(output.read_bytes(),b'existing asset')
            self.assertEqual(list(path.glob('.out.ttf.*')),[])

    def test_in_place_base_hash_and_source_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);base=path/'base.ttf';source=path/'source.ttf'
            fixture_font(base);fixture_font(source)
            base.chmod(0o640)
            mode=base.stat().st_mode & 0o777
            original=hashlib.sha256(base.read_bytes()).hexdigest()
            with patch('build_common_font.system_ui_characters',return_value=set()):
                report=build(base,source,0,base,[],languages=('en',))
            self.assertEqual(report['base_sha256'],original)
            self.assertEqual(base.stat().st_mode & 0o777,mode)
            self.assertEqual(report['sha256'],hashlib.sha256(base.read_bytes()).hexdigest())
            self.assertNotEqual(report['sha256'],original)
            source_bytes=source.read_bytes()
            with self.assertRaises(ValueError):build(base,source,0,source,[],languages=('en',))
            self.assertEqual(source.read_bytes(),source_bytes)


@unittest.skipUnless(os.environ.get('PXA_FONT_PROBE'), 'Set PXA_FONT_PROBE to the native FreeType probe')
class NativeProbeTest(unittest.TestCase):
    def run_probe(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);font=path/'font.ttf';corpus=path/'text.txt'
            fixture_font(font);corpus.write_bytes(text)
            return subprocess.run([os.environ['PXA_FONT_PROBE'],str(font),str(corpus)],
                                  capture_output=True,text=True)

    def test_missing_glyph_is_a_failed_check(self):
        self.assertEqual(self.run_probe('A骨'.encode()).returncode,0)
        result=self.run_probe(chr(0x10ffff).encode())
        self.assertEqual(result.returncode,1)
        self.assertIn('"missing":4',result.stdout)

    def test_malformed_utf8_is_rejected(self):
        for text in (b'\x80',b'\xc0\xaf',b'\xe0\x80\xaf',b'\xed\xa0\x80',
                     b'\xf4\x90\x80\x80',b'\xe4\xbd'):
            with self.subTest(text=text):self.assertEqual(self.run_probe(text).returncode,6)


if __name__=='__main__':
    unittest.main()
