import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
spec=importlib.util.spec_from_file_location('audit',Path(__file__).with_name('audit-system-binaries.py'))
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)
class BinaryPolicyTest(unittest.TestCase):
    def test_unknown_or_changed_license_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);build=root/'build';build.mkdir()
            vendor=root/'idf';vendor.mkdir();(vendor/'approved.a').write_bytes(b'archive')
            license=vendor/'LICENSE';license.write_text('reviewed license')
            rule={'root':'idf','archive_pattern':r'approved\.a','license':'LICENSE',
                  'license_sha256':hashlib.sha256(license.read_bytes()).hexdigest(),'selected':'MIT'}
            map_path=build/'firmware.map';map_path.write_text('LOAD '+str(vendor/'approved.a')+'\n')
            selected,failures=audit.audit(map_path,vendor,root,root/'bin/gcc',[rule]);self.assertEqual(len(selected),1);self.assertFalse(failures)
            license.write_text('changed license');self.assertTrue(audit.audit(map_path,vendor,root,root/'bin/gcc',[rule])[1])
            map_path.write_text('LOAD '+str(vendor/'unreviewed.a')+'\n');self.assertTrue(audit.audit(map_path,vendor,root,root/'bin/gcc',[rule])[1])
    def test_pinned_archive_change_requires_review(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);build=root/'build';build.mkdir();vendor=root/'idf';vendor.mkdir()
            archive=vendor/'runtime.a';archive.write_bytes(b'reviewed binary')
            license=vendor/'LICENSE';license.write_text('reviewed grant')
            rule={'root':'idf','archive_pattern':r'runtime\.a','license':'LICENSE',
                  'license_sha256':hashlib.sha256(license.read_bytes()).hexdigest(),
                  'archive_sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'selected':'MIT'}
            map_path=build/'firmware.map';map_path.write_text('LOAD '+str(archive)+'\n')
            self.assertFalse(audit.audit(map_path,vendor,root,root/'bin/gcc',[rule])[1])
            archive.write_bytes(b'changed binary, same license notice')
            self.assertTrue(audit.audit(map_path,vendor,root,root/'bin/gcc',[rule])[1])

    def test_xtensa_runtime_rules_are_narrow(self):
        import json,re
        rules=json.loads(Path(__file__).with_name('system-binary-licenses.json').read_text())
        rules=[rule for rule in rules if rule.get('archive_sha256')]
        allowed=('picolibc/lib/gcc/xtensa-esp-elf/16.1.0/esp32s3/libgcc.a',
                 'picolibc/xtensa-esp-elf/lib/esp32s3/libstdc++.a',
                 'picolibc/xtensa-esp-elf/lib/esp32s3/libc.a',
                 'picolibc/xtensa-esp-elf/lib/esp32s3/libm.a',
                 'components/xtensa/esp32s3/libxt_hal.a')
        for path in allowed:
            self.assertEqual(sum(bool(re.fullmatch(r['archive_pattern'],path)) for r in rules),1)
        for library in ('libquadmath.a','libgomp.a','libiberty.a','libfoo.a'):
            for directory in ('picolibc/lib/gcc/xtensa-esp-elf/16.1.0/esp32s3/',
                              'picolibc/xtensa-esp-elf/lib/esp32s3/'):
                self.assertFalse(any(re.fullmatch(r['archive_pattern'],directory+library) for r in rules))
        self.assertFalse(any(re.fullmatch(r['archive_pattern'],'components/xtensa/esp32/libxt_hal.a') for r in rules))

    def test_runtime_rules_do_not_cover_copyleft_libraries(self):
        import json,re
        rules=json.loads(Path(__file__).with_name('system-binary-licenses.json').read_text())
        rules=[rule for rule in rules if rule['root']=='toolchain']
        for library in ('libquadmath.a','libgomp.a','libiberty.a','libfoo.a'):
            path='picolibc/lib/gcc/riscv32-esp-elf/16.1.0/rv32/ilp32f/no-rtti/'+library
            self.assertFalse(any(re.fullmatch(rule['archive_pattern'],path) for rule in rules))
if __name__=='__main__':unittest.main()
