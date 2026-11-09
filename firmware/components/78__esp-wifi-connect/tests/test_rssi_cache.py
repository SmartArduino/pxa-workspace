#!/usr/bin/env python3
"""Check RSSI query bounds without Wi-Fi hardware or a wall-clock wait."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    component = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='pxa-rssi-test-') as directory:
        binary = Path(directory) / 'rssi-cache'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I', str(component / 'include'), str(component / 'tests/wifi_rssi_cache_test.cpp'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('RSSI cache: five-second cadence, reconnect reset and query failure handling passed')


if __name__ == '__main__':
    main()
