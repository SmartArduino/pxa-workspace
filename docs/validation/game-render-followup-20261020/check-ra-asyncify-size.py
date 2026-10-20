from pathlib import Path
import subprocess,shlex,json,hashlib
root=Path('local/app-build-cache/ra-size-wasm-ebaef03e270d0aca/cmake-build').resolve()
out=Path('local/ra-no-asyncify-size').resolve();out.mkdir(exist_ok=True)
cs=subprocess.check_output(['ninja','-t','commands','pxa_main'],cwd=root,text=True).splitlines()
parts=[];part=[]
for a in shlex.split(cs[-1]):
 if a=='&&':parts.append(part);part=[]
 else:part.append(a)
parts.append(part)
link=next(p for p in parts if p and '/bin/clang' in p[0] and '-o'in p)
wasm=out/'untransformed.wasm';link[link.index('-o')+1]=str(wasm)
subprocess.run(link,cwd=root,check=True)
wamrc=Path(subprocess.check_output(['bash','deps/pxa-system/tools/package/build_wamrc.sh'],text=True).strip()).resolve()
flags=['--target=riscv32','--target-abi=ilp32f','--cpu=generic-rv32','--cpu-features=+m,+a,+f,+c,+zba,+zbb,+zbs','--opt-level=3','--size-level=3']
meta={'raw_wasm':str(wasm),'raw_wasm_bytes':wasm.stat().st_size,'raw_wasm_sha256':hashlib.sha256(wasm.read_bytes()).hexdigest(),'wamrc':str(wamrc),'wamrc_sha256':hashlib.sha256(wamrc.read_bytes()).hexdigest(),'compiler_flags':flags,'scope':'Offline code-size diagnostic only. Raw coroutine imports are unresolved by current firmware; this is not an executable PXA package, a functional port, or FPS evidence.'}
(out/'inputs.json').write_text(json.dumps(meta,indent=2)+'\n')
subprocess.run([str(wamrc),*flags,'-o',str(out/'untransformed.aot'),str(wasm)],check=True)
print('OFFLINE RAW AOT COMPLETE',flush=True)
