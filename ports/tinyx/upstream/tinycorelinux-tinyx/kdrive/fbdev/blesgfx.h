/* BlesKernOS TinyX display bridge. Public-domain interface definition. */
#ifndef BLES_GFX_H
#define BLES_GFX_H
#include <stdint.h>

#define BLES_GFX_LAUNCH_BUSY       0xB110U /* ioctl arg: 0/1; owned by descriptor */
#define BLES_GFX_OPEN             0xB100U
#define BLES_GFX_PRESENT          0xB101U
#define BLES_GFX_EVENT            0xB102U
#define BLES_GFX_VIEWPORT         0xB103U
#define BLES_GFX_SURFACE_UPSERT   0xB104U
#define BLES_GFX_SURFACE_REMOVE   0xB105U
#define BLES_GFX_SURFACE_EVENT    0xB106U
#define BLES_GFX_NEEDS_PRESENT    0xB107U
#define BLES_GFX_WINE_VM_PERF      0xB108U
#define BLES_GFX_WINE_BOOT_READY   0xB109U
#define BLES_GFX_WINE_CACHE_PERF   0xB10AU
#define BLES_GFX_WINE_MEMORY       0xB10DU
#define BLES_GFX_WINE_PAGER_PERF   0xB10EU
#define BLES_GFX_HEAP_SUBSYSTEMS   0xB10FU
typedef struct { uint32_t bytes[8], blocks[8]; } bles_gfx_heap_subsystems_t;
typedef struct {
    uint32_t pid,anonymous_pages,file_private_pages,file_clean_pages,borrowed_pages;
} bles_gfx_wine_pager_perf_t;
typedef struct {
    uint32_t pid, heap_resident_pages, vm_allocated_pages, system_allocated_bytes;
    uint32_t process_heap_bytes, virtual_bytes, committed_bytes;
    uint32_t mapped_pages, resident_pages, writable_pages, cow_pages, table_pages;
    uint32_t paged_virtual_pages, paged_resident_pages, swapped_pages;
    uint32_t brk_capacity, brk_committed;
} bles_gfx_wine_memory_t;
#define BLES_GFX_3D                0xB10BU

/* Fixed-width Linux i386 graphics ABI. No kernel pointers cross this boundary. */
enum {
    BLES_3D_CAPS = 1, BLES_3D_CREATE, BLES_3D_DESTROY,
    BLES_3D_UPLOAD, BLES_3D_DOWNLOAD, BLES_3D_UPLOAD_MIP,
    BLES_3D_UPLOAD_REGION, BLES_3D_DEPTH_UPLOAD, BLES_3D_DEPTH_DOWNLOAD,
    BLES_3D_BEGIN, BLES_3D_DRAW, BLES_3D_END, BLES_3D_WAIT
};
typedef struct { uint32_t op, p[12]; } bles_gfx_3d_t;

#define BLES_GFX_SURFACE_FIXED    0x00000001U
#define BLES_GFX_SURFACE_DIALOG   0x00000002U

#define BLES_GFX_SURFACE_CLOSE    1U
#define BLES_GFX_SURFACE_RESIZE   2U
#define BLES_GFX_SURFACE_FOCUS    3U
#define BLES_GFX_SURFACE_BLUR     4U
#define BLES_GFX_SURFACE_MOVE     5U

#define BLES_GFX_OPEN_ROOTLESS    0x00000001U

typedef struct {
    uint32_t width, height, flags;
} bles_gfx_open_t;

typedef struct {
    uint32_t pixels;
    uint32_t width, height;
} bles_gfx_present_t;

typedef struct {
    uint32_t instance_id;
    uint32_t x, y;
    uint32_t width, height;
} bles_gfx_viewport_t;

typedef struct {
    uint32_t instance_id;
    uint32_t surface_id;      /* X11 Window id. */
    uint32_t parent_id;       /* WM_TRANSIENT_FOR, or zero. */
    uint32_t x, y;            /* source position in the X root. */
    uint32_t width, height;   /* source/client dimensions. */
    uint32_t flags;
    uint32_t z_index;         /* XQueryTree order, bottom to top. */
    char title[48];
} bles_gfx_surface_t;

typedef struct {
    uint32_t instance_id;     /* input: TinyX pid; output preserved. */
    uint32_t surface_id;
    uint32_t type;
    int32_t x, y;
    uint32_t width, height;
} bles_gfx_surface_event_t;

typedef struct {
    uint32_t pid;
    uint32_t mmap_calls;
    uint32_t file_maps;
    uint32_t cached_file_maps;
    uint32_t cache_hits;
    uint32_t cache_misses;
    uint32_t fallback_reads;
    uint32_t file_io_kib;
    uint32_t file_io_ms;
} bles_gfx_wine_vm_perf_t;

typedef struct {
    uint32_t cache_entries, cache_bytes, cache_evictions;
    uint32_t vfs_handles_peak, vfs_handles, private_fds;
} bles_gfx_wine_cache_perf_t;

typedef struct {
    uint32_t type;
    int32_t x, y, dx, dy;
    uint8_t buttons, button, key, shift, ctrl, alt;
    uint8_t reserved[2];
} bles_gfx_event_t;

#define BLES_GFX_MOUSE_MOVE 1U
#define BLES_GFX_MOUSE_DOWN 2U
#define BLES_GFX_MOUSE_UP   3U
#define BLES_GFX_KEY        5U
#define BLES_GFX_KEY_UP     6U
#define BLES_GFX_CLOSE      10U
#endif
