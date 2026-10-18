import fs from 'node:fs';
import {WASI} from 'node:wasi';
const [file='reference.wasm',width='320',height='240',stem='upstream-cpp-idle']=process.argv.slice(2);const W=Number(width),H=Number(height);
const w=new WASI({version:'preview1',args:[],env:{},preopens:{},returnOnExit:true});
const i=(await WebAssembly.instantiate(fs.readFileSync('local/openlara-original-compare/'+file),{wasi_snapshot_preview1:w.wasiImport})).instance;w.initialize(i);
const e=i.exports;new Uint8Array(e.memory.buffer,e.ref_data(),2356992).set(fs.readFileSync('local/upstream-openlara/src/platform/gba/data/LEVEL1.PKD'));
e.ref_start(2);for(let j=0;j<180;j++)e.ref_tick(0);
const pixels=Buffer.from(e.memory.buffer,e.ref_pixels(),W*H),pal=new Uint16Array(e.memory.buffer,e.ref_palette(),256),rgb=Buffer.alloc(W*H*3);
for(let j=0;j<W*H;++j){let c=pal[pixels[j]];rgb[j*3]=(c&31)*255/31;rgb[j*3+1]=((c>>5)&31)*255/31;rgb[j*3+2]=((c>>10)&31)*255/31;}
fs.writeFileSync('local/openlara-original-compare/'+stem+'.ppm',Buffer.concat([Buffer.from(`P6\n${W} ${H}\n255\n`),rgb]));
console.log([...new Int32Array(e.memory.buffer,e.ref_state(),8)]);
fs.writeFileSync('local/openlara-original-compare/'+stem+'-indices.bin',pixels);
fs.writeFileSync('local/openlara-original-compare/upstream-palette.bin',Buffer.from(pal.buffer,pal.byteOffset,512));
