"""The shipped system font must render teaching phonetics without lost letters."""
from pathlib import Path
import unittest

from fontTools.ttLib import TTFont


class StudyFontTest(unittest.TestCase):
    def test_shipped_font_has_pinyin_and_english_ipa(self):
        root = Path(__file__).resolve().parents[2]
        corpus = (root / "tools/fonts/study-phonetics.txt").read_text(encoding="utf-8")
        required = {ord(char) for char in corpus if not char.isspace()}
        sample = {ord(char) for char in "rén nǚ ā é ě ù ə ɪ ʊ ʃ ʒ ŋ ˈ ː Ω" if not char.isspace()}
        self.assertTrue({codepoint for codepoint in sample if codepoint > 127} <= required)
        with TTFont(root / "factory/base/system/fonts/noto_sans_cjk_common.ttf") as font:
            cmap = font.getBestCmap()
            self.assertFalse(required - set(cmap), "Missing teaching phonetic characters")
            self.assertTrue(sample <= set(cmap))
            self.assertTrue(all(font.getGlyphID(cmap[codepoint]) > 0 for codepoint in required))


if __name__ == "__main__":
    unittest.main()
