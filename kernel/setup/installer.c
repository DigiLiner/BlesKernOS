#include "../include/installer.h"
#include "../include/pit.h"
#include "../include/block.h"
#include "../include/fat.h"
#include "../include/bfs4.h"
#include "../include/iso9660.h"
#include "../include/keyboard.h"
#include "../include/memory.h"
#include "../include/pic.h"
#include "../include/types.h"
#include "../include/vfs.h"
#include "../include/vga.h"
#include "../libc/include/stdio.h"
#include "../libc/include/string.h"

#define SETUP_VERSION "1.0"
#define SETUP_MAX_TARGETS 8U
#define SETUP_MAX_ENTRIES 256U
#define SETUP_MAX_DEPTH 16U
/* ISO9660 reads at most 32 x 2048-byte sectors per bulk ATAPI command.
 * Keep memory bounded, but use the full bulk size for large files. */
#define SETUP_COPY_CHUNK (64U * 1024U)
/* Large BFS4 streams: fewer full journal transactions per file. */
#define SETUP_FAST_COPY_CHUNK (256U * 1024U)
#define SETUP_PROGRESS_STEP (1024U * 1024U)
#define SETUP_SPACE_MARGIN (16ULL * 1024ULL * 1024ULL)

#define INSTALL_STAGE2_LBA 1U
#define INSTALL_STAGE2_SECTORS 8U
#define INSTALL_KERNEL_LBA 9U
#define INSTALL_KERNEL_RESERVED_SECTORS 2560U
#define INSTALL_FSINFO_SECTOR \
    (INSTALL_KERNEL_LBA + INSTALL_KERNEL_RESERVED_SECTORS)
#define INSTALL_BACKUP_BOOT_SECTOR (INSTALL_FSINFO_SECTOR + 1U)
#define INSTALL_BACKUP_FSINFO_SECTOR (INSTALL_FSINFO_SECTOR + 2U)
#define INSTALL_RESERVED_SECTORS (INSTALL_BACKUP_FSINFO_SECTOR + 1U)

#define VGA_TEXT_MEMORY ((volatile uint16_t *)0x000B8000U)
#define UI_WIDTH 80
#define UI_HEIGHT 25

#define UI_ATTR(fg, bg) ((uint8_t)(((bg) << 4) | (fg)))
#define UI_NORMAL UI_ATTR(VGA_BLACK, VGA_LIGHT_GREY)
#define UI_DESKTOP UI_ATTR(VGA_WHITE, VGA_BLUE)
#define UI_TITLE UI_ATTR(VGA_WHITE, VGA_BLUE)
#define UI_STATUS UI_ATTR(VGA_BLACK, VGA_LIGHT_GREY)
#define UI_SELECT UI_ATTR(VGA_WHITE, VGA_BLUE)
#define UI_WARNING UI_ATTR(VGA_YELLOW, VGA_RED)
#define UI_OK UI_ATTR(VGA_LIGHT_GREEN, VGA_BLACK)
#define UI_ERROR UI_ATTR(VGA_LIGHT_RED, VGA_BLACK)

#define KEY_BYTE(c) ((uint8_t)(c))

typedef struct {
    block_device_t *device;
    char name[8];
    char type[16];
    uint32_t size_mb;
    bool floppy;
    bool partition;
    /* Detectado no equivale a apto para instalar. */
    bool occupied_mbr;
    bool blocked;
    block_device_t *parent;
    uint32_t part_index;
} installer_target_t;

typedef enum {
    INSTALL_MODE_CLEAN = 0,
    INSTALL_MODE_UPDATE = 1,
    INSTALL_MODE_REPAIR = 2
} installer_mode_t;

typedef struct {
    bool mounted;
    bool fat32;
    bool bfs4;
    bool boot_layout;
    bool has_files;
    uint32_t root_entries;
    uint32_t reserved_sectors;
    char label[12];
} installer_disk_info_t;

typedef struct {
    uint32_t files_total;
    uint32_t files_done;
    uint32_t directories;
    uint32_t bytes_total;
    uint32_t bytes_done;
    uint32_t current_file_size;
    uint32_t current_file_done;
    bool current_file_active;
    uint64_t unpacked_bytes;
    uint64_t needed_bytes;
    char current_path[VFS_MAX_PATH];
    char error[VFS_MAX_PATH];
} installer_copy_state_t;

static installer_copy_state_t g_copy;

static uint16_t ui_cell(char character, uint8_t attribute) {
    return (uint16_t)((uint16_t)attribute << 8) | (uint8_t)character;
}

static void ui_fill(int x, int y, int width, int height, char character,
                    uint8_t attribute) {
    if (x < 0 || y < 0 || width <= 0 || height <= 0) return;
    for (int row = 0; row < height && y + row < UI_HEIGHT; row++) {
        for (int column = 0; column < width && x + column < UI_WIDTH;
             column++) {
            VGA_TEXT_MEMORY[(y + row) * UI_WIDTH + x + column] =
                ui_cell(character, attribute);
        }
    }
}

static void ui_text(int x, int y, const char *text, uint8_t attribute) {
    if (!text || y < 0 || y >= UI_HEIGHT) return;
    while (*text && x < UI_WIDTH) {
        if (x >= 0)
            VGA_TEXT_MEMORY[y * UI_WIDTH + x] = ui_cell(*text, attribute);
        x++;
        text++;
    }
}

static void ui_text_clipped(int x, int y, const char *text, int width,
                            uint8_t attribute) {
    int written = 0;
    if (!text || width <= 0) return;
    while (*text && written < width) {
        char value = *text++;
        if ((uint8_t)value < 32U) value = ' ';
        if (x + written >= 0 && x + written < UI_WIDTH &&
            y >= 0 && y < UI_HEIGHT) {
            VGA_TEXT_MEMORY[y * UI_WIDTH + x + written] =
                ui_cell(value, attribute);
        }
        written++;
    }
    while (written < width) {
        if (x + written >= 0 && x + written < UI_WIDTH &&
            y >= 0 && y < UI_HEIGHT) {
            VGA_TEXT_MEMORY[y * UI_WIDTH + x + written] =
                ui_cell(' ', attribute);
        }
        written++;
    }
}

static void ui_box(int x, int y, int width, int height, uint8_t attribute) {
    if (width < 2 || height < 2) return;
    ui_fill(x, y, width, height, ' ', attribute);
    for (int column = 1; column < width - 1; column++) {
        ui_fill(x + column, y, 1, 1, '-', attribute);
        ui_fill(x + column, y + height - 1, 1, 1, '-', attribute);
    }
    for (int row = 1; row < height - 1; row++) {
        ui_fill(x, y + row, 1, 1, '|', attribute);
        ui_fill(x + width - 1, y + row, 1, 1, '|', attribute);
    }
    ui_fill(x, y, 1, 1, '+', attribute);
    ui_fill(x + width - 1, y, 1, 1, '+', attribute);
    ui_fill(x, y + height - 1, 1, 1, '+', attribute);
    ui_fill(x + width - 1, y + height - 1, 1, 1, '+', attribute);
}

static void ui_center(int y, const char *text, uint8_t attribute) {
    int length = (int)kstrlen(text ? text : "");
    int x = (UI_WIDTH - length) / 2;
    ui_text(x < 0 ? 0 : x, y, text, attribute);
}

static void ui_frame(const char *subtitle, const char *footer) {
    char title[80];
    ui_fill(0, 0, UI_WIDTH, UI_HEIGHT, ' ', UI_DESKTOP);
    ui_fill(0, 0, UI_WIDTH, 1, ' ', UI_TITLE);
    snprintf(title, sizeof(title), " BlesKernOS Setup %s ", SETUP_VERSION);
    ui_center(0, title, UI_TITLE);
    ui_box(2, 2, 76, 20, UI_NORMAL);
    if (subtitle) ui_text(5, 3, subtitle, UI_NORMAL);
    ui_fill(0, 24, UI_WIDTH, 1, ' ', UI_STATUS);
    if (footer) ui_text_clipped(2, 24, footer, 76, UI_STATUS);
    vga_set_cursor(79, 24);
}

static uint8_t ui_wait_key(void) {
    return KEY_BYTE(kbd_getchar());
}

static bool ui_wait_enter_or_escape(void) {
    for (;;) {
        uint8_t key = ui_wait_key();
        if (key == KEY_ENTER || key == '\n') return true;
        if (key == KEY_ESCAPE) return false;
    }
}

/* Avoid 32-bit overflow when displaying progress on large CD packages. */
static void ui_progress_bar(int x, int y, uint32_t width,
                            uint32_t done, uint32_t total) {
    uint32_t filled = total ?
        (uint32_t)(((uint64_t)done * width) / total) : 0U;
    if (filled > width) filled = width;
    ui_fill(x - 1, y, 1, 1, '[', UI_NORMAL);
    ui_fill(x + (int)width, y, 1, 1, ']', UI_NORMAL);
    for (uint32_t i = 0U; i < width; i++)
        ui_fill(x + (int)i, y, 1, 1, i < filled ? '#' : '.',
                i < filled ? UI_SELECT : UI_NORMAL);
}

static void ui_progress_path(const char *path) {
    const int width = 66;
    uint32_t length = (uint32_t)kstrlen(path ? path : "");
    if (length <= (uint32_t)width) {
        ui_text_clipped(7, 15, path, width, UI_NORMAL);
    } else {
        /* Keep the filename visible when the directory tree is long. */
        ui_text_clipped(7, 15, "...", 3, UI_NORMAL);
        ui_text_clipped(10, 15, path + length - (width - 3),
                        width - 3, UI_NORMAL);
    }
}

/* Counters measure synchronous ISO reads and VFS writes, not all setup
 * bookkeeping. Clock tick resolution may be coarse on older hardware. */
static uint64_t g_setup_cd_ticks, g_setup_disk_ticks;
static char g_setup_io_phase[64] = "Preparando...";

static void installer_io_phase(const char *phase) {
    kstrncpy(g_setup_io_phase, phase, sizeof(g_setup_io_phase) - 1U);
    g_setup_io_phase[sizeof(g_setup_io_phase) - 1U] = '\0';
    /* Only touch one line, no expensive full-screen redraw per chunk. */
    ui_fill(7, 21, 66, 1, ' ', UI_NORMAL);
    ui_text_clipped(7, 21, g_setup_io_phase, 66, UI_NORMAL);
}

static void installer_io_measure(bool cd, uint32_t before,
                                 const char *path, uint32_t bytes) {
    uint32_t elapsed = pit_get_ticks() - before;
    uint32_t hz = pit_get_frequency_hz();
    if (cd) g_setup_cd_ticks += elapsed;
    else g_setup_disk_ticks += elapsed;
    if (hz && elapsed >= hz * 3U)
        kprintf("[SETUP:IO] LENTO %s %u ms bytes=%u file=%s\n",
                cd ? "CD" : "IDE/VFS",
                (uint32_t)(((uint64_t)elapsed * 1000U) / hz),
                bytes, path ? path : "?");
}

static void ui_progress(uint32_t current, uint32_t total,
                        const char *message) {
    char count[80];
    uint32_t shown_file = g_copy.current_file_active ? current + 1U : current;
    if (shown_file > total) shown_file = total;

    ui_frame("Instalando BlesKernOS 1.0", "No apague el equipo ni retire el CD.");
    ui_text_clipped(6, 6, message ? message : "Procesando...", 68,
                    UI_NORMAL);

    snprintf(count, sizeof(count), "Archivos completados: %u de %u",
             current, total);
    ui_text_clipped(7, 8, count, 66, UI_NORMAL);
    ui_progress_bar(15, 10, 50U, current, total);

    snprintf(count, sizeof(count), "Archivo actual: %u de %u",
             shown_file, total);
    ui_text_clipped(7, 13, count, 66, UI_NORMAL);
    ui_progress_path(g_copy.current_path);

    if (g_copy.current_file_active || g_copy.current_file_size) {
        uint32_t percent = g_copy.current_file_size ? (uint32_t)(
            ((uint64_t)g_copy.current_file_done * 100U) /
            g_copy.current_file_size) : 100U;
        snprintf(count, sizeof(count),
                 "Archivo: %u / %u KiB (%u%%)",
                 g_copy.current_file_done / 1024U,
                 g_copy.current_file_size / 1024U +
                     (g_copy.current_file_size % 1024U != 0U), percent);
        ui_text_clipped(7, 17, count, 66, UI_NORMAL);
        ui_progress_bar(15, 19, 50U,
                        g_copy.current_file_size ? g_copy.current_file_done : 1U,
                        g_copy.current_file_size ? g_copy.current_file_size : 1U);
    } else {
        ui_text_clipped(7, 17, "Preparando siguiente archivo...", 66,
                        UI_NORMAL);
        ui_progress_bar(15, 19, 50U, 0U, 1U);
    }
    ui_text_clipped(7, 21, g_setup_io_phase, 66, UI_NORMAL);
    /* v4: one snapshot for each UI refresh; no disk I/O or per-sector logs. */
    block_io_stats_t io_stats;
    block_get_io_stats(&io_stats);
    snprintf(count, sizeof(count),
             "IDE R:%u W:%u (%u sct) CD R:%u COM:%u B",
             io_stats.ata_reads, io_stats.ata_writes,
             io_stats.ata_write_sectors, io_stats.atapi_reads,
             vga_com1_tx_bytes());
    ui_text_clipped(7, 23, count, 66, UI_NORMAL);
    uint32_t hz = pit_get_frequency_hz();
    if (hz) {
        snprintf(count, sizeof(count),
                 "Tiempo E/S: CD %u s | destino %u s (acumulados)",
                 (uint32_t)(g_setup_cd_ticks / hz),
                 (uint32_t)(g_setup_disk_ticks / hz));
        ui_text_clipped(7, 22, count, 66, UI_NORMAL);
    }
}

static void installer_halt_screen(const char *message) {
    ui_frame("Setup detenido", "Puede apagar o reiniciar el equipo.");
    ui_center(9, message ? message : "Instalador finalizado.", UI_NORMAL);
    ui_center(12, "El sistema no fue modificado despues de este punto.",
              UI_NORMAL);
    for (;;) __asm__ volatile ("hlt");
}

static void installer_reboot(void) {
    ui_fill(0, 24, UI_WIDTH, 1, ' ', UI_STATUS);
    ui_text(2, 24, "Reiniciando...", UI_STATUS);
    for (uint32_t wait = 0; wait < 100000U; wait++) io_wait();
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("hlt");
}

static uint32_t installer_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool installer_partition_type_supported(uint8_t type) {
    switch (type) {
        case 0x01U: case 0x04U: case 0x06U: case 0x0BU: case 0x0CU:
        case 0x0EU: case 0x11U: case 0x14U: case 0x16U: case 0x1BU:
        case 0x1CU: case 0x1EU: case 0x83U: /* BFS4 partition */
            return true;
        default:
            return false;
    }
}

/* Do not assume a disk is empty simply because its partition type is not
 * installable. Refuse GPT (including a damaged protective MBR), invalid MBRs,
 * extended partitions, and overlaps. All decisions are fail-closed. */
typedef enum {
    INSTALL_DISK_EMPTY, INSTALL_DISK_MBR, INSTALL_DISK_UNSAFE
} installer_table_kind_t;

static installer_table_kind_t installer_disk_table(block_device_t *device,
                                                   uint8_t *out_mbr) {
    uint8_t sector[BLOCK_SECTOR_SIZE], second[BLOCK_SECTOR_SIZE];
    bool entries = false;
    if (!device || !block_read(device, 0U, 1U, sector)) return INSTALL_DISK_UNSAFE;
    if (out_mbr) kmemcpy(out_mbr, sector, sizeof(sector));
    /* APM de Mac clasico / Tiger PPC: DDM "ER" en LBA 0 y
     * partition entries "PM" en LBA 1. Nunca tratarlo como vacio. */
    if (sector[0] == 'E' && sector[1] == 'R') return INSTALL_DISK_UNSAFE;
    if (device->sector_count > 1U &&
        (!block_read(device, 1U, 1U, second) ||
         (second[0] == 'P' && second[1] == 'M') ||
         (second[0] == 'E' && second[1] == 'F' &&
          second[2] == 'I' && second[3] == ' ' &&
          second[4] == 'P' && second[5] == 'A' &&
          second[6] == 'R' && second[7] == 'T')))
        return INSTALL_DISK_UNSAFE;
    for (uint32_t i = 0; i < 4U; ++i) {
        uint32_t off = 446U + i * 16U;
        uint8_t type = sector[off + 4U];
        uint32_t first = installer_le32(sector + off + 8U);
        uint32_t length = installer_le32(sector + off + 12U);
        if (type == 0xEEU) return INSTALL_DISK_UNSAFE;
        if (type || first || length) entries = true;
    }
    if (!entries) return INSTALL_DISK_EMPTY;
    if (sector[510] != 0x55U || sector[511] != 0xAAU)
        return INSTALL_DISK_UNSAFE;
    for (uint32_t i = 0; i < 4U; ++i) {
        uint32_t off = 446U + i * 16U;
        uint8_t active = sector[off], type = sector[off + 4U];
        uint32_t first = installer_le32(sector + off + 8U);
        uint32_t length = installer_le32(sector + off + 12U);
        if (!type && !first && !length) {
            if (active) return INSTALL_DISK_UNSAFE;
            continue;
        }
        if ((active != 0U && active != 0x80U) || !type || !first ||
            !length || first >= device->sector_count ||
            length > device->sector_count - first) return INSTALL_DISK_UNSAFE;
        for (uint32_t j = 0; j < i; ++j) {
            uint32_t prev = 446U + j * 16U;
            uint32_t other = installer_le32(sector + prev + 8U);
            uint32_t count = installer_le32(sector + prev + 12U);
            if (count && first < other + count && other < first + length)
                return INSTALL_DISK_UNSAFE;
        }
    }
    return INSTALL_DISK_MBR;
}

/* Partition views are created only for a valid, non-GPT primary MBR. */
static void installer_register_partition_views(void) {
    uint32_t physical_count = block_count();
    for (uint32_t index = 0; index < physical_count; index++) {
        block_device_t *device = block_at(index);
        uint8_t mbr[BLOCK_SECTOR_SIZE];
        if (!device || device->base_lba ||
            device->type == BLOCK_DEVICE_FLOPPY ||
            device->type == BLOCK_DEVICE_ATAPI || device->read_only ||
            !device->write || device->sector_size != BLOCK_SECTOR_SIZE ||
            installer_disk_table(device, mbr) != INSTALL_DISK_MBR) continue;
        for (uint32_t part = 0; part < 4U; part++) {
            uint32_t offset = 446U + part * 16U;
            uint8_t type = mbr[offset + 4U];
            uint32_t first = installer_le32(mbr + offset + 8U);
            uint32_t sectors = installer_le32(mbr + offset + 12U);
            char name[8];
            if (!installer_partition_type_supported(type) || !first || !sectors)
                continue;
            snprintf(name, sizeof(name), "%sp%u", device->name, part + 1U);
            (void)block_register_view(name, device, first, sectors);
        }
    }
}

static uint32_t installer_collect_targets(installer_target_t *targets,
                                          uint32_t capacity) {
    uint32_t count = 0;
    if (!targets || !capacity) return 0;
    installer_register_partition_views();
    for (uint32_t index = 0; index < block_count() && count < capacity;
         index++) {
        block_device_t *device = block_at(index);
        installer_target_t *target;
        installer_table_kind_t table_kind = INSTALL_DISK_EMPTY;
        if (!device || device->type == BLOCK_DEVICE_ATAPI) continue;
        /* Antes se descartaban sin mostrarlos los discos con MBR, como uno
         * con Mac OS X. Ahora se muestran, pero NO son seleccionables para
         * instalar sin una ruta de borrado completo explicitamente autorizada.
         * Una tabla ilegible, GPT, APM o invalida sigue bloqueada. */
        if (!device->base_lba && device->type != BLOCK_DEVICE_FLOPPY &&
            device->sector_size == BLOCK_SECTOR_SIZE)
            table_kind = installer_disk_table(device, NULL);
        target = &targets[count];
        kmemset(target, 0, sizeof(*target));
        target->device = device;
        kstrncpy(target->name, device->name, sizeof(target->name) - 1U);
        kstrncpy(target->type, block_type_name(device->type),
                 sizeof(target->type) - 1U);
        target->size_mb = device->sector_count / 2048U;
        target->floppy = device->type == BLOCK_DEVICE_FLOPPY;
        target->partition = device->base_lba != 0U;
        target->occupied_mbr = !target->partition &&
            table_kind == INSTALL_DISK_MBR;
        target->blocked = device->read_only || !device->write ||
            device->sector_size != BLOCK_SECTOR_SIZE ||
            (!target->partition && table_kind == INSTALL_DISK_UNSAFE);
        if (!target->partition && device->type != BLOCK_DEVICE_FLOPPY)
            kprintf("[SETUP:DISK] %s: %u sectores, bps=%u, "
                    "MBR=%s, escritura=%s\n", device->name,
                    device->sector_count, device->sector_size,
                    table_kind == INSTALL_DISK_MBR ? "ocupado" :
                    (table_kind == INSTALL_DISK_UNSAFE ? "no seguro" : "vacio"),
                    device->read_only || !device->write ? "no" : "si");
        if (target->partition) {
            for (uint32_t j = 0; j < block_count(); j++) {
                block_device_t *parent = block_at(j);
                if (!parent || parent == device || parent->base_lba ||
                    parent->driver_data != device->driver_data ||
                    parent->type != device->type) continue;
                uint8_t mbr[BLOCK_SECTOR_SIZE];
                if (installer_disk_table(parent, mbr) != INSTALL_DISK_MBR)
                    continue;
                for (uint32_t k = 0; k < 4U; k++) {
                    uint32_t off = 446U + k * 16U;
                    if (installer_le32(mbr + off + 8U) == device->base_lba &&
                        installer_le32(mbr + off + 12U) == device->sector_count) {
                        target->parent = parent;
                        target->part_index = k;
                    }
                }
            }
            if (!target->parent) continue; /* never use orphaned views */
        }
        ++count;
    }
    return count;
}

static int installer_choose_target(installer_target_t *targets,
                                   uint32_t count) {
    uint32_t selected = 0;
    for (;;) {
        ui_frame("Seleccione el dispositivo de destino",
                 "Flechas: mover  Enter: opciones  R: actualizar  Esc: salir");
        if (!count) {
            ui_center(9, "No se detectaron discos de destino.",
                      UI_NORMAL);
            ui_center(11, "Revise BIOS, cable IDE y registro [ATA].", UI_NORMAL);
        }
        for (uint32_t index = 0; index < count && index < 12U; index++) {
            char line[68];
            uint8_t attr = index == selected ? UI_SELECT : UI_NORMAL;
            snprintf(line, sizeof(line), "%-7s  %-12s  %u MiB  %s",
                     targets[index].name, targets[index].type,
                     targets[index].size_mb,
                     targets[index].blocked ? "PROTEGIDO: no escribible/tabla" :
                     (targets[index].occupied_mbr ? "MBR ocupado: requiere borrar todo" :
                     (targets[index].floppy ? "formato FAT12" :
                     (targets[index].partition ? "particion instalable" : "disco instalable"))));
            ui_text_clipped(7, 6 + (int)index, line, 66, attr);
        }
        for (;;) {
            uint8_t key = ui_wait_key();
            if (key == KEY_ESCAPE) return -1;
            if (key == 'r' || key == 'R') return -2;
            if (!count) continue;
            if (key == KEY_UP && selected > 0U) {
                selected--;
                break;
            }
            if (key == KEY_DOWN && selected + 1U < count) {
                selected++;
                break;
            }
            if (key == KEY_ENTER || key == '\n') return (int)selected;
        }
    }
}

/* Confirmacion distinta del formato de un volumen. Esta opcion destruye
 * TODAS las particiones de un disco con MBR, incluido el sistema preexistente.
 * No se ofrece en GPT/APM ni cuando las lecturas fallan. */
static bool installer_confirm_whole_disk_erase(const installer_target_t *target) {
    static const char phrase[] = "BORRARTODO";
    char typed[sizeof(phrase)];
    char line[72];
    uint32_t length = 0U;
    kmemset(typed, 0, sizeof(typed));
    for (;;) {
        uint8_t key;
        ui_frame("BORRADO COMPLETO DEL DISCO",
                 "Escriba BORRARTODO y Enter; Esc cancela");
        snprintf(line, sizeof(line), "DISCO: %s  (%u MiB)",
                 target->name, target->size_mb);
        ui_center(5, line, UI_WARNING);
        ui_center(8, "SE ELIMINARAN TODAS LAS PARTICIONES Y SISTEMAS", UI_WARNING);
        ui_center(10, "Tambien se borrara Mac OS X u otro SO existente.", UI_WARNING);
        ui_center(12, "Haga backup antes. No existe deshacer.", UI_WARNING);
        ui_text(19, 16, "Confirmar: [", UI_NORMAL);
        ui_text_clipped(32, 16, typed, 10, UI_SELECT);
        ui_text(42, 16, "]", UI_NORMAL);
        key = ui_wait_key();
        if (key == KEY_ESCAPE) return false;
        if (key == KEY_BACKSPACE) {
            if (length) typed[--length] = '\0';
            continue;
        }
        if (key == KEY_ENTER || key == '\n') {
            if (kstrcmp(typed, phrase) == 0) return true;
            length = 0U;
            typed[0] = '\0';
            continue;
        }
        if (key >= 'a' && key <= 'z') key = (uint8_t)(key - 'a' + 'A');
        if (key >= 'A' && key <= 'Z' && length + 1U < sizeof(typed)) {
            typed[length++] = (char)key;
            typed[length] = '\0';
        }
    }
}

static void installer_show_blocked_disk(const installer_target_t *target) {
    char line[72];
    ui_frame("Disco detectado, instalacion bloqueada", "Enter: volver");
    snprintf(line, sizeof(line), "Unidad: %s  Tamano: %u MiB",
             target->name, target->size_mb);
    ui_center(6, line, UI_NORMAL);
    ui_center(9, "El disco no cumple los requisitos de escritura.", UI_WARNING);
    ui_center(11, "Causas: solo lectura, sector no 512 B, GPT,", UI_NORMAL);
    ui_center(12, "Apple APM, MBR invalido o error de lectura.", UI_NORMAL);
    ui_center(15, "No se ha escrito nada en el disco.", UI_OK);
    ui_center(18, "Compruebe el formato desde otro SO.", UI_NORMAL);
    (void)ui_wait_enter_or_escape();
}

static bool installer_confirm_destroy(const installer_target_t *target,
                                      const char *operation) {
    static const char confirmation[] = "BORRAR";
    char typed[sizeof(confirmation)];
    char line[72];
    uint32_t length = 0U;

    kmemset(typed, 0, sizeof(typed));
    for (;;) {
        ui_frame("Confirmacion destructiva",
                 "Escriba BORRAR para continuar. Esc: cancelar");
        snprintf(line, sizeof(line), "%s en %s (%u MiB)",
                 operation ? operation : "Operacion", target->name,
                 target->size_mb);
        ui_center(6, line, UI_NORMAL);
        ui_fill(6, 9, 68, 5, ' ', UI_WARNING);
        ui_center(9, "ADVERTENCIA: SE ELIMINARAN TODOS LOS ARCHIVOS",
                  UI_WARNING);
        ui_center(11, "Esta operacion no se puede deshacer.", UI_WARNING);
        ui_text(25, 16, "Confirmacion: [", UI_NORMAL);
        ui_text_clipped(40, 16, typed, 6, UI_SELECT);
        ui_text(46, 16, "]", UI_NORMAL);

        {
            uint8_t key = ui_wait_key();
            if (key == KEY_ESCAPE) return false;
            if (key == KEY_BACKSPACE) {
                if (length) typed[--length] = '\0';
                continue;
            }
            if (key == KEY_ENTER || key == '\n') {
                if (kstrcmp(typed, confirmation) == 0) return true;
                length = 0U;
                typed[0] = '\0';
                continue;
            }
            if (key >= 'a' && key <= 'z') key = (uint8_t)(key - 'a' + 'A');
            if (key >= 'A' && key <= 'Z' && length + 1U < sizeof(typed)) {
                typed[length++] = (char)key;
                typed[length] = '\0';
            }
        }
    }
}

static void installer_probe_disk(const installer_target_t *target,
                                 installer_disk_info_t *info) {
    char root[VFS_MAX_PATH];
    fat_fs_t fs;
    vfs_dir_entry_t entries[32];
    uint32_t count = 0U;

    if (!info) return;
    kmemset(info, 0, sizeof(*info));
    if (!target || !target->device) return;

    snprintf(root, sizeof(root), "/%s", target->name);
    vfs_forget_volume(target->name);
    if (fat_mount_named(&fs, target->name)) {
        info->mounted = true;
        info->fat32 = fs.type == FAT_TYPE_FAT32;
        info->bfs4 = fs.type == FAT_TYPE_BFS4;
        info->reserved_sectors = fs.reserved_sector_count;
        info->boot_layout = info->bfs4 ||
            (info->fat32 && fs.reserved_sector_count >= INSTALL_RESERVED_SECTORS);
        kstrncpy(info->label, fs.volume_label, sizeof(info->label) - 1U);
    }
    if (vfs_listdir(root, entries, 32U, &count)) {
        info->root_entries = count;
        info->has_files = count != 0U;
    }
    vfs_forget_volume(target->name);
}

static bool installer_confirm_nonformat(const installer_target_t *target,
                                        installer_mode_t mode) {
    char line[72];
    ui_frame(mode == INSTALL_MODE_UPDATE ? "Actualizacion sin formatear" :
             "Reparacion sin formatear", "I: continuar  Esc: cancelar");
    snprintf(line, sizeof(line), "Destino: %s", target->name);
    ui_center(6, line, UI_NORMAL);
    ui_fill(5, 9, 70, 6, ' ', UI_WARNING);
    ui_center(9, "No se eliminara /DOCUMENTS ni carpetas personales.", UI_WARNING);
    ui_center(11, mode == INSTALL_MODE_UPDATE ?
        "Se reemplazaran archivos propios del bootstrap." :
        "Solo se copiaran archivos ausentes o de tamano distinto.", UI_WARNING);
    ui_center(12, "Se recomienda hacer antes una copia de seguridad.", UI_WARNING);
    for (;;) {
        uint8_t key = ui_wait_key();
        if (key == KEY_ESCAPE) return false;
        if (key == 'i' || key == 'I') return true;
    }
}

/* A primary-partition install must be chainloaded by a working BIOS MBR.
 * We only support the bundled chainloader, with exactly one active partition
 * and an active entry corresponding to the selected target. We do not modify
 * MBR bytes or partition flags here: doing so silently risks other OSes. */
/* Select a primary MBR target only after an explicit boot-sector plan.
 * Replacing boot code/active flags changes which OS starts, so the default
 * remains read-only. The original sector is saved inside the target volume. */
static bool installer_partition_boot_ok(const installer_target_t *target) {
    uint8_t mbr[BLOCK_SECTOR_SIZE];
    iso9660_entry_t template;
    uint8_t expected[BLOCK_SECTOR_SIZE];
    uint32_t got = 0U, active_count = 0U;
    if (!target->partition) return true;
    if (!target->parent ||
        installer_disk_table(target->parent, mbr) != INSTALL_DISK_MBR ||
        !iso9660_resolve("/INSTALL/MBR.BIN", &template) ||
        template.is_directory || template.size != BLOCK_SECTOR_SIZE ||
        !iso9660_read_at(&template, 0U, expected, sizeof(expected), &got) ||
        got != sizeof(expected) ||
        expected[510] != 0x55U || expected[511] != 0xAAU ||
        kmemcmp(mbr, expected, 440U) != 0) return false;
    for (uint32_t i = 0; i < 4U; i++) {
        if (mbr[446U + 16U * i] == 0x80U) {
            ++active_count;
            if (i != target->part_index) return false;
        }
    }
    return active_count == 1U;
}

static bool installer_confirm_mbr_change(void) {
    static const char confirm[] = "ARRANQUE";
    char typed[sizeof(confirm)];
    uint32_t n = 0U;
    kmemset(typed, 0, sizeof(typed));
    for (;;) {
        ui_frame("Modificar arranque MBR", "Escriba ARRANQUE y Enter; Esc cancela");
        ui_center(6, "El MBR actual no arranca esta particion con seguridad.",
                  UI_WARNING);
        ui_center(8, "Se instalara el MBR de BlesKernOS y se marcara", UI_NORMAL);
        ui_center(9, "la particion elegida como activa.", UI_NORMAL);
        ui_center(11, "Puede alterar el arranque de otros sistemas.", UI_WARNING);
        ui_center(13, "Se guardara el sector 0 en /SYSTEM/SETUP/MBR.BAK.",
                  UI_NORMAL);
        ui_text(18, 17, "Confirmar: [", UI_NORMAL);
        ui_text_clipped(31, 17, typed, 8, UI_SELECT);
        ui_text(39, 17, "]", UI_NORMAL);
        uint8_t key = ui_wait_key();
        if (key == KEY_ESCAPE) return false;
        if (key == KEY_BACKSPACE && n) { typed[--n] = 0; continue; }
        if (key == KEY_ENTER || key == '\n') {
            if (kstrcmp(typed, confirm) == 0) return true;
            n = 0U; typed[0] = 0; continue;
        }
        if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
        if (key >= 'A' && key <= 'Z' && n + 1U < sizeof(typed)) {
            typed[n++] = (char)key; typed[n] = 0;
        }
    }
}

static void installer_floppy_format(const installer_target_t *target) {
    ui_frame("Formateando disquete", "Espere mientras se escribe FAT12...");
    ui_center(7, "Los disquetes de 1.44 MB usan FAT12, no FAT32.", UI_NORMAL);
    ui_center(9, "FAT32 necesita muchos mas clusters y no es valido aqui.",
              UI_NORMAL);
    ui_center(12, "Formateando...", UI_NORMAL);
    vfs_forget_volume(target->name);
    if (!fat_format(target->name, "BLESFLOPPY")) {
        ui_frame("Error al formatear", "Enter: volver a la lista");
        ui_center(9, "No se pudo escribir el disquete.", UI_ERROR);
        ui_center(11, "Revise el medio, la proteccion y la unidad.", UI_NORMAL);
        (void)ui_wait_enter_or_escape();
        return;
    }
    ui_frame("Disquete listo", "Enter: volver a la lista");
    ui_center(8, "Formato FAT12 completado correctamente.", UI_OK);
    ui_center(11, "La instalacion completa no cabe en 1.44 MB.", UI_NORMAL);
    (void)ui_wait_enter_or_escape();
}

static bool installer_iso_file(const char *path, void **buffer,
                               uint32_t *size) {
    iso9660_entry_t entry;
    uint8_t *data;
    uint32_t got = 0;
    if (!path || !buffer || !size || !iso9660_resolve(path, &entry) ||
        entry.is_directory) return false;
    data = (uint8_t *)kmalloc(entry.size ? entry.size : 1U);
    if (!data) return false;
    if (entry.size &&
        (!iso9660_read_at(&entry, 0, data, entry.size, &got) ||
         got != entry.size)) {
        kfree(data);
        return false;
    }
    *buffer = data;
    *size = entry.size;
    return true;
}

/* Group raw boot/kernel transfers: one ATAPI read and one durable ATA
 * write per 16 KiB, instead of a cache flush after every 512-byte sector.
 * Fall back to the old single-sector path if the temporary buffer cannot
 * be allocated (e.g. low-memory physical machines). */
#define SETUP_RAW_BATCH_SECTORS 32U
static bool installer_write_raw(block_device_t *device, const char *iso_path,
                                uint32_t first_lba, uint32_t max_sectors) {
    uint8_t single[BLOCK_SECTOR_SIZE];
    uint8_t *bulk;
    iso9660_entry_t file;
    uint32_t sectors;
    bool ok = true;
    if (!device || !iso9660_resolve(iso_path, &file) ||
        file.is_directory || !file.size ||
        (uint64_t)file.size > (uint64_t)max_sectors * BLOCK_SECTOR_SIZE)
        return false;
    sectors = (file.size + BLOCK_SECTOR_SIZE - 1U) / BLOCK_SECTOR_SIZE;
    if (sectors > device->sector_count ||
        first_lba > device->sector_count - sectors) return false;
    bulk = (uint8_t *)kmalloc(SETUP_RAW_BATCH_SECTORS * BLOCK_SECTOR_SIZE);
    for (uint32_t i = 0; i < sectors;) {
        uint32_t run = bulk ? SETUP_RAW_BATCH_SECTORS : 1U;
        uint32_t offset = i * BLOCK_SECTOR_SIZE;
        uint32_t got = 0U, remaining = file.size - offset;
        uint8_t *buffer = bulk ? bulk : single;
        uint32_t bytes;
        if (run > sectors - i) run = sectors - i;
        bytes = run * BLOCK_SECTOR_SIZE;
        if (remaining < bytes) bytes = remaining;
        kmemset(buffer, 0, run * BLOCK_SECTOR_SIZE);
        if (!iso9660_read_at(&file, offset, buffer, bytes, &got) ||
            got != bytes) {
            ok = false;
            break;
        }
        if (!block_write(device, first_lba + i, (uint8_t)run, buffer)) {
            /* Some old controllers dislike long PIO requests. Retry the
             * same bytes with the known-compatible one-sector path. */
            bool recovered = run > 1U;
            for (uint32_t j = 0; recovered && j < run; ++j)
                if (!block_write(device, first_lba + i + j, 1U,
                                 buffer + j * BLOCK_SECTOR_SIZE))
                    recovered = false;
            if (!recovered) {ok = false; break;}
            kprintf("[SETUP:IO] raw batch fallback LBA=%u count=%u\n",
                    first_lba + i, run);
        }
        i += run;
    }
    if (bulk) kfree(bulk);
    return ok;
}

static bool installer_patch_boot_sector(block_device_t *device) {
    uint8_t *template_data;
    uint32_t template_size;
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint8_t signature[BLOCK_SECTOR_SIZE];
    bool bfs4;
    if (!device ||
        !installer_iso_file("/INSTALL/BOOTF32.BIN",
                            (void **)&template_data, &template_size))
        return false;
    if (template_size != BLOCK_SECTOR_SIZE) {
        kfree(template_data);
        return false;
    }
    if (!block_read(device, BFS4_BOOT_SECTORS, 1, signature) ||
        !block_read(device, 0, 1, sector)) {
        kfree(template_data);
        return false;
    }
    bfs4 = signature[0] == 'B' && signature[1] == 'F' &&
           signature[2] == 'S' && signature[3] == '4';
    if (bfs4) kmemcpy(sector, template_data, BLOCK_SECTOR_SIZE);
    else {
        kmemcpy(sector, template_data, 3U);
        kmemcpy(sector + 90U, template_data + 90U,
                BLOCK_SECTOR_SIZE - 90U);
    }
    /* FAT32 BPB_HiddSec: Stage 1 and Stage 2 add this value to every BIOS
     * read. Zero for a superfloppy, partition start LBA for ata0pN. */
    sector[28] = (uint8_t)(device->base_lba);
    sector[29] = (uint8_t)(device->base_lba >> 8);
    sector[30] = (uint8_t)(device->base_lba >> 16);
    sector[31] = (uint8_t)(device->base_lba >> 24);
    sector[510] = 0x55U;
    sector[511] = 0xAAU;
    kfree(template_data);
    if (!block_write(device, 0, 1, sector) ||
        (!bfs4 && !block_write(device, INSTALL_BACKUP_BOOT_SECTOR, 1, sector)))
        return false;
    return true;
}

static bool installer_should_skip(const char *parent, const char *name) {
    if (!parent || !name) return true;
    if (kstrcmp(parent, "/") != 0) return false;
    return kstrcmp(name, "INSTALL") == 0 ||
           kstrcmp(name, "BOOT.IMG") == 0 ||
           kstrcmp(name, "BOOT.CAT") == 0 ||
           kstrcmp(name, "CDINFO.TXT") == 0;
}

static bool installer_join(char *out, uint32_t capacity, const char *parent,
                           const char *name) {
    uint32_t parent_length;
    uint32_t name_length;
    if (!out || !capacity || !parent || !name) return false;
    parent_length = (uint32_t)kstrlen(parent);
    name_length = (uint32_t)kstrlen(name);
    if (parent_length + name_length + 2U > capacity) return false;
    kstrncpy(out, parent, capacity - 1U);
    out[capacity - 1U] = '\0';
    if (kstrcmp(parent, "/") != 0) kstrcat(out, "/");
    kstrcat(out, name);
    return true;
}

static bool installer_commit_mbr(const installer_target_t *target,
                                 const char *destination_root) {
    uint8_t old[BLOCK_SECTOR_SIZE], patched[BLOCK_SECTOR_SIZE];
    uint8_t template_data[BLOCK_SECTOR_SIZE], verify[BLOCK_SECTOR_SIZE];
    iso9660_entry_t template;
    char backup[VFS_MAX_PATH];
    uint32_t got = 0U;
    if (!target->partition || !target->parent ||
        installer_disk_table(target->parent, old) != INSTALL_DISK_MBR ||
        !iso9660_resolve("/INSTALL/MBR.BIN", &template) ||
        template.is_directory || template.size != sizeof(template_data) ||
        !iso9660_read_at(&template, 0U, template_data,
                         sizeof(template_data), &got) ||
        got != sizeof(template_data) ||
        template_data[510] != 0x55U || template_data[511] != 0xAAU ||
        !installer_join(backup, sizeof(backup), destination_root,
                        "SYSTEM/SETUP/MBR.BAK")) return false;
    /* Keep partition entries, disk ID/signature, geometry and boundaries. */
    kmemcpy(patched, old, sizeof(patched));
    kmemcpy(patched, template_data, 440U);
    for (uint32_t i = 0; i < 4U; i++)
        patched[446U + 16U * i] = i == target->part_index ? 0x80U : 0U;
    if (!vfs_write_all(backup, old, sizeof(old)) || !vfs_sync_all())
        return false;
    if (!block_write(target->parent, 0U, 1U, patched) ||
        !block_read(target->parent, 0U, 1U, verify) ||
        kmemcmp(patched, verify, sizeof(verify)) != 0) {
        (void)block_write(target->parent, 0U, 1U, old);
        return false;
    }
    kprintf("[SETUP] MBR instalado; backup=%s active=p%u\n",
            backup, target->part_index + 1U);
    return true;
}

static bool installer_scan_tree(const char *source_path, uint32_t depth) {
    iso9660_entry_t directory;
    iso9660_entry_t *entries;
    uint32_t count = 0;
    if (!source_path || depth > SETUP_MAX_DEPTH ||
        !iso9660_resolve(source_path, &directory) ||
        !directory.is_directory) return false;
    entries = (iso9660_entry_t *)kmalloc(sizeof(*entries) * SETUP_MAX_ENTRIES);
    if (!entries) return false;
    if (!iso9660_list(&directory, entries, SETUP_MAX_ENTRIES, &count)) {
        kfree(entries);
        return false;
    }
    for (uint32_t index = 0; index < count; index++) {
        char child[VFS_MAX_PATH];
        if (installer_should_skip(source_path, entries[index].name)) continue;
        if (!installer_join(child, sizeof(child), source_path,
                            entries[index].name)) {
            kfree(entries);
            return false;
        }
        if (entries[index].is_directory) {
            g_copy.directories++;
            if (!installer_scan_tree(child, depth + 1U)) {
                kfree(entries);
                return false;
            }
        } else {
            g_copy.files_total++;
            if (0xFFFFFFFFU - g_copy.bytes_total < entries[index].size)
                g_copy.bytes_total = 0xFFFFFFFFU;
            else
                g_copy.bytes_total += entries[index].size;
        }
    }
    kfree(entries);
    return true;
}

static bool installer_ensure_directory(const char *path) {
    vfs_dir_entry_t entry;
    if (!path || !path[0]) return false;
    if (vfs_stat(path, &entry)) return entry.type == VFS_NODE_DIR;
    return vfs_mkdir(path);
}

/* No recursive deletion in non-destructive installation modes. The clean
 * install formats first; update is an overlay; repair fills missing files. */
static bool installer_file_matches(const iso9660_entry_t *source,
                                   const char *destination) {
    uint8_t *expected, *actual;
    uint32_t offset = 0U;
    vfs_dir_entry_t stat;
    int fd;
    bool equal = true;
    if (!source || !destination || !vfs_stat(destination, &stat) ||
        stat.type != VFS_NODE_FILE || stat.size != source->size) return false;
    fd = vfs_open(destination, VFS_O_RDONLY);
    if (fd < 0) return false;
    expected = (uint8_t *)kmalloc(SETUP_COPY_CHUNK * 2U);
    if (!expected) { (void)vfs_close(fd); return false; }
    actual = expected + SETUP_COPY_CHUNK;
    while (offset < source->size) {
        uint32_t chunk = source->size - offset, got = 0U;
        if (chunk > SETUP_COPY_CHUNK) chunk = SETUP_COPY_CHUNK;
        if (!iso9660_read_at(source, offset, expected, chunk, &got) ||
            got != chunk || vfs_read(fd, actual, chunk) != (int)chunk ||
            kmemcmp(expected, actual, chunk) != 0) {
            equal = false;
            break;
        }
        offset += chunk;
    }
    kfree(expected);
    (void)vfs_close(fd);
    return equal;
}

static bool installer_copy_tree(const char *source_path,
                                const char *destination_path,
                                uint32_t depth, installer_mode_t mode) {
    iso9660_entry_t directory;
    iso9660_entry_t *entries;
    uint32_t count = 0;
    if (!source_path || !destination_path || depth > SETUP_MAX_DEPTH ||
        !iso9660_resolve(source_path, &directory) ||
        !directory.is_directory) return false;
    entries = (iso9660_entry_t *)kmalloc(sizeof(*entries) * SETUP_MAX_ENTRIES);
    if (!entries) return false;
    if (!iso9660_list(&directory, entries, SETUP_MAX_ENTRIES, &count)) {
        kfree(entries);
        return false;
    }
    for (uint32_t index = 0; index < count; index++) {
        char source_child[VFS_MAX_PATH];
        char destination_child[VFS_MAX_PATH];
        if (installer_should_skip(source_path, entries[index].name)) continue;
        if (!installer_join(source_child, sizeof(source_child), source_path,
                            entries[index].name) ||
            !installer_join(destination_child, sizeof(destination_child),
                            destination_path, entries[index].name)) {
            kstrncpy(g_copy.error, entries[index].name,
                     sizeof(g_copy.error) - 1U);
            kfree(entries);
            return false;
        }
        if (entries[index].is_directory) {
            if (!installer_ensure_directory(destination_child) ||
                !installer_copy_tree(source_child, destination_child,
                                     depth + 1U, mode)) {
                kstrncpy(g_copy.error, destination_child,
                         sizeof(g_copy.error) - 1U);
                kfree(entries);
                return false;
            }
        } else {
            uint8_t *data = NULL;
            uint32_t offset = 0U;
            int fd = -1;
            bool ok = true;
            bool skip = mode == INSTALL_MODE_REPAIR &&
                installer_file_matches(&entries[index], destination_child);
            uint32_t last_progress = 0U;
            uint32_t progress_step = SETUP_PROGRESS_STEP;
            uint32_t copy_chunk = SETUP_COPY_CHUNK;
            /* Avoid repainting the entire VGA screen for every 64 KiB.
             * Aim for at most ~100 intermediate updates per large file. */
            if (entries[index].size / 100U > progress_step)
                progress_step = entries[index].size / 100U;
            kstrncpy(g_copy.current_path, source_child,
                     sizeof(g_copy.current_path) - 1U);
            g_copy.current_path[sizeof(g_copy.current_path) - 1U] = '\0';
            g_copy.current_file_size = entries[index].size;
            g_copy.current_file_done = 0U;
            g_copy.current_file_active = true;
            ui_progress(g_copy.files_done, g_copy.files_total,
                        skip ? "Verificando archivos existentes..." :
                               "Copiando bootstrap y paquetes BKL3...");
            if (!skip) {
                char temporary[VFS_MAX_PATH];
                uint32_t name_length = (uint32_t)kstrlen(destination_child);
                if (name_length + sizeof(".BKNEW") > sizeof(temporary)) ok = false;
                if (ok) {
                    kstrcpy(temporary, destination_child);
                    kstrcat(temporary, ".BKNEW");
                    (void)vfs_remove(temporary); /* leftover from failed attempt */
                    /* Prefer 256 KiB for large files, with 64 KiB fallback.
                     * Avoid a large allocation for thousands of tiny DLLs. */
                    if (entries[index].size > 1024U * 1024U) {
                        data = (uint8_t *)kmalloc(SETUP_FAST_COPY_CHUNK);
                        if (data) copy_chunk = SETUP_FAST_COPY_CHUNK;
                    }
                    if (!data) data = (uint8_t *)kmalloc(SETUP_COPY_CHUNK);
                    installer_io_phase("Fase: creando archivo de destino...");
                    uint32_t opened_at = pit_get_ticks();
                    fd = data ? vfs_open(temporary,
                          VFS_O_WRONLY | VFS_O_CREATE | VFS_O_TRUNC) : -1;
                    installer_io_measure(false, opened_at, source_child, 0U);
                    if (fd < 0) ok = false;
                }
                while (ok && offset < entries[index].size) {
                    uint32_t chunk = entries[index].size - offset, got = 0U;
                    if (chunk > copy_chunk) chunk = copy_chunk;
                    installer_io_phase("Fase: leyendo datos del CD...");
                    uint32_t started = pit_get_ticks();
                    bool read_ok = iso9660_read_at(&entries[index], offset,
                                                   data, chunk, &got);
                    installer_io_measure(true, started, source_child, chunk);
                    if (!read_ok || got != chunk) {
                        ok = false;
                        break;
                    }
                    installer_io_phase("Fase: escribiendo en disco IDE...");
                    started = pit_get_ticks();
                    int written = vfs_write(fd, data, chunk);
                    installer_io_measure(false, started, source_child, chunk);
                    if (written != (int)chunk)
                        ok = false;
                    else {
                        offset += chunk;
                        g_copy.current_file_done = offset;
                        if (offset - last_progress >= progress_step ||
                            offset == entries[index].size) {
                            ui_progress(g_copy.files_done, g_copy.files_total,
                                        "Copiando bootstrap y paquetes BKL3...");
                            last_progress = offset;
                        }
                    }
                }
                if (fd >= 0) {
                    installer_io_phase("Fase: sincronizando archivo...");
                    uint32_t started = pit_get_ticks();
                    bool flushed = vfs_flush(fd);
                    installer_io_measure(false, started, source_child, 0U);
                    if (!flushed) ok = false;
                    /* Close even on flush errors; it may retry persistence. */
                    started = pit_get_ticks();
                    bool closed = vfs_close(fd);
                    installer_io_measure(false, started, source_child, 0U);
                    if (!closed) ok = false;
                    fd = -1;
                }
                if (data) kfree(data);
                if (ok) {
                    installer_io_phase("Fase: renombrando archivo...");
                    uint32_t started = pit_get_ticks();
                    bool renamed = vfs_rename(temporary, destination_child);
                    installer_io_measure(false, started, source_child, 0U);
                    if (!renamed) ok = false;
                }
                if (!ok) {
                    if (fd >= 0) (void)vfs_close(fd);
                    if (name_length + sizeof(".BKNEW") <= sizeof(temporary))
                        (void)vfs_remove(temporary);
                    kstrncpy(g_copy.error, destination_child,
                             sizeof(g_copy.error) - 1U);
                    kfree(entries);
                    return false;
                }
            }
            /* The last displayed path must be the file just copied,
             * not a stale name from the previous completed file. */
            g_copy.current_file_done = entries[index].size;
            g_copy.files_done++;
            /* A compact COM1 summary only once per 128 files. */
            if ((g_copy.files_done & 127U) == 0U) {
                block_io_stats_t io;
                block_get_io_stats(&io);
                kprintf("[SETUP:STATS] files=%u ataR=%u ataW=%u sectorsW=%u cdR=%u errors=%u comBytes=%u comTimeouts=%u\n",
                        g_copy.files_done, io.ata_reads, io.ata_writes,
                        io.ata_write_sectors, io.atapi_reads,
                        io.failed_requests, vga_com1_tx_bytes(),
                        vga_com1_timeout_count());
            }
            g_copy.current_file_active = false;
            if (0xFFFFFFFFU - g_copy.bytes_done < entries[index].size)
                g_copy.bytes_done = 0xFFFFFFFFU;
            else
                g_copy.bytes_done += entries[index].size;
            ui_progress(g_copy.files_done, g_copy.files_total,
                        skip ? "Archivo verificado; sin cambios." :
                               "Archivo copiado correctamente.");
            g_copy.current_file_size = 0U;
            g_copy.current_file_done = 0U;
        }
    }
    kfree(entries);
    return true;
}

static bool installer_verify(const installer_target_t *target) {
    uint8_t sector[BLOCK_SECTOR_SIZE];
    char path[VFS_MAX_PATH];
    vfs_dir_entry_t entry;
    if (!block_read(target->device, 0, 1, sector) ||
        sector[510] != 0x55U || sector[511] != 0xAAU) return false;
    snprintf(path, sizeof(path), "/%s/SYSTEM/PROGRAMS/SETUP.BEX",
             target->name);
    if (!vfs_stat(path, &entry) || entry.type != VFS_NODE_FILE || !entry.size)
        return false;
    /* Setup runs inside Vision; checking SETUP.BEX alone accepted images
     * that inevitably entered the recovery shell on their first HDD boot. */
    snprintf(path, sizeof(path), "/%s/SYSTEM/SERVICES/VISION.BEX",
             target->name);
    if (!vfs_stat(path, &entry) || entry.type != VFS_NODE_FILE || !entry.size)
        return false;
    snprintf(path, sizeof(path), "/%s/SYSTEM/USER/START.INI", target->name);
    if (!vfs_stat(path, &entry) || entry.type != VFS_NODE_FILE || !entry.size)
        return false;
    snprintf(path, sizeof(path), "/%s/SYSTEM/SETUP/CORE.BKL", target->name);
    if (!vfs_stat(path, &entry) || entry.type != VFS_NODE_FILE || !entry.size)
        return false;
    snprintf(path, sizeof(path), "/%s/SYSTEM/DRIVERS/VESA.DVR",
             target->name);
    return vfs_stat(path, &entry) && entry.type == VFS_NODE_FILE && entry.size;
}

static bool installer_media_preflight(bool partition) {
    static const struct {
        const char *path;
        uint32_t max_size;
    } required[] = {
        {"/INSTALL/BOOTF32.BIN", BLOCK_SECTOR_SIZE},
        {"/INSTALL/STAGE2.BIN", INSTALL_STAGE2_SECTORS * BLOCK_SECTOR_SIZE},
        {"/INSTALL/KERNEL.BIN", INSTALL_KERNEL_RESERVED_SECTORS * BLOCK_SECTOR_SIZE}
    };
    iso9660_entry_t entry;
    uint8_t boot[BLOCK_SECTOR_SIZE];
    uint32_t got = 0U;
    for (uint32_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!iso9660_resolve(required[i].path, &entry) || entry.is_directory ||
            !entry.size || entry.size > required[i].max_size ||
            (i == 0U && entry.size != BLOCK_SECTOR_SIZE)) return false;
    }
    if (!iso9660_resolve("/INSTALL/BOOTF32.BIN", &entry) ||
        !iso9660_read_at(&entry, 0U, boot, sizeof(boot), &got) ||
        got != sizeof(boot) || boot[510] != 0x55U || boot[511] != 0xAAU)
        return false;
    if (partition && (!iso9660_resolve("/INSTALL/MBR.BIN", &entry) ||
        entry.is_directory || entry.size != BLOCK_SECTOR_SIZE ||
        !iso9660_read_at(&entry, 0U, boot, sizeof(boot), &got) ||
        got != sizeof(boot) || boot[510] != 0x55U || boot[511] != 0xAAU))
        return false;
    /* A bootable first-run install must already contain its GUI host.
     * Checking this before formatting avoids destroying a usable disk with
     * a CD that would otherwise only boot into Recovery. */
    static const char *first_boot[] = {
        "/INSTALL/BOOTSTRAP/SYSTEM/SERVICES/VISION.BEX",
        "/INSTALL/BOOTSTRAP/SYSTEM/PROGRAMS/SETUP.BEX"
    };
    for (uint32_t i = 0; i < sizeof(first_boot)/sizeof(first_boot[0]); i++) {
        if (!iso9660_resolve(first_boot[i], &entry) ||
            entry.is_directory || !entry.size) return false;
    }
    static const char *packages[] = {"CORE", "PROGRAMS", "ASSETS", "WINE"};
    for (uint32_t i = 0; i < sizeof(packages)/sizeof(packages[0]); i++) {
        char path[96];
        uint8_t head[16];
        snprintf(path, sizeof(path),
                 "/INSTALL/BOOTSTRAP/SYSTEM/SETUP/%s.BKL", packages[i]);
        if (!iso9660_resolve(path, &entry) || entry.is_directory ||
            entry.size < sizeof(head) ||
            !iso9660_read_at(&entry, 0U, head, sizeof(head), &got) ||
            got != sizeof(head) || kmemcmp(head, "BKL3", 4U) != 0 ||
            installer_le32(head + 4U) != 1U) return false;
    }
    return true;
}

/* MANIFEST.TXT is generated from the actual staged CD. Its raw total is
 * the maximum extraction footprint of all BKLs (including optional Wine).
 * Conservatively demand room for the copied archives AND expanded contents. */
static bool installer_manifest_unpacked(uint64_t *size) {
    iso9660_entry_t manifest;
    char buffer[2049];
    uint32_t got = 0U;
    if (!size || !iso9660_resolve("/INSTALL/BOOTSTRAP/SYSTEM/SETUP/MANIFEST.TXT",
                                  &manifest) || manifest.is_directory ||
        !manifest.size || manifest.size > sizeof(buffer) - 1U ||
        !iso9660_read_at(&manifest, 0U, buffer, manifest.size, &got) ||
        got != manifest.size) return false;
    buffer[got] = '\0';
    for (uint32_t i = 0; i + 11U < got; ++i) {
        if (kmemcmp(buffer + i, "TOTAL: raw=", 11U) == 0) {
            uint64_t value = 0U;
            uint32_t j = i + 11U;
            if (j >= got || buffer[j] < '0' || buffer[j] > '9') return false;
            while (j < got && buffer[j] >= '0' && buffer[j] <= '9') {
                uint32_t digit = (uint32_t)(buffer[j++] - '0');
                if (value > (0xFFFFFFFFFFFFFFFFULL - digit) / 10ULL)
                    return false;
                value = value * 10ULL + digit;
            }
            *size = value;
            return true;
        }
    }
    return false;
}

static bool installer_space_preflight(const installer_target_t *target,
                                      installer_mode_t mode) {
    uint64_t raw = 0U, capacity, headroom, available;
    char mount_path[16];
    if (!installer_manifest_unpacked(&raw) ||
        g_copy.bytes_total == 0xFFFFFFFFU) {
        kstrcpy(g_copy.error, "Manifest ausente o tamanos no validos");
        return false;
    }
    g_copy.unpacked_bytes = raw;
    g_copy.needed_bytes = (uint64_t)g_copy.bytes_total + raw;
    if (g_copy.needed_bytes < raw ||
        g_copy.needed_bytes > 0xFFFFFFFFFFFFFFFFULL - SETUP_SPACE_MARGIN) {
        kstrcpy(g_copy.error, "Tamano de instalacion fuera de rango");
        return false;
    }
    g_copy.needed_bytes += SETUP_SPACE_MARGIN;
    snprintf(mount_path, sizeof(mount_path), "/%s", target->name);
    if (mode == INSTALL_MODE_CLEAN) {
        capacity = (uint64_t)target->device->sector_count * BLOCK_SECTOR_SIZE;
        headroom = capacity / 8ULL; /* BFS4 metadata, cache y reserva 12.5% */
        available = capacity > headroom ? capacity - headroom : 0ULL;
    } else {
        uint64_t total = 0U;
        if (!vfs_get_space_path(mount_path, &total, &available)) {
            kstrcpy(g_copy.error, "No se puede medir espacio libre del volumen");
            return false;
        }
    }
    kprintf("[SETUP:SPACE] mode=%u bkl+bootstrap=%u raw=%u required=%u MiB "
            "available=%u MiB\n", (uint32_t)mode, g_copy.bytes_total,
            (uint32_t)(raw / 1048576ULL),
            (uint32_t)(g_copy.needed_bytes / 1048576ULL),
            (uint32_t)(available / 1048576ULL));
    if (available < g_copy.needed_bytes) {
        snprintf(g_copy.error, sizeof(g_copy.error),
                 "Espacio insuficiente: necesita %u MiB, hay %u MiB",
                 (uint32_t)((g_copy.needed_bytes + 1048575ULL) / 1048576ULL),
                 (uint32_t)(available / 1048576ULL));
        return false;
    }
    return true;
}

static bool installer_install_target(const installer_target_t *target,
                                     installer_mode_t mode) {
    char destination_root[16];
    bool change_mbr = false;
    kmemset(&g_copy, 0, sizeof(g_copy));
    g_setup_cd_ticks = g_setup_disk_ticks = 0U;
    kstrcpy(g_setup_io_phase, "Fase: preparando instalacion...");
    kstrcpy(g_copy.current_path, "Preparando instalacion...");
    ui_progress(0, 1, "Analizando el contenido del CD...");
    if (!installer_scan_tree("/INSTALL/BOOTSTRAP", 0U)) {
        kstrcpy(g_copy.error, "No se pudo leer el arbol ISO9660");
        return false;
    }
    if (!g_copy.files_total) {
        kstrcpy(g_copy.error, "El CD no contiene el bootstrap BKL3");
        return false;
    }
    if (!installer_media_preflight(target->partition)) {
        kstrcpy(g_copy.error, "CD incompleto: boot, Stage2, kernel, MBR o BKL3");
        return false;
    }

    if (!installer_space_preflight(target, mode)) return false;
    if (target->partition && !installer_partition_boot_ok(target)) {
        if (!installer_confirm_mbr_change()) {
            kstrcpy(g_copy.error, "Cambios en MBR cancelados por el usuario");
            return false;
        }
        change_mbr = true;
    }

    if (mode == INSTALL_MODE_CLEAN) {
        /* Para un disco antes particionado debemos retirar el MBR viejo:
         * bfs4_format() conserva intencionalmente los sectores de arranque.
         * Solo se llega aqui tras confirmar BORRARTODO, y solo para MBR
         * valido que siga siendo reconocible justo antes de escribir. */
        if (target->occupied_mbr) {
            uint8_t empty[BLOCK_SECTOR_SIZE];
            uint8_t verify[BLOCK_SECTOR_SIZE];
            if (target->partition || target->blocked ||
                installer_disk_table(target->device, NULL) != INSTALL_DISK_MBR) {
                kstrcpy(g_copy.error, "MBR cambio; no se borro el disco");
                return false;
            }
            kmemset(empty, 0, sizeof(empty));
            if (!block_write(target->device, 0U, 1U, empty) ||
                !block_read(target->device, 0U, 1U, verify) ||
                kmemcmp(empty, verify, sizeof(empty)) != 0) {
                kstrcpy(g_copy.error, "Error al eliminar tabla MBR");
                return false;
            }
            kprintf("[SETUP] MBR anterior de %s eliminado por solicitud expresa\n",
                    target->name);
        }
        kstrcpy(g_copy.current_path, "Creando volumen BFS4...");
        ui_progress(0, g_copy.files_total, "Formateando el disco destino...");
        vfs_forget_volume(target->name);
        fat_forget_device(target->name);
        kprintf("[SETUP] Formato BFS4: destino=%s sectores=%u bps=%u "
                "ro=%u writer=%s reserved=%u\n",
                target->name, target->device->sector_count,
                target->device->sector_size,
                target->device->read_only ? 1U : 0U,
                target->device->write ? "si" : "no",
                INSTALL_RESERVED_SECTORS);
        if (!bfs4_format(target->device, "BLESKERNOS")) {
            kstrcpy(g_copy.error, "No se pudo crear BFS4");
            kprintf("[SETUP] BFS4 fallo: %s\n", g_copy.error);
            return false;
        }
    } else {
        kstrcpy(g_copy.current_path, "Conservando volumen existente...");
        ui_progress(0, g_copy.files_total,
                    "Preparando instalacion sin formatear...");
        vfs_forget_volume(target->name);
        fat_forget_device(target->name);
    }

    kstrcpy(g_copy.current_path, "Instalando sector de arranque...");
    ui_progress(0, g_copy.files_total, "Instalando el arranque de disco...");
    if (!installer_patch_boot_sector(target->device) ||
        !installer_write_raw(target->device, "/INSTALL/STAGE2.BIN",
                             INSTALL_STAGE2_LBA,
                             INSTALL_STAGE2_SECTORS) ||
        !installer_write_raw(target->device, "/INSTALL/KERNEL.BIN",
                             INSTALL_KERNEL_LBA,
                             INSTALL_KERNEL_RESERVED_SECTORS)) {
        kstrcpy(g_copy.error, "No se pudo instalar boot, Stage 2 o kernel");
        return false;
    }

    snprintf(destination_root, sizeof(destination_root), "/%s", target->name);
    kstrcpy(g_copy.current_path, "Montando el nuevo volumen...");
    ui_progress(0, g_copy.files_total, "Preparando directorios...");
    {
        vfs_dir_entry_t root_entry;

        /* /ata0, /usb0, etc. son puntos de volumen virtuales del VFS.
         * No deben crearse con mkdir: la ruta FAT interna de su raiz es
         * vacia y fat_mkdir_path("") la rechaza correctamente.
         * vfs_stat fuerza el montaje nominal y valida la raiz existente. */
        vfs_forget_volume(target->name);
        if (!vfs_stat(destination_root, &root_entry)) {
            snprintf(g_copy.error, sizeof(g_copy.error),
                     "No se pudo montar %s despues del formato",
                     target->name);
            return false;
        }
        if (root_entry.type != VFS_NODE_DIR) {
            snprintf(g_copy.error, sizeof(g_copy.error),
                     "%s no expone una raiz valida", target->name);
            return false;
        }
    }
    kstrcpy(g_copy.current_path, "Copiando bootstrap sin tocar /DOCUMENTS...");
    ui_progress(0, g_copy.files_total, "Copiando componentes del sistema...");
    if (!installer_copy_tree("/INSTALL/BOOTSTRAP", destination_root,
                             0U, mode)) return false;

    kstrcpy(g_copy.current_path, "Comprobando archivos y arranque...");
    ui_progress(g_copy.files_total, g_copy.files_total,
                "Verificando la instalacion...");
    if (!installer_verify(target)) {
        kstrcpy(g_copy.error, "La verificacion final fallo");
        return false;
    }
    if (change_mbr && !installer_commit_mbr(target, destination_root)) {
        kstrcpy(g_copy.error, "No se pudo respaldar/verificar el MBR");
        return false;
    }
    if (!installer_partition_boot_ok(target)) {
        kstrcpy(g_copy.error, "Arranque MBR sin verificar");
        return false;
    }
    return true;
}

static int installer_format_menu(const installer_target_t *target) {
    installer_disk_info_t info;
    char line[72];

    installer_probe_disk(target, &info);
    for (;;) {
        ui_frame("Opciones de formato",
                 "A: formatear e instalar  F: solo formatear  Esc: volver");
        snprintf(line, sizeof(line), "Dispositivo: %s  Tipo: %s  Tamano: %u MiB",
                 target->name, target->type, target->size_mb);
        ui_text(6, 6, line, UI_NORMAL);
        snprintf(line, sizeof(line), "Estado: %s  Etiqueta: %s",
                 info.mounted ? (info.bfs4 ? "BFS4 detectado" :
                                 (info.fat32 ? "FAT32 detectado" : "FAT detectado"))
                              : "sin volumen reconocido",
                 info.label[0] ? info.label : "(ninguna)");
        ui_text_clipped(6, 8, line, 68, UI_NORMAL);
        ui_box(6, 11, 68, 7, UI_NORMAL);
        ui_text(9, 12, "A  Formatear BFS4 e instalar BlesKernOS User", UI_NORMAL);
        ui_text(9, 14, "F  Formatear BFS4 solamente", UI_NORMAL);
        ui_text(9, 16, "Esc  Volver sin modificar el disco", UI_NORMAL);
        ui_center(20, "Ambas opciones de formato eliminan todos los archivos.",
                  UI_WARNING);
        {
            uint8_t key = ui_wait_key();
            if (key == KEY_ESCAPE) return 0;
            if (key == 'a' || key == 'A') return 2;
            if (key == 'f' || key == 'F') return 1;
        }
    }
}

static int installer_target_actions(const installer_target_t *target,
                                    installer_disk_info_t *info) {
    char line[72];
    /* En un disco particionado solo se permite borrar TODO, no actualizar
     * ni formatear directamente el disco fisico encima del MBR. */
    if (target->occupied_mbr) {
        for (;;) {
            uint8_t key;
            ui_frame("Disco con particiones existentes",
                     "B: borrar disco entero e instalar   Esc: volver");
            snprintf(line, sizeof(line), "%s (%u MiB): MBR valido y ocupado",
                     target->name, target->size_mb);
            ui_center(6, line, UI_NORMAL);
            ui_center(9, "No hay espacio libre para instalar directamente.", UI_NORMAL);
            ui_center(12, "B eliminara TODAS las particiones, incluido", UI_WARNING);
            ui_center(13, "Mac OS X / Windows / Linux si estan presentes.", UI_WARNING);
            ui_center(17, "Guarde antes cualquier dato importante.", UI_NORMAL);
            key = ui_wait_key();
            if (key == KEY_ESCAPE) return 0;
            if (key == 'b' || key == 'B') return 4;
        }
    }
    installer_probe_disk(target, info);
    for (;;) {
        ui_frame("Opciones del disco",
                 "U: actualizar  R: reparar  F: formato  Esc: volver");
        snprintf(line, sizeof(line), "%s (%s, %u MiB%s)", target->name,
                 target->type, target->size_mb,
                 target->partition ? ", particion MBR" : "");
        ui_center(5, line, UI_NORMAL);
        snprintf(line, sizeof(line), "Sistema de archivos: %s",
                 info->mounted ? (info->bfs4 ? "BFS4" :
                                  (info->fat32 ? "FAT32" : "FAT12/FAT16"))
                               : "no reconocido");
        ui_text(8, 8, line, UI_NORMAL);
        snprintf(line, sizeof(line), "Archivos visibles: %u  Reservados: %u sectores",
                 info->root_entries, info->reserved_sectors);
        ui_text(8, 10, line, UI_NORMAL);
        ui_box(7, 12, 66, 8, UI_NORMAL);
        ui_text(10, 13, "U  Actualizar sistema, conservar datos", UI_NORMAL);
        ui_text(10, 15, "R  Reparar bootstrap y arranque", UI_NORMAL);
        ui_text(10, 17, "F  Instalacion limpia (formato)", UI_NORMAL);
        if (!info->boot_layout)
            ui_center(20, "Para instalar, este disco debe formatearse primero.",
                      UI_WARNING);
        else if (info->has_files)
            ui_center(20, "Hay archivos: Setup advertira antes de continuar.",
                      UI_WARNING);
        {
            uint8_t key = ui_wait_key();
            if (key == KEY_ESCAPE) return 0;
            if (key == 'f' || key == 'F') return 2;
            if (key == 'u' || key == 'U') return 1;
            if (key == 'r' || key == 'R') return 3;
        }
    }
}

static void installer_show_error(const installer_target_t *target) {
    char line[72];
    ui_frame("La instalacion no pudo completarse",
             "Enter: volver a la lista   Esc: detener Setup");
    ui_center(6, "BlesKernOS no fue instalado correctamente.", UI_ERROR);
    snprintf(line, sizeof(line), "Destino: %s", target->name);
    ui_text(7, 9, line, UI_NORMAL);
    ui_text_clipped(7, 11, g_copy.error[0] ? g_copy.error
                                          : "Error desconocido",
                    66, UI_NORMAL);
    ui_center(15, "Puede corregir el problema e intentarlo otra vez.",
              UI_NORMAL);
    if (!ui_wait_enter_or_escape())
        installer_halt_screen("Setup fue cancelado por el usuario.");
}

static void installer_show_complete(const installer_target_t *target) {
    char line[72];
    ui_frame("Instalacion completada",
             "R: reiniciar desde el disco   Esc: apagar mas tarde");
    ui_center(6, "El bootstrap de BlesKernOS se copio correctamente.", UI_OK);
    snprintf(line, sizeof(line), "Sistema instalado en %s (%u MiB).",
             target->name, target->size_mb);
    ui_center(9, line, UI_NORMAL);
    ui_center(12, "El primer arranque abrira el asistente grafico SETUP.BEX.",
              UI_NORMAL);
    ui_center(15, "Alli podra elegir usuario, zona horaria y la capa Wine.",
              UI_NORMAL);
    for (;;) {
        uint8_t key = ui_wait_key();
        if (key == 'r' || key == 'R') installer_reboot();
        if (key == KEY_ESCAPE)
            installer_halt_screen("Instalacion finalizada. Retire el CD.");
    }
}

void installer_run(void) {
    installer_target_t targets[SETUP_MAX_TARGETS];

    vga_init();
    ui_frame("Bienvenido", "Enter: continuar   Esc: salir");
    ui_center(6, "Bienvenido al instalador de BlesKernOS 1.0", UI_NORMAL);
    ui_center(9, "Este asistente copiara el arranque y paquetes BKL3", UI_NORMAL);
    ui_center(10, "a un disco ATA o USB y lo dejara arrancable.", UI_NORMAL);
    ui_center(13, "Tambien puede formatear disquetes de 1.44 MB en FAT12.",
              UI_NORMAL);
    ui_center(16, "La instalacion en disco utiliza BFS4.", UI_NORMAL);
    if (!ui_wait_enter_or_escape())
        installer_halt_screen("Setup fue cancelado antes de comenzar.");

    for (;;) {
        uint32_t count = installer_collect_targets(targets,
                                                   SETUP_MAX_TARGETS);
        int chosen = installer_choose_target(targets, count);
        installer_target_t *target;
        if (chosen == -2) continue;
        if (chosen < 0)
            installer_halt_screen("Setup fue cancelado por el usuario.");
        target = &targets[(uint32_t)chosen];
        if (target->blocked) {
            installer_show_blocked_disk(target);
            continue;
        }

        if (target->floppy) {
            if (installer_confirm_destroy(target, "Formatear FAT12"))
                installer_floppy_format(target);
            continue;
        }
        for (;;) {
            installer_disk_info_t info;
            int action = installer_target_actions(target, &info);
            if (action == 0) break;
            if (action == 4) {
                if (!installer_confirm_whole_disk_erase(target)) continue;
                if (!installer_install_target(target, INSTALL_MODE_CLEAN)) {
                    installer_show_error(target);
                    /* El MBR pudo haberse borrado: refrescar la lista en
                     * vez de conservar una clasificacion ya obsoleta. */
                    break;
                }
                installer_show_complete(target);
            }
            if (action == 2) {
                int format_action = installer_format_menu(target);
                if (format_action == 0) continue;
                if (!installer_confirm_destroy(target,
                        format_action == 2 ? "Formatear e instalar"
                                           : "Formatear BFS4"))
                    continue;
                if (format_action == 1) {
                    ui_frame("Formateando BFS4", "Espere...");
                    vfs_forget_volume(target->name);
                    fat_forget_device(target->name);
                    if (!bfs4_format(target->device, "BLESKERNOS")) {
                        ui_frame("Error al formatear", "Enter: volver");
                        ui_text_clipped(7, 10, "No se pudo crear BFS4", 66,
                                        UI_ERROR);
                        (void)ui_wait_enter_or_escape();
                    } else {
                        ui_frame("Formato completado", "Enter: volver");
                        ui_center(10, "El disco BFS4 esta listo.", UI_OK);
                        (void)ui_wait_enter_or_escape();
                    }
                    continue;
                }
                if (!installer_install_target(target, INSTALL_MODE_CLEAN)) {
                    installer_show_error(target);
                    continue;
                }
                installer_show_complete(target);
            }
            if (action == 1 || action == 3) {
                if (!info.boot_layout) {
                    ui_frame("Formato necesario", "Enter: volver");
                    ui_center(8, "El volumen actual no reserva espacio suficiente",
                              UI_ERROR);
                    ui_center(10, "para Stage 2 y el kernel de BlesKernOS.",
                              UI_NORMAL);
                    ui_center(13, "Use F y elija Formatear e instalar.",
                              UI_NORMAL);
                    (void)ui_wait_enter_or_escape();
                    continue;
                }
                if (!installer_confirm_nonformat(target,
                        action == 1 ? INSTALL_MODE_UPDATE : INSTALL_MODE_REPAIR))
                    continue;
                if (!installer_install_target(target, action == 1 ?
                        INSTALL_MODE_UPDATE : INSTALL_MODE_REPAIR)) {
                    installer_show_error(target);
                    continue;
                }
                installer_show_complete(target);
            }
        }
    }
}
