/* Layout alternativo para el bootstrap con TinyCC. Sus objetos son mayores
 * que los de GCC, por lo que BSS comienza en 2.5 MiB. */
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x00100000;
    .text : ALIGN(16) { KEEP(*entry.o(.text)) *(.text) *(.text.*) KEEP(*(.ap_trampoline)) } :text
    ASSERT(_start == 0x00100000, "_start debe permanecer en 0x00100000")
    .rodata : ALIGN(16) { *(.rodata) *(.rodata.*) } :text
    . = ALIGN(0x1000);
    .data : ALIGN(16) { *(.data) *(.data.*) } :data
    __kernel_file_end = .;
    ASSERT(__kernel_file_end <= 0x002A0000, "kernel self-host excede 1.625 MiB")
    .bss 0x002A0000 (NOLOAD) : ALIGN(16) {
        __bss_start = .; *(.bss) *(.bss.*) *(COMMON) __bss_end = .;
    } :data
    ASSERT(__bss_end <= 0x003F0000, "BSS self-host invade la pila bootstrap")
    /DISCARD/ : { *(.eh_frame) *(.eh_frame.*) *(.eh_frame_hdr) *(.gcc_except_table) *(.gcc_except_table.*) *(.note*) *(.comment) }
}
