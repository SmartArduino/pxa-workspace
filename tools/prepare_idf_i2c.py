#!/usr/bin/env python3
"""Apply the reviewed ESP-IDF I2C fix to a build-local IDF source copy."""
import argparse
import hashlib
from pathlib import Path

UPSTREAM_SHA256 = "a235f01f9bb2dd665624fb0fde57998a1c7446db1aaa32a75c487bba82723e49"


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError("ESP-IDF I2C source changed; review the receive/probe fix")
    return text.replace(before, after, 1)


def patch_source(text):
    text = replace_once(text,
        "    i2c_master->rx_cnt = 0;\n    i2c_master->read_len_static = 0;",
        "    i2c_master->rx_cnt = 0;\n"
        "    i2c_master->contains_read = false;\n"
        "    i2c_master->read_len_static = 0;")
    start = text.index("esp_err_t i2c_master_probe(")
    end = text.index("\nesp_err_t ", start + 1)
    probe = text[start:end]
    probe = replace_once(probe,
        "    bus_handle->trans_done = false;",
        "    bus_handle->trans_done = false;\n"
        "    // A failed read must not make this write-only probe drain RX.\n"
        "    bus_handle->contains_read = false;\n"
        "    bus_handle->read_len_static = 0;\n"
        "    bus_handle->rx_cnt = 0;\n"
        "    bus_handle->read_buf_pos = 0;")
    probe = replace_once(probe,
        "    bus_handle->i2c_trans = (i2c_transaction_t) {",
        "    // Interrupt-visible commands outlive this function's stack.\n"
        "    memcpy(bus_handle->i2c_ops, i2c_ops, sizeof(i2c_ops));\n"
        "    bus_handle->i2c_trans = (i2c_transaction_t) {")
    probe = replace_once(probe, "        .ops = i2c_ops,",
                         "        .ops = bus_handle->i2c_ops,")
    probe = replace_once(probe,
        "    i2c_ll_enable_intr_mask(hal->dev, I2C_LL_MASTER_EVENT_INTR);",
        "    i2c_ll_clear_intr_mask(hal->dev, I2C_LL_MASTER_EVENT_INTR);\n"
        "    i2c_ll_enable_intr_mask(hal->dev, I2C_LL_MASTER_EVENT_INTR);")
    return text[:start] + probe + text[end:]


def prepare(source, output):
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != UPSTREAM_SHA256:
        raise ValueError("Unreviewed ESP-IDF i2c_master.c; review the ESP-IDF I2C fix before upgrading IDF")
    patched = patch_source(data.decode()).encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != patched:
        output.write_bytes(patched)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output)
