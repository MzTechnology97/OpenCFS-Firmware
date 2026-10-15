# Analysis tools

Offline helpers used to write `docs/spec`. They read a firmware image you provide
and never contact a device. Outputs derived from manufacturer code (SRAM images,
Ghidra projects, exports) must stay private.

| Tool | Purpose |
| --- | --- |
| `scatter_unpack.py IMAGE OUT` | rebuild the initial SRAM (Keil scatter RW decompression) for an app linked at `0x08003000` |
| `app_header.py IMAGE...` | check the loader header at `0x20C`: CRC-16 (poly `0x8005`, metadata zeroed) and length |
| `thumbdis.py IMAGE START END` | linear Thumb disassembly with literal-pool values (needs `capstone`) |
| `peek.py IMAGE SRAM ADDR...` | print words as u32/i32/float and follow pointers |
| `ghidra/CfsSetup.java` | pre-script: SRAM block, GD32F30x peripherals, vector table and IRQ names |
| `ghidra/CfsFixups.java` | create functions reached only through code pointers |
| `ghidra/CfsNames.java` + `names.tsv` | apply the function names used in the specification |
| `ghidra/CfsExport.java` | export function list, call graph, strings and per-function C (private use) |
