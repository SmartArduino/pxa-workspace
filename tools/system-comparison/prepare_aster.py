#!/usr/bin/env python3
"""Prepare a pinned, isolated Aster IDF port; never modify the PXA firmware."""
from pathlib import Path
import argparse
import re
import shutil
import struct
import subprocess

WORKSPACE = Path(__file__).resolve().parents[2]
REVISION = "24adaff82c437c0530f55fdb7ba2b490a7348e8c"


def replace(root, name, old, new):
    path = root / name
    text = path.read_text()
    if old not in text:
        if not new or new in text:
            return
        raise RuntimeError(f"Pinned input changed: {name}: {old!r}")
    if new and old in new and new in text:
        return
    path.write_text(text.replace(old, new))


def subset_font(source, destination, codepoints):
    raw = source.read_bytes()
    if raw[:8] != b"CPFONT\0\0" or struct.unpack_from("<HH", raw, 8) != (4, 0):
        raise ValueError(source)
    count, glyphs = struct.unpack_from("<II", raw, 36)
    intervals = [struct.unpack_from("<III", raw, 64 + i * 12) for i in range(count)]
    record_base = 64 + count * 12
    bitmap_base = record_base + glyphs * 16
    ivs, records, bits = [], bytearray(), bytearray()
    for cp in sorted(codepoints):
        index = next((base + cp - first for first, last, base in intervals if first <= cp <= last), None)
        if index is None:
            continue
        record = bytearray(raw[record_base + index * 16:record_base + (index + 1) * 16])
        length = struct.unpack_from("<H", record, 8)[0]
        offset = struct.unpack_from("<I", record, 12)[0]
        struct.pack_into("<I", record, 12, len(bits))
        ivs.append((cp, cp, len(records) // 16))
        records += record
        bits += raw[bitmap_base + offset:bitmap_base + offset + length]
    header = bytearray(raw[:64])
    struct.pack_into("<II", header, 36, len(ivs), len(ivs))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(header + b"".join(struct.pack("<III", *v) for v in ivs) + records + bits)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aster", type=Path, default=WORKSPACE / "local/aster-firmware-pai-touch")
    args = parser.parse_args()
    root = args.aster.resolve()
    if subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip() != REVISION:
        raise RuntimeError("This overlay requires the reviewed Aster revision")
    source = Path(__file__).parent / "aster/pai-touch"
    destination = root / "boards/pai_touch/idf"
    shutil.copytree(source, destination, dirs_exist_ok=True)
    # Keep the actual supplier command table, without a PXA runtime dependency.
    hardware = (WORKSPACE / "firmware/boards/pai-touch/src/pai_touch_hardware.cc").read_text()
    table = re.search(r"constexpr LcdCommand kLcdCommands\[\] = \{(.*?)\n\};", hardware, re.S).group(1)
    (destination / "main/lcd_commands.inc").write_text(table + "\n")
    replace(root, "systems/espidf/components/aster_render/CMakeLists.txt", '"ppa_fill.c"', f'"{destination}/main/ppa_stub.c"')
    replace(root, "systems/espidf/components/aster_render/CMakeLists.txt", "log esp_driver_ppa fatfs", "log fatfs")
    replace(root, "systems/espidf/components/aster_connectivity/CMakeLists.txt", '"clock.c" "sdhost.c"', '"clock.c"')
    replace(root, "systems/espidf/components/aster_connectivity/CMakeLists.txt", "esp_wifi_remote esp_hosted", "")
    path = root / "systems/espidf/components/aster_connectivity/CMakeLists.txt"
    path.write_text(path.read_text().split("# Apply the board")[0].rstrip()+"\n")
    replace(root, "systems/espidf/components/aster_connectivity/wifi.c", "esp_console_dev_uart_config_t uart=ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();", "esp_console_dev_usb_serial_jtag_config_t uart=ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();")
    replace(root, "systems/espidf/components/aster_connectivity/wifi.c", "esp_console_new_repl_uart(&uart,&config,&repl)", "esp_console_new_repl_usb_serial_jtag(&uart,&config,&repl)")
    replace(root, "systems/espidf/components/aster_transfer/serial.c", '#include "driver/uart.h"', '#include "driver/uart.h"\n#include "driver/usb_serial_jtag.h"')
    replace(root, "systems/espidf/components/aster_transfer/serial.c", "uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM,data,size,pdMS_TO_TICKS(30000))", "usb_serial_jtag_read_bytes(data,size,pdMS_TO_TICKS(30000))")
    replace(root, "systems/espidf/components/aster_transfer/serial.c", "if(result)uart_flush_input(CONFIG_ESP_CONSOLE_UART_NUM);", "if(result){uint8_t drain[64];while(usb_serial_jtag_read_bytes(drain,sizeof(drain),0)>0){}}")
    replace(root, "systems/espidf/components/aster_transfer/serial.c", "target=riscv", "target=xtensa")
    replace(root, "systems/espidf/components/aster_transfer/CMakeLists.txt", "console esp_driver_uart)", "console esp_driver_uart esp_driver_usb_serial_jtag)")
    replace(root, "systems/espidf/components/aster_ui/CMakeLists.txt", "CONFIG_ASTER_FIRST_BOOT=1", "CONFIG_ASTER_FIRST_BOOT=0")
    replace(root, "systems/espidf/components/aster_ui/CMakeLists.txt", "CONFIG_ASTER_DISPLAY_WIDTH=480 CONFIG_ASTER_DISPLAY_HEIGHT=480", "CONFIG_ASTER_DISPLAY_WIDTH=296 CONFIG_ASTER_DISPLAY_HEIGHT=240")
    replace(root, "systems/espidf/components/aster_ui/CMakeLists.txt", "osptek-p4-st7102", "pai-touch")
    replace(root, "systems/espidf/components/aster_ui/platform.c", ".gpio_num=CONFIG_ASTER_P4_BACKLIGHT_GPIO", ".gpio_num=41")
    replace(root, "systems/espidf/components/aster_ui/platform.c", "ASTER_ORIENT_PORTRAIT", "ASTER_ORIENT_LANDSCAPE")
    replace(root, "systems/espidf/components/aster_runtime/graphics.c", ".orientation=ASTER_ORIENT_PORTRAIT", ".orientation=ASTER_ORIENT_LANDSCAPE")
    replace(root, "systems/espidf/components/aster_ui/platform.c", "480*480*2", "296*240*2")
    replace(root, "systems/espidf/components/aster_ui/platform.c", "*data=copy;*w=*h=*pitch=480;", "*data=copy;*w=*pitch=296;*h=240;")
    replace(root, "systems/zephyr/ui/launcher/flux.c", "if(aster_setup_ui_tick())return;", "if(IS_ENABLED(CONFIG_ASTER_FIRST_BOOT) && aster_setup_ui_tick())return;")
    replace(root, "systems/espidf/components/aster_runtime/CMakeLists.txt", 'CONFIG_ASTER_TARGET_ID=\\\"riscv\\\"', 'CONFIG_ASTER_TARGET_ID=\\\"xtensa\\\"')
    replace(root, "systems/espidf/components/aster_runtime/CMakeLists.txt", "CONFIG_ASTER_AOT_TARGET_ID=2", "CONFIG_ASTER_AOT_TARGET_ID=1")
    # S3 follows Aster's existing non-interruptible Xtensa AOT contract.
    replace(root, "systems/espidf/components/aster_runtime/runtime.c", "bool profile_ok=!aot_image || aster_idf_aot_interruptible(wasm_bytes,wasm_size);", "bool profile_ok=true; /* Pai Touch: standard Xtensa AOT, matching Aster S3. */")
    replace(root, "systems/espidf/components/aster_runtime/CMakeLists.txt", "if(CONFIG_IDF_TARGET_ESP32P4)", "if(CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32S3)")
    replace(root, "systems/espidf/components/aster_apps/CMakeLists.txt", "if(CONFIG_IDF_TARGET_ESP32P4)", "if(CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32S3)")
    # Pai Touch has no SD slot. Keep Aster's VFS semantics on a dedicated FAT partition.
    chars = set(range(32, 127))
    for name in ["common/setup/ui-strings.txt", "systems/zephyr/ui/launcher/flux.c", "systems/zephyr/ui/qpanel.c", "systems/zephyr/ui/wifi_settings.c"]:
        chars.update(ord(c) for c in (root / name).read_text() if ord(c) > 127)
    assets = destination / "seed/aster/fonts"
    for font in (root / "resources/fonts").glob("ui_*.cpfont"):
        subset_font(font, assets / font.name, chars)
    for notice in (root/"resources/fonts").iterdir():
        if "LICENSE" in notice.name or notice.name=="NOTICE":
            shutil.copy(notice,assets/notice.name)
    print(f"Aster Pai Touch prepared: {root}; font subset {sum(p.stat().st_size for p in assets.iterdir())} bytes")


if __name__ == "__main__":
    main()
