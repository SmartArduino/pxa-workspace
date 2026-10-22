#!/usr/bin/env python3
"""Remove real long installer descendants through the production PXADB entry."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#include "pxa/posix/pxa_posix_fs.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#define CONFIG_PXA_ENABLED 1
constexpr size_t kFsAbsolutePathLength=246;
// PRODUCTION
int main() {
    char root[]="/tmp/pxadb-resource-cleanup-XXXXXX";
    assert(mkdtemp(root));
    std::string owners=std::string(root)+"/"+std::string(64,'a');assert(!mkdir(owners.c_str(),0700));
    owners+="/"+std::string(64,'b');assert(!mkdir(owners.c_str(),0700));
    owners+="/owners";assert(!mkdir(owners.c_str(),0700));
    std::string hidden=owners+"/."+std::string(64,'c')+"-"+std::string(64,'d');
    assert(hidden.size()>kFsAbsolutePathLength);
    FILE *file=fopen(hidden.c_str(),"wb");assert(file && fwrite("owner",1,5,file)==5 && !fclose(file));
    assert(RemoveTreeAt(root) && access(root,F_OK)!=0 && errno==ENOENT);
    assert(RemoveTreeAt(root));
    char links[]="/tmp/pxadb-resource-link-XXXXXX";assert(mkdtemp(links));
    char outside[]="/tmp/pxadb-resource-outside-XXXXXX";assert(mkdtemp(outside));
    std::string sentinel=std::string(outside)+"/keep";
    file=fopen(sentinel.c_str(),"wb");assert(file && fwrite("keep",1,4,file)==4 && !fclose(file));
    std::string link=std::string(links)+"/outside";assert(!symlink(outside,link.c_str()));
    assert(RemoveTreeAt(links) && access(links,F_OK)!=0);
    assert(access(sentinel.c_str(),F_OK)==0);assert(!unlink(sentinel.c_str()) && !rmdir(outside));
    puts("PXADB remove: long hidden installer ownership paths, missing roots and symlink containment passed");
}
'''


def main():
    source = function((ROOT / 'firmware/components/pxadb/src_pxa_service.cc').read_text(), 'bool RemoveTreeAt(')
    sdk = ROOT / 'deps/pxa-system/libpxa'
    includes = [f'-I{sdk / "include"}', f'-I{sdk / "adapters/include"}']
    checks = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g', '-O1']
    with tempfile.TemporaryDirectory(prefix='pxadb-fs-remove-') as directory:
        work = Path(directory)
        unit = work / 'remove.cpp'
        unit.write_text(FIXTURE.replace('// PRODUCTION', source))
        subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-D_GNU_SOURCE',
                        '-ffunction-sections', '-fdata-sections', *checks, *includes,
                        '-c', str(sdk / 'adapters/posix/pxa_posix_fs.c'), '-o', str(work / 'fs.o')], check=True)
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        *checks, *includes, str(unit), str(work / 'fs.o'), '-Wl,--gc-sections',
                        '-o', str(work / 'remove')], check=True)
        subprocess.run([str(work / 'remove')], check=True)


if __name__ == '__main__':
    main()
