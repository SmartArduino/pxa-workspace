from pathlib import Path
import subprocess,json,hashlib
out=Path('local/ra-no-asyncify-size').resolve();raw=out/'untransformed.wasm';wasm=out/'optimized-no-asyncify.wasm'
opt=Path('local/recrof-ports-tools/binaryen-version_130/bin/wasm-opt').resolve()
flags=['-O2','--always-inline-max-function-size=0','--one-caller-inline-max-function-size=0','--flexible-inline-max-function-size=0','-g','--enable-bulk-memory','--enable-sign-ext','--enable-nontrapping-float-to-int','--enable-mutable-globals']
subprocess.run([str(opt),str(raw),*flags,'-o',str(wasm)],check=True)
m=json.loads((out/'inputs.json').read_text());m.update(optimization_flags=flags,optimized_wasm_sha256=hashlib.sha256(wasm.read_bytes()).hexdigest(),optimized_wasm_bytes=wasm.stat().st_size,comparison='Same Binaryen optimization and inlining settings as asyncify_rts.py, with only the Asyncify transform/removelist omitted. Not executable on current Host.')
(out/'optimized-inputs.json').write_text(json.dumps(m,indent=2)+'\n')
subprocess.run([m['wamrc'],*m['compiler_flags'],'-o',str(out/'optimized-no-asyncify.aot'),str(wasm)],check=True)
print('OFFLINE OPTIMIZED NO-ASYNCIFY AOT COMPLETE',flush=True)
