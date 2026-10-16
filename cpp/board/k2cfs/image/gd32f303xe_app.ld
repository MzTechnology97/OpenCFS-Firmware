/* SPDX-License-Identifier: Apache-2.0
 * OpenCFS application image for the K2 CFS board (GD32F303VE, 512 KiB flash,
 * 64 KiB SRAM). EXPERIMENTAL: not qualified on hardware.
 *
 * 0x08000000..0x08002FFF  Creality loader: never part of this image
 * 0x08003000              vector table (the loader takes MSP and reset from here)
 * 0x08003200..0x08003211  Creality header: version, CRC-16, length (packager)
 * 0x08003220..0x08003247  OpenCFS image info: variant, revision (main.cpp)
 * ..0x0807F7FF            rest of the application
 * 0x0807F800..0x0807FFFF  parameter page: never part of this image
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08003000, LENGTH = 0x0807F800 - 0x08003000
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_min_stack = 0x1000;

SECTIONS
{
    .isr_vector ORIGIN(FLASH) :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .cfs_header ORIGIN(FLASH) + 0x200 :
    {
        KEEP(*(.cfs_header))
    } > FLASH

    /* OpenCFS image identity (variant, revision), read by host tools from the file. */
    .ocfs_info ORIGIN(FLASH) + 0x220 :
    {
        KEEP(*(.ocfs_info))
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH

    .init_array :
    {
        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > FLASH

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    _image_end = LOADADDR(.data) + SIZEOF(.data);

    /* Never run at exit, and no exception tables are used. */
    /DISCARD/ :
    {
        *(.fini_array*)
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    }
}

ASSERT(ADDR(.isr_vector) == 0x08003000, "vector table must be at the loader's application base")
ASSERT(SIZEOF(.isr_vector) <= 0x200, "vector table overlaps the Creality header")
ASSERT(ADDR(.cfs_header) == 0x08003200, "Creality header must be at +0x200")
ASSERT(SIZEOF(.cfs_header) >= 0x12, "Creality header area too small")
ASSERT(ADDR(.ocfs_info) == 0x08003220 && SIZEOF(.ocfs_info) == 40, "OpenCFS image info must be at +0x220")
ASSERT(_image_end <= 0x0807F800, "image reaches the parameter page")
ASSERT(_ebss + _min_stack <= _estack, "not enough RAM left for the stack")
