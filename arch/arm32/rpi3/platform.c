#include "uart.h"
#include "exceptions.h"
#include "irq.h"
#include "timer.h"
#include "mmu.h"
#include "physmem.h"
#include "mailbox.h"
#include "framebuffer.h"
#include "sd.h"
#include "os_bridge.h"
#include "ui_stage8.h"
#include "input_stage9.h"
#include "audio_pwm.h"
#include "task_arm.h"
#include "elf_arm.h"
#include "user_stage12_apps.h"
#include "../../../kernel/include/bootsplash.h"
#include "../../../kernel/include/types.h"
#include "../../../kernel/include/task.h"
#include "../../../kernel/include/language.h"
#include "../../../kernel/include/network.h"
#include "../../../kernel/include/driver.h"
#include "../../../kernel/include/sound.h"
#include "../../../kernel/libc/include/stdio.h"

extern const bk_driver_module_t *bleskernos_driver_query(void);

static uint32_t g_last_heartbeat;
static uint8_t g_sector0[512] __attribute__((aligned(4)));

static void stage10_idle_task(void *argument) {
    (void)argument;
    for (;;) __asm__ volatile("wfi");
}

static void stage10_gui_task(void *argument) {
    (void)argument;
    rpi3_uart_puts("[TASK:ARM] gui task running\n");
    for (;;) {
        rpi3_stage8_service();
        task_sleep(1U);
    }
}

/* Keep USB HID polling independent from compositor/userland work.  Stage 12
 * originally polled DWC2 only from the GUI service path; once native user
 * processes were added, long GUI/syscall bursts could stretch HID polling
 * enough for QEMU's relative mouse reports to appear frozen. */
static void stage12_input_task(void *argument) {
    (void)argument;
    rpi3_uart_puts("[TASK:ARM] input task running (USB poll @100Hz)\n");
    for (;;) {
        rpi3_stage9_input_poll();
        sound_poll();
        task_sleep(1U);
    }
}

static void stage10_heartbeat_task(void *argument) {
    (void)argument;
    for (;;) {
        task_sleep(RPI3_TIMER_HZ);
        rpi3_uart_puts("[TASK:ARM] heartbeat tick=");
        rpi3_uart_puthex(rpi3_timer_ticks());
        rpi3_uart_puts(" switches=");
        rpi3_uart_puthex(rpi3_task_switch_count());
        rpi3_uart_puts(" live=");
        rpi3_uart_puthex(rpi3_task_live_count());
        rpi3_uart_puts("\n");
    }
}

#if defined(RPI3_BEX_SMOKE_TEST)
static void stage13_bex_smoke_task(void *argument) {
    static const char *paths[] = {
        "/SYSTEM/COMMANDS/cpuinfo.BEX",
        "/SYSTEM/COMMANDS/ps.BEX"
    };
    (void)argument;
    task_sleep(20U);
    for (uint32_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int32_t status = -1;
        int pid = rpi3_elf_spawn(paths[i], paths[i]);
        if (pid < 0) {
            kprintf("[SMOKE:BEX] FAIL path=%s reason=%s\n",
                    paths[i], rpi3_elf_last_error());
            continue;
        }
        while (task_waitpid((uint32_t)pid, &status) == 0) task_yield();
        kprintf("[SMOKE:BEX] path=%s pid=%u status=%d\n",
                paths[i], (uint32_t)pid, status);
    }
    {
        static const char *gui_paths[] = {
            "/SYSTEM/PROGRAMS/CALCULATOR.BEX",
            "/SYSTEM/PROGRAMS/FILE.BEX",
            "/SYSTEM/PROGRAMS/PROCESSMANAGER.BEX"
        };
        for (uint32_t i = 0U; i < sizeof(gui_paths) / sizeof(gui_paths[0]); ++i) {
            int pid = rpi3_elf_spawn(gui_paths[i], NULL);
            if (pid < 0)
                kprintf("[SMOKE:GUI] FAIL path=%s reason=%s\n",
                        gui_paths[i], rpi3_elf_last_error());
            else kprintf("[SMOKE:GUI] path=%s pid=%u\n",
                         gui_paths[i], (uint32_t)pid);
        }
        /* Dé tiempo a callbacks de contenido, eventos y refrescos periódicos.
           La compilación de smoke vive sólo durante la prueba automatizada. */
        task_sleep(5U * RPI3_TIMER_HZ);
        kprintf("[SMOKE:GUI] callbacks survived 5 seconds\n");
    }
    task_exit();
}
#endif

static void stage10_start_scheduler(void) NORETURN;
static void stage10_start_scheduler(void) {
    int idle_pid, gui_pid, input_pid, heartbeat_pid;
    rpi3_task_init();
    rpi3_uart_puts("[LOCK:ARM] kmutex + krwlock + GUI serialization ONLINE\n");
    idle_pid = rpi3_task_create("idle", stage10_idle_task, NULL, true);
    gui_pid = task_create_kernel("gui", stage10_gui_task, NULL);
    input_pid = task_create_kernel("input", stage12_input_task, NULL);
    heartbeat_pid = task_create_kernel("heartbeat", stage10_heartbeat_task, NULL);
    {
        const bk_driver_module_t *netstack = bleskernos_driver_query();
        if (netstack && netstack->init && netstack->init()) {
            rpi3_uart_puts("[NET:ARM] IPv4/DHCP/DNS/TCP/HTTP stack ONLINE\n");
            network_start_autoconfigure();
        }
    }
#if defined(RPI3_BEX_SMOKE_TEST)
    (void)task_create_kernel("bex-smoke", stage13_bex_smoke_task, NULL);
#endif
    if (idle_pid < 0 || gui_pid < 0 || input_pid < 0 || heartbeat_pid < 0) {
        rpi3_uart_puts("[TASK:ARM] scheduler task creation FAILED idle=");
        rpi3_uart_puthex((uint32_t)idle_pid);
        rpi3_uart_puts(" gui=");
        rpi3_uart_puthex((uint32_t)gui_pid);
        rpi3_uart_puts(" input=");
        rpi3_uart_puthex((uint32_t)input_pid);
        rpi3_uart_puts(" heartbeat=");
        rpi3_uart_puthex((uint32_t)heartbeat_pid);
        rpi3_uart_puts("\n");
        for (;;) {
            rpi3_stage9_input_poll();
            rpi3_stage8_service();
            __asm__ volatile("wfi");
        }
    }
    /* Stage 11/12 used to auto-launch ARMTEST.BEX and uname here.  Those
       smoke tests were useful during bring-up but surprising in a normal
       desktop boot, so native programs now run only after explicit user action. */
    rpi3_uart_puts("[BLES] Stage 12 ARM scheduler + USR/SVC/ELF + native apps ONLINE\n");
    rpi3_uart_puts("[TASK:ARM] GUI + input + idle + heartbeat scheduled; user apps are on-demand\n");
    rpi3_task_start();
}

static void print_dec(uint32_t value) {
    char digits[11];
    uint32_t n = 0U;
    if (value == 0U) { rpi3_uart_putc('0'); return; }
    while (value != 0U && n < sizeof(digits)) {
        digits[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (n != 0U) rpi3_uart_putc(digits[--n]);
}

void rpi3_boot_main(uint32_t device_tree) {
    const rpi3_framebuffer_t *fb;
    const rpi3_sd_info_t *sd;
    uint32_t arm_base = 0U, arm_bytes = 0U;
    int mmu_status;
    int rc;

    rpi3_uart_init();
    rpi3_uart_puts("\n[BlesKernOS] ARM32 Raspberry Pi 3 Stage 13\n");
    rpi3_uart_puts("[ARCH] AArch32 ARMv7-A baseline (Pi 3 compatible)\n");
    rpi3_uart_puts("[BOOT] DTB pointer: ");
    rpi3_uart_puthex(device_tree);
    rpi3_uart_puts("\n");

    arm_exceptions_init();
    rpi3_uart_puts("[EXC] VBAR installed\n");

    mmu_status = rpi3_mmu_init();
    if (mmu_status != 0) {
        rpi3_uart_puts("[MMU] init FAILED status=");
        rpi3_uart_puthex((uint32_t)mmu_status);
        rpi3_uart_puts("\n");
        for (;;) __asm__ volatile("wfe");
    }
    rpi3_uart_puts("[MMU] ON, identity map + Device MMIO, TTBR0=");
    rpi3_uart_puthex(rpi3_mmu_ttbr0());
    rpi3_uart_puts("\n");

    rpi3_physmem_init();
    rpi3_uart_puts("[PMM] bootstrap pages: ");
    rpi3_uart_puthex(rpi3_phys_base());
    rpi3_uart_puts("..");
    rpi3_uart_puthex(rpi3_phys_limit());
    rpi3_uart_puts(" free=");
    rpi3_uart_puthex(rpi3_phys_free_bytes());
    rpi3_uart_puts("\n");

    rc = rpi3_mailbox_get_arm_memory(&arm_base, &arm_bytes);
    if (rc == 0) {
        rpi3_uart_puts("[MBOX] ARM memory firmware range: ");
        rpi3_uart_puthex(arm_base);
        rpi3_uart_puts(" + ");
        rpi3_uart_puthex(arm_bytes);
        rpi3_uart_puts("\n");
    } else {
        rpi3_uart_puts("[MBOX] ARM memory query FAILED rc=");
        rpi3_uart_puthex((uint32_t)rc);
        rpi3_uart_puts("\n");
    }

    rc = rpi3_framebuffer_init(1024U, 768U, 32U);
    if (rc == 0) {
        fb = rpi3_framebuffer_get();
        rpi3_uart_puts("[FB] online ");
        print_dec(fb->width); rpi3_uart_putc('x'); print_dec(fb->height);
        rpi3_uart_puts(" pitch="); print_dec(fb->pitch);
        rpi3_uart_puts(" order=");
        rpi3_uart_puts(fb->pixel_order == 1U ? "RGB" : "BGR");
        rpi3_uart_puts(" arm="); rpi3_uart_puthex((uint32_t)(uintptr_t)fb->base);
        rpi3_uart_puts(" bus="); rpi3_uart_puthex(fb->bus_address);
        rpi3_uart_puts("\n");
        rc = rpi3_stage8_video_bootstrap();
        if (rc != 0) {
            rpi3_uart_puts("[BLES:GFX] early Stage 8 video FAILED rc=");
            rpi3_uart_puthex((uint32_t)rc);
            rpi3_uart_puts("\n");
        }
    } else {
        rpi3_uart_puts("[FB] mailbox framebuffer FAILED rc=");
        rpi3_uart_puthex((uint32_t)rc);
        rpi3_uart_puts(" (serial boot continues)\n");
    }

    rpi3_stage8_boot_status("Inicializando interrupciones...", 24U);
    rpi3_irq_init();
    rpi3_timer_init();
    rpi3_uart_puts("[IRQ] generic GPU IRQ dispatcher installed\n");
    rpi3_uart_puts("[TIMER] BCM2837 system timer C1 @ 100 Hz\n");
    g_last_heartbeat = 0U;
    rpi3_irq_enable();
    rpi3_uart_puts("[IRQ] enabled\n");

    rpi3_stage8_boot_status("Buscando almacenamiento...", 38U);
    rc = rpi3_sd_init();
    if (rc == 0) {
        sd = rpi3_sd_info();
        rpi3_uart_puts("[SD] online RCA="); rpi3_uart_puthex(sd->rca);
        rpi3_uart_puts(sd->high_capacity ? " SDHC/SDXC" : " SDSC");
        rpi3_uart_puts(" clk="); print_dec(sd->bus_clock_hz);
        rpi3_uart_puts(" Hz\n");
        rc = rpi3_sd_read_block(0U, g_sector0);
        if (rc == 0) {
            rpi3_uart_puts("[SD] LBA0 read OK signature=");
            rpi3_uart_puthex(((uint32_t)g_sector0[510] << 8) | g_sector0[511]);
            rpi3_uart_puts((g_sector0[510] == 0x55U && g_sector0[511] == 0xAAU) ? " (MBR/boot signature OK)\n" : "\n");
            /* Stage 4/5 used to paint a green SD-success strip directly into
               the framebuffer here.  Once the real bootsplash owns the screen,
               platform diagnostics must stay on serial only. */
            rpi3_stage8_boot_status("Montando sistema FAT32...", 56U);
            rc = rpi3_bles_core_init();
            if (rc == 0) {
                rpi3_uart_puts("[BLES] shared block/FAT/VFS core ONLINE\n");
                language_init();
                rpi3_uart_puts("[BLES] language/config catalog ONLINE\n");
                rpi3_stage8_boot_status("Inicializando USB e input...", 70U);
                (void)rpi3_stage9_input_init();
                (void)rpi3_audio_pwm_init();
                rpi3_stage8_boot_status("Iniciando interfaz de BlesKernOS...", 76U);
                rc = rpi3_stage8_shell_init();
                if (rc == 0) {
                    rpi3_uart_puts("[BLES] Stage 9 interactive desktop shell ONLINE\n");
                    stage10_start_scheduler();
                } else {
                    rpi3_uart_puts("[BLES] Stage 8 desktop shell FAILED rc=");
                    rpi3_uart_puthex((uint32_t)rc);
                    rpi3_uart_puts("\n");
                }
            } else {
                rpi3_uart_puts("[BLES] shared core not mounted rc=");
                rpi3_uart_puthex((uint32_t)rc);
                rpi3_uart_puts(" (hardware bootstrap remains usable)\n");
            }
        } else {
            rpi3_uart_puts("[SD] LBA0 read FAILED rc=");
            rpi3_uart_puthex((uint32_t)rc);
            rpi3_uart_puts(" last_irpt="); rpi3_uart_puthex(sd->last_interrupt);
            rpi3_uart_puts("\n");
        }
    } else {
        sd = rpi3_sd_info();
        rpi3_uart_puts("[SD] init FAILED rc="); rpi3_uart_puthex((uint32_t)rc);
        rpi3_uart_puts(" last_irpt="); rpi3_uart_puthex(sd->last_interrupt);
        rpi3_uart_puts("\n");
    }

    rpi3_uart_puts("[NEXT] Stage 13: graphical Ring-3 API/upcalls + native GUI processes\n");

    for (;;) {
        uint32_t ticks;
        __asm__ volatile("wfi");
        ticks = rpi3_timer_ticks();
        rpi3_stage9_input_poll();
        rpi3_stage8_service();
        if ((ticks - g_last_heartbeat) >= RPI3_TIMER_HZ) {
            g_last_heartbeat = ticks;
            rpi3_uart_puts("[IRQ] timer tick=");
            rpi3_uart_puthex(ticks);
            rpi3_uart_puts("\n");
        }
    }
}
