#!/usr/bin/env python3
"""Exercise the real migration code, using OpenSSL for the mbedTLS interface."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
with tempfile.TemporaryDirectory(prefix="pxa-font-") as workspace:
    work = Path(workspace)
    internal, sd = work / "internal", work / "sd"
    (work / "mbedtls").mkdir()
    (work / "sdkconfig.h").write_text(f'#define CONFIG_PXA_MOUNT_POINT "{internal}"\n')
    (work / "mbedtls/md.h").write_text(r'''
#include <openssl/evp.h>
typedef struct { EVP_MD_CTX *ctx; } mbedtls_md_context_t;
typedef EVP_MD mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 1
static inline void mbedtls_md_init(mbedtls_md_context_t *c) { c->ctx=0; }
static inline const mbedtls_md_info_t *mbedtls_md_info_from_type(int t) { (void)t; return EVP_sha256(); }
static inline int mbedtls_md_setup(mbedtls_md_context_t *c,const EVP_MD *m,int h) { (void)m;(void)h;c->ctx=EVP_MD_CTX_new();return c->ctx?0:-1; }
static inline int mbedtls_md_starts(mbedtls_md_context_t *c) { return EVP_DigestInit_ex(c->ctx,EVP_sha256(),0)==1?0:-1; }
static inline int mbedtls_md_update(mbedtls_md_context_t *c,const unsigned char *p,size_t n) { return EVP_DigestUpdate(c->ctx,p,n)==1?0:-1; }
static inline int mbedtls_md_finish(mbedtls_md_context_t *c,unsigned char *p) { return EVP_DigestFinal_ex(c->ctx,p,0)==1?0:-1; }
static inline void mbedtls_md_free(mbedtls_md_context_t *c) { EVP_MD_CTX_free(c->ctx); }
''')
    (work / "main.c").write_text(r'''
#include "pxa/pxa_esp_system_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int __real_fsync(int);
int __wrap_fsync(int fd) { return getenv("FAIL_SYNC")?-1:__real_fsync(fd); }
int __real_rename(const char *,const char *);
int __wrap_rename(const char *a,const char *b) { return getenv("FAIL_RENAME")?-1:__real_rename(a,b); }
int main(int argc,char **argv) {
    if(argc!=3) return 2;
    int ok=pxa_esp_system_font_prepare(argv[1],atoi(argv[2])!=0);
    printf("%d\n%s\n",ok,pxa_esp_system_font_path()); return 0;
}
''')
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-g", "-I"+str(work),
        "-I"+str(ROOT / "firmware/components/pxa/include"),
        str(ROOT / "firmware/components/pxa/src/services/pxa_esp_system_font.c"),
        str(work / "main.c"), "-Wl,--wrap=fsync", "-Wl,--wrap=rename", "-lcrypto", "-o", str(work / "test")], check=True)
    filename = "noto_sans_cjk_common.ttf"
    original = ROOT / "factory/base/system/fonts"
    destination = sd / "PXA/system/fonts"
    def reset():
        shutil.rmtree(internal, ignore_errors=True)
        shutil.rmtree(sd, ignore_errors=True)
        (internal / "system").mkdir(parents=True)
        shutil.copytree(original, internal / "system/fonts")
        sd.mkdir()
    def run(ok, reclaim=True, **faults):
        result = subprocess.run([str(work / "test"), str(sd), str(int(reclaim))],
            env={**os.environ, **faults}, text=True, capture_output=True, check=True)
        lines = result.stdout.splitlines()
        assert lines[0] == str(int(ok)), result.stdout
        expected = destination if ok else internal / "system/fonts"
        assert lines[1] == str(expected / filename), result.stdout
    reset()
    run(True)
    assert not (internal / "system/fonts" / filename).exists()
    for name in [filename, "NOTICE", "OFL.txt"]:
        assert (destination / name).read_bytes() == (original / name).read_bytes()
    run(True)  # Reboot with only the SD font remaining.
    (destination / filename).write_bytes(b"tampered")
    run(False)  # Damaged removable font never becomes a selected resource.
    reset(); run(True, reclaim=False)
    assert (internal / "system/fonts" / filename).exists()
    for fault in ("FAIL_SYNC", "FAIL_RENAME"):
        reset(); run(False, **{fault: "1"})
        assert (internal / "system/fonts" / filename).read_bytes() == (original / filename).read_bytes()
        assert not (destination / ".font-copy").exists()
    reset(); destination.mkdir(parents=True)
    (destination / filename).write_bytes(b"another font")
    run(False)
    assert (destination / filename).read_bytes() == b"another font"
    assert (internal / "system/fonts" / filename).exists()
    reset(); shutil.rmtree(sd); run(False)
    assert not sd.exists()  # No internal fallback directory masquerading as SD.
    reset(); (internal / "system/fonts" / filename).write_bytes(b"custom font")
    run(False)
    assert (internal / "system/fonts" / filename).read_bytes() == b"custom font"
print("System font migration: copy, hash, reboot, tamper, rollback and no-card tests passed")
