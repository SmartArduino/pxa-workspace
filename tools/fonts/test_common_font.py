import io
import random
import unittest
from fontTools.ttLib import TTFont,newTable
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
from build_common_font import CompactCmap4,big5_characters


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

    def test_common_big5_includes_last_level_one_row(self):
        common=big5_characters('common')
        self.assertIn(ord(bytes.fromhex('c67e').decode('big5')),common)
        self.assertIn(ord('靈'),common)
        self.assertLess(len(common),len(big5_characters('all')))


if __name__=='__main__':
    unittest.main()
