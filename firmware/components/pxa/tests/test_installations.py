#!/usr/bin/env python3
"""Real signed fixtures for the ESP volume-local installation control plane."""
import json
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

sdk = Path(sys.argv[2]).resolve()
sys.path.insert(0, str(sdk / "tools/package"))
from build_package_manifest import main as sign_package

with tempfile.TemporaryDirectory(prefix="signed-installations-") as temporary:
    root = Path(temporary)
    key = root / "publisher.pem"
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key)], check=True)
    subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER", "-out", str(root / "publisher.der")], check=True)
    for version in (1, 2):
        package = root / f"source{version}"
        (package / "artifacts").mkdir(parents=True)
        (package / "artifacts/main.wasm").write_bytes(b"\0asm\x01\0\0\0")
        metadata = root / f"source{version}.json"
        metadata.write_text(json.dumps({
            "format": "pxa-package-source-0.1", "id": "volume-test",
            "version": f"{version}.0.0", "release_sequence": version,
            "components": [{"id": "main", "kind": "ui", "artifact": "wasm", "services": ["fs"]}],
        }))
        sign_package([str(metadata), str(package), str(key), "linux-x86_64", "fixture", "--artifact-mode", "wasm"])
    package = root / "resources"
    package.mkdir()
    (package / "pak0.pak").write_bytes(b"signed media" * 900)
    metadata = root / "resources.json"
    metadata.write_text(json.dumps({"format": "pxa-package-source-0.2", "package_type": "resources",
        "id": "volume-test", "version": "1", "release_sequence": 1}))
    sign_package([str(metadata), str(package), str(key), "resources", "none"])
    digest = hashlib.sha256((package / "manifest.pxm").read_bytes()).hexdigest()
    package = root / "with-resources"
    (package / "artifacts").mkdir(parents=True)
    (package / "artifacts/main.wasm").write_bytes(b"\0asm\x01\0\0\0")
    metadata = root / "with-resources.json"
    metadata.write_text(json.dumps({"format": "pxa-package-source-0.2", "id": "volume-test",
        "version": "3", "release_sequence": 3, "resources": {"manifest_sha256": digest},
        "components": [{"id": "main", "kind": "ui", "artifact": "wasm", "services": ["fs"]}]}))
    sign_package([str(metadata), str(package), str(key), "linux-x86_64", "fixture", "--artifact-mode", "wasm"])
    for sequence in (4, 5):
        package = root / f"manual-update{sequence}"
        (package / "artifacts").mkdir(parents=True)
        (package / "artifacts/main.wasm").write_bytes(b"\0asm\x01\0\0\0")
        metadata = root / f"manual-update{sequence}.json"
        metadata.write_text(json.dumps({"format": "pxa-package-source-0.2", "id": "volume-test",
            "version": str(sequence), "release_sequence": sequence,
            "components": [{"id": "main", "kind": "ui", "artifact": "wasm", "services": ["fs"]}]}))
        sign_package([str(metadata), str(package), str(key), "linux-x86_64", "fixture", "--artifact-mode", "wasm"])
    # A legacy code directory has neither an app-home record nor a resource
    # declaration. Exercise adoption with real signatures and declared grants.
    package = root / "legacy-template"
    (package / "artifacts").mkdir(parents=True)
    (package / "artifacts/main.wasm").write_bytes(b"\0asm\x01\0\0\0")
    metadata = root / "legacy-template.json"
    metadata.write_text(json.dumps({"format": "pxa-package-source-0.1", "id": "volume-test",
        "version": "1.0.0", "release_sequence": 1,
        "permissions": [{"name": "audio.playback", "scope": "media", "required": False},
                        {"name": "network.http", "scope": "", "required": False}],
        "components": [{"id": "main", "kind": "ui", "artifact": "wasm", "services": ["fs"]}]}))
    sign_package([str(metadata), str(package), str(key), "linux-x86_64", "fixture", "--artifact-mode", "wasm"])
    subprocess.run([sys.argv[1], str(root)], check=True, timeout=60)
