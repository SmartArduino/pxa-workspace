import fs from 'node:fs';
import path from 'node:path';
import {WASI} from 'node:wasi';
const [file,output]=process.argv.slice(2);
const wasi=new WASI({version:'preview1',returnOnExit:true,args:[file,'/data/level2.phd','/output/'+output],env:{},preopens:{'/data':path.resolve('local/pxa-apps/openlara/assets-simulator/game'),'/output':path.resolve('local/openlara-full')}});
const module=await WebAssembly.compile(fs.readFileSync(file));
const instance=await WebAssembly.instantiate(module,{wasi_snapshot_preview1:wasi.wasiImport});
const status=wasi.start(instance);if(status)throw Error('probe exit '+status);
