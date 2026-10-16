ENTRY(_start)

SECTIONS
{
    /*
     * Real Pi firmware loads kernel7.img at 0x8000. QEMU's generic
     * 32-bit -kernel path loads a raw image at 0x10000, so the Makefile
     * can override this symbol when producing kernel7-qemu.img.
     */
    . = DEFINED(KERNEL_LOAD_BASE) ? KERNEL_LOAD_BASE : 0x00008000;

    .text : ALIGN(32) {
        KEEP(*(.text.boot))
        KEEP(*(.vectors))
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    }

    /* ARMv7 first-level translation tables require 16 KiB alignment. */
    .mmu_tables (NOLOAD) : ALIGN(16384) {
        *(.mmu_tables)
    }

    /* Banked exception modes need valid stacks before IRQs are enabled. */
    . = ALIGN(16);
    __und_stack_bottom = .; . += 0x1000; __und_stack_top = .;
    __abt_stack_bottom = .; . += 0x1000; __abt_stack_top = .;
    __fiq_stack_bottom = .; . += 0x1000; __fiq_stack_top = .;
    __irq_stack_bottom = .; . += 0x4000; __irq_stack_top = .;
    __svc_stack_bottom = .; . += 0x10000; __svc_stack_top = .;
    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.ARM.attributes)
        *(.note*)
    }
}
