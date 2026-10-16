/* linker.ld - Script de linkeo para BleskernOS kernel
 * El kernel se carga en 0x00100000 (1 MiB)
 * El Stage 2 salta a kernel_main() que debe estar al inicio
 */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5); /* R-X */
    data PT_LOAD FLAGS(6); /* RW- */
}

SECTIONS {
    . = 0x00100000; /* Stage 2 entra aqui: isla de bootstrap descartable */
    __boot_discard_start = .;

    .boot : ALIGN(16) {
        /* Solo codigo/datos necesarios antes de kernel_main.  Wine i386 usa
         * 0x00010000..0x0010ffff como area DOS y debe poder reemplazarla luego. */
        KEEP(*entry.o(.boot.text))
        KEEP(*entry.o(.boot.rodata))
    } :text

    ASSERT(_start == 0x00100000,
           "_start debe permanecer en 0x00100000: Stage 2 usa salto fijo")
    ASSERT(. <= 0x00110000,
           "bootstrap supera los 64 KiB reservados antes del kernel residente")

    /* Todo lo que siga vivo despues del arranque empieza justo despues del
     * area DOS que Wine preloader puede materializar con MAP_FIXED. */
    . = 0x00110000;
    __boot_discard_end = .;
    __kernel_live_start = .;

    .text : ALIGN(16) {
        /* gdt_flush/tss_flush de entry.o son runtime y deben vivir aqui. */
        KEEP(*entry.o(.text))
        *(.text)
        *(.text.*)
        KEEP(*(.ap_trampoline))
    } :text

    __kernel_text_end = .;

    .rodata : ALIGN(16) {
        *(.rodata)
        *(.rodata.*)
    } :text

    . = ALIGN(0x1000);

    .data : ALIGN(16) {
        *(.data)
        *(.data.*)
    } :data

    /*
     * 0xA0000..0xFFFFF is not general-purpose RAM on a PC: it contains the
     * VGA aperture and BIOS ROMs.  The 0.4 kernel's BSS no longer fits below
     * that hole, so keep all zero-initialized globals above 1 MiB.
     */
    __kernel_file_end = .;
    ASSERT(__kernel_file_end <= 0x00240000,
           "kernel .text/.rodata/.data supera la reserva de 1280 KiB")

    /* Keep resident globals below the 4 MiB Windows image window. Linux
     * graphics contexts are allocated on demand, so the BSS fits here. */
    . = ALIGN(0x1000);
    .bss ALIGN(0x1000) (NOLOAD) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    ASSERT(__bss_end <= 0x003F0000,
           "kernel .bss invade la pila bootstrap (3F0000-400000)")
    ASSERT(__bss_start >= ALIGN(__kernel_file_end, 0x1000),
           "BSS solapa la imagen cargada")

    /* Metadata generada por toolchains modernos que no se usa en el kernel.
     * Descartarla evita inflar el binario plano y ocupar el espacio reservado
     * antes de la BSS. */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame.*)
        *(.eh_frame_hdr)
        *(.gcc_except_table)
        *(.gcc_except_table.*)
        *(.note*)
        *(.comment)
    }
}
