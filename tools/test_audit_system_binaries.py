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
    def test_runtime_rules_do_not_cover_copyleft_libraries(self):
        import json,re
        rules=json.loads(Path(__file__).with_name('system-binary-licenses.json').read_text())
        rules=[rule for rule in rules if rule['root']=='toolchain']
        for library in ('libquadmath.a','libgomp.a','libiberty.a','libfoo.a'):
            path='picolibc/lib/gcc/riscv32-esp-elf/16.1.0/rv32/ilp32f/no-rtti/'+library
            self.assertFalse(any(re.fullmatch(rule['archive_pattern'],path) for rule in rules))
if __name__=='__main__':unittest.main()
