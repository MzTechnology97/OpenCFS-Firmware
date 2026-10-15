# OpenCFS Firmware

Independent, open-source firmware research and development for the Creality CFS controller.

> **Development-only. Nothing in this repository is currently flashable onto a CFS.** Do not attempt to replace the production MCU firmware with bootstrap artifacts.

The project will implement original, auditable C code for hardware abstraction, communication, RFID handling, motor coordination, and diagnostics. Its goal is a source-built alternative rather than a patched redistribution of Creality firmware.

Initial development is limited to desktop simulations, cross-compilation of isolated modules for Cortex-M4, and read-only firmware/hardware evidence. The physical MCU, bootloader ownership, complete protocol, motor stop feedback, and recovery procedure remain unqualified.

Work in progress is reviewed via draft pull requests. See the bootstrap PR for the first testable core and development plan.

License: Apache-2.0 (project-authored source; third-party dependencies retain their own licenses).
