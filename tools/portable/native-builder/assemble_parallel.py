"""Keep the upstream archive layout/signing, compress canonical tar streams in parallel."""
import os,subprocess,sys,tarfile,shutil,json,hashlib
from pathlib import Path
sys.path.insert(0,'/src/deps/pxa-system/tools/release')
import assemble
from audit_runtime import audit
checked=False

def archive(root, paths, destination, epoch):
    global checked
    if not checked:
        root=Path(root)
        recipe=root/'licenses/native-runtime-build';recipe.mkdir(parents=True,exist_ok=True)
        here=Path(__file__).resolve().parent
        for name in ['Dockerfile','Dockerfile.archive','sources.json','audit_runtime.py','assemble_parallel.py']:
            shutil.copyfile(here/name,recipe/name)
        for source,name in [('/native/src/sdl2/LICENSE.txt','SDL2-Zlib.txt'),('/native/src/curl/COPYING','curl.txt'),('/native/src/libarchive-3.8.11/COPYING','libarchive.txt')]:
            shutil.copyfile(source,root/'licenses'/name)
        with (root/'README.md').open('a') as f:
            f.write('\nLinux audio uses PipeWire; ALSA/PulseAudio-only hosts are not supported by this kit. X11/Wayland window backends are available. Pillow uses basic FreeType layout without optional Raqm, TIFF or ImageQuant. See native-runtime.json for the audited ELF closure and licenses/native-runtime-build for build inputs.\n')
        audit(root);checked=True
    if 'runtime' in paths: paths=[*paths,'native-runtime.json']
    with destination.open('wb') as output:
        process=subprocess.Popen(['pigz','-n','-6','-p','3'],stdin=subprocess.PIPE,stdout=output)
        try:
            with tarfile.open(fileobj=process.stdin,mode='w|',format=tarfile.PAX_FORMAT) as tar:
                def normalize(info):
                    info.uid=info.gid=0;info.uname=info.gname='';info.mtime=epoch;return info
                for path in sorted(paths):tar.add(root/path,arcname=path,recursive=True,filter=normalize)
            process.stdin.close()
            if process.wait()!=0:raise RuntimeError('parallel archive compression failed')
        except BaseException:
            process.kill();process.wait();raise
assemble.archive=archive
assemble.main()
