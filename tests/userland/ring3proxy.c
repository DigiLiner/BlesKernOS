#include "../../kernel/include/api.h"
#include "../../kernel/include/syscall.h"

/* Deliberadamente usa la API v3 normal: el ELF loader debe convertir cada
 * simbolo externo en una transicion int 0x80. */
void bleskernos_program_main(gui_desktop_t *desktop UNUSED) {
    register uint32_t preserved_ebx __asm__("ebx") = 0xB16B00B5U;
    void *memory;
    void *intentional_leak;
    vfs_dir_entry_t *entries;
    uint32_t count = 0;

    if (bk_sys_api_version() != BK_API_VERSION)
        (void)syscall1(SYS_EXIT, 1U);
    __asm__ volatile ("" : "+r"(preserved_ebx));
    (void)bk_sys_getpid();
    __asm__ volatile ("" : "+r"(preserved_ebx));
    if (preserved_ebx != 0xB16B00B5U)
        (void)syscall1(SYS_EXIT, 6U);
    if (bk_sys_getpid() == 0U) (void)syscall1(SYS_EXIT, 2U);
    /* Syscalls must reject kernel/static metadata aliases before touching
     * their contents. Exercise the real CPL3 -> int 0x80 copy boundary. */
    const uint32_t protected_addresses[] = {0U, 0x00100000U, 0x02000000U};
    for (uint32_t i = 0U; i < 3U; i++) {
        if (syscall3(SYS_WRITE, 1U, protected_addresses[i], 1U) != -BK_EFAULT)
            (void)syscall1(SYS_EXIT, 8U);
    }
    system_memory_info_t ram;
    if (!bk_sys_memory_info(&ram) || ram.used_bytes + ram.free_bytes != ram.total_bytes)
        (void)syscall1(SYS_EXIT, 9U);
    kprintf("[RING3:RAM] total=%u KiB used=%u KiB free=%u KiB reserved=%u KiB\n",
            ram.total_bytes / 1024U, ram.used_bytes / 1024U,
            ram.free_bytes / 1024U, ram.reserved_bytes / 1024U);
    memory = bk_sys_alloc_zero(256U);
    if (!memory) (void)syscall1(SYS_EXIT, 3U);
    bk_sys_free(memory);
    entries = (vfs_dir_entry_t *)bk_sys_alloc_zero(
        sizeof(vfs_dir_entry_t) * 4U);
    if (!entries) (void)syscall1(SYS_EXIT, 4U);
    if (!bk_file_list_dir("/", entries, 4U, &count) || count == 0U)
        (void)syscall1(SYS_EXIT, 5U);
    bk_sys_free(entries);
    /* La prueba deja memoria sin liberar a propósito. El recolector asociado
       al process_id debe recuperarla al procesar el zombie. */
    intentional_leak = bk_sys_alloc_zero(96U * 1024U);
    if (!intentional_leak) (void)syscall1(SYS_EXIT, 7U);
    (void)syscall1(SYS_EXIT, 0U);
    for (;;) (void)syscall0(SYS_YIELD);
}
