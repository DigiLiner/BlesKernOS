#include "../include/swap.h"
#include "../include/recovery_console.h"
#include "../include/admin.h"
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/memory.h"
#include "../include/paging.h"
#include "../include/keyboard.h"
#include "../include/pic.h"
#include "../include/idt.h"
#include "../include/shell.h"
#include "../include/pit.h"
#include "../include/sound.h"
#include "../include/block.h"
#include "../include/ata.h"
#include "../include/ahci.h"
#include "../include/vfs.h"
#include "../include/iso9660.h"
#include "../include/pci.h"
#include "../include/gfx.h"
#include "../include/mouse.h"
#include "../../gui/gui.h"
#include "../include/task.h"
#include "../include/gdt.h"
#include "../include/bootsplash.h"
#include "../include/usb_storage.h"
#include "../include/driver.h"
#include "../include/graphics_resources.h"
#include "../include/startup_sound.h"
#include "../include/system_sounds.h"
#include "../include/elf_loader.h"
#include "../include/network.h"
#include "../include/language.h"
#include "../include/boot_mode.h"
#include "../include/installer.h"
#include "../include/floppy.h"
#include "../include/floppy_installer.h"
#include "../include/bkl_setup.h"
#include "../include/setup_boot.h"
#include "../include/lpt.h"
#include "../include/api.h"
#include "../include/perfmon.h"
#include "../include/compat_mode.h"
#include "../include/smp.h"
#include "../include/smp_watchdog.h"
#include "../include/kernel_domains.h"
#include "../include/bex_loader.h"
#include "../include/syscall.h"
#include "../include/wine_policy.h"

/* Poll the optical drive after boot. Hardware eject/insert has no IRQ path
 * on this BIOS/ATAPI stack. Read the PVD directly, bypassing ISO9660 cache:
 * exchanging CDs of the same size must invalidate the previous directory.
 * Never touch the live installer CD or remount over open /CDROM handles. */
static uint32_t kernel_cd_signature(const uint8_t *pvd) {
    uint32_t hash = 2166136261U;
    for (uint32_t i = 0; i < 2048U; ++i)
        hash = (hash ^ pvd[i]) * 16777619U;
    return hash ? hash : 1U;
}

static void kernel_cdrom_mount_retry(void *argument UNUSED) {
    uint32_t hz = pit_get_frequency_hz();
    uint32_t pause = hz ? hz * 2U : 200U;
    uint8_t *pvd = kmalloc(2048U);
    if (!pvd) return;
    for (;;) {
        task_sleep(pause);
        if (vfs_cdrom_is_root()) continue;
        block_device_t *cd = block_get("cd0");
        if (!cd || cd->type != BLOCK_DEVICE_ATAPI) continue;
        bool media_ready = ata_refresh_media("cd0") &&
                           cd->sector_size == 2048U &&
                           cd->sector_count > 16U &&
                           block_read(cd, 16U, 1U, pvd) &&
                           pvd[0] == 1U &&
                           kmemcmp(pvd + 1U, "CD001", 5U) == 0;
        uint32_t signature = media_ready ? kernel_cd_signature(pvd) : 0U;
        kernel_domains_enter(KDOMAIN_VFS);
        bool mounted = iso9660_is_mounted();
        if (mounted && (!signature || signature != iso9660_media_signature())) {
            /* Open handles must not be redirected to another CD. On removal
             * invalidate immediately: old read_at calls now return failure. */
            iso9660_unmount();
            kprintf("[CDROM] medio retirado o cambiado; cache invalidada\n");
        }
        if (signature && !iso9660_is_mounted() && !vfs_cdrom_busy()) {
            if (iso9660_mount_default()) {
                kprintf("[CDROM] nuevo medio montado en /CDROM\n");
            }
        }
        kernel_domains_exit(KDOMAIN_VFS);
    }
}

static void kernel_report_ram(const char *stage) {
    heap_info_t heap;
    system_memory_info_t ram;
    mm_get_info(&heap);
    mm_get_system_info(&ram);
    uint32_t managed = ram.total_bytes >= ram.reserved_bytes
        ? (uint32_t)(ram.total_bytes - ram.reserved_bytes) : 0U;
    kprintf("[RAM:%s] installed_kib=%u managed_kib=%u used_kib=%u "
        "free_kib=%u unmanaged_kib=%u heap_used_kib=%u "
        "heap_free_kib=%u vm_free_kib=%u\n",
        stage, (uint32_t)(ram.total_bytes / 1024U), managed / 1024U,
        (uint32_t)(ram.used_bytes / 1024U),
        (uint32_t)(ram.free_bytes / 1024U),
        (uint32_t)(ram.reserved_bytes / 1024U),
        (uint32_t)(heap.used_bytes / 1024U),
        (uint32_t)(heap.free_bytes / 1024U),
        mm_vm_frame_free_pages() * 4U);
}

static bool kernel_start_print_spooler(void) {
    vfs_dir_entry_t service;
    int pid;

    if (!vfs_stat("/SYSTEM/SERVICES/PRINTSPL.BEX", &service) ||
        service.type != VFS_NODE_FILE) {
        kprintf("[PRINT] PRINTSPL.BEX no encontrado; impresion desactivada\n");
        return false;
    }

    bk_print_spooler_set_ready(false);
    pid = elf_spawn_program_ex("/SYSTEM/SERVICES/PRINTSPL.BEX",
                               (gui_desktop_t *)(uintptr_t)1U, NULL);
    if (pid < 0) {
        kprintf("[PRINT] no se pudo iniciar PRINTSPL.BEX: %s\n",
                elf_last_error());
        return false;
    }

    /* La impresión no es una dependencia para construir el escritorio.
       Esperar aquí hasta cinco segundos detenía todo el boot y aun podía
       vencer justo antes de que el proceso Ring 3 marcara READY. El daemon
       publica su estado por la misma API; las operaciones de impresión pueden
       consultarlo sin serializar el arranque gráfico. */
    admin_adopt(2,(uint32_t)pid);
    kprintf("[PRINT] spooler iniciado en segundo plano, pid=%d\n", pid);
    return true;
}


static int kernel_start_vision(void) {
    vfs_dir_entry_t service;
    int pid;

    if (!vfs_stat("/SYSTEM/SERVICES/VISION.BEX", &service) ||
        service.type != VFS_NODE_FILE) {
        kprintf("[VISION] VISION.BEX no encontrado\n");
        bootsplash_show("VISION: SERVICE MISSING", 70);
        return -1;
    }

    /* Visible milestones distinguish a loader stall from a blocked GUI. */
    bootsplash_show("VISION: LOADING ELF", 71);
    pid = elf_spawn_vision_service("/SYSTEM/SERVICES/VISION.BEX",
                               (gui_desktop_t *)(uintptr_t)1U, NULL);
    if (pid < 0) {
        kprintf("[VISION] no se pudo iniciar: %s\n", elf_last_error());
        bootsplash_show("VISION: ELF LOAD FAILED", 71);
        return -1;
    }
    admin_adopt(1,(uint32_t)pid);
    kprintf("[VISION] servicio lanzado pid=%d\n", pid);
    return pid;
}

static bool kernel_wait_vision_ready(int pid) {
    int32_t status = 0;
    uint32_t hz = pit_get_frequency_hz();
    uint32_t start = pit_get_ticks();
    uint32_t last_pulse = start;
    uint32_t last_report = start;
    uint32_t timeout;

    if (!hz) hz = 100U;
    /* VirGL setup and icon decoding can exceed ten seconds on a busy host.
     * A premature timeout strands a desktop that becomes READY moments later
     * behind the splash forever. Keep a bounded wait for genuine failures. */
    timeout = hz * 120U; /* slow PIII/P4 laptops may decode icons slowly */
    while (!gui_vision_ready()) {
        uint32_t now;
        int waited = task_waitpid((uint32_t)pid, &status);
        if (waited > 0) {
            kprintf("[VISION] termino antes de READY, status=%d\n", status);
            bootsplash_show("VISION: PROCESS EXITED", 90);
            return false;
        }
        if (waited < 0) {
            kprintf("[VISION] pid=%d ya no existe en task_waitpid\n", pid);
            bootsplash_show("VISION: PROCESS LOST", 90);
            return false;
        }
        now = pit_get_ticks();
        if ((uint32_t)(now - start) >= timeout) {
            kprintf("[VISION] timeout esperando READY, pid=%d ticks=%u\n",
                    pid, now - start);
            bootsplash_show("VISION: TIMEOUT", 90);
            return false;
        }
        /* The existing marquee only moves when a caller pulses it.
         * A stationary bar did not previously imply a frozen kernel. */
        if ((uint32_t)(now - last_pulse) >= (hz / 3U ? hz / 3U : 1U)) {
            last_pulse = now;
            bootsplash_pulse();
        }
        if ((uint32_t)(now - last_report) >= hz * 5U) {
            last_report = now;
            kprintf("[VISION:WAIT] pid=%d elapsed=%u ready=%u\n", pid,
                    (uint32_t)((now - start) / hz), gui_vision_ready() ? 1U : 0U);
        }
        task_yield();
    }
    return true;
}


static void kernel_ring3_proxy_selftest(void) {
    int pid;
    int32_t status = -1;

    pid = elf_spawn_program_ex("/SYSTEM/PROGRAMS/RING3PROXY.BEX",
                               (gui_desktop_t *)(uintptr_t)1U, NULL);
    /* Developer images keep self-tests outside the user Programs menu. */
    if (pid < 0)
        pid = elf_spawn_program_ex(
            "/SYSTEM/TESTS/NATIVE/RING3PROXY.BEX",
            (gui_desktop_t *)(uintptr_t)1U, NULL);
    if (pid < 0) {
        kprintf("[RING3] no se pudo cargar autoprueba: %s\n",
                elf_last_error());
        return;
    }
    while (task_waitpid((uint32_t)pid, &status) == 0) task_yield();
    kprintf("[RING3] proxy API y retorno a user-space: %s\n",
            status == 0 ? "OK" : "ERROR");
}

static void kernel_linux_elf_selftests(void) {
    static const char *tests[] = {
        "/SYSTEM/TESTS/NATIVE/LINUXPIE.ELF",
        "/SYSTEM/TESTS/NATIVE/INTERPPIE.ELF",
        "/SYSTEM/TESTS/NATIVE/DYNAMIC.ELF",
        "/SYSTEM/TESTS/NATIVE/GLIBCHEL.ELF"
    };
    vfs_dir_entry_t entry;
    for (uint32_t i=0U; i<sizeof(tests)/sizeof(tests[0]); i++) {
        int pid;
        int32_t status=-1;
        if (!vfs_stat(tests[i], &entry)) continue; /* user edition */
        pid=elf_spawn_program_ex(tests[i],
            (gui_desktop_t *)(uintptr_t)1U, NULL);
        if (pid < 0) {
            kprintf("[ELF:LINUX] autoprueba %s: LOAD ERROR (%s)\n",
                    tests[i], elf_last_error());
            continue;
        }
        while (task_waitpid((uint32_t)pid,&status)==0) task_yield();
        kprintf("[ELF:LINUX] autoprueba %s: %s status=%d\n", tests[i],
                status==0 ? "OK" : "ERROR", status);
    }
    /* Launch two copies before yielding. Both use the same Linux virtual
     * addresses; their private marker catches accidental shared mappings. */
    if(vfs_stat("/SYSTEM/TESTS/NATIVE/EXECFIX.ELF",&entry)) {
        int first=elf_spawn_program_ex(
            "/SYSTEM/TESTS/NATIVE/EXECFIX.ELF",
            (gui_desktop_t *)(uintptr_t)1U,NULL);
        int second=elf_spawn_program_ex(
            "/SYSTEM/TESTS/NATIVE/EXECFIX.ELF",
            (gui_desktop_t *)(uintptr_t)1U,NULL);
        int32_t first_status=-1,second_status=-1;
        if(first>=0 && second>=0) {
            while(task_waitpid((uint32_t)first,&first_status)==0) task_yield();
            while(task_waitpid((uint32_t)second,&second_status)==0) task_yield();
        }
        kprintf("[ELF:LINUX] ET_EXEC dual CR3: %s status=%d/%d\n",
            first>=0 && second>=0 && first_status==0 && second_status==0
                ? "OK" : "ERROR",first_status,second_status);
    }
}


/* Panic/triple fault cannot be recovered in-place, but a bad autostart,
 * driver or filesystem state must not make the next boot inescapable.
 * Give the operator a guaranteed early F8 window (no filesystem writes).
 * This runs after PIT/keyboard IRQ are initialized and before mount/GUI. */
static bool kernel_recovery_f8_window(void) {
    uint32_t start = pit_get_ticks();
    uint32_t hz = pit_get_frequency_hz();
    uint32_t window = hz ? hz * 2U : 200U;
    kbd_key_event_t event;

    if (!window) window = 200U;
    bootsplash_show("F8: RECOVERY MODE (2 SEC)", 58);
    kprintf("[BOOT] Pulse F8 para entrar a recovery sin Vision ni servicios\n");
    while ((uint32_t)(pit_get_ticks() - start) < window) {
        while (kbd_next_event(&event)) {
            if (event.pressed && event.key == KEY_F8) {
                kprintf("[BOOT] F8 detectada: recovery solicitado\n");
                return true;
            }
        }
        task_sleep(1U);
    }
    return false;
}

/* Keep a recovery shell reachable on a Vision startup failure or exit.
 * Its command implementation is linked in programs/shell.c (not
 * kernel/console/shell.c).  Returning from shell must never continue boot. */
static NORETURN void kernel_recovery_shell(const char *why) {
    bootsplash_disable();
    recovery_console_enter();
    kprintf("[RECOVERY] %s\n", why ? why : "Arranque interrumpido");
    kprintf("[RECOVERY] Pruebe help, bootlog 50, mem, disks y reboot\n");
    shell_run();
    for (;;) task_sleep(100U);
}

/* Wine is deliberately not a boot dependency. The installed launcher starts
 * wineserver/TinyX only when a Windows application is requested. */

void kernel_main(void) {
    bool installer_boot = boot_mode_is_installer();
    bool graphical_setup_boot = false;
    vga_init();
    /* No dejar VESA, E820 ni paging delante de los exception handlers. En
     * hardware real una excepcion aqui antes terminaba como triple fault. */
    gdt_init();
    idt_init();
    /* Early video setup must wait until boot information is snapshotted and
     * paging has installed the supervisor mapping of the BIOS framebuffer. */
    /* Stage 2 leaves the E820 map at physical 0x500, inside page zero.
     * Preserve it before paging deliberately unmaps that page. */
    mm_boot_snapshot();
    (void)paging_init();
    mm_init();
    compat_mode_init();
    gfx_init();
    bootsplash_show("STARTING KERNEL", 3);
    bootsplash_show("MEMORY MANAGER", 10);
    task_init();
    idt_enable_runtime_handlers();
    kernel_domains_init();
    bootsplash_show("TASK SYSTEM", 18);
    pic_init();
    pit_init();
    admin_init();
    smp_watchdog_init();
    perfmon_init();
    bootsplash_show("INTERRUPTS READY", 28);
    kbd_init();
    bootsplash_show("INPUT DEVICES", 38);
    pci_init();
    lpt_init();
    block_init();
    usb_storage_init();
    ata_init();
    ahci_init();
    swap_init();
    if (installer_boot) floppy_init();
    bootsplash_show("STORAGE DRIVERS", 52);
    vfs_init();
    driver_loader_init();
    bootsplash_show("MOUNTING FILESYSTEM", 62);
    sti();
    bool recovery_f8 = !installer_boot && kernel_recovery_f8_window();

    /* El Stage 1 de instalacion escribe el magic INST. Primero se intenta
       montar un CD ISO9660; si no existe, Setup entra en modo multidisquete. */
    if (installer_boot) {
        usb_storage_start_hotplug_task();
        bootsplash_disable();
        if (iso9660_builtin_init() && iso9660_mount_default()) {
            kprintf("[SETUP] Instalador CD ISO9660 detectado.\n");
            vfs_use_cdrom_as_root(true);
            installer_run();
        } else {
            kprintf("[SETUP] Sin CD ISO9660; usando instalador en disquetes.\n");
            vfs_use_cdrom_as_root(false);
            floppy_installer_run();
        }
        return;
    }

    if (compat_mode_allow_optional_services())
        usb_storage_start_hotplug_task();
    if (compat_mode_allow_user_programs() &&
        compat_mode_allow_optional_services()) {
        /* The old smoke test entered a kernel-resident function at CPL3.
         * Supervisor-only kernel text correctly rejects that after Phase 1,
         * so it was only generating a false boot failure.  Real ELF loading
         * below is now the Ring-3 validation path. */
        task_yield();
    }

    if (!vfs_mount_default()) {
        /* Live CD: el kernel trae un lector ISO9660 minimo integrado para
           romper el ciclo driver-en-CD / CD-sin-driver. El resto de drivers
           y programas se cargan normalmente desde la raiz ISO de solo lectura. */
        kprintf("[BOOT] FAT no disponible; intentando Live CD ISO9660...\n");
        if (iso9660_builtin_init() && iso9660_mount_default()) {
            vfs_use_cdrom_as_root(true);
            kprintf("[BOOT] Live CD montado como raiz de solo lectura.\n");
        } else {
            /* Conservar el framebuffer que ya activo Stage 2. */
            bootsplash_show("RECOVERY SHELL", 100);
            bootsplash_disable();
            recovery_console_enter();
            kprintf("[Kernel] No se pudo montar BFS, FAT ni ISO9660.\n");
            kprintf("[Kernel] Iniciando shell de recuperacion.\n");
            shell_run();
        }
    }

    swap_init_file();

    if (recovery_f8)
        kernel_recovery_shell("F8: inicio seguro; Vision y autostart omitidos");

    if (bkl_setup_pending()) {
        bootsplash_disable();
        vga_init();
        kprintf("[BKL] Instalacion pendiente detectada; extrayendo paquete.\n");
        if (!bkl_setup_finalize()) {
            kprintf("[BKL] Error al finalizar la instalacion.\n");
            kprintf("Conserve /SETUP para diagnostico y reinicie.\n");
            kernel_recovery_shell("Instalacion BKL incompleta; no continuar con escritorio");
        }
    }

    vfs_dir_entry_t recovery;
    if(vfs_stat("/SYSTEM/USER/CONFIG/RECOVERY.NEXT",&recovery)) {
        vfs_remove("/SYSTEM/USER/CONFIG/RECOVERY.NEXT");
        bootsplash_disable();
        recovery_console_enter();
        kprintf("[RECOVERY] consola solicitada. Use help, bootlog y reboot.\n");
        shell_run();
    }
    graphical_setup_boot = setup_boot_requested();
    if (graphical_setup_boot)
        kprintf("[SETUP] START.INI solicita el asistente de primer arranque.\n");

    language_init();

    admin_storage_ready();

    if (compat_mode_allow_optional_services()) {
        kernel_ring3_proxy_selftest();
        kernel_linux_elf_selftests();
        kprintf("[DVR] Buscando modulos en /SYSTEM/DRIVERS\n");
        kprintf("[DVR] %u modulo(s) cargado(s)\n",
                driver_load_directory("/SYSTEM/DRIVERS"));
        network_start_autoconfigure();
    } else {
        /* Los módulos que no detectan su hardware se liberan inmediatamente
         * en driver_load(). Con vfs_read_all sin doble cache, cargar la tabla
         * completa cuesta sólo las imágenes de los controladores útiles. */
        kprintf("[COMPAT] cargando drivers completos; servicios pesados bajo demanda\n");
        kprintf("[DVR] %u modulo(s) cargado(s) en perfil de 8 MiB\n",
                driver_load_directory("/SYSTEM/DRIVERS"));
    }

    /* Mount data CDs before applications start. A drive that is still becoming
       ready gets bounded retries in its own task, never from a GUI callback. */
    if (!vfs_has_cdrom() && iso9660_mount_default())
        kprintf("[VFS] CD-ROM ISO9660 montado en /CDROM\n");
    if (block_get("cd0") &&
        task_create("cdrom-watch", kernel_cdrom_mount_retry, NULL) < 0)
        kprintf("[VFS] No se pudo crear el monitor de CD-ROM\n");

    /* El arranque temprano conserva VGA/texto. Con PCI ya enumerado, esta
       segunda pasada prioriza VMware SVGA-II y conserva VESA como fallback. */
    gfx_init();
    /* A backed 1 MiB frame pool permits private paging at 16-24 MiB. Keep
     * the initial desktop small enough to coexist with that physical pool. */
    if(mm_total_bytes()<25U*1024U*1024U) {
        uint16_t w=mm_total_bytes()<17U*1024U*1024U?640U:800U;
        uint16_t h=w==640U?480U:600U;
        /* VMware/VirtIO expose a 32-bit scanout. Retain the small geometry
         * there too; Vision applies its 8-bit software output separately. */
        bool initial_mode=gfx_set_display_mode(w,h,8U);
        if(!initial_mode)initial_mode=gfx_set_display_mode(w,h,32U);
        if(initial_mode)
            kprintf("[MM] escritorio inicial %ux%u, salida de 8 bits para RAM limitada\n",w,h);
    }
    {
        const gfx_info_t *boot_graphics = gfx_get_info();
        bool keep_default_256 = boot_graphics &&
            boot_graphics->width == 640U &&
            boot_graphics->height == 480U &&
            boot_graphics->bpp == 8U;

        if (!keep_default_256 && compat_mode_force_vga13h() && !gfx_set_mode13h())
        kprintf("[COMPAT] no se pudo forzar VGA 320x200x8\n");
        else if (!keep_default_256 && compat_mode_force_vga12h() && !gfx_set_mode12h())
        kprintf("[COMPAT] no se pudo forzar VGA 640x480x4\n");
    }
    /* El splash vive en VRAM y no mantiene un backbuffer adicional: también
       se conserva durante el arranque del perfil de 8 MiB. */

    /* GRAPHICS.PAK se leía por primera vez mientras PrintSpool todavía
       terminaba su arranque Ring 3. Precargar aquí hace determinista el
       paquete y su caché antes de permitir I/O concurrente de servicios. */
    if (compat_mode_allow_icon_images())
        (void)bk_graphics_preload_boot_icons();
    gui_alert_resources_init();

    /* Precarga los sonidos cortos antes de Vision. Desde este punto abrir,
       cerrar o redimensionar ventanas no realiza I/O FAT en la ruta GUI. */
    system_sounds_init();

    /* Inicie PrintSpool sin bloquear la construcción del escritorio. */
    if (compat_mode_allow_optional_services()) {
        bootsplash_show("LOADING PRINT SERVICE", 68);
        if(admin_enabled(2)) (void)kernel_start_print_spooler();
    } else {
        kprintf("[COMPAT] PrintSpool omitido para ahorrar RAM\n");
    }

    bootsplash_show("STARTING VISION", 70);
    gui_set_setup_mode(graphical_setup_boot);
    int vision_pid = kernel_start_vision();
    if (vision_pid < 0 || !kernel_wait_vision_ready(vision_pid)) {
        kprintf("[VISION] el servicio grafico no pudo iniciar; entrando a recovery\n");
        kernel_recovery_shell("Vision no arranco o excedio timeout");
    }
    admin_ready(1);
    bootsplash_debug("kernel_main: Vision READY");
    kernel_report_ram("DESKTOP");


    /*
     * Arranque SMP tardio:
     *
     * La primera implementacion encendia el Local APIC y dejaba los AP
     * esperando antes de montar FAT, cargar ICONS.PAK y construir el
     * escritorio. Ademas, task_preempt_* consultaba el APIC ID por MMIO en
     * cada operacion de heap/FAT. En KVM -cpu host esa combinacion podia
     * convertir la precarga de iconos en una pausa aparente o permanente.
     *
     * La inicializacion del escritorio es necesariamente monohilo. Iniciar
     * los AP justo antes del bucle GUI conserva el boot probado de un solo
     * CPU y activa el paralelismo donde realmente puede utilizarse.
     */
    if (!compat_mode_is_low_memory()) {
        kprintf("[SMP] escritorio listo; iniciando procesadores secundarios\n");
        smp_init();
        smp_start_scheduler();
    } else {
        kprintf("[SMP] desactivado por perfil de memoria reducido\n");
    }
    kprintf("[BKBOOT] VISION ONLINE pid=%d\n", vision_pid);
    /* Wine 11.0 is activated by its launcher on the first explicit request,
     * never by kernel boot. Failure of an optional Windows program cannot
     * strand Vision behind the splash or force Recovery. */
    kprintf("[WINE:BOOT] deferred until Windows program launch\n");
    task_set_current_console_route(0U);
    bootsplash_disable();
    gui_request_paint();
    kprintf("[BKBOOT] DESKTOP RELEASED\n");
    admin_desktop_ready();
    if (graphical_setup_boot) {
        if (!setup_boot_launch())
            kprintf("[SETUP] El asistente grafico no pudo iniciarse.\n");
    } else if (compat_mode_allow_startup_sound()) {
        kprintf("[BKBOOT] QUEUE STARTUP SOUND\n");
        startup_sound_play();
    } else {
        kprintf("[COMPAT] sonido de inicio omitido\n");
    }

    /* El boot task ya no ejecuta el compositor: Vision posee el GUI loop. */
    uint32_t ram_report_at = pit_get_ticks();
    bool ram_reported = false;
    for (;;) {
        if (!ram_reported && pit_get_ticks() - ram_report_at >=
                5U * pit_get_frequency_hz()) {
            kernel_report_ram("IDLE");
            ram_reported = true;
        }
        int32_t vision_status = 0;
        int waited = task_waitpid((uint32_t)vision_pid, &vision_status);
        if (waited > 0 || (waited < 0 && !gui_vision_owner_pid())) {
            kprintf("[VISION] servicio detenido status=%d; reiniciando servidor\n",
                    vision_status);
            /* A fault in the main thread must also end its screensaver and
             * other workers before freeing code or admitting the new owner. */
            (void)task_request_exit_process((uint32_t)vision_pid,vision_status);
            while(gui_vision_owner_pid()==(uint32_t)vision_pid) {
                task_reap_deferred();
                task_sleep(1U);
            }
            task_sleep(pit_get_frequency_hz()/10U+1U);
            for(;;) {
                vision_pid=kernel_start_vision();
                if(vision_pid>=0 && kernel_wait_vision_ready(vision_pid)) {
                    admin_ready(1);
                    gui_request_paint();
                    kprintf("[VISION] RESTART READY pid=%d\n",vision_pid);
                    break;
                }
                /* Keep the kernel, existing applications and storage alive
                 * even when the service image cannot currently be loaded. */
                if(vision_pid>=0) {
                    (void)task_request_exit_process((uint32_t)vision_pid,1);
                    while(gui_vision_owner_pid()==(uint32_t)vision_pid) {
                        task_reap_deferred();task_sleep(1U);
                    }
                }
                kprintf("[VISION] reintento en 2 segundos\n");
                task_sleep(2U*pit_get_frequency_hz()+1U);
            }
        }
        task_sleep(1U);
    }
}
