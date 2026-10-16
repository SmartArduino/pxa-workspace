# PXA on Windows, macOS and other Linux hosts

[简体中文](README.md)

The native DevKit 0.4.0 binary release supports Linux x86_64 with glibc 2.39+. This companion uses the same DevKit inside a Linux amd64 container for builds/signing/headless simulation, and runs the Python PXADB client on the host for USB serial access. USB passthrough is unnecessary.

Load the verified container archive with `docker load --input pxa-devkit-container-0.4.0-linux-amd64.tar.gz`. The exported image is named `localhost/pxa-devkit:0.4.0-linux-amd64`; add the wrapper's default name before running it. This adds a name without copying image layers. With Python 3.11+:

```sh
docker tag localhost/pxa-devkit:0.4.0-linux-amd64 pxa-devkit:0.4.0-linux-amd64
python pxa-host-tools-0.4.0/pxa_container.py --workspace . -- init hello
python pxa-host-tools-0.4.0/pxa_container.py --workspace . -- build hello --offline --target esp32s3 --output hello-output
python pxa-host-tools-0.4.0/pxa_container.py --workspace . -- run hello --offline --duration-ms 1500 --screenshot hello.png
```

Install `requirements.txt` in a host virtual environment, then run `python pxadb.py devices`. Use `COM7` on Windows, `/dev/cu.usbmodem...` on macOS or `/dev/ttyACM...` on Linux. Run `package install`, `package run`, `screenshot`, `input` and `logcat` on that host, using the actual package path and App ID printed by the build. Serial-bridge baud rates must match the firmware.

The wrapper preserves signing identities under `.pxa-container/data`, caches under `.pxa-container/cache`, and normal project `.pxa` state. It forwards commands without a shell, supports paths with spaces, runs with the host UID/GID on Linux, and rejects `--device` with host-debugging guidance. Container paths in commands are relative to `--workspace`; keep shared app sources in that parent directory. Apple Silicon and ARM Windows use amd64 emulation and build more slowly. Simulation is headless; open the saved screenshots on the host.

For WSL 2, use Ubuntu 24.04 and the existing Linux DevKit. Native Windows PXADB remains the simplest serial route. Direct WSL USB access uses [Microsoft's usbipd-win instructions](https://learn.microsoft.com/en-us/windows/wsl/connect-usb); [Docker USB/IP](https://docs.docker.com/desktop/features/usbip/) requires additional setup.

[WASI SDK 34](https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-34) provides native host compilers on more platforms, but not PXA's pinned Espressif LLVM/WAMR AOT compiler or Host simulator. A complete native port must preserve `config/wamr.json` commits and CPU backends, port POSIX-specific tooling, and validate real Guests, signatures and input. The verified Linux maintainer build is `tools/release.py`; Windows/macOS native DevKits are not claimed by this companion. See the Chinese guide for complete commands and limitations.

Verify before loading the image or running tools, using a separately trusted DevKit release key (or one from verified system source):

```sh
python pxa-host-tools-0.4.0/verify_manifest.py host-tools-0.4.0.json --public-key /path/to/trusted/release-public.pem
```

The script uses native OpenSSL to verify the manifest signature, then streams both archive size/hash checks. Install OpenSSL or supply `--openssl /path/to/openssl`; Windows users may verify from WSL. The bundled key copy is not an independent first-download trust anchor.


For Podman, replace `docker load/tag` with `podman load/tag` and add `--engine podman` to the wrapper.
