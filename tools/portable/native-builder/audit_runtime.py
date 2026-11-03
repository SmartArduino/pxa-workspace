"""Fail closed on the actual ELF dependency closure of the distributed kit."""
from pathlib import Path
import hashlib,json,os,re,subprocess
BASE={'libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1','libresolv.so.2','libutil.so.1','ld-linux-x86-64.so.2'}
# Each family corresponds to the named upstream implementation, not an exception
# for libraries generally. No distro audio/network/GUI aggregation is admitted.
LIBRARIES={
 'libSDL2-2.0.so':'Zlib (curated upstream SDL2)', 'libcurl.so':'curl (curated HTTP/TLS build)',
 'libarchive.so':'BSD-2-Clause / BSD-3-Clause / CC0 (curated upstream)',
 'libedit.so':'BSD-3-Clause (NetBSD editline)', 'libpanelw.so':'MIT (ncurses)',
 'libpipewire-0.3.so':'MIT (PipeWire client library)',
 'libpng16.so':'Libpng-2.0','libz.so':'Zlib','libbz2.so':'bzip2-1.0.6','liblzma.so':'public domain / 0BSD library',
 'liblz4.so':'BSD-2-Clause','libzstd.so':'BSD-3-Clause branch', 'libbrotlidec.so':'MIT','libbrotlicommon.so':'MIT',
 'libssl.so':'Apache-2.0','libcrypto.so':'Apache-2.0','libfreetype.so':'FTL branch',
 'libjpeg.so':'IJG/BSD (libjpeg-turbo)','libturbojpeg.so':'IJG/BSD (libjpeg-turbo)',
 'libvorbisfile.so':'BSD-3-Clause','libvorbis.so':'BSD-3-Clause','libvorbisenc.so':'BSD-3-Clause','libogg.so':'BSD-3-Clause','libopusfile.so':'BSD-3-Clause','libopus.so':'BSD-3-Clause',
 'libffi.so':'MIT','libexpat.so':'MIT','libsqlite3.so':'public domain','libmpdec.so':'BSD-2-Clause','libuuid.so':'BSD-3-Clause',
 'libwebp.so':'BSD-3-Clause','libwebpmux.so':'BSD-3-Clause','libwebpdemux.so':'BSD-3-Clause','libsharpyuv.so':'BSD-3-Clause','libopenjp2.so':'BSD-2-Clause','liblcms2.so':'MIT',
 'libX11.so':'MIT/X11','libXext.so':'MIT/X11','libXcursor.so':'MIT/X11','libXi.so':'MIT/X11','libXfixes.so':'MIT/X11','libXrandr.so':'MIT/X11','libXss.so':'MIT/X11','libXrender.so':'MIT/X11','libXau.so':'MIT/X11','libXdmcp.so':'MIT/X11','libxcb.so':'MIT/X11',
 'libwayland-client.so':'MIT','libwayland-cursor.so':'MIT','libwayland-egl.so':'MIT','libxkbcommon.so':'MIT/X11',
 'libbsd.so':'BSD family','libmd.so':'BSD family','libuv.so':'MIT','libjsoncpp.so':'MIT','librhash.so':'0BSD','libtinfo.so':'MIT (ncurses)', 'libncursesw.so':'MIT (ncurses)',
 'libxml2.so':'MIT','libicudata.so':'Unicode/ICU','libicuuc.so':'Unicode/ICU','libicui18n.so':'Unicode/ICU',
 'libnghttp2.so':'MIT','libnghttp3.so':'MIT','libngtcp2.so':'MIT','libngtcp2_crypto_ossl.so':'MIT',
 'libgcc_s.so':'GPL-3.0 WITH GCC-exception-3.1 (runtime only)','libstdc++.so':'GPL-3.0 WITH GCC-exception-3.1 (runtime only)',
}
def classification(name):
 for prefix,license in LIBRARIES.items():
  if name==prefix or name.startswith(prefix+'.'):return license
 if name.startswith(('libLLVM','libclang','liblldb')):return 'Apache-2.0 WITH LLVM-exception (pinned compiler runtime)'
 return None

def audit(root):
 root=Path(root);env={**os.environ,'LD_LIBRARY_PATH':str(root/'runtime/lib')+':'+str(root/'toolchains/wasi-sdk/lib')}
 binaries=[];errors=[];libraries={}
 for p in sorted(root.rglob('*')):
  if not p.is_file() or p.is_symlink():continue
  with p.open('rb') as f:
   if f.read(4)!=b'\x7fELF':continue
  raw=subprocess.run(['ldd',str(p)],text=True,capture_output=True,env=env)
  if raw.returncode:
   dynamic=subprocess.check_output(['readelf','-d',str(p)],text=True,stderr=subprocess.DEVNULL)
   if '(NEEDED)' in dynamic:errors.append(str(p)+': ldd failed: '+raw.stdout+raw.stderr)
   continue
  needed=[]
  for line in raw.stdout.splitlines():
   if '=> not found' in line:errors.append(str(p)+': '+line);continue
   match=re.match(r'\s*(\S+) => (\S+)',line)
   if not match:continue
   name,path=match.groups();needed.append(name)
   if name in BASE:
    if Path(path).resolve().is_relative_to(root.resolve()):errors.append('OS C runtime must not be bundled: '+name)
    continue
   license=classification(name)
   if not license:errors.append('Unreviewed dependency: '+name);continue
   if not Path(path).resolve().is_relative_to(root.resolve()):errors.append('Dependency escaped private kit: '+name)
   libraries[name]={'license':license,'sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest()}
  binaries.append({'file':str(p.relative_to(root)),'needed':needed})
 if errors:raise RuntimeError('\n'.join(sorted(set(errors))))
 report={'schema':'pxa-native-runtime-1','libraries':libraries,'binaries':binaries,'os_runtime':{'components':sorted(BASE),'bundled':False,'scope':'OS-provided glibc 2.39+ and loader; dynamically replaceable. LGPL-2.1 using-library conditions reviewed separately from optional implementation dependencies.'}}
 (root/'native-runtime.json').write_text(json.dumps(report,sort_keys=True,indent=2)+'\n')
 (root/'licenses/OS-RUNTIME.txt').write_text('Linux DevKit requires the operating system glibc 2.39+ and ELF loader. These are not bundled. PXA uses the dynamically replaceable OS runtime, without imposing restrictions on replacement or debugging modifications of that runtime. This OS-runtime case does not admit arbitrary LGPL implementations. See glibc COPYING.LIB (LGPL-2.1), sections 5 and 6. Bundled GCC runtime libraries retain the GCC Runtime Library Exception 3.1. All other bundled dependencies must match the named permissive implementations in native-runtime.json.\nhttps://github.com/bminor/glibc/blob/glibc-2.39/COPYING.LIB\n')
 print('Native runtime closure verified:',len(binaries),'ELFs,',len(libraries),'named private libraries',flush=True)
 return report
if __name__=='__main__':
 import sys;audit(Path(sys.argv[1]))
