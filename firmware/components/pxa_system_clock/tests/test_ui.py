#!/usr/bin/env python3
"""Test the production clock and reference UI using a built simulator's libraries."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def main():
    directory = Path(__file__).resolve().parent
    root = directory.parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", default="pai-touch")
    args = parser.parse_args()
    build = root / "build/simulator" / args.board
    cmake = build / "CMakeFiles/pxsys_desktop_simulator.dir"
    if not (cmake / "link.txt").is_file():
        parser.error(f"Build the {args.board} simulator first")
    flags = (cmake / "flags.make").read_text()
    arguments = []
    for variable in ("C_DEFINES", "C_INCLUDES", "C_FLAGS"):
        arguments += shlex.split(re.search("^" + variable + r" = (.*)$", flags, re.M)[1])
    arguments = [arg for arg in arguments if arg != "-DNDEBUG"]
    arguments += ["-D_DEFAULT_SOURCE", "-I" + str(directory / "stubs"),
                  "-I" + str(directory.parent / "include")]
    sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="pxa-clock-ui-") as temporary:
        test = Path(temporary)
        subprocess.run([os.environ.get("CC", "clang"), *arguments, *sanitizers,
                        "-c", str(directory / "ui_clock_test.c"),
                        "-o", str(test / "test.o")], check=True)
        link = shlex.split((cmake / "link.txt").read_text())
        link[0] = os.environ.get("CC", "clang")
        link = [arg for arg in link if not arg.startswith("-Wl,--dependency-file=")]
        link = [str(test / "test.o") if arg == "CMakeFiles/pxsys_desktop_simulator.dir/main.c.o"
                else arg for arg in link]
        link[link.index("-o") + 1] = str(test / "test")
        subprocess.run([*link, *sanitizers], cwd=build, check=True)
        subprocess.run([str(test / "test")], check=True,
                       env={**os.environ, "SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})


if __name__ == "__main__":
    main()
