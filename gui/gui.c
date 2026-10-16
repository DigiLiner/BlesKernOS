#include "../kernel/include/file_dialog.h"
#include "../kernel/include/vision_resources.h"
#include "../kernel/include/vision_ipc.h"
#define BK_API_IMPLEMENTATION
#include "../kernel/include/api.h"
#include "gui.h"
#include "../sdk/include/bleskernos_input.h"
#include "../programs/programs.h"
#include "../kernel/include/gfx.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/vga.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/task.h"
#include "../kernel/include/kernel_domains.h"
#include "../kernel/include/bootsplash.h"
#include "../kernel/include/user_config.h"
#include "../kernel/include/mouse.h"
#include "../kernel/include/vfs.h"
#include "../kernel/include/memory.h"
#include "../kernel/include/perfmon.h"
#include "../kernel/include/compat_mode.h"
#include "../kernel/include/system_sounds.h"
#include "../kernel/libc/include/string.h"
#include "../kernel/libc/include/stdio.h"

static gui_desktop_t *g_desktop;
static volatile uint32_t g_last_input_tick;
/*
 * Un booleano pierde solicitudes si una app termina de pintar mientras el
 * compositor esta consumiendo/limpiando la solicitud anterior. Un contador
 * permite distinguir cada invalidacion, incluso si llega durante un frame.
 */
static volatile uint32_t g_paint_generation;
/* Counts GUI loop turns before reloading the wallpaper after a mode change.
   A Ring-3 display callback sets it while still returning to the scheduler;
   delaying one extra turn keeps VFS/bitmap work out of that transaction. */
static volatile uint32_t g_mode_layout_refresh_pending;
static bool g_setup_mode;
static volatile uint32_t g_vision_owner_pid;
static volatile bool g_vision_ready;
static volatile bool g_vision_layout_refresh_pending;

static int gui_parse_int_value(const char *text, int fallback) {
    int value = 0;
    bool seen_digit = false;

    if (!text) return fallback;
    if (*text == '+') text++;
    while (*text >= '0' && *text <= '9') {
        seen_digit = true;
        value = value * 10 + (*text - '0');
        text++;
    }
    return seen_digit ? value : fallback;
}

static void gui_load_mouse_preferences(void) {
    void *config = NULL;
    uint32_t config_size = 0;
    bool trail_enabled = gui_desktop_cursor_trail_enabled(g_desktop);

    if (!bk_user_config_read_all(BK_MOUSE_CONFIG_PATH,
                                 BK_MOUSE_CONFIG_LEGACY_PATH,
                                 &config, &config_size) || !config)
        return;
    (void)config_size;

    {
        char *line = (char *)config;

        while (*line) {
            char *end = line;
            char *eq = NULL;
            char saved;

            while (*end && *end != '\r' && *end != '\n') {
                if (*end == '=' && !eq) eq = end;
                end++;
            }
            saved = *end;
            *end = '\0';

            if (eq) {
                int value;

                *eq++ = '\0';
                value = gui_parse_int_value(eq, 0);
                if (kstrcmp(line, "sensitivity") == 0) {
                    mouse_set_sensitivity((uint8_t)((value >= 1 && value <= 5)
                        ? value : mouse_get_sensitivity()));
                } else if (kstrcmp(line,"swap") == 0) {
                    (void)bk_input_preference_set(BK_INPUT_SWAP_BUTTONS,(uint32_t)value);
                } else if (kstrcmp(line,"double_click") == 0) {
                    (void)bk_input_preference_set(BK_INPUT_DOUBLE_CLICK,(uint32_t)value);
                } else if (kstrcmp(line,"scheme") == 0) {
                    (void)bk_input_preference_set(BK_INPUT_CURSOR_SCHEME,(uint32_t)value);
                } else if (kstrcmp(line, "trail") == 0) {
                    trail_enabled = value != 0;
                }
            }

            *end = saved;
            line = end;
            while (*line == '\r' || *line == '\n') line++;
        }
    }

    kfree(config);
    gui_desktop_set_cursor_trail(g_desktop, trail_enabled);
}

static void gui_load_keyboard_preferences(void) {
    void *config=NULL; uint32_t size=0;
    if (!bk_user_config_read_all("/SYSTEM/USER/CONFIG/KEYBD.INI","/KEYBD.INI",&config,&size) || !config) return;
    char *line=config;
    while (*line) {
        char *end=line,*eq=NULL;
        while (*end && *end!='\r' && *end!='\n') { if (*end=='=' && !eq) eq=end; end++; }
        char saved=*end;*end=0;
        if (eq) {
            *eq++=0;uint32_t value=(uint32_t)gui_parse_int_value(eq,0);
            if (!kstrcmp(line,"layout")) (void)bk_input_preference_set(BK_INPUT_LAYOUT,!kstrcmp(eq,"ES"));
            else if (!kstrcmp(line,"delay")) (void)bk_input_preference_set(BK_INPUT_REPEAT_DELAY,value);
            else if (!kstrcmp(line,"interval")) (void)bk_input_preference_set(BK_INPUT_REPEAT_INTERVAL,value);
            else if (!kstrcmp(line,"blink")) (void)bk_input_preference_set(BK_INPUT_CARET_BLINK,value);
        }
        *end=saved;line=end;while (*line=='\r'||*line=='\n') line++;
    }
    kfree(config);
}

static void gui_load_display_preferences(void) {
    void *config = NULL;
    uint32_t config_size = 0;
    bool outline_enabled = true;

    if (bk_user_config_read_all(BK_DISPLAY_CONFIG_PATH,
                                BK_DISPLAY_CONFIG_LEGACY_PATH,
                                &config, &config_size) && config) {
        char *line = (char *)config;
        (void)config_size;

        while (*line) {
            char *end = line;
            char *eq = NULL;
            char saved;

            while (*end && *end != '\r' && *end != '\n') {
                if (*end == '=' && !eq) eq = end;
                end++;
            }
            saved = *end;
            *end = '\0';
            if (eq) {
                *eq++ = '\0';
                if (kstrcmp(line, "drag_outline") == 0)
                    outline_enabled = gui_parse_int_value(eq, 1) != 0;
            }
            *end = saved;
            line = end;
            while (*line == '\r' || *line == '\n') line++;
        }
        kfree(config);
    }

    gui_desktop_set_drag_outline(g_desktop, outline_enabled);
}

void gui_set_setup_mode(bool enabled) {
    g_setup_mode = enabled;
}

bool gui_setup_mode(void) {
    return g_setup_mode;
}

uint8_t gui_get_cpu_usage(void) {
    return task_cpu_usage();
}

gui_desktop_t *gui_get_desktop(void) {
    return g_desktop;
}

uint32_t gui_get_last_input_tick(void) {
    return g_last_input_tick;
}

void gui_request_paint(void) {
    __asm__ volatile ("lock; incl %0"
                      : "+m"(g_paint_generation) : : "memory", "cc");
}

bool gui_change_resolution(gui_desktop_t *desktop, uint16_t width,
                           uint16_t height, uint8_t bpp) {
    const gfx_info_t *info = gfx_get_info();
    gui_desktop_t *real_desktop = gui_get_desktop();
    uint16_t old_width;
    uint16_t old_height;
    uint8_t old_hardware_bpp;
    bool old_16_colors;
    uint8_t old_output_depth;
    bool software_colors = false;
    bool use_16_colors;
    uint8_t hardware_bpp;

    /* Ring 3 receives a geometry-only desktop proxy.  It must never become
       the compositor target: modifying that copy switches the hardware while
       the real desktop keeps its old surface, leaving only the clear color.
       BlesKernOS has one active desktop, so resolve every public request to
       that authoritative instance. */
    if (real_desktop) desktop = real_desktop;
    if (!desktop || !info || !gfx_can_change_mode()) return false;
    if (compat_mode_is_tiny() &&
        (width != 320U || height != 200U || (bpp != 0U && bpp != 8U)))
        return false;

    /* Serialize hardware/backbuffer replacement with the compositor. Do not
       render synchronously here: this API is normally entered from a Ring-3
       widget callback. Painting from inside that callback queued more Ring-3
       paints to the same owner while preemption was blocked, leaving only the
       newly cleared desktop background visible. */
    gui_desktop_paint_lock();
    old_width = info->width;
    old_height = info->height;
    old_hardware_bpp = info->bpp;
    old_16_colors = gui_gfx_16_color_mode();
    old_output_depth = gui_gfx_output_depth();
    if (!bpp) bpp = gui_display_color_depth();
    if (info->bpp == 32U && width == info->width && height == info->height &&
        (info->mode == GFX_MODE_VMWARE_SVGA || info->mode == GFX_MODE_VIRTIO_GPU) &&
        (bpp == 4U || bpp == 8U || bpp == 16U || bpp == 32U)) {
        /* Palette-only changes must not reallocate an identical backbuffer.
           A second contiguous surface may not fit in a fragmented 32 MiB heap. */
        gui_gpu_compositor_shutdown();
        gui_gfx_set_16_color_mode(bpp == 4U);
        gui_gfx_set_output_depth(bpp == 32U ? 0U : bpp);
        gui_desktop_reset_after_mode_change(desktop);
        gui_desktop_paint_unlock();
        gui_request_paint();
        kprintf("[GUI:COLORS] output=%u hardware=32; existing buffers retained\n", bpp);
        return true;
    }
    use_16_colors = bpp == 4;
    /* VGA 12h is genuinely planar 4 bpp. VESA/SVGA backends emulate the
       same palette through indexed 8 bpp when they do not expose 4 bpp. */
    hardware_bpp = use_16_colors &&
        (info->mode == GFX_MODE_VGA_13H || info->mode == GFX_MODE_VGA_12H)
        ? 4 : (use_16_colors ? 8 : bpp);
    kprintf("[GUI:MODE] solicitud %ux%ux%u desde %ux%ux%u pid=%u\n",
            width, height, hardware_bpp, old_width, old_height,
            old_hardware_bpp, task_current_pid());

    /* GPU compositor surfaces are tied to the old transport/mode generation.
       Drop them before the driver tears that generation down. */
    gui_gpu_compositor_shutdown();
    if (!gfx_set_display_mode(width, height, hardware_bpp)) {
        if ((bpp != 4U && bpp != 8U && bpp != 16U) ||
            !gfx_set_display_mode(width, height, 32U)) {
            kprintf("[GUI:MODE] FAIL hardware\n");
            gui_desktop_paint_unlock();
            return false;
        }
        software_colors = true;
        hardware_bpp = 32U;
    }
    info = gfx_get_info();
    if (!info || !info->width || !info->height || !info->bpp || !info->pitch) {
        kprintf("[GUI:MODE] FAIL geometria driver got=%ux%ux%u pitch=%u "
                "wanted=%ux%ux%u pitch_min=%u\n",
                info ? info->width : 0U, info ? info->height : 0U,
                info ? info->bpp : 0U, info ? info->pitch : 0U,
                width, height, hardware_bpp,
                (uint32_t)width * ((uint32_t)hardware_bpp + 7U) / 8U);
        (void)gfx_set_display_mode(old_width, old_height, old_hardware_bpp);
        gui_desktop_paint_unlock();
        return false;
    }
    /* El backend ya leyó los registros del adaptador y confirmó la operación.
       Algunos SVGA ajustan pitch o profundidad internamente; rechazar esa
       geometría publicada convertía un cambio exitoso en una vuelta forzada
       a 640x480. Reconfigurar el escritorio con los valores reales. */
    if (info->width != width || info->height != height ||
        info->bpp != hardware_bpp) {
        kprintf("[GUI:MODE] geometria ajustada por driver: %ux%ux%u\n",
                info->width, info->height, info->bpp);
        width = info->width;
        height = info->height;
        hardware_bpp = info->bpp;
    }

    kprintf("[GUI:MODE] hardware OK; reservando backbuffer\n");
    gui_gfx_set_16_color_mode(use_16_colors);
    gui_gfx_set_output_depth(software_colors ? bpp : 0U);
    if (!gui_gfx_reconfigure(&desktop->surface)) {
        (void)gfx_set_display_mode(old_width, old_height, old_hardware_bpp);
        gui_gfx_set_16_color_mode(old_16_colors);
        gui_gfx_set_output_depth(old_output_depth);
        /* The constrained-memory reconfigure may have released the previous
           backbuffer before retrying. Recreate it after restoring hardware. */
        if (!desktop->surface.pixels)
            (void)gui_gfx_reconfigure(&desktop->surface);
        kprintf("[GUI:MODE] FAIL backbuffer; modo anterior restaurado\n");
        gui_desktop_paint_unlock();
        return false;
    }

    kprintf("[GUI:MODE] backbuffer OK; reflow e invalidacion diferida\n");
    /* Start from a deterministic surface even though the normal GUI loop owns
       the first full repaint after this Ring-3 callback returns.  SVGA limpia
       o reutiliza su scanout al escribir WIDTH/HEIGHT; dejar esta imagen solo
       en RAM hasta el próximo frame hacía que se vieran filas de la VRAM del
       modo anterior (o negro) durante la transición. Publicar el fondo entero
       ahora establece una base válida con el pitch nuevo. */
    gui_gfx_clear(&desktop->surface, deskmanager_get_background());
    gui_gfx_present(&desktop->surface);
    gui_desktop_reflow(desktop);
    gui_desktop_reset_after_mode_change(desktop);
    /* No recargue wallpaper ni enumere discos aqui. Esta funcion puede ser
       invocada desde un callback Ring 3 y esas rutas toman VFS/heap mientras
       el cambio grafico sigue en curso. El siguiente compositor reutiliza el
       fondo ya residente (o el color de escritorio) y por eso siempre puede
       presentar un frame completo sin depender del filesystem. */
    gui_gfx_invalidate_front();
    /* Rebase the input sampler after mouse_set_bounds() clamped coordinates.
       Otherwise the old button/position state can synthesize a click or a huge
       movement immediately after the mode switch. */
    gui_desktop_paint_unlock();
    __sync_lock_test_and_set(&g_mode_layout_refresh_pending, 2U);
    /* Nunca componga desde este callback: desktop_paint puede encolar la
       pintura de la misma aplicacion Ring 3 que aun no regreso. Eso deja al
       task actual esperando su propio upcall y congela tanto eventos como el
       escritorio en el color base. reset_after_mode_change ya invalida toda
       la pantalla; el bucle GUI hara el primer frame al recuperar control. */
    gui_request_paint();
    kprintf("[GUI:MODE] OK %ux%ux%u; repaint pendiente\n",
            width, height, hardware_bpp);
    return true;
}

uint8_t gui_display_color_depth(void) {
    const gfx_info_t *info = gfx_get_info();
    if (gui_gfx_output_depth()) return gui_gfx_output_depth();
    if (gui_gfx_16_color_mode()) return 4;
    return info ? info->bpp : 0;
}

bool gui_vision_ready(void) {
    return g_vision_ready;
}

uint32_t gui_vision_owner_pid(void) {
    return g_vision_owner_pid;
}

gui_desktop_t *gui_vision_desktop(void) {
    if (!g_desktop || g_vision_owner_pid != task_current_process_id())
        return NULL;
    return g_desktop;
}

bool gui_vision_setup_mode(void) {
    return g_setup_mode;
}

bool gui_vision_take_layout_refresh(void) {
    if (g_vision_owner_pid != task_current_process_id()) return false;
    return __sync_lock_test_and_set(&g_vision_layout_refresh_pending, false);
}

/* Frame timing and composition state are private to VISION.BEX. */
static struct {bool initialized;} g_vision_loop;
static bool g_gui_engine_ready;
static bool g_gui_shell_ready;

static bool gui_initialize_engine_internal(void) {
    gui_surface_t surface;

    if (g_gui_engine_ready) return true;

    /* The authoritative desktop must be visible to the trusted Vision Ring-3
     * service. Keep ordinary applications on the geometry-only proxy; only
     * Vision can obtain this pointer through bk_vision_desktop(). */
    if (!g_desktop) {
        g_desktop = (gui_desktop_t *)kzalloc(sizeof(*g_desktop));
        if (!g_desktop) {
            kprintf("[VISION] sin memoria para desktop compartido\n");
            return false;
        }
        (void)mm_set_allocation_owner(g_desktop, 0U);
    }

    if(mm_total_bytes()<25U*1024U*1024U)gui_gfx_set_output_depth(8U);
    if (!gui_gfx_init(&surface)) {
        kprintf("[GUI] No se pudo iniciar modo grafico.\n");
        return false;
    }

    bootsplash_show("@H31F338DB", 74);
    gui_desktop_init(g_desktop, surface);
    gui_load_mouse_preferences();
    gui_load_keyboard_preferences();
    gui_load_display_preferences();
    if (compat_mode_is_low_memory()) {
        gui_desktop_set_cursor_trail(g_desktop, false);
        gui_desktop_set_drag_outline(g_desktop, true);
    }
    g_last_input_tick = pit_get_ticks();
    gui_request_paint();
    g_gui_engine_ready = true;
    return true;
}

static bool gui_start_shell_internal(void) {
    if (g_gui_shell_ready) return true;
    if (!g_gui_engine_ready) return false;

    /* Vision v3 installs DeskManager/Deskbar from VISION.BEX itself. The
     * kernel only records that the service completed shell installation. */
    g_gui_shell_ready = true;
    return true;
}

bool gui_vision_claim_service(void) {
    uint32_t pid = task_current_process_id();
    uint32_t previous;

    if (!pid) return false;
    previous = __sync_val_compare_and_swap(&g_vision_owner_pid, 0U, pid);
    if (previous != 0U && previous != pid) {
        kprintf("[VISION] servicio ya activo en pid=%u\n", previous);
        return false;
    }
    g_vision_ready = false;
    kprintf("[VISION] ownership grafico adquirido pid=%u\n", pid);
    return true;
}

bool gui_vision_initialize_engine(void) {
    if (g_vision_owner_pid != task_current_process_id()) return false;
    if (!gui_initialize_engine_internal()) return false;
    g_vision_loop.initialized=true;
    return true;
}

bool gui_vision_start_shell(void) {
    if (g_vision_owner_pid != task_current_process_id()) return false;
    return gui_start_shell_internal();
}

void gui_vision_publish_ready(void) {
    if (g_vision_owner_pid != task_current_process_id()) return;
    if (!g_gui_engine_ready || !g_gui_shell_ready) return;
    g_vision_ready = true;
    vision_ipc_server_online(g_vision_owner_pid);
    kprintf("[VISION] READY: motor, escritorio y compositor bajo Vision\n");
}

/* Retain the old private ABI token so mixed binaries fail cleanly. The
 * compositor is no longer a callable kernel loop. */
bool gui_vision_step(void) {return false;}

void gui_vision_process_cleanup(uint32_t pid) {
    if (!pid || g_vision_owner_pid != pid) return;
    vision_ipc_server_offline(pid);
    g_vision_ready = false;
    g_vision_loop.initialized = false;
    g_gui_shell_ready = false;
    g_vision_layout_refresh_pending = false;
    __sync_bool_compare_and_swap(&g_vision_owner_pid, pid, 0U);
    kprintf("[VISION] ownership grafico liberado pid=%u\n", pid);
}

void gui_vision_release_service(void) {
    gui_vision_process_cleanup(task_current_process_id());
}

void gui_init(void) {
    if (!gui_initialize_engine_internal()) return;
    (void)gui_start_shell_internal();
}

void gui_run(void) {
    /* Compatibility entry: the boot supervisor launches the Ring-3 server. */
    while(true)task_sleep(1U);
}

/* Frame locks span CPL3 execution. Explicit ownership permits abnormal-exit
 * cleanup to release them before any reaper attempts to retire GUI objects. */
static volatile uint32_t g_vision_frame_tid;
static volatile uint32_t g_vision_waiting_tid;
bool gui_vision_frame_waiting(void) {
    return g_vision_ready && g_vision_waiting_tid &&
           g_vision_waiting_tid==g_vision_owner_pid;
}
#define VISION_FRAME_DOMAINS (KDOMAIN_TASK|KDOMAIN_VFS|KDOMAIN_GUI|KDOMAIN_GFX)
void bk_vision_worker_begin(void) {
    if(task_current_process_id()==g_vision_owner_pid && task_current_pid()!=g_vision_owner_pid)
        kernel_domains_enter(VISION_FRAME_DOMAINS);
}
void bk_vision_worker_end(void) {
    if(task_current_pid()!=g_vision_owner_pid)kernel_domains_exit(VISION_FRAME_DOMAINS);
}
bool bk_vision_frame_begin(bk_vision_frame_t *frame) {
    uint32_t tid=task_current_pid();
    if(!frame || tid!=g_vision_owner_pid || !g_vision_ready ||
       g_vision_frame_tid || bootsplash_active())return false;
    vision_file_dialog_poll();
    g_vision_waiting_tid=tid;
    kernel_domains_enter(VISION_FRAME_DOMAINS);
    gui_desktop_paint_lock();
    g_vision_waiting_tid=0U;
    g_vision_frame_tid=tid;task_block_user_upcalls(true);
    uint32_t mode=__sync_lock_test_and_set(&g_mode_layout_refresh_pending,0U);
    frame->layout_refresh=false;
    if(mode>1U)__sync_lock_test_and_set(&g_mode_layout_refresh_pending,mode-1U);
    else if(mode==1U) {
        frame->layout_refresh=true;
        gui_desktop_invalidate_all(g_desktop);gui_request_paint();
    }
    frame->ticks=pit_get_ticks();frame->frequency=pit_get_frequency_hz();
    frame->paint_generation=g_paint_generation;frame->last_input=g_last_input_tick;
    return true;
}
void bk_vision_frame_end(void) {
    if(g_vision_frame_tid!=task_current_pid())return;
    if(!vision_resources_checkpoint(g_desktop))task_exit_status(2);
    task_block_user_upcalls(false);g_vision_frame_tid=0;
    gui_desktop_paint_unlock();kernel_domains_exit(VISION_FRAME_DOMAINS);
}
void bk_vision_note_input(void) {
    if(g_vision_frame_tid==task_current_pid())g_last_input_tick=pit_get_ticks();
}
void gui_vision_abort_frame(uint32_t tid) {
    if(tid && __sync_bool_compare_and_swap(&g_vision_frame_tid,tid,0))
        gui_desktop_abandon_paint_lock(tid);
}
