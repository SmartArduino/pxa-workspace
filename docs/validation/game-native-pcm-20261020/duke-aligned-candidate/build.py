import hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path.cwd();sdk=root/'deps/pxa-system';base=root/'build/game-native-pcm-20261020';out=base/'aligned-duke';out.mkdir(exist_ok=True)
sys.path.insert(0,str(sdk/'tools'))
from pxa_tools.project import aot_flags
profile=json.loads((root/'tools/game-perf/profiles/duke-native-pcm/package.json').read_text())
profile.update(version='0.1.33',release_sequence=34)
source=out/'package.json';source.write_text(json.dumps(profile,indent=2)+'\n')
package=out/'pxa-duke3d-sd';(package/'artifacts').mkdir(parents=True,exist_ok=True)
wasm=base/'wasm-cache/duke-native-pcm-wasm-a76ad430c10fe21a/artifacts/main.wasm'
compiler=base/'wamrc-known-alignment/wamrc'
flags=['--target=riscv32',*aot_flags(profile,'esp32s31')]
recipe={'wasm_sha256':hashlib.sha256(wasm.read_bytes()).hexdigest(),'compiler_sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'patch_sha256':hashlib.sha256((root/'tools/game-perf/wamr-known-alignment.patch').read_bytes()).hexdigest(),'wamr_base':'6a54186abc1c10c37c28e443107107c97af8f25f','flags':flags,'same_wasm_as_baseline':True,'baseline_aot_sha256':json.loads((base/'artifacts.json').read_text())['profiles']['duke']['aot']['sha256']}
(out/'recipe.json').write_text(json.dumps(recipe,indent=2)+'\n')
for script,args in [('verify_wasm_memory.py',[str(wasm),'--maximum-bytes','3670016']),('verify_wasm_core_imports.py',[str(wasm),'--core-major','2','--core-minor','1','--manifest-source',str(source),'--component-id','main'])]:
 subprocess.run([sys.executable,str(sdk/'tools/package'/script),*args],check=True)
aot=package/'artifacts/main.esp32-s31.aot'
subprocess.run([str(compiler),*flags,'-o',str(aot),str(wasm)],check=True)
key=sdk/'apps/pxa/.dev-signing/publisher-private.pem'
engine_abi=subprocess.check_output([sys.executable,str(sdk/'tools/wamr/metadata.py'),'engine_abi'],text=True).strip()
subprocess.run([sys.executable,str(sdk/'tools/package/build_package_manifest.py'),str(source),str(package),str(key),'esp32-s31',engine_abi,'--aot-only'],check=True)
container=out/'pxa-duke3d-sd.pxa'
subprocess.run([sys.executable,str(sdk/'tools/package/build_pxa_container.py'),str(package),str(key),str(container),'--codec','lz4'],check=True)
recipe['output']={p.name:{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in [aot,container]}
(out/'recipe.json').write_text(json.dumps(recipe,indent=2)+'\n')
print(json.dumps(recipe),flush=True)
