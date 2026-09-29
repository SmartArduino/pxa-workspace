#!/usr/bin/env python3
"""Regression checks for rejecting unbudgeted codec link paths."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("audit", Path(__file__).with_name("audit-codec-memory.py"))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class AuditTest(unittest.TestCase):
    def setUp(self):
        self.elf = "\n".join(f"1: 00000001 16 FUNC GLOBAL DEFAULT 1 {h}" for h in audit.HOOKS)
        self.map = "esp-idf/pxa/libpxa.a(pxa_codec_memory.cc.obj)\n"
        self.archives = {}
        for name in audit.ARCHIVES:
            self.map += f"/lib/{name}(decoder.obj)\n"
            self.archives[name] = f"File: /lib/{name}(decoder.obj)\n" + "\n".join(
                f"1: 00000000 0 NOTYPE GLOBAL DEFAULT UND {h}" for h in audit.HOOKS)
            # Optional unselected decoders may import native allocators.
            self.archives[name] += f"\nFile: /lib/{name}(optional.obj)\n1: 00000000 0 NOTYPE GLOBAL DEFAULT UND malloc\n"
        self.map += "Discarded input sections\n/lib/libesp_audio_codec.a(optional.obj)\n"

    def test_selected_only(self):
        self.assertEqual(len(audit.audit(self.elf, self.map, self.archives)), 2)

    def test_weak_fallback(self):
        with self.assertRaisesRegex(ValueError, "missing strong"):
            audit.audit(self.elf.replace("FUNC GLOBAL", "FUNC WEAK", 1), self.map, self.archives)

    def test_direct_allocation(self):
        name = next(iter(self.archives))
        self.archives[name] = self.archives[name].replace("UND media_lib_module_malloc", "UND malloc")
        with self.assertRaisesRegex(ValueError, "unaudited allocation"):
            audit.audit(self.elf, self.map, self.archives)

    def test_new_hook(self):
        name = next(iter(self.archives))
        self.archives[name] = self.archives[name].replace("UND media_lib_module_malloc", "UND media_lib_caps_malloc_align")
        with self.assertRaisesRegex(ValueError, "unaudited allocation"):
            audit.audit(self.elf, self.map, self.archives)

    def test_missing_object(self):
        self.map = "libesp_audio_codec.a(missing.obj)\n" + self.map
        with self.assertRaisesRegex(ValueError, "missing symbol tables"):
            audit.audit(self.elf, self.map, self.archives)

    def test_internal_helper_still_checks_final_allocator(self):
        name = next(iter(self.archives))
        self.map = f"/lib/{name}(helper.obj)\n" + self.map
        self.archives[name] = self.archives[name].replace(
            "UND media_lib_module_malloc", "UND custom_alloc")
        self.archives[name] += (f"\nFile: /lib/{name}(helper.obj)\n"
                               "1: 00000001 16 FUNC WEAK DEFAULT 1 custom_alloc\n"
                               "2: 00000000 0 NOTYPE GLOBAL DEFAULT UND media_lib_module_malloc\n")
        self.assertEqual(len(audit.audit(self.elf, self.map, self.archives)), 3)
        self.archives[name] = self.archives[name].replace("UND media_lib_module_malloc", "UND malloc")
        with self.assertRaisesRegex(ValueError, "unaudited allocation"):
            audit.audit(self.elf, self.map, self.archives)


if __name__ == "__main__":
    unittest.main()
