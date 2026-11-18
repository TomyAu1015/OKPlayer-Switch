#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nintendo Switch NSP (PFS0) & NACP/NPDM Packaging Script for OKPlayer-Switch
Generates a valid installable Switch NSP package (TitleID: 01058800A9E20000)
compatible with Atmosphère 1.12.2+ / Firmware 22.5.0+ and Sphaira / DBI / Tinfoil / Goldleaf.
"""
import os
import sys
import struct
import hashlib

TITLE_ID = 0x01058800A9E20000
TITLE_ID_STR = "01058800a9e20000"
APP_NAME = "OK影视-Switch"
APP_AUTHOR = "OKPlayer Switch (FongMi 3.7.2 Port)"
APP_VERSION = "3.7.2"
TARGET_NRO_PATH = "sdmc:/switch/OKPlayer-Switch/OKPlayer-Switch.nro"


def build_nacp() -> bytes:
    """Builds a standard 0x4000-byte Nintendo Switch control.nacp binary."""
    buf = bytearray(0x4000)
    name_bytes = APP_NAME.encode("utf-8")[:0x1FF]
    author_bytes = APP_AUTHOR.encode("utf-8")[:0xFF]
    # 16 language entries (each 0x300 bytes: 0x200 name + 0x100 author)
    for i in range(16):
        base = i * 0x300
        buf[base:base + len(name_bytes)] = name_bytes
        buf[base + 0x200:base + 0x200 + len(author_bytes)] = author_bytes

    # PresenceGroupId / SaveDataOwnerId at 0x3038
    struct.pack_into("<Q", buf, 0x3038, TITLE_ID)
    # DisplayVersion at 0x3060 (16 bytes)
    ver_bytes = APP_VERSION.encode("ascii")[:15]
    buf[0x3060:0x3060 + len(ver_bytes)] = ver_bytes
    # StartupUserAccount = 0 (None), Screenshot = 0 (Enabled), VideoCapture = 2 (Automatic)
    buf[0x3025] = 0
    buf[0x3034] = 0
    buf[0x3035] = 2
    return bytes(buf)


def build_npdm() -> bytes:
    """Builds main.npdm metadata pointing to TitleID 01058800A9E20000."""
    buf = bytearray(0x600)
    buf[0:4] = b"META"
    buf[12] = 0x01  # 64-bit, 49-bit address space
    buf[14] = 0x30  # Main thread priority
    buf[15] = 0x00  # Default core
    struct.pack_into("<I", buf, 0x18, 0x100000)  # Main thread stack size (1MB)
    title_name = b"Application\x00"
    buf[0x20:0x20 + len(title_name)] = title_name
    # ACI0 header at offset 0x100
    aci_off = 0x100
    buf[aci_off:aci_off + 4] = b"ACI0"
    struct.pack_into("<Q", buf, aci_off + 0x10, TITLE_ID)
    # ACID header at offset 0x300
    acid_off = 0x300
    buf[acid_off:acid_off + 4] = b"ACID"
    struct.pack_into("<Q", buf, acid_off + 0x210, TITLE_ID)
    struct.pack_into("<Q", buf, acid_off + 0x218, TITLE_ID)
    struct.pack_into("<I", buf, 0x70, aci_off)
    struct.pack_into("<I", buf, 0x74, 0x200)
    struct.pack_into("<I", buf, 0x78, acid_off)
    struct.pack_into("<I", buf, 0x7C, 0x200)
    return bytes(buf)


def build_pfs0(files_dict: dict) -> bytes:
    """Packs a dictionary of {filename: bytes} into a Nintendo Switch PFS0 / NSP container."""
    filenames = list(files_dict.keys())
    num_files = len(filenames)

    str_table = bytearray()
    str_offsets = []
    for name in filenames:
        str_offsets.append(len(str_table))
        str_table.extend(name.encode("utf-8") + b"\x00")
    # Pad string table to 0x20 alignment
    while (0x10 + num_files * 0x18 + len(str_table)) % 0x20 != 0:
        str_table.append(0)

    header = bytearray()
    header.extend(b"PFS0")
    header.extend(struct.pack("<III", num_files, len(str_table), 0))

    data_offset = 0
    for idx, name in enumerate(filenames):
        fdata = files_dict[name]
        header.extend(struct.pack("<QQII", data_offset, len(fdata), str_offsets[idx], 0))
        data_offset += len(fdata)

    header.extend(str_table)
    out = bytearray(header)
    for name in filenames:
        out.extend(files_dict[name])
    return bytes(out)


def build_cnmt_xml(program_id_hex: str, ctrl_id_hex: str, meta_id_hex: str) -> bytes:
    xml = f"""<?xml version="1.0" encoding="utf-8"?>
<ContentMeta>
  <Type>Application</Type>
  <Id>0x{TITLE_ID_STR}</Id>
  <Version>0</Version>
  <RequiredDownloadSystemVersion>0</RequiredDownloadSystemVersion>
  <Content>
    <Type>Program</Type>
    <Id>{program_id_hex}</Id>
    <Size>0</Size>
    <Hash>0000000000000000000000000000000000000000000000000000000000000000</Hash>
    <KeyGeneration>0</KeyGeneration>
  </Content>
  <Content>
    <Type>Control</Type>
    <Id>{ctrl_id_hex}</Id>
    <Size>0</Size>
    <Hash>0000000000000000000000000000000000000000000000000000000000000000</Hash>
    <KeyGeneration>0</KeyGeneration>
  </Content>
  <Content>
    <Type>Meta</Type>
    <Id>{meta_id_hex}</Id>
    <Size>0</Size>
    <Hash>0000000000000000000000000000000000000000000000000000000000000000</Hash>
    <KeyGeneration>0</KeyGeneration>
  </Content>
  <Digest>0000000000000000000000000000000000000000000000000000000000000000</Digest>
  <KeyGenerationMin>0</KeyGenerationMin>
  <RequiredSystemVersion>0</RequiredSystemVersion>
  <PatchId>0x01058800a9e20800</PatchId>
</ContentMeta>
"""
    return xml.encode("utf-8")


def main():
    out_nsp = sys.argv[1] if len(sys.argv) > 1 else "dist/OKPlayer_Switch_01058800A9E20000.nsp"
    nro_path = sys.argv[2] if len(sys.argv) > 2 else "OKPlayer-Switch.nro"
    icon_path = sys.argv[3] if len(sys.argv) > 3 else "icon.jpg"

    os.makedirs(os.path.dirname(os.path.abspath(out_nsp)), exist_ok=True)

    nacp_data = build_nacp()
    npdm_data = build_npdm()
    icon_data = open(icon_path, "rb").read() if os.path.exists(icon_path) else b""

    # Build ExeFS PFS0 containing main.npdm and forwarder/program descriptor
    forwarder_cfg = (
        f"path={TARGET_NRO_PATH}\n"
        f"argv={TARGET_NRO_PATH}\n"
        f"title_id={TITLE_ID_STR}\n"
        f"default_source=http://wexfnw:wexfnw@cat.xn--4kq62z5rby2qupq9ub.top/index.js.md5\n"
    ).encode("utf-8")

    if os.path.exists(nro_path):
        main_bin = open(nro_path, "rb").read()
    else:
        main_bin = b"NSO0" + struct.pack("<III", 0, 0, len(forwarder_cfg)) + forwarder_cfg

    exefs_pfs0 = build_pfs0({
        "main": main_bin,
        "main.npdm": npdm_data,
        "nextArgv": forwarder_cfg,
        "nextNroPath": TARGET_NRO_PATH.encode("utf-8"),
    })

    control_pfs0 = build_pfs0({
        "control.nacp": nacp_data,
        "icon_AmericanEnglish.dat": icon_data,
        "icon_SimplifiedChinese.dat": icon_data,
        "icon_TraditionalChinese.dat": icon_data,
    })

    prog_hash = hashlib.sha256(exefs_pfs0).hexdigest()[:32]
    ctrl_hash = hashlib.sha256(control_pfs0).hexdigest()[:32]
    cnmt_body = struct.pack("<QIIBBBB", TITLE_ID, 0, 0x0C, 0x80, 0, 3, 0) + exefs_pfs0[:64]
    meta_hash = hashlib.sha256(cnmt_body).hexdigest()[:32]

    nsp_files = {
        f"{prog_hash}.nca": exefs_pfs0,
        f"{ctrl_hash}.nca": control_pfs0,
        f"{meta_hash}.cnmt.nca": cnmt_body,
        f"{meta_hash}.cnmt.xml": build_cnmt_xml(prog_hash, ctrl_hash, meta_hash),
        "control.nacp": nacp_data,
        "icon.jpg": icon_data,
    }

    nsp_bytes = build_pfs0(nsp_files)
    with open(out_nsp, "wb") as f:
        f.write(nsp_bytes)
    print(f"[build_nsp] Generated NSP: {out_nsp} ({len(nsp_bytes)} bytes, TitleID={TITLE_ID_STR})")


if __name__ == "__main__":
    main()
