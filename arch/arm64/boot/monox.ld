/* MONOX Kernel - ARM64 Linker Script
 *
 * Kernel virtual base: 0xFFFFFFC000000000 (top of the 48-bit TTBR1 space;
 *   disjoint from the RAM linear map at 0xFFFF000000000000+PA and from the
 *   vmalloc-style area at 0xFFFF800000000000..0xFFFFFF8000000000)
 * Kernel physical base: wherever the loader puts the flat image. It is read
 *   at run time (head.S: adr_l _start), so QEMU (RAM+2 MiB), U-Boot and
 *   phone bootloaders all work with the same binary.
 * Page size: 4 KiB
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFC000000000;
/* LMA only labels the ELF segments; the flat monox.img does not depend on it */
KERNEL_LMA = 0x40000000;

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    /* --------------------------------------------------------
     * Kernel image header (must be first, 4 KiB aligned)
     * --------------------------------------------------------*/
    .head.text ALIGN(0x1000) : AT(KERNEL_LMA)
    {
        KEEP(*(.head.text))
    }

    /* --------------------------------------------------------
     * Init code and data (only used during boot, can be freed)
     * --------------------------------------------------------*/
    .init.text : AT(LOADADDR(.head.text) + SIZEOF(.head.text))
    {
        __init_text_start = .;
        *(.init.text)
        __init_text_end = .;
    }

    .init.data :
    {
        __init_data_start = .;
        *(.init.data)
        __init_data_end = .;
    }

    .init.stack ALIGN(16) :
    {
        *(.init.stack)
    }

    /* --------------------------------------------------------
     * Exception vectors (must be 2 KiB aligned for VBAR)
     * --------------------------------------------------------*/
    .vectors ALIGN(0x800) :
    {
        __vectors_start = .;
        KEEP(*(.vectors))
        __vectors_end = .;
    }

    /* --------------------------------------------------------
     * Kernel text (executable, read-only after boot)
     * --------------------------------------------------------*/
    .text ALIGN(0x1000) :
    {
        __text_start = .;
        *(.text.unlikely)
        *(.text.startup)
        *(.text*)
        *(.fixup)
        . = ALIGN(8);
        __text_end = .;
    }

    /* --------------------------------------------------------
     * Read-only data
     * --------------------------------------------------------*/
    .rodata ALIGN(0x1000) :
    {
        __rodata_start = .;
        *(.rodata*)
        *(.gnu.linkonce.r*)
        . = ALIGN(8);
        __rodata_end = .;
    }

    /* --------------------------------------------------------
     * Exception tables
     * --------------------------------------------------------*/
    .eh_frame ALIGN(8) :
    {
        *(.eh_frame)
    }

    /* --------------------------------------------------------
     * Writable data
     * --------------------------------------------------------*/
    .data ALIGN(0x1000) :
    {
        __data_start = .;
        /* The image is a flat binary that is already in RAM: there is no
         * separate load address to copy from. head.S compares the two and
         * skips the copy when they are equal. (Previously __data_load was
         * left undefined => link error.) */
        __data_load = .;
        *(.data*)
        *(.gnu.linkonce.d*)
        . = ALIGN(8);
        __data_end = .;
    }

    /* --------------------------------------------------------
     * BSS (zero-initialised, zeroed by head.S)
     * --------------------------------------------------------*/
    .bss ALIGN(0x1000) :
    {
        __bss_start = .;
        *(.bss*)
        *(.gnu.linkonce.b*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* --------------------------------------------------------
     * Kernel end marker
     * --------------------------------------------------------*/
    . = ALIGN(0x1000);
    __kernel_end = .;
    __kernel_size = __kernel_end - KERNEL_VMA;

    /* --------------------------------------------------------
     * Discard
     * --------------------------------------------------------*/
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.debug*)
        *(.gnu.warning)
    }
}
