#!/usr/bin/env python3
"""Compile the same kernel with WASI SDK 34 and the same Xtensa AOT recipe."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import struct
import subprocess

ROOT=Path(__file__).resolve().parent
WORKSPACE=ROOT.parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aster",type=Path,default=WORKSPACE/"local/aster-firmware-pai-touch")
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--wasi-sdk",type=Path,default=Path.home()/".local/share/pxa/releases/0.11.0/toolchains/wasi-sdk")
    parser.add_argument("--wamrc",type=Path,default=Path.home()/".local/share/pxa/releases/0.11.0/bin/wamrc")
    parser.add_argument("--enable-builtins",action="store_true",help="Match PXA's optional BUILTINS build profile")
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    wasm=args.output/"benchmark.wasm";aot=args.output/"benchmark.aot"
    clang=args.wasi_sdk/"bin/clang"
    common_flags=["--target=wasm32-wasip1","-O3","-DNDEBUG","-ffunction-sections","-fdata-sections","-ffp-contract=off"]
    if not args.enable_builtins:common_flags.append("-fno-builtin")
    kernel=args.output/"bench_kernel.c.obj"
    kernel_command=[str(clang),*common_flags,"-I"+str(ROOT/"common"),"-c",str(ROOT/"common/bench_kernel.c"),"-o",str(kernel)]
    subprocess.run(kernel_command,check=True)
    exports=["aster_app_init","aster_app_start","aster_app_stop","aster_app_pause","aster_app_resume","aster_app_event","aster_app_destroy","__heap_base","__data_end","__stack_pointer"]
    compile_command=[str(clang),*common_flags,"-nostdlib",
        "-I"+str(args.aster/"contract/include"),"-I"+str(ROOT/"common"),
        str(ROOT/"aster/bench.c"),str(ROOT/"aster/report.c"),str(kernel),
        str(args.wasi_sdk/"share/wasi-sysroot/lib/wasm32-wasip1/libc.a"),
        "-Wl,--no-entry","-Wl,--initial-memory=1048576","-Wl,--max-memory=2097152","-Wl,-z,stack-size=32768",
        *["-Wl,--export="+e for e in exports],"-o",str(wasm)]
    subprocess.run(compile_command,check=True)
    aot_command=[str(args.wamrc),"--target=xtensa","--cpu=esp32s3","--target-abi=gnu",
        "--stack-bounds-checks=0","--opt-level=3","--size-level=0","--mllvm=-mtext-section-literals",
        "-o",str(aot),str(wasm)]
    subprocess.run(aot_command,check=True)
    manifest={"product":"aster","target_id":"xtensa","id":"bench.comparison","name":"System comparison","version":"1.0.0",
        "abi":{"min":"4.3","target":"4.3.0"},"memory":{"initial":1048576,"max":2097152,"stack":32768,"heap":1048576},
        "requires":["display"],"permissions":[],"category":"tool"}
    blobs=[json.dumps(manifest,separators=(",",":")).encode(),wasm.read_bytes(),aot.read_bytes()]
    data=bytearray(64);data[:4]=b"ASTP";struct.pack_into("<HHI",data,4,1,64,0)
    for index,blob in enumerate(blobs):
        while len(data)%4:data.append(0)
        struct.pack_into("<II",data,12+index*8,len(data),len(blob));data+=blob
    struct.pack_into("<I",data,36,1)
    package=args.output/"benchmark.ast";package.write_bytes(data)
    target=args.aster/"boards/pai_touch/idf/seed/aster/install/benchmark.ast"
    target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
    provenance={"kernel_compile":kernel_command,"compile":compile_command,"aot":aot_command,"kernel_sha256":hashlib.sha256((ROOT/"common/bench_kernel.c").read_bytes()).hexdigest(),"kernel_object_sha256":hashlib.sha256(kernel.read_bytes()).hexdigest(),"package_sha256":hashlib.sha256(data).hexdigest()}
    (args.output/"provenance.json").write_text(json.dumps(provenance,indent=2)+"\n")
    print(f"Aster benchmark: {package} ({len(data)} bytes), seeded into firmware FAT image")

if __name__=="__main__":main()
