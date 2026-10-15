# Original evidence and upstream components

OpenCFS does not include copied manufacturer code or firmware dumps.

## Original hardware and firmware observations

- [Source-only CFS reverse engineering and GPIO/motor provenance](https://github.com/MzTechnology97/k2-cfs-rfid-tools/pull/29):
  original 0xAC-byte motor records and GPIOD pin mapping.
- [Original GPIO direct callsite census](https://github.com/MzTechnology97/k2-cfs-rfid-tools/pull/30):
  118/122 source-classified branch candidates; no live motor proof.
- [Original GPIO tail branches and RT-Thread callback](https://github.com/MzTechnology97/k2-cfs-rfid-tools/pull/31).
- [Original MSH-compatible `pin` command export](https://github.com/MzTechnology97/k2-cfs-rfid-tools/pull/32):
  command exists in ROM, external console reachability unknown.
- [Original CFS RS485 F7 framing and CRC-8 ROM audit](https://github.com/MzTechnology97/k2-cfs-rfid-tools/pull/33):
  verifies physical APP Thumb source for CRC polynomial 0x07,
  transmit length +3 and receive checks on saved v3.13/v3.22.

Facts recovered from an OEM binary may help interoperability, but are
not independently licensed OEM code and must not be copied here.

## Future third-party dependencies (NOT YET IMPORTED)

- [RT-Thread](https://github.com/RT-Thread/rt-thread), Apache-2.0.
  Select and pin an exact revision when adding the MCU BSP.
- [GigaDevice GD32F303VET6 datasheet and reference material](https://www.gigadevice.com/product/mcu/mcus-product-selector/gd32f303vet6).
  HAL/library licensing must be checked separately before vendoring.

The physical GD32 variant, board revision, RAM/Flash ownership and
recovery procedure must be independently verified before release.
