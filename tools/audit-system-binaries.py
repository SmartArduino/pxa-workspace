#!/usr/bin/env python3
"""Check linked prebuilt archives against reviewed platform license grants."""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]

def audit(map_path, idf_root, firmware_root, compiler, policy):
    build=map_path.resolve().parent
    toolchain=compiler.resolve().parent.parent
    roots={'idf':idf_root.resolve(),'firmware':firmware_root.resolve(),'toolchain':toolchain}
    failures=[];selected=[]
    for raw in sorted(set(re.findall(r'^LOAD (.+\.a)$',map_path.read_text(),re.M))):
        path=Path(raw)
        if not path.is_absolute():path=build/path
        path=path.resolve()
        if path.is_relative_to(build):continue # Compiled sources are checked separately.
        matched=False
        for rule in policy:
            root=roots[rule['root']]
            if not path.is_relative_to(root):continue
            relative=path.relative_to(root).as_posix()
            if not re.fullmatch(rule['archive_pattern'],relative):continue
            license_path=root/rule['license']
            if not license_path.is_file() or hashlib.sha256(license_path.read_bytes()).hexdigest()!=rule['license_sha256']:
                failures.append(f'{path}: license changed; re-review required');matched=True;break
            archive_hash=hashlib.sha256(path.read_bytes()).hexdigest()
            if rule.get('archive_sha256') and archive_hash!=rule['archive_sha256']:
                failures.append(f'{path}: archive changed; re-review required');matched=True;break
            selected.append({'path':str(path),'license':rule['selected'],'license_file':str(license_path),
                'sha256':archive_hash})
            matched=True;break
        if not matched:failures.append(f'{path}: prebuilt archive has no reviewed license rule')
    return selected,failures

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('map','idf-root','firmware-root','compiler'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    policy=json.loads((ROOT/'tools/system-binary-licenses.json').read_text())
    selected,failures=audit(args.map,args.idf_root,args.firmware_root,args.compiler,policy)
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps({'archives':selected,'failures':failures},indent=2)+'\n')
    for error in failures:print('LICENSE ERROR:',error)
    print(f'System binary license gate: {len(selected)} reviewed archives, {len(failures)} violations')
    return int(bool(failures))
if __name__=='__main__':raise SystemExit(main())
