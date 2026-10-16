# Specification sources and method

This specification describes the **behavior** of the Creality CFS controller
application `cfs0_000_153` (board `cfs0_050_G32`, reported as 1.5.3). It was written
to make an independent implementation possible. No manufacturer bytes, disassembly
listings or decompiled functions are part of this repository.

## Analysed images (not distributed)

| Image | SHA-256 | Where it comes from |
| --- | --- | --- |
| stock `cfs0_050_G32-cfs0_000_153.bin` | `5b076563f474da1e8741ee88a9b019c2dcb1b7de6c1c651f201f1be61c345dea` | shipped with the K2 firmware, `/usr/share/klipper/fw/cfs` on the printer |
| K2-OpenHost v3.22 | `40dbaad88de7b6f9435592fa03b081e9b96663a9e84896b1ad8021a8f13cf8c7` | stock 153 + 63 patched bytes + 2468 appended bytes (`07-v322-vs-stock.md`) |
| public 2025 full flash dump | `58b8b90890c08aae81235dd4625fc75ff955e09e82f6c47ee2d74276498bc125` | loader `0x08000000..0x08002FFF` and app `cfs0_000_108` |

Older images of the same family were used for cross-checks:
`cfs0_050_G30-cfs0_000_150`, `cfs0_050_G32-cfs0_000_122` and `cfs6_100_G31-cfs6_222_000`.

## Method

1. Rebuild the initial SRAM from the Keil scatter table (`tools/re/scatter_unpack.py`).
2. Import the app into Ghidra 12.1.4 at **`0x08003000`**, with the SRAM, peripheral and
   vector setup from `tools/re/ghidra/CfsSetup.java`.
3. Name functions from RT-Thread assertion strings, `__func__` strings and the
   K2-OpenHost hook symbols (`tools/re/ghidra/names.tsv`, `CfsNames.java`). Then
   create pointer-reached functions (`CfsFixups.java`).
4. Read the command dispatcher, the stage machines and the drivers. Write down only
   behavior, timing, thresholds and wire formats.
5. Cross-check against the host side ([kalico-k2pro extras](https://github.com/MzTechnology97/kalico-k2pro/tree/k2-pro-openhost/klippy/extras))
   and live K2 Pro observations ([K2-OpenHost CFS validation](https://github.com/MzTechnology97/K2-OpenHost/blob/main/docs/en/CFS_VALIDATION.md)).

Reproduce the import:

```bash
python3 tools/re/scatter_unpack.py <app.bin> sram_init.bin
analyzeHeadless ghidra CFS -import <app.bin> -loader BinaryLoader \
  -loader-baseAddr 0x08003000 -loader-blockName FLASH -processor ARM:LE:32:Cortex \
  -scriptPath tools/re/ghidra -preScript CfsSetup.java sram_init.bin \
  -postScript CfsFixups.java -postScript CfsNames.java tools/re/ghidra/names.tsv \
  -postScript CfsExport.java <private-output-dir>
```

Keep the Ghidra project and export private; they contain manufacturer code.

## Evidence tags

- **[str]** — firmware string or `__func__` name
- **[code]** — confirmed in the analysed code
- **[res]** — earlier K2-OpenHost research: [k2-cfs-rfid-tools](https://github.com/MzTechnology97/k2-cfs-rfid-tools) audits and PRs, [k2-openhost-firmware-tools](https://github.com/MzTechnology97/k2-openhost-firmware-tools)
- **[host]** — host-side Kalico/Creality code
- **[hw]** — measured on a real printer

Addresses written before 2026-10-12 in older research use `0x08010000` as the base.
Subtract `0xD000` to get the real address.

## Public third-party research

- [fake-name/cfs-reverse-engineering](https://github.com/fake-name/cfs-reverse-engineering)
  (read 2026-10-16). `PCBs.md`: chip list (GD32F303, HTD8236, MS8828, FM17622,
  TM1621B, 24C16, TPT75176H, MT6826S) and traced pinouts (feeder IN1/IN2, current
  feedback, J5, J6, J8/J19, BLDC). `probing/Mag Encoder.dsl` and
  `probing/RFID Reader I2Cs.dsl`: DSLogic captures at 10 MHz, decoded with our own
  scripts (MT6826S framing, 20 ms poll, FM17622 soft reset). `reversing/live_dumps`
  holds the public 2025 dumps cited elsewhere in this spec. The captures come from
  an earlier firmware build than 1.5.3, so every finding is cross-checked against
  the stock code.
- [DnG-Crafts/K2-RFID](https://github.com/DnG-Crafts/K2-RFID) (read 2026-10-16, no
  licence: used only as a source of facts, no code or data copied). The ESP32
  writer confirms the Key A derivation, the block encryption, the 48-character
  record layout and the weight-to-length codes. Its keys were compared with the
  extracted ones by SHA-256 fingerprint only.
- Tag format specifications (read 2026-10-16), used for facts and published
  examples only; no code copied, vendor secrets kept in the user's local header:
  [SnapmakerResearchGroup/RFID](https://github.com/SnapmakerResearchGroup/RFID)
  (MIT, data layout) with the KDF in
  [proxmark3 `common/generator.c`](https://github.com/RfidResearchGroup/proxmark3) (GPL, facts only);
  [TigerTag-RFID-Guide](https://github.com/TigerTag-Project/TigerTag-RFID-Guide) (CC-BY-4.0);
  [anycubic-nfc-filament `format.md`](https://github.com/Molodos/anycubic-nfc-filament) (no licence);
  [ELEGOO-RFID-Tag-Guide](https://github.com/elegooofficial/ELEGOO-RFID-Tag-Guide) (no licence);
  [OpenTag3D](https://github.com/queengooborg/OpenTag3D) (GPL-3.0, `spec.json`).
