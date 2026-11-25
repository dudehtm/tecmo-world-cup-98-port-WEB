"""Prepare and verify G3 Subtask 1 synchronized baseline assets inside G3-01/data/."""
from __future__ import annotations

import hashlib
import json
import struct
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
G3_DIR = Path(__file__).resolve().parents[1]
DATA_DIR = G3_DIR / "data"

ROM_ZIP = ROOT / "Rom/twcup98.zip"
MAME_XML = ROOT / ".local/experiments/toolchain-check/twcup98-mame0289.xml"
MEM_MAP_JSON = ROOT / ".local/experiments/003-memory-map/prepared/memory-map.json"
BANK_DIR = ROOT / ".local/experiments/003-memory-map/bank-selected"
VIS_NEUTRAL_DIR = ROOT / ".local/experiments/012-h4-control-trace/visual-sync1027b-neutral"
VIS_UP_DIR = ROOT / ".local/experiments/012-h4-control-trace/visual-sync1027b-up"
LEFT01_DIR = ROOT / ".local/experiments/012-h4-control-trace/left01"
CTRL07_DIR = ROOT / ".local/experiments/011-h4-kickoff/controlled07"

K_HIGH_SIZE = 1048576
K_LOW_SIZE = 1048576
K_HEADER_SIZE = 616
K_CPN8_MAGIC = 0x384E5043
K_CPF3_FORMAT = 0x33465043


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def fnv1a_cpn8(data: bytes) -> int:
    h = 2166136261
    for b in data[16:]:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def pack_cpn8_from_tsv_row(
    main_row: dict[str, str],
    slave_row: dict[str, str],
    high_wram: bytes,
    low_wram: bytes,
) -> bytes:
    assert len(high_wram) == K_HIGH_SIZE
    assert len(low_wram) == K_LOW_SIZE
    total_len = K_HEADER_SIZE + K_HIGH_SIZE + K_LOW_SIZE + 8
    buf = bytearray(total_len)

    def wr32(off: int, val: int) -> None:
        struct.pack_into("<I", buf, off, val & 0xFFFFFFFF)

    def pack_one_cpu(row: dict[str, str], cpu_off: int, adapter_off: int) -> None:
        for i in range(16):
            wr32(cpu_off + i * 4, int(row[f"R{i}"], 16))
        pc = int(row["PC"], 16)
        sr = int(row["SR"], 16)
        wr32(cpu_off + 16 * 4, pc)
        wr32(cpu_off + 17 * 4, int(row["PR"], 16))
        wr32(cpu_off + 18 * 4, int(row["GBR"], 16))
        wr32(cpu_off + 19 * 4, int(row["VBR"], 16))
        wr32(cpu_off + 20 * 4, int(row["MACH"], 16))
        wr32(cpu_off + 21 * 4, int(row["MACL"], 16))
        wr32(cpu_off + 22 * 4, (sr >> 9) & 1)
        wr32(cpu_off + 23 * 4, (sr >> 8) & 1)
        wr32(cpu_off + 24 * 4, (sr >> 4) & 0xF)
        wr32(cpu_off + 25 * 4, (sr >> 1) & 1)
        wr32(cpu_off + 26 * 4, sr & 1)
        wr32(cpu_off + 28 * 4, 0)  # delay = 0
        wr32(cpu_off + 29 * 4, pc)  # cur_pc = PC

        wr32(adapter_off + 0, (sr >> 4) & 0xF)
        wr32(adapter_off + 4, 1)  # context_loaded = 1

    wr32(0, K_CPN8_MAGIC)
    wr32(4, 3)
    wr32(8, total_len)
    wr32(16, K_CPF3_FORMAT)
    wr32(20, 3)
    pack_one_cpu(main_row, 88, 328)
    pack_one_cpu(slave_row, 208, 356)
    wr32(592, 3)   # PAUSED_CLEAN
    wr32(596, 0)   # active_cpu = 0
    wr32(600, 0)   # completed_in_slice = 0
    wr32(604, 32)  # remaining = 32
    wr32(608, 1)
    wr32(612, 0)   # mode = 0

    buf[K_HEADER_SIZE : K_HEADER_SIZE + K_HIGH_SIZE] = high_wram
    buf[K_HEADER_SIZE + K_HIGH_SIZE : K_HEADER_SIZE + K_HIGH_SIZE + K_LOW_SIZE] = low_wram
    wr32(12, fnv1a_cpn8(buf))
    return bytes(buf)


def read_frame_rows(tsv_path: Path, frame_num: int) -> dict[str, dict[str, str]]:
    lines = tsv_path.read_text(encoding="utf-8").splitlines()
    headers = lines[0].split("\t")
    rows: dict[str, dict[str, str]] = {}
    for line in lines[1:]:
        parts = line.split("\t")
        if len(parts) != len(headers):
            continue
        rec = dict(zip(headers, parts))
        if int(rec["frame"]) == frame_num:
            rows[rec["cpu"]] = rec
    if "maincpu" not in rows or "slavecpu" not in rows:
        raise RuntimeError(f"Missing frame {frame_num} CPUs in {tsv_path}")
    return rows


def build_and_verify_cart_rom() -> dict:
    zip_sha256 = sha256_file(ROM_ZIP)
    meta = ET.parse(MAME_XML)
    machine = next(m for m in meta.getroot().findall("machine") if m.get("name") == "twcup98")
    expected = [r.attrib for r in machine.findall("rom") if r.get("region") == "cart"]

    members_info = []
    with zipfile.ZipFile(ROM_ZIP) as zf:
        members = {r["name"]: zf.read(r["name"]) for r in expected}
    for r in expected:
        data = members[r["name"]]
        if len(data) != int(r["size"]) or hashlib.sha1(data).hexdigest() != r["sha1"]:
            raise RuntimeError(f"ROM member mismatch: {r['name']}")
        members_info.append({
            "name": r["name"],
            "size": len(data),
            "sha1": r["sha1"],
            "sha256": sha256_bytes(data),
        })

    # Reconstruct full 48 MiB (0x02000000..0x04FFFFFF) logical ST-V cart layout
    image = bytearray(0x03000000)
    epr = members["epr20819.24"]
    image[1:0x200000:2] = epr
    image[0x200000:0x300000] = epr
    image[0x300000:0x400000] = epr

    for off, name in (
        (0x400000, "mpr20821.12"),
        (0x800000, "mpr20822.13"),
        (0xC00000, "mpr20823.14"),
        (0x1000000, "mpr20824.15"),
    ):
        raw = members[name]
        swapped = bytearray(len(raw))
        swapped[0::2] = raw[1::2]
        swapped[1::2] = raw[0::2]
        image[off : off + len(swapped)] = swapped

    full_sha256 = sha256_bytes(bytes(image))
    expected_map = json.loads(MEM_MAP_JSON.read_text(encoding="utf-8"))
    if expected_map["cart_sha256"] != full_sha256:
        raise RuntimeError(f"48 MiB SHA-256 mismatch: {full_sha256} vs {expected_map['cart_sha256']}")

    # Verify all 17 cart-*.bin MAME memory-map samples against logical image
    sample_by_addr = {int(s["address"], 16): s for s in expected_map["samples"]}
    sample_checks = []
    for path in sorted(BANK_DIR.glob("cart-*.bin")):
        addr = int(path.stem.split("-")[1], 16)
        rel_off = addr - 0x02000000
        disk_sample = path.read_bytes()
        length = len(disk_sample)
        actual_sample = bytes(image[rel_off : rel_off + length])
        if actual_sample != disk_sample:
            raise RuntimeError(f"Sample byte mismatch for {path.name}")
        actual_sha = sha256_bytes(actual_sample)
        expected_entry = sample_by_addr[addr]
        if length != int(expected_entry["bytes"]) or actual_sha != expected_entry["sha256"]:
            raise RuntimeError(f"Sample SHA-256 mismatch for {path.name}")
        sample_checks.append({
            "file": path.name,
            "address": expected_entry["address"],
            "size_bytes": length,
            "sha256": actual_sha,
        })
    if len(sample_checks) != 17:
        raise RuntimeError(f"Expected 17 cart-*.bin samples, found {len(sample_checks)}")

    # Verify upper 28 MiB (0x01400000..0x02FFFFFF) is completely zero
    tail_nonzero = any(image[0x01400000:])
    if tail_nonzero:
        raise RuntimeError("Unexpected non-zero byte in tail 28 MiB of logical cart image")

    pop_bytes = bytes(image[:0x01400000])
    cart_out = DATA_DIR / "twcup98_cart_20m.bin"
    cart_out.write_bytes(pop_bytes)

    return {
        "zip_path": str(ROM_ZIP.relative_to(ROOT)),
        "zip_sha256": zip_sha256,
        "members": members_info,
        "full_48m_sha256": full_sha256,
        "tail_28m_all_zero": True,
        "populated_20m_file": str(cart_out.relative_to(G3_DIR)),
        "populated_20m_bytes": len(pop_bytes),
        "populated_20m_sha256": sha256_bytes(pop_bytes),
        "samples_verified": len(sample_checks),
        "sample_checks": sample_checks,
    }


def inspect_endpoint_cp(cp_path: Path) -> dict:
    raw = cp_path.read_bytes()
    hdr = struct.unpack_from("<IIIIII", raw, 0)
    main_pc = struct.unpack_from("<I", raw, 88 + 16 * 4)[0]
    slave_pc = struct.unpack_from("<I", raw, 208 + 16 * 4)[0]
    return {
        "file": str(cp_path.relative_to(G3_DIR)),
        "bytes": len(raw),
        "sha256": sha256_bytes(raw),
        "fnv1a_ok": fnv1a_cpn8(raw) == hdr[3],
        "main_pc": f"0x{main_pc:08x}",
        "slave_pc": f"0x{slave_pc:08x}",
    }


def build_visual_sync_baseline() -> dict:
    rows = read_frame_rows(VIS_NEUTRAL_DIR / "frame-samples.tsv", 8175)
    high_wram = (VIS_NEUTRAL_DIR / "control-window-start-maincpu-high-wram.bin").read_bytes()
    low_wram = (VIS_NEUTRAL_DIR / "control-window-start-maincpu-low-wram.bin").read_bytes()
    cp_bytes = pack_cpn8_from_tsv_row(rows["maincpu"], rows["slavecpu"], high_wram, low_wram)
    out_cp = DATA_DIR / "visual_sync1027b_f8175.cp"
    out_cp.write_bytes(cp_bytes)

    synced_files = {}
    for label, d, prefix in (
        ("visual_sync1027b_neutral_f8175", VIS_NEUTRAL_DIR, "control-window-start"),
        ("visual_sync1027b_neutral_f8235", VIS_NEUTRAL_DIR, "control-window-end-60f"),
        ("visual_sync1027b_up_f8175", VIS_UP_DIR, "control-window-start"),
        ("visual_sync1027b_up_f8235", VIS_UP_DIR, "control-window-end-60f"),
        ("left01_before", LEFT01_DIR, "before-input"),
        ("left01_after", LEFT01_DIR, "after-input"),
        ("ctrl07_before", CTRL07_DIR, "before-input"),
        ("ctrl07_after_25s", CTRL07_DIR, "after-match-start-25s"),
    ):
        entries = {}
        for p in sorted(d.glob(f"{prefix}*")):
            entries[p.name] = {
                "bytes": p.stat().st_size,
                "sha256": sha256_file(p),
            }
        synced_files[label] = entries

    return {
        "visual_sync_f8175_cp": inspect_endpoint_cp(out_cp),
        "endpoint_frame8175_cp": inspect_endpoint_cp(DATA_DIR / "endpoint_frame8175.cp"),
        "synced_snapshots": synced_files,
    }


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    cart_info = build_and_verify_cart_rom()
    baseline_info = build_visual_sync_baseline()
    manifest = {
        "schema": "g3_subtask1_assets_v1",
        "cart_rom": cart_info,
        "baselines": baseline_info,
    }
    manifest_path = DATA_DIR / "g3_assets_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "status": "OK",
        "cart_48m_sha256": cart_info["full_48m_sha256"],
        "cart_20m_sha256": cart_info["populated_20m_sha256"],
        "samples_verified": cart_info["samples_verified"],
        "visual_sync_f8175_cp": baseline_info["visual_sync_f8175_cp"],
        "endpoint_frame8175_cp": baseline_info["endpoint_frame8175_cp"],
    }, indent=2))


if __name__ == "__main__":
    main()
