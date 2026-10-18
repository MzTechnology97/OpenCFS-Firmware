#!/usr/bin/env python3
"""Evidence-only manifest contract. Never interacts with hardware."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = json.loads(
    (root / "boards/k2-cfs-gd32f303ve/board-manifest.json").read_text()
)
assert manifest["schema"] == "opencfs.board-evidence.v0"
assert manifest["target_flash_authorized"] is False
assert manifest["target_motor_io_enabled"] is False
mcu = manifest["mcu"]
assert mcu["physical_chip_marking_verified"] is False
assert mcu["application_link_base_from_original_oem_image"] == "0x08003000"
assert mcu["loader_erase_page_ownership"] == "unknown"
assert manifest["startup_vectors_or_linker_script"] == "absent by design"
assert manifest["approved_target_artifacts"] == []
records = manifest["motor_output_gpio_from_original_rom_pin_map"]
assert [m["working_label"] for m in records] == ["M1", "M2", "M3", "M4"]
pins = [pin for motor in records for pin in motor["physical_pins"]]
assert pins == ["PD2", "PD3", "PD0", "PD1", "PD14", "PD15", "PD12", "PD13"]
assert len(pins) == len(set(pins))
assert all(motor["hardware_feedback_validated"] is False for motor in records)

# This repo is not allowed to produce a target image at bootstrap.
for output_dir in ("firmware", "dist", "release"):
    target = root / output_dir
    if target.exists():
        for extension in ("*.bin", "*.elf", "*.hex"):
            assert not list(target.rglob(extension)), (
                f"Unexpected target artifact in {output_dir}: {extension}"
            )
print("OpenCFS board manifest: PASS (source evidence, flash not authorized)")
