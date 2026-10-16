#include "../../include/perfmon.h"
#include "../../include/driver.h"
#include "../../include/gfx3d_driver.h"
#include "../../include/svga3d_protocol.h"
#include "../../include/svga_transport.h"
#include "../../include/memory.h"
#include "../../libc/include/stdio.h"

/*
 * VMware SVGA3D fixed-function extension.
 *
 * This is deliberately a second driver module. VMWARESVGA.DVR owns the PCI
 * device, FIFO and VRAM. SVGA3D.DVR consumes only the private transport
 * ABI and stays dormant when the host does not advertise SVGA_CAP_3D. Thus
 * QEMU keeps the mature 2D path while VMware Workstation/Fusion can enable 3D.
 */

#define SVGA3D_CONTEXT_ID              1U
#define SVGA3D_VERTEX_SURFACE_ID       1U
#define SVGA3D_FIRST_USER_SURFACE_ID  16U
#define SVGA3D_FIRST_DEPTH_SURFACE_ID 128U
#define SVGA3D_MAX_SURFACES           96U
_Static_assert(SVGA3D_FIRST_USER_SURFACE_ID + SVGA3D_MAX_SURFACES <=
               SVGA3D_FIRST_DEPTH_SURFACE_ID, "color/depth SIDs must not overlap");
_Static_assert(2U * SVGA3D_MAX_SURFACES + 1U <= BK_SVGA_TRANSPORT_MAX_ALLOCATIONS,
               "SVGA transport must hold all color/depth surfaces and vertices");
_Static_assert(BK_SVGA_TRANSPORT_MAX_ALLOCATIONS < 256U,
               "SVGA transport allocation slot must fit its handle");
/* Keep the 4 MiB budget, but do not burn 128 KiB for a glyph or a single
 * terrain triangle. Large batches are split by draw_triangles; every slot
 * retains its own fence before reuse. */
#define SVGA3D_VERTEX_SLOT_BYTES    (16U * 1024U)
#define SVGA3D_VERTEX_RING_SLOTS       256U
#define SVGA3D_VERTEX_BUFFER_BYTES \
    (SVGA3D_VERTEX_SLOT_BYTES * SVGA3D_VERTEX_RING_SLOTS)
#define SVGA3D_VRAM_ALIGNMENT          64U
#define ALIGN_UP(v, a) (((v) + ((a) - 1U)) & ~((a) - 1U))
#define SVGA3D_MAX_DRAW_VERTICES \
    (SVGA3D_VERTEX_SLOT_BYTES / (uint32_t)sizeof(gfx3d_vertex_t))

typedef struct {
    bool used;
    uint8_t generation;
    uint16_t width;
    uint16_t height;
    gfx3d_format_t format;
    uint32_t flags;
    uint32_t sid;
    uint32_t bytes_per_pixel;
    uint32_t pitch;
    uint32_t size;
    uint32_t vram_handle;
    uint32_t vram_offset;
    uint32_t mip_levels;
    uint32_t staging_fence;
    bool staging_valid;

    /* ABI v3: persistent Z16 attachment owned by this render target. */
    bool depth_defined;
    uint32_t depth_sid;
    uint32_t depth_vram_handle;
    uint32_t depth_vram_offset;
    uint32_t depth_pitch;
    uint32_t depth_size;
    uint32_t depth_fence;
} svga3d_surface_t;

typedef struct {
    const bk_svga_transport_ops_t *transport;
    uint32_t transport_generation;
    bool ready;
    bool absence_reported;
    bool validation_failed;
    uint32_t host_hw_version;
    uint32_t guest_hw_version;

    uint32_t vertex_vram_handle;
    uint32_t vertex_vram_offset;
    uint32_t vertex_fence[SVGA3D_VERTEX_RING_SLOTS];
    uint32_t vertex_slot;
    uint32_t vertex_slot_offset;
    uint32_t vertex_active_slot;
    uint32_t vertex_base;
    bool context_defined;
    bool vertex_surface_defined;

    uint8_t surface_generations[SVGA3D_MAX_SURFACES];
    svga3d_surface_t surfaces[SVGA3D_MAX_SURFACES];
} svga3d_state_t;

static svga3d_state_t g_3d;
/* One shared host context: cache the state actually submitted, including GUI
 * changes. Invalidate on resource destruction/reset, never cache per caller. */
static struct {
    bool render_valid, texture_valid, zrange_valid;
    bool target_valid[2], rect_valid[2];
    uint32_t render_flags, texture_flags, texture_sid, target_sid[2];
    bk_svga3d_set_rect_t rect[2];
} g_state_cache;
static uint32_t g_svga3d_trace_draws;
static uint32_t g_svga3d_trace_uploads;
static uint32_t g_svga3d_trace_begins;

static bool svga3d_ensure_ready(void);
static bool svga3d_run_selftest(uint32_t *fence_out);

static bool transport_active(const bk_svga_transport_ops_t *transport) {
    return transport && transport->is_active &&
           transport->is_active(transport->context);
}

static bool transport_emit(const void *bytes, uint32_t byte_count) {
    return g_3d.transport && g_3d.transport->emit &&
           g_3d.transport->emit(g_3d.transport->context, bytes, byte_count);
}

static bool transport_submit(bool wait, uint32_t *fence_out) {
    return g_3d.transport && g_3d.transport->submit &&
           g_3d.transport->submit(g_3d.transport->context, wait, fence_out);
}

static bool transport_wait(uint32_t fence) {
    uint64_t started = perfmon_scope_begin();
    bool ok = !fence || (g_3d.transport && g_3d.transport->wait_fence &&
           g_3d.transport->wait_fence(g_3d.transport->context, fence));
    perfmon_scope_end(PERF_SCOPE_3D_WAIT, started);
    return ok;
}

static bool svga3d_emit(uint32_t command_id, const void *payload,
                        uint32_t payload_bytes, const void *tail,
                        uint32_t tail_bytes) {
    uint32_t total = (uint32_t)sizeof(bk_svga3d_header_t) +
                     payload_bytes + tail_bytes;
    uint32_t padded = ALIGN_UP(total, 4U);
    uint8_t stack_packet[512];
    uint8_t *packet;
    bk_svga3d_header_t *header;
    bool heap_packet = false;
    bool ok;

    if (!g_3d.ready || (payload_bytes && !payload) ||
        (tail_bytes && !tail)) return false;
    if (padded <= sizeof(stack_packet)) {
        packet = stack_packet;
    } else {
        packet = (uint8_t *)kmalloc(padded);
        if (!packet) return false;
        heap_packet = true;
    }
    kmemset(packet, 0, padded);
    header = (bk_svga3d_header_t *)packet;
    header->id = command_id;
    header->size = payload_bytes + tail_bytes;
    if (payload_bytes)
        kmemcpy(packet + sizeof(*header), payload, payload_bytes);
    if (tail_bytes)
        kmemcpy(packet + sizeof(*header) + payload_bytes, tail, tail_bytes);
    ok = transport_emit(packet, padded);
    if (heap_packet) kfree(packet);
    return ok;
}

static uint32_t svga3d_bytes_per_pixel(gfx3d_format_t format) {
    switch (format) {
        case GFX3D_FORMAT_Z16: return 2U;
        case GFX3D_FORMAT_XRGB8888:
        case GFX3D_FORMAT_ARGB8888: return 4U;
        default: return 0U;
    }
}

static uint32_t svga3d_host_format(gfx3d_format_t format) {
    switch (format) {
        case GFX3D_FORMAT_XRGB8888: return SVGA3D_X8R8G8B8;
        case GFX3D_FORMAT_ARGB8888: return SVGA3D_A8R8G8B8;
        case GFX3D_FORMAT_Z16: return SVGA3D_Z_D16;
        default: return 0U;
    }
}

static uint32_t svga3d_surface_flags_from_desc(
    const gfx3d_surface_desc_t *desc) {
    uint64_t flags = 0U;
    if (desc->flags & GFX3D_SURFACE_DYNAMIC)
        flags |= SVGA3D_SURFACE_HINT_DYNAMIC;
    if (desc->flags & GFX3D_SURFACE_TEXTURE)
        flags |= SVGA3D_SURFACE_HINT_TEXTURE;
    if (desc->flags & GFX3D_SURFACE_RENDER_TARGET)
        flags |= SVGA3D_SURFACE_HINT_RENDERTARGET;
    if (desc->flags & GFX3D_SURFACE_DEPTH)
        flags |= SVGA3D_SURFACE_HINT_DEPTHSTENCIL;
    return (uint32_t)flags;
}

static bool svga3d_define_surface_mips(uint32_t sid, uint32_t format,
                                     uint32_t flags, uint32_t width,
                                     uint32_t height, uint32_t levels) {
    bk_svga3d_define_surface_t command;
    bk_svga3d_size_t sizes[16];
    if (!sid || !format || !width || !height || !levels || levels > 16U) return false;
    kmemset(&command, 0, sizeof(command));
    command.sid = sid;
    command.surface_flags_low = flags;
    command.format = format;
    command.face[0].num_mip_levels = levels;
    for (uint32_t i = 0U; i < levels; i++) {
        sizes[i] = (bk_svga3d_size_t){width, height, 1U};
        if (width > 1U) width >>= 1U;
        if (height > 1U) height >>= 1U;
    }
    return svga3d_emit(SVGA_3D_CMD_SURFACE_DEFINE, &command,
                       sizeof(command), sizes, levels * sizeof(sizes[0]));
}

static bool svga3d_define_surface_id(uint32_t sid, uint32_t format,
                                     uint32_t flags, uint32_t width,
                                     uint32_t height) {
    return svga3d_define_surface_mips(sid, format, flags, width, height, 1U);
}

static bool svga3d_destroy_surface_id(uint32_t sid) {
    kmemset(&g_state_cache, 0, sizeof(g_state_cache));
    bk_svga3d_destroy_surface_t command;
    if (!sid || sid == SVGA3D_INVALID_ID) return false;
    command.sid = sid;
    return svga3d_emit(SVGA_3D_CMD_SURFACE_DESTROY, &command,
                       sizeof(command), NULL, 0U);
}

static bool svga3d_define_context(void) {
    bk_svga3d_context_t command = {SVGA3D_CONTEXT_ID};
    return svga3d_emit(SVGA_3D_CMD_CONTEXT_DEFINE, &command,
                       sizeof(command), NULL, 0U);
}

static bool svga3d_destroy_context(void) {
    bk_svga3d_context_t command = {SVGA3D_CONTEXT_ID};
    return svga3d_emit(SVGA_3D_CMD_CONTEXT_DESTROY, &command,
                       sizeof(command), NULL, 0U);
}

static bool svga3d_surface_dma_mip(uint32_t sid, uint32_t guest_offset,
                               uint32_t guest_pitch, uint32_t bpp,
                               uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height, uint32_t transfer, uint32_t level) {
    bk_svga3d_surface_dma_t command;
    bk_svga3d_copy_box_t box;

    if (!sid || !guest_pitch || !bpp || !width || !height) return false;
    kmemset(&command, 0, sizeof(command));
    kmemset(&box, 0, sizeof(box));
    command.guest.ptr.gmr_id = SVGA_GMR_FRAMEBUFFER;
    command.guest.ptr.offset = guest_offset;
    command.guest.pitch = guest_pitch;
    command.host.sid = sid;
    command.host.mipmap = level;
    command.transfer = transfer;

    box.x = x;
    box.y = y;
    box.z = 0U;
    box.w = width;
    box.h = height;
    box.d = 1U;
    box.srcx = x;
    box.srcy = y;
    box.srcz = 0U;
    return svga3d_emit(SVGA_3D_CMD_SURFACE_DMA, &command,
                       sizeof(command), &box, sizeof(box));
}

static bool svga3d_surface_dma(uint32_t sid, uint32_t guest_offset,
                               uint32_t guest_pitch, uint32_t bpp,
                               uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height, uint32_t transfer) {
    return svga3d_surface_dma_mip(sid, guest_offset, guest_pitch, bpp,
                                 x, y, width, height, transfer, 0U);
}

static bool svga3d_set_render_target(uint32_t sid, uint32_t type) {
    bk_svga3d_set_render_target_t command;
    uint32_t index = type == SVGA3D_RT_COLOR0 ? 0U : 1U;
    if (!sid) return false;
    if (g_state_cache.target_valid[index] && g_state_cache.target_sid[index] == sid) return true;
    command.cid = SVGA3D_CONTEXT_ID;
    command.type = type;
    command.target.sid = sid;
    command.target.face = 0U;
    command.target.mipmap = 0U;
    if (!svga3d_emit(SVGA_3D_CMD_SETRENDERTARGET, &command,
                       sizeof(command), NULL, 0U)) return false;
    g_state_cache.target_valid[index] = true;
    g_state_cache.target_sid[index] = sid;
    return true;
}

static bool svga3d_set_rect(uint32_t command_id, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height) {
    bk_svga3d_set_rect_t command;
    command.cid = SVGA3D_CONTEXT_ID;
    command.rect.x = x;
    command.rect.y = y;
    command.rect.w = width;
    command.rect.h = height;
    uint32_t index = command_id == SVGA_3D_CMD_SETVIEWPORT ? 0U : 1U;
    if (g_state_cache.rect_valid[index] &&
        kmemcmp(&g_state_cache.rect[index], &command, sizeof(command)) == 0) return true;
    if (!svga3d_emit(command_id, &command, sizeof(command), NULL, 0U)) return false;
    g_state_cache.rect_valid[index] = true;
    g_state_cache.rect[index] = command;
    return true;
}

static bool svga3d_set_viewport(uint32_t width, uint32_t height) {
    const bk_svga3d_zrange_t zrange = {SVGA3D_CONTEXT_ID, 0.0f, 1.0f};
    if (!g_state_cache.zrange_valid) {
        if (!svga3d_emit(SVGA_3D_CMD_SETZRANGE, &zrange, sizeof(zrange), NULL, 0U)) return false;
        g_state_cache.zrange_valid = true;
    }
    return svga3d_set_rect(SVGA_3D_CMD_SETVIEWPORT, 0U, 0U, width, height) &&
           svga3d_set_rect(SVGA_3D_CMD_SETSCISSORRECT, 0U, 0U,
                           width, height);
}

static bool svga3d_set_scissor(gfx_rect_t clip, uint32_t width,
                               uint32_t height) {
    int32_t x = clip.x, y = clip.y, w = clip.w, h = clip.h;
    if (w <= 0 || h <= 0) return svga3d_set_viewport(width, height);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int32_t)width || y >= (int32_t)height) return false;
    if (x + w > (int32_t)width) w = (int32_t)width - x;
    if (y + h > (int32_t)height) h = (int32_t)height - y;
    if (w <= 0 || h <= 0) return false;
    return svga3d_set_rect(SVGA_3D_CMD_SETSCISSORRECT,
        (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

static uint32_t svga3d_depth_compare(uint32_t flags) {
    /* SVGA3D comparison values follow the D3D fixed-function encoding. */
    switch (gfx3d_draw_depth_func(flags)) {
    case GFX3D_DEPTH_NEVER: return 1U;
    case GFX3D_DEPTH_LESS: return 2U;
    case GFX3D_DEPTH_EQUAL: return 3U;
    case GFX3D_DEPTH_LEQUAL: return 4U;
    case GFX3D_DEPTH_GREATER: return 5U;
    case GFX3D_DEPTH_NOTEQUAL: return 6U;
    case GFX3D_DEPTH_GEQUAL: return 7U;
    default: return 8U;
    }
}

static bool svga3d_set_render_states(uint32_t flags) {
    flags &= GFX3D_DRAW_DEPTH_TEST | GFX3D_DRAW_DEPTH_WRITE | GFX3D_DRAW_BLEND |
        GFX3D_DRAW_REVERSED_DEPTH | GFX3D_DRAW_PREMULTIPLIED |
        GFX3D_DRAW_DEPTH_FUNC_VALID | GFX3D_DRAW_DEPTH_FUNC_MASK |
        GFX3D_DRAW_BLEND_ADDITIVE | GFX3D_DRAW_ALPHA_TEST | (0x7ffU << 17U);
    if (g_state_cache.render_valid && g_state_cache.render_flags == flags) return true;
    bk_svga3d_set_render_state_t command;
    bk_svga3d_render_state_t states[24];
    uint32_t count = 0U;
#define ADD_STATE(name_, value_) do { \
        states[count].state = (name_); \
        states[count].value = (value_); \
        count++; \
    } while (0)
    command.cid = SVGA3D_CONTEXT_ID;
    /* BLESKERNOS_SVGA3D_STATE_BASELINE_V9_1
     * CONTEXT_DEFINE no garantiza que el estado fixed-function que nos
     * interesa arranque con valores utiles. Publica una base conocida. */
    ADD_STATE(SVGA3D_RS_ALPHATESTENABLE, (flags & GFX3D_DRAW_ALPHA_TEST) ? 1U : 0U);
    ADD_STATE(SVGA3D_RS_ALPHAFUNC, ((flags >> GFX3D_DRAW_ALPHA_FUNC_SHIFT) & 7U) + 1U);
    {
        union { float f; uint32_t u; } ref;
        ref.f = (float)((flags >> GFX3D_DRAW_ALPHA_REF_SHIFT) & 255U) / 255.0f;
        ADD_STATE(SVGA3D_RS_ALPHAREF, ref.u);
    }
    ADD_STATE(SVGA3D_RS_DITHERENABLE, 0U);
    ADD_STATE(SVGA3D_RS_FOGENABLE, 0U);
    ADD_STATE(SVGA3D_RS_SPECULARENABLE, 0U);
    ADD_STATE(SVGA3D_RS_STENCILENABLE, 0U);
    ADD_STATE(SVGA3D_RS_NORMALIZENORMALS, 0U);
    ADD_STATE(SVGA3D_RS_POINTSPRITEENABLE, 0U);
    ADD_STATE(SVGA3D_RS_POINTSCALEENABLE, 0U);
    ADD_STATE(SVGA3D_RS_FILLMODE,
              SVGA3D_FILL_MODE(SVGA3D_FILLMODE_FILL, SVGA3D_FACE_FRONT_BACK));
    ADD_STATE(SVGA3D_RS_SHADEMODE, SVGA3D_SHADEMODE_SMOOTH);
    ADD_STATE(SVGA3D_RS_COORDINATETYPE, SVGA3D_COORDINATE_LEFTHANDED);
    ADD_STATE(SVGA3D_RS_ZENABLE,
              (flags & GFX3D_DRAW_DEPTH_TEST) ? 1U : 0U);
    ADD_STATE(SVGA3D_RS_ZWRITEENABLE,
              (flags & GFX3D_DRAW_DEPTH_WRITE) ? 1U : 0U);
    ADD_STATE(SVGA3D_RS_BLENDENABLE,
              (flags & GFX3D_DRAW_BLEND) ? 1U : 0U);
    ADD_STATE(SVGA3D_RS_LIGHTINGENABLE, 0U);
    ADD_STATE(SVGA3D_RS_SRCBLEND,
              (flags & GFX3D_DRAW_PREMULTIPLIED)
                ? SVGA3D_BLENDOP_ONE : SVGA3D_BLENDOP_SRCALPHA);
    ADD_STATE(SVGA3D_RS_DSTBLEND,
              (flags & GFX3D_DRAW_BLEND_ADDITIVE)
                ? SVGA3D_BLENDOP_ONE : SVGA3D_BLENDOP_INVSRCALPHA);
    ADD_STATE(SVGA3D_RS_BLENDEQUATION, SVGA3D_BLENDEQ_ADD);
    ADD_STATE(SVGA3D_RS_CULLMODE, SVGA3D_FACE_NONE);
    ADD_STATE(SVGA3D_RS_ZFUNC, svga3d_depth_compare(flags));
    ADD_STATE(SVGA3D_RS_COLORWRITEENABLE, SVGA3D_COLORWRITE_ALL);
    ADD_STATE(SVGA3D_RS_SCISSORTESTENABLE, 1U);
#undef ADD_STATE
    if (!svga3d_emit(SVGA_3D_CMD_SETRENDERSTATE, &command,
                       sizeof(command), states,
                       count * (uint32_t)sizeof(states[0]))) return false;
    g_state_cache.render_valid = true;
    g_state_cache.render_flags = flags;
    return true;
}

static bool svga3d_set_texture(uint32_t sid, uint32_t flags) {
    flags &= GFX3D_DRAW_LINEAR | GFX3D_DRAW_REPEAT_U | GFX3D_DRAW_REPEAT_V |
             GFX3D_DRAW_TEXTURE_DECAL | GFX3D_DRAW_MIPMAP_NEAREST;
    if (g_state_cache.texture_valid && g_state_cache.texture_sid == sid &&
        g_state_cache.texture_flags == flags) return true;
    bk_svga3d_set_texture_state_t command;
    bk_svga3d_texture_state_t states[17];
    uint32_t count = 0U;
    uint32_t filter = (flags & GFX3D_DRAW_LINEAR)
        ? SVGA3D_TEX_FILTER_LINEAR : SVGA3D_TEX_FILTER_NEAREST;
#define ADD_TEX(stage_, name_, value_) do { \
        states[count].stage = (stage_); \
        states[count].name = (name_); \
        states[count].value = (value_); \
        count++; \
    } while (0)
    command.cid = SVGA3D_CONTEXT_ID;
    /* One texture coordinate set and one combiner stage in the public ABI. */
    ADD_TEX(0U, SVGA3D_TS_TEXCOORDINDEX, 0U);
    ADD_TEX(0U, SVGA3D_TS_TEXTURETRANSFORMFLAGS, 0U);
    ADD_TEX(0U, SVGA3D_TS_TEXCOORDGEN, 0U);
    ADD_TEX(1U, SVGA3D_TS_COLOROP, SVGA3D_TC_DISABLE);
    ADD_TEX(1U, SVGA3D_TS_ALPHAOP, SVGA3D_TC_DISABLE);
    ADD_TEX(0U, SVGA3D_TS_BIND_TEXTURE, sid ? sid : SVGA3D_INVALID_ID);
    if (sid) {
        ADD_TEX(0U, SVGA3D_TS_COLOROP, (flags & GFX3D_DRAW_TEXTURE_DECAL)
            ? SVGA3D_TC_BLENDTEXTUREALPHA : SVGA3D_TC_MODULATE);
        ADD_TEX(0U, SVGA3D_TS_COLORARG1, SVGA3D_TA_TEXTURE);
        ADD_TEX(0U, SVGA3D_TS_COLORARG2, SVGA3D_TA_DIFFUSE);
        ADD_TEX(0U, SVGA3D_TS_ALPHAOP, (flags & GFX3D_DRAW_TEXTURE_DECAL)
            ? SVGA3D_TC_SELECTARG2 : SVGA3D_TC_MODULATE);
        ADD_TEX(0U, SVGA3D_TS_ALPHAARG1, SVGA3D_TA_TEXTURE);
        ADD_TEX(0U, SVGA3D_TS_ALPHAARG2, SVGA3D_TA_DIFFUSE);
        ADD_TEX(0U, SVGA3D_TS_ADDRESSU,
                (flags & GFX3D_DRAW_REPEAT_U)
                    ? SVGA3D_TEX_ADDRESS_WRAP : SVGA3D_TEX_ADDRESS_CLAMP);
        ADD_TEX(0U, SVGA3D_TS_ADDRESSV,
                (flags & GFX3D_DRAW_REPEAT_V)
                    ? SVGA3D_TEX_ADDRESS_WRAP : SVGA3D_TEX_ADDRESS_CLAMP);
        ADD_TEX(0U, SVGA3D_TS_MIPFILTER, (flags & GFX3D_DRAW_MIPMAP_NEAREST)
                ? SVGA3D_TEX_FILTER_NEAREST : SVGA3D_TEX_FILTER_NONE);
        ADD_TEX(0U, SVGA3D_TS_MAGFILTER, filter);
        ADD_TEX(0U, SVGA3D_TS_MINFILTER, filter);
    } else {
        ADD_TEX(0U, SVGA3D_TS_COLOROP, SVGA3D_TC_SELECTARG1);
        ADD_TEX(0U, SVGA3D_TS_COLORARG1, SVGA3D_TA_DIFFUSE);
        ADD_TEX(0U, SVGA3D_TS_ALPHAOP, SVGA3D_TC_SELECTARG1);
        ADD_TEX(0U, SVGA3D_TS_ALPHAARG1, SVGA3D_TA_DIFFUSE);
    }
#undef ADD_TEX
    if (!svga3d_emit(SVGA_3D_CMD_SETTEXTURESTATE, &command,
                       sizeof(command), states,
                       count * (uint32_t)sizeof(states[0]))) return false;
    g_state_cache.texture_valid = true;
    g_state_cache.texture_sid = sid;
    g_state_cache.texture_flags = flags;
    return true;
}

static bool svga3d_clear_current(uint32_t color, float depth,
                                 uint32_t flags, uint32_t width,
                                 uint32_t height) {
    bk_svga3d_clear_t command;
    bk_svga3d_rect_t rect;
    uint32_t clear = 0U;
    if (flags & GFX3D_DRAW_CLEAR_COLOR) clear |= SVGA3D_CLEAR_COLOR;
    if (flags & GFX3D_DRAW_CLEAR_DEPTH) clear |= SVGA3D_CLEAR_DEPTH;
    if (!clear) return true;
    command.cid = SVGA3D_CONTEXT_ID;
    command.clear_flags = clear;
    command.color = color;
    command.depth = depth;
    command.stencil = 0U;
    if (!width || !height) return false;
    rect.x = 0U;
    rect.y = 0U;
    rect.w = width;
    rect.h = height;
    return svga3d_emit(SVGA_3D_CMD_CLEAR, &command, sizeof(command),
                       &rect, sizeof(rect));
}

static bool svga3d_define_depth(svga3d_surface_t *target) {
    uint32_t pitch, size;
    if (!target || !(target->flags & GFX3D_SURFACE_RENDER_TARGET)) return false;
    if (target->depth_defined && target->depth_vram_handle) return true;
    pitch = ALIGN_UP((uint32_t)target->width * 2U, 4U);
    size = pitch * (uint32_t)target->height;
    if (!target->depth_sid) return false;
    if (!g_3d.transport->vram_allocate(g_3d.transport->context, size,
            SVGA3D_VRAM_ALIGNMENT, &target->depth_vram_handle,
            &target->depth_vram_offset)) return false;
    if (!svga3d_define_surface_id(target->depth_sid, SVGA3D_Z_D16,
            (uint32_t)SVGA3D_SURFACE_HINT_DEPTHSTENCIL,
            target->width, target->height)) {
        (void)g_3d.transport->vram_free(g_3d.transport->context,
                                        target->depth_vram_handle);
        target->depth_vram_handle = 0U;
        return false;
    }
    target->depth_defined = true;
    target->depth_pitch = pitch;
    target->depth_size = size;
    target->depth_fence = 0U;
    return true;
}

/* The context is shared by all clients. Every draw must restore its own
 * attachments; BEGIN and DRAW can be interleaved with compositor operations. */
static bool svga3d_bind_target(svga3d_surface_t *target, uint32_t flags) {
    uint32_t depth_sid = SVGA3D_INVALID_ID;
    if (!target || !(target->flags & GFX3D_SURFACE_RENDER_TARGET)) return false;
    if (flags & (GFX3D_DRAW_DEPTH_TEST | GFX3D_DRAW_DEPTH_WRITE |
                 GFX3D_DRAW_CLEAR_DEPTH)) {
        if (!svga3d_define_depth(target)) return false;
        depth_sid = target->depth_sid;
    }
    return svga3d_set_render_target(target->sid, SVGA3D_RT_COLOR0) &&
           svga3d_set_render_target(depth_sid, SVGA3D_RT_DEPTH) &&
           svga3d_set_viewport(target->width, target->height);
}

static gfx3d_surface_handle_t svga3d_make_handle(uint32_t slot) {
    return ((uint32_t)g_3d.surfaces[slot].generation << 8) | (slot + 1U);
}

static svga3d_surface_t *svga3d_surface_from_handle(
    gfx3d_surface_handle_t handle) {
    uint32_t slot = handle & 0xFFU;
    uint8_t generation = (uint8_t)(handle >> 8);
    if (!slot || slot > SVGA3D_MAX_SURFACES) return NULL;
    slot--;
    if (!g_3d.surfaces[slot].used ||
        g_3d.surfaces[slot].generation != generation) return NULL;
    return &g_3d.surfaces[slot];
}

static void svga3d_forget_state(void) {
    kmemset(&g_state_cache, 0, sizeof(g_state_cache));
    g_3d.ready = false;
    g_3d.host_hw_version = 0U;
    g_3d.guest_hw_version = 0U;
    g_3d.vertex_vram_handle = 0U;
    g_3d.vertex_vram_offset = 0U;
    kmemset(g_3d.vertex_fence, 0, sizeof(g_3d.vertex_fence));
    g_3d.vertex_slot = 0U;
    g_3d.vertex_slot_offset = 0U;
    g_3d.vertex_active_slot = 0U;
    g_3d.vertex_base = 0U;
    g_3d.context_defined = false;
    g_3d.vertex_surface_defined = false;
    kmemset(g_3d.surfaces, 0, sizeof(g_3d.surfaces));
}

static void svga3d_release(bool emit_destroy) {
    const bk_svga_transport_ops_t *transport = g_3d.transport;
    bool same_generation = transport_active(transport) &&
        transport->generation(transport->context) ==
        g_3d.transport_generation;
    bool can_emit = emit_destroy && g_3d.ready && same_generation;

    if (can_emit) {
        for (uint32_t i = 0; i < SVGA3D_MAX_SURFACES; i++)
            if (g_3d.surfaces[i].used) {
                if (g_3d.surfaces[i].depth_defined)
                    (void)svga3d_destroy_surface_id(
                        g_3d.surfaces[i].depth_sid);
                (void)svga3d_destroy_surface_id(g_3d.surfaces[i].sid);
            }
        if (g_3d.vertex_surface_defined)
            (void)svga3d_destroy_surface_id(SVGA3D_VERTEX_SURFACE_ID);
        if (g_3d.context_defined)
            (void)svga3d_destroy_context();
        (void)transport_submit(true, NULL);
    }
    if (same_generation) {
        for (uint32_t i = 0; i < SVGA3D_MAX_SURFACES; i++)
            if (g_3d.surfaces[i].used) {
                if (g_3d.surfaces[i].vram_handle)
                    (void)transport->vram_free(transport->context,
                        g_3d.surfaces[i].vram_handle);
                if (g_3d.surfaces[i].depth_vram_handle)
                    (void)transport->vram_free(transport->context,
                        g_3d.surfaces[i].depth_vram_handle);
            }
        if (g_3d.vertex_vram_handle)
            (void)transport->vram_free(transport->context,
                                       g_3d.vertex_vram_handle);
    }
    svga3d_forget_state();
}

static bool svga3d_ensure_ready(void) {
    const bk_svga_transport_ops_t *transport = svga_transport_get();
    uint32_t generation, register_count, fifo_caps, host_version;

    if (!transport_active(transport)) {
        if (g_3d.ready) svga3d_release(false);
        g_3d.transport = transport;
        return false;
    }
    generation = transport->generation(transport->context);
    if (g_3d.transport != transport ||
        g_3d.transport_generation != generation) {
        if (g_3d.ready) svga3d_release(false);
        g_3d.transport = transport;
        g_3d.transport_generation = generation;
        g_3d.absence_reported = false;
        g_3d.validation_failed = false;
    }
    if (g_3d.ready) return true;
    if (g_3d.validation_failed) return false;
    if (!(transport->device_capabilities(transport->context) & SVGA_CAP_3D)) {
        if (!g_3d.absence_reported) {
            kprintf("[SVGA3D.DVR] SVGA3D no negociado; queda activo el driver 2D\n");
            g_3d.absence_reported = true;
        }
        return false;
    }

    register_count = transport->fifo_register_count(transport->context);
    if (register_count <= SVGA_FIFO_3D_HWVERSION) {
        if (!g_3d.absence_reported) {
            kprintf("[SVGA3D.DVR] FIFO demasiado corto para HWVERSION regs=%u\n",
                    register_count);
            g_3d.absence_reported = true;
        }
        return false;
    }
    fifo_caps = transport->fifo_capabilities(transport->context);
    host_version = 0U;
    if ((fifo_caps & SVGA_FIFO_CAP_3D_HWVERSION_REVISED) &&
        register_count > SVGA_FIFO_3D_HWVERSION_REVISED)
        host_version = transport->fifo_register_read(transport->context,
                         SVGA_FIFO_3D_HWVERSION_REVISED);
    if (!host_version)
        host_version = transport->fifo_register_read(transport->context,
                         SVGA_FIFO_3D_HWVERSION);
    if (host_version < SVGA3D_HWVERSION_WS65_B1) {
        if (!g_3d.absence_reported) {
            kprintf("[SVGA3D.DVR] host HWVERSION=%x, minimo=%x\n",
                    host_version, SVGA3D_HWVERSION_WS65_B1);
            g_3d.absence_reported = true;
        }
        return false;
    }

    /* VMWARESVGA.DVR advertised this version before CONFIG_DONE. */
    g_3d.host_hw_version = host_version;
    g_3d.guest_hw_version = SVGA3D_GUEST_HWVERSION;
    g_3d.ready = true; /* enables command emission during initialization */
    if (!transport->vram_allocate(transport->context,
            SVGA3D_VERTEX_BUFFER_BYTES, SVGA3D_VRAM_ALIGNMENT,
            &g_3d.vertex_vram_handle, &g_3d.vertex_vram_offset)) {
        svga3d_release(true);
        return false;
    }
    if (!svga3d_define_context()) {
        svga3d_release(true);
        return false;
    }
    g_3d.context_defined = true;
    if (!svga3d_define_surface_id(SVGA3D_VERTEX_SURFACE_ID, SVGA3D_BUFFER,
            (uint32_t)(SVGA3D_SURFACE_HINT_VERTEXBUFFER |
                       SVGA3D_SURFACE_HINT_DYNAMIC),
            SVGA3D_VERTEX_BUFFER_BYTES, 1U)) {
        svga3d_release(true);
        return false;
    }
    g_3d.vertex_surface_defined = true;
    if (!transport_submit(true, NULL)) {
        svga3d_release(true);
        return false;
    }
    kprintf("[SVGA3D.DVR] validating pixels over %s; protocol=%x guest=%x\n",
            transport->name, host_version, g_3d.guest_hw_version);
    if (!svga3d_run_selftest(NULL)) {
        g_3d.validation_failed = true;
        svga3d_release(true);
        kprintf("[SVGA3D.DVR] pixel validation failed; using software rendering\n");
        return false;
    }
    kprintf("[SVGA3D.DVR] activo sobre %s; pixel validation passed\n",
            transport->name);
    g_svga3d_trace_uploads = 0U;
    g_svga3d_trace_draws = 0U;
    g_svga3d_trace_begins = 0U;
    return true;
}

static bool svga3d_probe(gfx3d_info_t *info) {
    bool available = svga3d_ensure_ready();
    if (info) {
        info->available = available;
        info->driver_name = "vmware_svga3d";
        info->transport_name = g_3d.transport ? g_3d.transport->name
                                              : "ninguno";
        info->capabilities = available ?
            GFX3D_CAP_FIXED_FUNCTION | GFX3D_CAP_RENDER_TARGETS |
            GFX3D_CAP_VERTEX_BUFFERS | GFX3D_CAP_SURFACE_DMA |
            GFX3D_CAP_PRESENT | GFX3D_CAP_ALPHA_BLEND |
            GFX3D_CAP_TEXTURES | GFX3D_CAP_SCALE |
            GFX3D_CAP_TRANSFORM | GFX3D_CAP_WINDOW_SURFACES |
            GFX3D_CAP_GLYPH_ATLAS | GFX3D_CAP_TINYGL |
            GFX3D_CAP_DEPTH_BUFFER | GFX3D_CAP_DEPTH_SURFACE_IO |
            GFX3D_CAP_DEPTH_FUNCS | GFX3D_CAP_BLEND_ADDITIVE |
            GFX3D_CAP_TEXTURE_REGION_UPLOAD | GFX3D_CAP_ALPHA_TEST | GFX3D_CAP_TEXTURE_DECAL | GFX3D_CAP_TEXTURE_MIPMAPS : 0U;
        info->host_hw_version = g_3d.host_hw_version;
        info->guest_hw_version = g_3d.guest_hw_version;
        info->transport_generation = g_3d.transport_generation;
    }
    return available;
}

static void svga3d_reset_driver(void) {
    svga3d_release(true);
    g_3d.validation_failed = false;
}

static bool svga3d_create_surface(const gfx3d_surface_desc_t *desc,
                                  gfx3d_surface_handle_t *handle_out) {
    uint32_t slot, format, bpp, pitch, size;
    uint32_t vram_handle = 0U, vram_offset = 0U;
    svga3d_surface_t *surface;

    if (handle_out) *handle_out = GFX3D_SURFACE_INVALID;
    if (!svga3d_ensure_ready() || !desc || !handle_out ||
        !desc->width || !desc->height) return false;
    uint32_t levels = ((desc->flags >> 8U) & 15U) + 1U;
    uint32_t max_dimension = desc->width > desc->height ? desc->width : desc->height;
    if (levels > 1U && (!(desc->flags & GFX3D_SURFACE_TEXTURE) ||
        (desc->flags & GFX3D_SURFACE_RENDER_TARGET) || desc->format == GFX3D_FORMAT_Z16 ||
        (max_dimension >> (levels - 1U)) == 0U)) return false;
    format = svga3d_host_format(desc->format);
    bpp = svga3d_bytes_per_pixel(desc->format);
    if (!format || !bpp) return false;
    for (slot = 0; slot < SVGA3D_MAX_SURFACES; slot++)
        if (!g_3d.surfaces[slot].used) break;
    if (slot == SVGA3D_MAX_SURFACES) return false;
    pitch = ALIGN_UP((uint32_t)desc->width * bpp, SVGA3D_VRAM_ALIGNMENT);
    size = ALIGN_UP(pitch * desc->height, SVGA3D_VRAM_ALIGNMENT);
    if (!g_3d.transport->vram_allocate(g_3d.transport->context, size,
            SVGA3D_VRAM_ALIGNMENT, &vram_handle, &vram_offset)) return false;

    surface = &g_3d.surfaces[slot];
    if (++g_3d.surface_generations[slot] == 0U)
        g_3d.surface_generations[slot] = 1U;
    kmemset(surface, 0, sizeof(*surface));
    surface->generation = g_3d.surface_generations[slot];
    surface->width = desc->width;
    surface->height = desc->height;
    surface->format = desc->format;
    surface->flags = desc->flags;
    surface->mip_levels = levels;
    surface->sid = SVGA3D_FIRST_USER_SURFACE_ID + slot;
    surface->depth_sid = SVGA3D_FIRST_DEPTH_SURFACE_ID + slot;
    surface->bytes_per_pixel = bpp;
    surface->pitch = pitch;
    surface->size = size;
    surface->vram_handle = vram_handle;
    surface->vram_offset = vram_offset;
    if (!svga3d_define_surface_mips(surface->sid, format,
            svga3d_surface_flags_from_desc(desc),
            desc->width, desc->height, levels)) {
        (void)g_3d.transport->vram_free(g_3d.transport->context, vram_handle);
        kmemset(surface, 0, sizeof(*surface));
        return false;
    }
    /* FIFO ordering guarantees DEFINE precedes the first DMA/draw which uses
     * this SID. A synchronous host round-trip per texture was catastrophic
     * during Tux Racer startup; queue the define and let the frame boundary
     * kick the FIFO instead. */
    if (!transport_submit(false, NULL)) {
        (void)svga3d_destroy_surface_id(surface->sid);
        (void)transport_submit(true, NULL);
        (void)g_3d.transport->vram_free(g_3d.transport->context, vram_handle);
        kmemset(surface, 0, sizeof(*surface));
        return false;
    }
    surface->used = true;
    *handle_out = svga3d_make_handle(slot);
    return true;
}

static bool svga3d_destroy_surface(gfx3d_surface_handle_t handle) {
    svga3d_surface_t *surface;
    if (!svga3d_ensure_ready()) return false;
    surface = svga3d_surface_from_handle(handle);
    if (!surface) return false;
    if ((surface->staging_fence && !transport_wait(surface->staging_fence)) ||
        (surface->depth_fence && !transport_wait(surface->depth_fence)))
        return false;
    if (surface->depth_defined &&
        !svga3d_destroy_surface_id(surface->depth_sid)) return false;
    if (!svga3d_destroy_surface_id(surface->sid) ||
        !transport_submit(true, NULL)) return false;
    (void)g_3d.transport->vram_free(g_3d.transport->context,
                                    surface->vram_handle);
    if (surface->depth_vram_handle)
        (void)g_3d.transport->vram_free(g_3d.transport->context,
                                        surface->depth_vram_handle);
    surface->used = false;
    return true;
}

static bool svga3d_clip_surface_rect(const svga3d_surface_t *surface,
                                     const gfx_rect_t *requested,
                                     gfx_rect_t *rect_out) {
    gfx_rect_t rect;
    if (!surface || !rect_out) return false;
    rect = requested ? *requested :
        (gfx_rect_t){0, 0, surface->width, surface->height};
    if (rect.x < 0) { rect.w += rect.x; rect.x = 0; }
    if (rect.y < 0) { rect.h += rect.y; rect.y = 0; }
    if (rect.x >= surface->width || rect.y >= surface->height) return false;
    if (rect.x + rect.w > surface->width) rect.w = surface->width - rect.x;
    if (rect.y + rect.h > surface->height) rect.h = surface->height - rect.y;
    if (rect.w <= 0 || rect.h <= 0) return false;
    *rect_out = rect;
    return true;
}

static bool svga3d_upload_surface(gfx3d_surface_handle_t handle,
                                  const uint32_t *pixels,
                                  uint32_t source_pitch,
                                  const gfx_rect_t *requested) {
    svga3d_surface_t *surface;
    gfx_rect_t rect;
    uint8_t *destination;
    uint32_t row_bytes, fence = 0U;

    if (!pixels || !source_pitch || !svga3d_ensure_ready()) return false;
    surface = svga3d_surface_from_handle(handle);
    if (!surface || source_pitch < surface->width ||
        !svga3d_clip_surface_rect(surface, requested, &rect)) return false;
    if (surface->staging_fence && !transport_wait(surface->staging_fence))
        return false;
    destination = (uint8_t *)g_3d.transport->vram_pointer(
        g_3d.transport->context, surface->vram_offset, surface->size);
    if (!destination) return false;
    row_bytes = (uint32_t)rect.w * surface->bytes_per_pixel;
    for (int32_t row = 0; row < rect.h; row++) {
        const uint8_t *source_row = (const uint8_t *)pixels +
            ((uint32_t)(rect.y + row) * source_pitch + (uint32_t)rect.x) *
            surface->bytes_per_pixel;
        uint8_t *destination_row = destination +
            (uint32_t)(rect.y + row) * surface->pitch +
            (uint32_t)rect.x * surface->bytes_per_pixel;
        kmemcpy(destination_row, source_row, row_bytes);
    }
    if (!svga3d_surface_dma(surface->sid, surface->vram_offset,
            surface->pitch, surface->bytes_per_pixel,
            (uint32_t)rect.x, (uint32_t)rect.y,
            (uint32_t)rect.w, (uint32_t)rect.h,
            SVGA3D_WRITE_HOST_VRAM) ||
        !transport_submit(false, &fence)) return false;
    surface->staging_fence = fence;
    surface->staging_valid = true;
    if (g_svga3d_trace_uploads < 20U) {
        g_svga3d_trace_uploads++;
        kprintf("[SVGA3D:TRACE] upload#%u sid=%u %ux%u fence=%u\n",
                g_svga3d_trace_uploads, surface->sid,
                (uint32_t)rect.w, (uint32_t)rect.h, fence);
    }
    return true;
}

static bool svga3d_upload_surface_region(gfx3d_surface_handle_t handle,
                                         const uint32_t *pixels,
                                         uint32_t source_pitch,
                                         uint32_t destination_x,
                                         uint32_t destination_y,
                                         uint32_t width, uint32_t height) {
    svga3d_surface_t *surface;
    uint8_t *destination;
    uint32_t fence = 0U;
    if (!pixels || !source_pitch || source_pitch < width || !width || !height ||
        !svga3d_ensure_ready()) return false;
    surface = svga3d_surface_from_handle(handle);
    if (!surface || destination_x + width > surface->width ||
        destination_y + height > surface->height) return false;
    if (surface->staging_fence && !transport_wait(surface->staging_fence))
        return false;
    destination = (uint8_t *)g_3d.transport->vram_pointer(
        g_3d.transport->context, surface->vram_offset, surface->size);
    if (!destination) return false;
    for (uint32_t row = 0U; row < height; row++)
        kmemcpy(destination + (destination_y + row) * surface->pitch +
                    destination_x * surface->bytes_per_pixel,
                (const uint8_t *)pixels + row * source_pitch *
                    surface->bytes_per_pixel,
                width * surface->bytes_per_pixel);
    if (!svga3d_surface_dma(surface->sid, surface->vram_offset,
            surface->pitch, surface->bytes_per_pixel, destination_x,
            destination_y, width, height, SVGA3D_WRITE_HOST_VRAM) ||
        !transport_submit(false, &fence)) return false;
    surface->staging_fence = fence;
    surface->staging_valid = true;
    return true;
}

static bool svga3d_upload_mip(gfx3d_surface_handle_t handle, uint32_t level,
                              const uint32_t *pixels, uint32_t source_pitch) {
    if (!pixels || !source_pitch || !svga3d_ensure_ready()) return false;
    svga3d_surface_t *surface = svga3d_surface_from_handle(handle);
    if (!surface || level >= surface->mip_levels) return false;
    uint32_t width = surface->width >> level, height = surface->height >> level;
    uint32_t fence = 0U;
    if (!width) width = 1U;
    if (!height) height = 1U;
    if (source_pitch < width || !transport_wait(surface->staging_fence)) return false;
    uint8_t *destination = g_3d.transport->vram_pointer(g_3d.transport->context,
        surface->vram_offset, surface->size);
    if (!destination) return false;
    for (uint32_t row = 0U; row < height; row++)
        kmemcpy(destination + row * surface->pitch,
            (const uint8_t *)pixels + row * source_pitch * surface->bytes_per_pixel,
            width * surface->bytes_per_pixel);
    if (!svga3d_surface_dma_mip(surface->sid, surface->vram_offset,
            surface->pitch, surface->bytes_per_pixel, 0U, 0U, width, height,
            SVGA3D_WRITE_HOST_VRAM, level) || !transport_submit(false, &fence)) return false;
    surface->staging_fence = fence;
    surface->staging_valid = level == 0U;
    return true;
}

static bool svga3d_download_surface(gfx3d_surface_handle_t handle,
                                    uint32_t *pixels,
                                    uint32_t destination_pitch,
                                    const gfx_rect_t *requested) {
    svga3d_surface_t *surface;
    gfx_rect_t rect;
    uint8_t *source;
    uint32_t row_bytes;

    if (!pixels || !destination_pitch || !svga3d_ensure_ready()) return false;
    surface = svga3d_surface_from_handle(handle);
    if (!surface || destination_pitch < surface->width ||
        !svga3d_clip_surface_rect(surface, requested, &rect)) return false;
    if (surface->staging_fence && !transport_wait(surface->staging_fence))
        return false;
    if (!svga3d_surface_dma(surface->sid, surface->vram_offset,
            surface->pitch, surface->bytes_per_pixel,
            (uint32_t)rect.x, (uint32_t)rect.y,
            (uint32_t)rect.w, (uint32_t)rect.h,
            SVGA3D_READ_HOST_VRAM) || !transport_submit(true, NULL))
        return false;
    source = (uint8_t *)g_3d.transport->vram_pointer(
        g_3d.transport->context, surface->vram_offset, surface->size);
    if (!source) return false;
    row_bytes = (uint32_t)rect.w * surface->bytes_per_pixel;
    for (int32_t row = 0; row < rect.h; row++) {
        const uint8_t *source_row = source +
            (uint32_t)(rect.y + row) * surface->pitch +
            (uint32_t)rect.x * surface->bytes_per_pixel;
        uint8_t *destination_row = (uint8_t *)pixels +
            ((uint32_t)(rect.y + row) * destination_pitch +
             (uint32_t)rect.x) * surface->bytes_per_pixel;
        kmemcpy(destination_row, source_row, row_bytes);
    }
    surface->staging_fence = 0U;
    surface->staging_valid = true;
    return true;
}

static bool svga3d_upload_vertices(const gfx3d_vertex_t *vertices,
                                   uint32_t vertex_count) {
    uint32_t bytes = vertex_count * (uint32_t)sizeof(*vertices);
    uint32_t slot, base;
    void *destination;
    if (!vertices || !vertex_count || bytes > SVGA3D_VERTEX_SLOT_BYTES)
        return false;

    slot = g_3d.vertex_slot;
    if (g_3d.vertex_slot_offset + bytes > SVGA3D_VERTEX_SLOT_BYTES) {
        /* Seal the complete slot, rather than fencing every small draw.
         * All DMA/draw commands using its bytes precede this fence. No byte
         * is overwritten until we wrap around and wait for it below. */
        if (g_3d.vertex_slot_offset &&
            !transport_submit(false, &g_3d.vertex_fence[slot])) return false;
        slot = (slot + 1U) % SVGA3D_VERTEX_RING_SLOTS;
        g_3d.vertex_slot = slot;
        g_3d.vertex_slot_offset = 0U;
    }
    /* Append into unused bytes. The latest fence for this slot covers every
     * previous DMA/draw from it; only wait when wrapping back to offset zero.
     * A six-vertex glyph now occupies 168 bytes, not a whole staging slot. */
    if (!g_3d.vertex_slot_offset) {
        if (g_3d.vertex_fence[slot] &&
            !transport_wait(g_3d.vertex_fence[slot])) return false;
        g_3d.vertex_fence[slot] = 0U;
    }
    base = slot * SVGA3D_VERTEX_SLOT_BYTES + g_3d.vertex_slot_offset;
    destination = g_3d.transport->vram_pointer(g_3d.transport->context,
        g_3d.vertex_vram_offset + base, bytes);
    if (!destination) return false;
    kmemcpy(destination, vertices, bytes);

    g_3d.vertex_active_slot = slot;
    g_3d.vertex_base = base;
    g_3d.vertex_slot_offset += bytes;
    return svga3d_surface_dma(SVGA3D_VERTEX_SURFACE_ID,
        g_3d.vertex_vram_offset, SVGA3D_VERTEX_BUFFER_BYTES, 1U,
        base, 0U, bytes, 1U, SVGA3D_WRITE_HOST_VRAM);
}

static bool svga3d_emit_draw(const gfx3d_vertex_t *vertices,
                             uint32_t vertex_count, uint32_t texture_sid,
                             uint32_t flags, uint32_t *fence_out) {
    bk_svga3d_draw_primitives_t command;
    struct PACKED {
        bk_svga3d_vertex_decl_t declarations[3];
        bk_svga3d_primitive_range_t range;
    } tail;
    const uint32_t declaration_count = 3U;
    uint32_t stride = sizeof(gfx3d_vertex_t);
    uint32_t fence = 0U;

    if (fence_out) *fence_out = 0U;
    if (!vertices || vertex_count < 3U || vertex_count % 3U ||
        vertex_count > SVGA3D_MAX_DRAW_VERTICES) return false;
    if (!svga3d_upload_vertices(vertices, vertex_count) ||
        !svga3d_set_render_states(flags) ||
        !svga3d_set_texture(texture_sid, flags)) return false;

    kmemset(&tail, 0, sizeof(tail));
    /* BLESKERNOS_SVGA3D_RANGE_HINT_V8
     * SVGA3D rangeHint is half-open [first,last).  Mesa/vmwgfx uses
     * max_index + 1 for last.  Using vertex_count - 1 advertised one
     * vertex too few, so a 6-vertex triangle list claimed only 5 vertices. */
    tail.declarations[0].identity.type = SVGA3D_DECLTYPE_FLOAT4;
    tail.declarations[0].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    tail.declarations[0].identity.usage = SVGA3D_DECLUSAGE_POSITIONT;
    tail.declarations[0].array.surface_id = SVGA3D_VERTEX_SURFACE_ID;
    tail.declarations[0].array.offset = g_3d.vertex_base;
    tail.declarations[0].array.stride = stride;
    tail.declarations[0].range_hint.first = 0U;
    tail.declarations[0].range_hint.last = vertex_count;

    tail.declarations[1].identity.type = SVGA3D_DECLTYPE_D3DCOLOR;
    tail.declarations[1].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    tail.declarations[1].identity.usage = SVGA3D_DECLUSAGE_COLOR;
    tail.declarations[1].array.surface_id = SVGA3D_VERTEX_SURFACE_ID;
    tail.declarations[1].array.offset = g_3d.vertex_base + 16U;
    tail.declarations[1].array.stride = stride;
    tail.declarations[1].range_hint.first = 0U;
    tail.declarations[1].range_hint.last = vertex_count;

    tail.declarations[2].identity.type = SVGA3D_DECLTYPE_FLOAT2;
    tail.declarations[2].identity.method = SVGA3D_DECLMETHOD_DEFAULT;
    tail.declarations[2].identity.usage = SVGA3D_DECLUSAGE_TEXCOORD;
    tail.declarations[2].array.surface_id = SVGA3D_VERTEX_SURFACE_ID;
    tail.declarations[2].array.offset = g_3d.vertex_base + 20U;
    tail.declarations[2].array.stride = stride;
    tail.declarations[2].range_hint.first = 0U;
    tail.declarations[2].range_hint.last = vertex_count;
    tail.range.prim_type = SVGA3D_PRIMITIVE_TRIANGLELIST;
    tail.range.primitive_count = vertex_count / 3U;
    tail.range.index_array.surface_id = SVGA3D_INVALID_ID;

    command.cid = SVGA3D_CONTEXT_ID;
    command.num_vertex_decls = declaration_count;
    command.num_ranges = 1U;
    if (!svga3d_emit(SVGA_3D_CMD_DRAW_PRIMITIVES, &command,
            sizeof(command), &tail, (uint32_t)sizeof(tail))) return false;
    /* A compositor copy may need a public fence. Application draws defer
     * submission to END; their private vertex lifetime is tracked per slot. */
    if (fence_out && !transport_submit(false, &fence)) return false;
    if (g_svga3d_trace_draws < 12U) {
        g_svga3d_trace_draws++;
        kprintf("[SVGA3D:TRACE] draw#%u vc=%u texsid=%u flags=%x slot=%u hint=[0,%u) fence=%u\n",
                g_svga3d_trace_draws, vertex_count, texture_sid, flags,
                g_3d.vertex_active_slot, vertex_count, fence);
    }
    if (fence_out) *fence_out = fence;
    return true;
}

static bool svga3d_depth_transfer(gfx3d_surface_handle_t target_handle,
                                    uint16_t *depth, uint32_t pitch,
                                    const gfx_rect_t *requested,
                                    bool upload) {
    svga3d_surface_t *target;
    gfx_rect_t rect;
    uint8_t *staging;
    uint32_t fence = 0U;
    if (!depth || !pitch || !svga3d_ensure_ready()) return false;
    target = svga3d_surface_from_handle(target_handle);
    if (!target || pitch < target->width ||
        !svga3d_clip_surface_rect(target, requested, &rect) ||
        !svga3d_define_depth(target)) return false;
    if (target->depth_fence && !transport_wait(target->depth_fence)) return false;
    staging = (uint8_t *)g_3d.transport->vram_pointer(g_3d.transport->context,
        target->depth_vram_offset, target->depth_size);
    if (!staging) return false;
    if (!upload) {
        if (!svga3d_surface_dma(target->depth_sid,
                target->depth_vram_offset, target->depth_pitch, 2U,
                (uint32_t)rect.x, (uint32_t)rect.y,
                (uint32_t)rect.w, (uint32_t)rect.h,
                SVGA3D_READ_HOST_VRAM) || !transport_submit(true, NULL))
            return false;
    }
    for (int32_t row = 0; row < rect.h; row++) {
        uint16_t *gpu = (uint16_t *)(staging +
            (uint32_t)(rect.y + row) * target->depth_pitch) + rect.x;
        uint16_t *cpu = depth + (uint32_t)(rect.y + row) * pitch + rect.x;
        if (upload) kmemcpy(gpu, cpu, (uint32_t)rect.w * 2U);
        else kmemcpy(cpu, gpu, (uint32_t)rect.w * 2U);
    }
    if (upload) {
        if (!svga3d_surface_dma(target->depth_sid,
                target->depth_vram_offset, target->depth_pitch, 2U,
                (uint32_t)rect.x, (uint32_t)rect.y,
                (uint32_t)rect.w, (uint32_t)rect.h,
                SVGA3D_WRITE_HOST_VRAM) ||
            !transport_submit(false, &fence)) return false;
        target->depth_fence = fence;
    }
    return true;
}

static bool svga3d_depth_upload(gfx3d_surface_handle_t target,
                                const uint16_t *depth, uint32_t pitch,
                                const gfx_rect_t *rect) {
    return svga3d_depth_transfer(target, (uint16_t *)depth, pitch, rect, true);
}

static bool svga3d_depth_download(gfx3d_surface_handle_t target,
                                  uint16_t *depth, uint32_t pitch,
                                  const gfx_rect_t *rect) {
    return svga3d_depth_transfer(target, depth, pitch, rect, false);
}

static bool svga3d_clear_surface(gfx3d_surface_handle_t handle,
                                 uint32_t color, uint32_t *fence_out) {
    svga3d_surface_t *surface;
    uint32_t fence = 0U;
    if (fence_out) *fence_out = 0U;
    if (!svga3d_ensure_ready()) return false;
    surface = svga3d_surface_from_handle(handle);
    if (!surface ||
        !(surface->flags & GFX3D_SURFACE_RENDER_TARGET) ||
        !svga3d_bind_target(surface, 0U) ||
        !svga3d_set_render_states(0U) ||
        !svga3d_clear_current(color, 1.0f, GFX3D_DRAW_CLEAR_COLOR,
                              surface->width, surface->height) ||
        !transport_submit(false, &fence)) return false;
    surface->staging_valid = false;
    if (fence_out) *fence_out = fence;
    return true;
}

static uint8_t svga3d_u8_mul(uint8_t a, uint8_t b) {
    return (uint8_t)(((uint32_t)a * b + 127U) / 255U);
}

static uint32_t svga3d_vertex_color(uint32_t modulation, uint8_t opacity,
                                      bool premultiplied) {
    uint8_t a, r, g, b;
    if (!modulation) modulation = 0xFFFFFFFFU;
    a = svga3d_u8_mul((uint8_t)(modulation >> 24), opacity);
    r = (uint8_t)(modulation >> 16);
    g = (uint8_t)(modulation >> 8);
    b = (uint8_t)modulation;
    if (premultiplied) {
        r = svga3d_u8_mul(r, opacity);
        g = svga3d_u8_mul(g, opacity);
        b = svga3d_u8_mul(b, opacity);
    }
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | b;
}

static bool svga3d_composite_surface(gfx3d_surface_handle_t source_handle,
                                     gfx3d_surface_handle_t dest_handle,
                                     const gfx3d_composite_t *operation,
                                     uint32_t *fence_out) {
    svga3d_surface_t *source;
    svga3d_surface_t *destination;
    gfx3d_vertex_t vertices[6];
    gfx_rect_t src;
    float x0, y0, x1, y1, x2, y2, x3, y3;
    float u0, v0, u1, v1;
    uint32_t color, flags = GFX3D_DRAW_TEXTURED | GFX3D_DRAW_BLEND;

    if (fence_out) *fence_out = 0U;
    if (!operation || !svga3d_ensure_ready()) return false;
    source = svga3d_surface_from_handle(source_handle);
    destination = svga3d_surface_from_handle(dest_handle);
    if (!source || !destination ||
        !(source->flags & GFX3D_SURFACE_TEXTURE) ||
        !(destination->flags & GFX3D_SURFACE_RENDER_TARGET)) return false;
    src = operation->source;
    if (src.w <= 0 || src.h <= 0 || src.x < 0 || src.y < 0 ||
        src.x + src.w > source->width || src.y + src.h > source->height)
        return false;
    if (operation->filter == GFX3D_FILTER_LINEAR)
        flags |= GFX3D_DRAW_LINEAR;
    if (!svga3d_bind_target(destination, 0U) ||
        !svga3d_set_scissor(operation->clip, destination->width,
                            destination->height)) return false;

    x0 = operation->transform.tx;
    y0 = operation->transform.ty;
    x1 = operation->transform.m00 * src.w + operation->transform.tx;
    y1 = operation->transform.m10 * src.w + operation->transform.ty;
    x2 = operation->transform.m00 * src.w +
         operation->transform.m01 * src.h + operation->transform.tx;
    y2 = operation->transform.m10 * src.w +
         operation->transform.m11 * src.h + operation->transform.ty;
    x3 = operation->transform.m01 * src.h + operation->transform.tx;
    y3 = operation->transform.m11 * src.h + operation->transform.ty;
    u0 = (float)src.x / source->width;
    v0 = (float)src.y / source->height;
    u1 = (float)(src.x + src.w) / source->width;
    v1 = (float)(src.y + src.h) / source->height;
    if (operation->source_premultiplied)
        flags |= GFX3D_DRAW_PREMULTIPLIED;
    color = svga3d_vertex_color(operation->modulation_color,
                                operation->opacity,
                                operation->source_premultiplied);
    /* POSITIONT uses D3D pixel centers at integer coordinates. Our 2D
     * transforms describe pixel edges: align texel centers even for linear
     * filtering, atlas subrectangles and exact one-to-one desktop copies. */
#define SET_VERTEX(index_, px_, py_, pu_, pv_) do { \
        vertices[index_].x = (px_) - 0.5f; \
        vertices[index_].y = (py_) - 0.5f; \
        vertices[index_].z = 0.0f; vertices[index_].rhw = 1.0f; \
        vertices[index_].color = color; \
        vertices[index_].u = (pu_); vertices[index_].v = (pv_); \
    } while (0)
    SET_VERTEX(0, x0, y0, u0, v0); SET_VERTEX(1, x1, y1, u1, v0);
    SET_VERTEX(2, x2, y2, u1, v1); SET_VERTEX(3, x0, y0, u0, v0);
    SET_VERTEX(4, x2, y2, u1, v1); SET_VERTEX(5, x3, y3, u0, v1);
#undef SET_VERTEX
    destination->staging_valid = false;
    return svga3d_emit_draw(vertices, 6U, source->sid, flags, fence_out);
}

static bool svga3d_present_surface(gfx3d_surface_handle_t handle,
                                   const gfx_rect_t *requested,
                                   uint32_t *fence_out) {
    svga3d_surface_t *surface;
    bk_svga3d_present_t command;
    bk_svga3d_copy_rect_t rect;
    gfx_rect_t clipped;
    uint32_t fence = 0U;

    if (fence_out) *fence_out = 0U;
    if (!svga3d_ensure_ready()) return false;
    surface = svga3d_surface_from_handle(handle);
    if (!surface ||
        !svga3d_clip_surface_rect(surface, requested, &clipped)) return false;
    command.sid = surface->sid;
    rect.x = (uint32_t)clipped.x;
    rect.y = (uint32_t)clipped.y;
    rect.w = (uint32_t)clipped.w;
    rect.h = (uint32_t)clipped.h;
    rect.srcx = rect.x;
    rect.srcy = rect.y;
    if (!svga3d_emit(SVGA_3D_CMD_PRESENT, &command, sizeof(command),
                     &rect, sizeof(rect)) ||
        !transport_submit(false, &fence)) return false;
    if (fence_out) *fence_out = fence;
    return true;
}

static bool svga3d_begin_render(gfx3d_surface_handle_t target_handle,
                                uint32_t clear_color, float clear_depth,
                                uint32_t flags) {
    svga3d_surface_t *target;
    if (!svga3d_ensure_ready()) return false;
    target = svga3d_surface_from_handle(target_handle);
    if (!target || !(target->flags & GFX3D_SURFACE_RENDER_TARGET)) return false;
    /* Draws restore their complete state and attachments themselves. Mesa
     * opens a batch for every glBegin/glEnd, including individual glyphs.
     * A BEGIN without a clear need not emit a duplicate state baseline. */
    if (!(flags & (GFX3D_DRAW_CLEAR_COLOR | GFX3D_DRAW_CLEAR_DEPTH))) return true;
    if (!svga3d_bind_target(target, flags)) return false;
    if (!svga3d_set_render_states(flags) ||
        !svga3d_clear_current(clear_color, clear_depth, flags,
                              target->width, target->height)) return false;
    target->staging_valid = false;
    if (g_svga3d_trace_begins < 8U) {
        g_svga3d_trace_begins++;
        kprintf("[SVGA3D:TRACE] begin#%u sid=%u flags=%x clear=%x\n",
                g_svga3d_trace_begins, target->sid, flags, clear_color);
    }
    return true;
}

static bool svga3d_draw_triangles(gfx3d_surface_handle_t target_handle,
                                  gfx3d_surface_handle_t texture_handle,
                                  const gfx3d_vertex_t *vertices,
                                  uint32_t vertex_count, uint32_t flags,
                                  uint32_t *fence_out) {
    svga3d_surface_t *target;
    svga3d_surface_t *texture;
    uint32_t offset = 0U;

    if (fence_out) *fence_out = 0U;
    if (!vertices || vertex_count < 3U || vertex_count % 3U ||
        !svga3d_ensure_ready()) return false;
    target = svga3d_surface_from_handle(target_handle);
    texture = texture_handle ? svga3d_surface_from_handle(texture_handle) : NULL;
    if (!target || (texture_handle && !texture)) return false;
    if (texture && !(texture->flags & GFX3D_SURFACE_TEXTURE)) return false;
    if (!svga3d_bind_target(target, flags)) return false;
    while (offset < vertex_count) {
        uint32_t count = vertex_count - offset;
        if (count > SVGA3D_MAX_DRAW_VERTICES)
            count = SVGA3D_MAX_DRAW_VERTICES -
                    (SVGA3D_MAX_DRAW_VERTICES % 3U);
        if (!svga3d_emit_draw(vertices + offset, count,
                texture ? texture->sid : 0U,
                flags | (texture ? GFX3D_DRAW_TEXTURED : 0U),
                NULL)) return false;
        offset += count;
    }
    target->staging_valid = false;
    /* Private slot fences protect rotating vertex staging. Do not expose a
     * fence that makes Mesa wait after every draw; END kicks the ordered FIFO. */
    if (fence_out) *fence_out = 0U;
    return true;
}

static bool svga3d_end_render(gfx3d_surface_handle_t target_handle,
                              uint32_t *fence_out) {
    svga3d_surface_t *target;
    if (fence_out) *fence_out = 0U;
    if (!svga3d_ensure_ready()) return false;
    target = svga3d_surface_from_handle(target_handle);
    if (!target) return false;
    target->staging_valid = false;
    /* BLESKERNOS_SVGA3D_ASYNC_END_V7
     * END is the frame's doorbell, not a CPU/GPU barrier. The compositor
     * submits its sampling command later on the same FIFO, so FIFO ordering
     * already guarantees it sees all prior draws. Returning a fence here made
     * Mesa busy-wait once after glClear and again at render finish. */
    if (!transport_submit(false, NULL)) return false;
    return true;
}

static bool svga3d_wait_driver_fence(uint32_t fence) {
    return svga3d_ensure_ready() && transport_wait(fence);
}

static bool svga3d_run_selftest(uint32_t *fence_out) {
    gfx3d_surface_desc_t desc = {32U, 32U, GFX3D_FORMAT_ARGB8888,
        GFX3D_SURFACE_RENDER_TARGET | GFX3D_SURFACE_TEXTURE};
    gfx3d_surface_handle_t surface = 0U, texture = 0U, other = 0U;
    gfx3d_surface_handle_t pool[SVGA3D_MAX_SURFACES] = {0U};
    gfx3d_vertex_t triangle[3] = {
        {4.0f, 4.0f, 0.5f, 1.0f, 0xFFFF0000U, 0.0f, 0.0f},
        {28.0f, 4.0f, 0.5f, 1.0f, 0xFFFF0000U, 1.0f, 0.0f},
        {4.0f, 28.0f, 0.5f, 1.0f, 0xFFFF0000U, 0.0f, 1.0f}
    };
    uint32_t *pixels, texels[4] = {
        0xFFFF0000U, 0xFF00FF00U, 0xFF0000FFU, 0xFFFFFFFFU};
    const char *phase = "allocate";
    bool ok = false;
    if (fence_out) *fence_out = 0U;
    if (!svga3d_ensure_ready()) return false;
    pixels = (uint32_t *)kmalloc(32U * 32U * 4U);
    if (!pixels) return false;
    if (!svga3d_create_surface(&desc, &surface)) goto done;
    phase = "dma-roundtrip";
    for (uint32_t i = 0U; i < 32U*32U; i++) pixels[i] = 0xFF193B57U;
    if (!svga3d_upload_surface(surface, pixels, 32U, NULL)) goto done;
    {
        svga3d_surface_t *test = svga3d_surface_from_handle(surface);
        if (!transport_wait(test->staging_fence)) goto done;
        void *staging = g_3d.transport->vram_pointer(g_3d.transport->context,
            test->vram_offset, test->size);
        if (!staging) goto done;
        kmemset(staging, 0xA5, test->size);
    }
    if (!svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
    kprintf("[SVGA3D:SELFTEST] dma pixel=%x expected=0xff193b57 poison=0xa5a5a5a5\n", pixels[8U*32U+8U]);
    if (pixels[8U*32U+8U] != 0xFF193B57U) goto done;
    phase = "clear";
    if (!svga3d_begin_render(surface, 0xFF102030U, 1.0f,
            GFX3D_DRAW_CLEAR_COLOR) ||
        !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
    kprintf("[SVGA3D:SELFTEST] clear pixel=%x expected=0xff102030\n", pixels[8U*32U+8U]);
    if (pixels[8U*32U+8U] != 0xFF102030U) goto done;
    phase = "triangle";
    if (!svga3d_draw_triangles(surface, 0U, triangle, 3U, 0U, NULL) ||
        !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
    kprintf("[SVGA3D:SELFTEST] triangle pixel=%x expected=0xffff0000 outside=%x\n",
            pixels[8U*32U+8U], pixels[31U*32U+31U]);
    if (pixels[8U*32U+8U] != 0xFFFF0000U ||
        pixels[31U*32U+31U] != 0xFF102030U) goto done;
    desc.width = 2U; desc.height = 2U; desc.flags = GFX3D_SURFACE_TEXTURE;
    phase = "texture";
    if (!svga3d_create_surface(&desc, &texture) ||
        !svga3d_upload_surface(texture, texels, 2U, NULL)) goto done;
    for (uint32_t i = 0U; i < 3U; i++) triangle[i].color = 0xFFFFFFFFU;
    if (!svga3d_draw_triangles(surface, texture, triangle, 3U, 0U, NULL) ||
        !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
    kprintf("[SVGA3D:SELFTEST] texture rgb=%x/%x/%x\n",
            pixels[8U*32U+8U], pixels[6U*32U+20U], pixels[20U*32U+6U]);
    if (pixels[8U*32U+8U] != 0xFFFF0000U ||
        pixels[6U*32U+20U] != 0xFF00FF00U ||
        pixels[20U*32U+6U] != 0xFF0000FFU) goto done;
    phase = "blend-ring";
    if (!svga3d_clear_surface(surface, 0xFF000000U, NULL)) goto done;
    /* Exercise all staging slots and wrap back to a previously fenced slot. */
    {
        const uint32_t count = SVGA3D_MAX_DRAW_VERTICES - SVGA3D_MAX_DRAW_VERTICES % 3U;
        gfx3d_vertex_t *batch = kmalloc(count * sizeof(*batch));
        if (!batch) goto done;
        for (uint32_t i = 0U; i < count; i++) {
            batch[i] = triangle[i % 3U];
            batch[i].color = 0xFF000000U;
        }
        bool wrapped = true;
        for (uint32_t n = 0U; n < SVGA3D_VERTEX_RING_SLOTS + 2U; n++) {
            if (!svga3d_draw_triangles(surface, 0U, batch, count, 0U, NULL)) {
                wrapped = false;
                break;
            }
        }
        kfree(batch);
        if (!wrapped) goto done;
    }
    for (uint32_t i = 0U; i < 3U; i++) triangle[i].color = 0x80FF0000U;
    if (!svga3d_draw_triangles(surface, 0U, triangle, 3U,
            GFX3D_DRAW_BLEND, NULL) ||
        !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
    kprintf("[SVGA3D:SELFTEST] blend rgb=%x expected=0x800000\n",
            pixels[8U*32U+8U] & 0xFFFFFFU);
    if ((pixels[8U*32U+8U] & 0xFFFFFFU) != 0x00800000U) goto done;
    phase = "depth-switch";
    if (!svga3d_begin_render(surface, 0xFF000000U, 1.0f,
            GFX3D_DRAW_CLEAR_COLOR | GFX3D_DRAW_CLEAR_DEPTH)) goto done;
    for (uint32_t i = 0U; i < 3U; i++) {
        triangle[i].color = 0xFF00FF00U; triangle[i].z = 0.25f;
    }
    if (!svga3d_draw_triangles(surface, 0U, triangle, 3U,
            GFX3D_DRAW_DEPTH_TEST | GFX3D_DRAW_DEPTH_WRITE, NULL)) goto done;
    desc.width = 16U; desc.height = 16U;
    desc.flags = GFX3D_SURFACE_RENDER_TARGET | GFX3D_SURFACE_TEXTURE;
    if (!svga3d_create_surface(&desc, &other) ||
        !svga3d_clear_surface(other, 0xFF102030U, NULL)) goto done;
    /* Switching back must restore surface's depth, even without another BEGIN. */
    for (uint32_t i = 0U; i < 3U; i++) {
        triangle[i].color = 0xFFFF0000U; triangle[i].z = 0.75f;
    }
    if (!svga3d_draw_triangles(surface, 0U, triangle, 3U,
            GFX3D_DRAW_DEPTH_TEST | GFX3D_DRAW_DEPTH_WRITE, NULL) ||
        !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
    kprintf("[SVGA3D:SELFTEST] depth pixel=%x expected=0xff00ff00\n", pixels[8U*32U+8U]);
    if (pixels[8U*32U+8U] != 0xFF00FF00U) goto done;
    phase = "composite-scissor";
    {
        gfx3d_composite_t op;
        kmemset(&op, 0, sizeof(op));
        op.source = (gfx_rect_t){0, 0, 32, 32};
        op.transform = (gfx3d_transform2d_t){0.5f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f};
        op.clip = (gfx_rect_t){2, 2, 8, 8};
        op.opacity = 255U;
        op.modulation_color = 0xFFFFFFFFU;
        if (!svga3d_composite_surface(surface, other, &op, NULL) ||
            !svga3d_download_surface(other, pixels, 16U, NULL)) goto done;
        kprintf("[SVGA3D:SELFTEST] composite pixel=%x outside=%x\n",
                pixels[4U*16U+4U], pixels[0]);
        if (pixels[4U*16U+4U] != 0xFF00FF00U || pixels[0] != 0xFF102030U) goto done;
    }
    phase = "texel-centers";
    if (!svga3d_destroy_surface(texture)) goto done;
    texture = 0U;
    desc.width = 32U; desc.height = 32U;
    desc.flags = GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_RENDER_TARGET;
    if (!svga3d_create_surface(&desc, &texture)) goto done;
    for (uint32_t y = 0U; y < 32U; y++)
        for (uint32_t x = 0U; x < 32U; x++)
            pixels[y*32U+x] = 0xFF000000U | (x*5U << 16) | (y*5U << 8);
    if (!svga3d_upload_surface(texture, pixels, 32U, NULL)) goto done;
    for (uint32_t filter = 0U; filter < 2U; filter++) {
        gfx3d_composite_t op;
        kmemset(&op, 0, sizeof(op));
        op.source = (gfx_rect_t){3, 5, 16, 16};
        op.transform = (gfx3d_transform2d_t){1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        op.clip = (gfx_rect_t){0, 0, 16, 16};
        op.opacity = 255U; op.modulation_color = 0xFFFFFFFFU;
        op.filter = filter ? GFX3D_FILTER_LINEAR : GFX3D_FILTER_NEAREST;
        if (!svga3d_composite_surface(texture, other, &op, NULL) ||
            !svga3d_download_surface(other, pixels, 16U, NULL)) goto done;
        for (uint32_t y = 0U; y < 16U; y++) {
            for (uint32_t x = 0U; x < 16U; x++) {
                uint32_t expected = 0xFF000000U |
                    ((x+3U)*5U << 16) | ((y+5U)*5U << 8);
                if (pixels[y*16U+x] != expected) {
                    kprintf("[SVGA3D:SELFTEST] texel filter=%u xy=%u,%u got=%x expected=%x\n",
                            filter, x, y, pixels[y*16U+x], expected);
                    goto done;
                }
            }
        }
    }
    phase = "published-frame";
    {
        gfx3d_composite_t op;
        for (uint32_t i = 0U; i < 32U*32U; i++) pixels[i] = 0x8040A060U;
        if (!svga3d_upload_surface(texture, pixels, 32U, NULL) ||
            !svga3d_clear_surface(other, 0U, NULL)) goto done;
        kmemset(&op, 0, sizeof(op));
        op.source = (gfx_rect_t){0, 0, 16, 16};
        op.clip = op.source;
        op.transform = (gfx3d_transform2d_t){1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        op.opacity = 255U; op.modulation_color = 0xFFFFFFFFU;
        op.source_premultiplied = true;
        if (!svga3d_composite_surface(texture, other, &op, NULL)) goto done;
        /* The producer starts its next frame before the consumer samples the
         * published copy. Neither RGB nor alpha of the copy may change. */
        kmemset(pixels, 0, 32U*32U*4U);
        if (!svga3d_upload_surface(texture, pixels, 32U, NULL) ||
            !svga3d_download_surface(other, pixels, 16U, NULL)) goto done;
        kprintf("[SVGA3D:SELFTEST] published pixel=%x expected=0x8040a060\n", pixels[0]);
        for (uint32_t i = 0U; i < 16U*16U; i++)
            if (pixels[i] != 0x8040A060U) goto done;
    }
    phase = "alpha-test";
    /* Texture alpha must participate in the comparison before color/depth
     * writes. Check all comparisons below, at, and above reference 128. */
    for (uint32_t ref = 127U; ref <= 129U; ref++) {
        for (uint32_t func = 0U; func < 8U; func++) {
            bool expected[] = {false, 128U < ref, 128U == ref, 128U <= ref,
                128U > ref, 128U != ref, 128U >= ref, true};
            for (uint32_t i = 0U; i < 32U*32U; i++) pixels[i] = 0x80FFFFFFU;
            if (!svga3d_upload_surface(texture, pixels, 32U, NULL) ||
                !svga3d_clear_surface(surface, 0xFF102030U, NULL) ||
                !svga3d_draw_triangles(surface, texture, triangle, 3U,
                    GFX3D_DRAW_ALPHA(func, ref), NULL) ||
                !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
            if (pixels[8U*32U+8U] != (expected[func] ? 0x80FF0000U : 0xFF102030U)) {
                kprintf("[SVGA3D:SELFTEST] alpha func=%u ref=%u pixel=%x\n",
                        func, ref, pixels[8U*32U+8U]);
                goto done;
            }
        }
    }
    kprintf("[SVGA3D:SELFTEST] alpha comparisons=24 PASS\n");
    phase = "split-batch";
    {
        const uint32_t first_count = SVGA3D_MAX_DRAW_VERTICES - SVGA3D_MAX_DRAW_VERTICES % 3U;
        const uint32_t count = first_count + 3U;
        gfx3d_vertex_t *batch = kmalloc(count * sizeof(*batch));
        if (!batch) goto done;
        for (uint32_t i = 0U; i < count; i++) {
            batch[i] = triangle[i % 3U];
            batch[i].color = i < first_count ? 0xFFFF0000U : 0xFF00FF00U;
        }
        bool split_ok = svga3d_draw_triangles(surface, 0U, batch, count, 0U, NULL);
        kfree(batch);
        if (!split_ok || !svga3d_download_surface(surface, pixels, 32U, NULL) ||
            pixels[8U*32U+8U] != 0xFF00FF00U) goto done;
    }
    kprintf("[SVGA3D:SELFTEST] split-batch PASS\n");
    phase = "texture-decal";
    for (uint32_t a = 0U; a <= 2U; a++) {
        uint32_t alpha = a == 0U ? 0U : a == 1U ? 128U : 255U;
        for (uint32_t i = 0U; i < 32U*32U; i++) pixels[i] = (alpha << 24) | 0x0000FF00U;
        for (uint32_t i = 0U; i < 3U; i++) triangle[i].color = 0x40FF0000U;
        if (!svga3d_upload_surface(texture, pixels, 32U, NULL) ||
            !svga3d_draw_triangles(surface, texture, triangle, 3U,
                GFX3D_DRAW_TEXTURE_DECAL, NULL) ||
            !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
        uint32_t expected = 0x40000000U | ((255U-alpha) << 16) | (alpha << 8);
        if (pixels[8U*32U+8U] != expected) {
            kprintf("[SVGA3D:SELFTEST] decal alpha=%u pixel=%x expected=%x\n",
                    alpha, pixels[8U*32U+8U], expected);
            goto done;
        }
    }
    kprintf("[SVGA3D:SELFTEST] decal RGB+alpha PASS\n");
    phase = "texture-mipmaps";
    if (!svga3d_destroy_surface(texture)) goto done;
    texture = 0U;
    desc = (gfx3d_surface_desc_t){2U, 2U, GFX3D_FORMAT_ARGB8888,
        GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_MIP_LEVELS(2U)};
    if (!svga3d_create_surface(&desc, &texture)) goto done;
    for (uint32_t i = 0U; i < 4U; i++) texels[i] = 0xFFFF0000U;
    uint32_t small_mip = 0xFF00FF00U;
    if (!svga3d_upload_mip(texture, 0U, texels, 2U) ||
        !svga3d_upload_mip(texture, 1U, &small_mip, 1U) ||
        svga3d_upload_mip(texture, 2U, &small_mip, 1U)) goto done;
    for (uint32_t test = 0U; test < 4U; test++) {
        float span = test ? 64.0f : 1.0f;
        triangle[0] = (gfx3d_vertex_t){4, 4, .5f, 1, 0xFFFFFFFFU, 0, 0};
        triangle[1] = (gfx3d_vertex_t){28, 4, .5f, 1, 0xFFFFFFFFU, span, 0};
        triangle[2] = (gfx3d_vertex_t){4, 28, .5f, 1, 0xFFFFFFFFU, 0, span};
        if (test == 2U) {
            small_mip = 0xFF0000FFU;
            if (!svga3d_upload_mip(texture, 1U, &small_mip, 1U)) goto done;
        }
        uint32_t expected = test == 1U ? 0xFF00FF00U :
            test == 2U ? 0xFF0000FFU : 0xFFFF0000U;
        if (!svga3d_draw_triangles(surface, texture, triangle, 3U,
                test == 3U ? 0U : GFX3D_DRAW_MIPMAP_NEAREST, NULL) ||
            !svga3d_download_surface(surface, pixels, 32U, NULL)) goto done;
        if (pixels[8U*32U+8U] != expected) {
            kprintf("[SVGA3D:SELFTEST] mip test=%u pixel=%x expected=%x\n",
                    test, pixels[8U*32U+8U], expected);
            goto done;
        }
    }
    kprintf("[SVGA3D:SELFTEST] mipmaps near+far+update+off PASS\n");
    phase = "surface-budget";
    if (!svga3d_destroy_surface(texture)) goto done;
    texture = 0U;
    desc = (gfx3d_surface_desc_t){32U, 32U, GFX3D_FORMAT_ARGB8888,
        GFX3D_SURFACE_TEXTURE | GFX3D_SURFACE_RENDER_TARGET};
    if (!svga3d_create_surface(&desc, &texture)) goto done;
    if (!svga3d_begin_render(texture, 0U, 1.0f, GFX3D_DRAW_CLEAR_DEPTH) ||
        !svga3d_begin_render(other, 0U, 1.0f, GFX3D_DRAW_CLEAR_DEPTH)) goto done;
    desc.width = 8U; desc.height = 8U;
    for (uint32_t i = 0U; i < SVGA3D_MAX_SURFACES - 3U; i++) {
        if (!svga3d_create_surface(&desc, &pool[i]) ||
            !svga3d_begin_render(pool[i], 0xFF123456U, 1.0f,
                GFX3D_DRAW_CLEAR_COLOR | GFX3D_DRAW_CLEAR_DEPTH)) goto done;
    }
    if (!svga3d_download_surface(pool[SVGA3D_MAX_SURFACES - 4U], pixels,
            8U, NULL) || pixels[0] != 0xFF123456U) goto done;
    kprintf("[SVGA3D:SELFTEST] surfaces=%u color+depth+vertices=%u\n",
            SVGA3D_MAX_SURFACES, 2U * SVGA3D_MAX_SURFACES + 1U);
    ok = true;
done:
    kprintf("[SVGA3D:SELFTEST] %s phase=%s\n", ok ? "PASS" : "FAIL", phase);
    (void)svga3d_set_render_target(SVGA3D_INVALID_ID, SVGA3D_RT_COLOR0);
    (void)svga3d_set_render_target(SVGA3D_INVALID_ID, SVGA3D_RT_DEPTH);
    (void)svga3d_set_texture(0U, 0U);
    for (uint32_t i = 0U; i < SVGA3D_MAX_SURFACES; i++)
        if (pool[i]) (void)svga3d_destroy_surface(pool[i]);
    if (other) (void)svga3d_destroy_surface(other);
    if (texture) (void)svga3d_destroy_surface(texture);
    if (surface) (void)svga3d_destroy_surface(surface);
    kfree(pixels);
    return ok;
}

static bool vmware_svga3d_driver_init(void) {
    static const gfx3d_driver_ops_t ops = {
        BK_GFX3D_DRIVER_ABI_VERSION,
        sizeof(gfx3d_driver_ops_t),
        "vmware_svga3d",
        300U,
        GFX3D_CAP_FIXED_FUNCTION | GFX3D_CAP_RENDER_TARGETS |
        GFX3D_CAP_VERTEX_BUFFERS | GFX3D_CAP_SURFACE_DMA |
        GFX3D_CAP_PRESENT | GFX3D_CAP_ALPHA_BLEND |
        GFX3D_CAP_TEXTURES | GFX3D_CAP_SCALE | GFX3D_CAP_TRANSFORM |
        GFX3D_CAP_WINDOW_SURFACES | GFX3D_CAP_GLYPH_ATLAS |
        GFX3D_CAP_TINYGL | GFX3D_CAP_DEPTH_BUFFER |
        GFX3D_CAP_DEPTH_SURFACE_IO | GFX3D_CAP_DEPTH_FUNCS |
        GFX3D_CAP_BLEND_ADDITIVE | GFX3D_CAP_TEXTURE_REGION_UPLOAD | GFX3D_CAP_ALPHA_TEST | GFX3D_CAP_TEXTURE_DECAL | GFX3D_CAP_TEXTURE_MIPMAPS,
        svga3d_probe,
        svga3d_reset_driver,
        svga3d_create_surface,
        svga3d_destroy_surface,
        svga3d_upload_surface,
        svga3d_download_surface,
        svga3d_clear_surface,
        svga3d_composite_surface,
        svga3d_present_surface,
        svga3d_begin_render,
        svga3d_draw_triangles,
        svga3d_end_render,
        svga3d_wait_driver_fence,
        svga3d_run_selftest,
        svga3d_upload_surface_region,
        svga3d_depth_upload,
        svga3d_depth_download,
        svga3d_upload_mip
    };
    if (!gfx3d_register_driver(&ops)) return false;
    kprintf("[SVGA3D.DVR] extension 3D registrada; activacion por capacidades\n");
    return true;
}

static void vmware_svga3d_driver_shutdown(void) {
    svga3d_release(true);
}

const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module = {
        BK_DRIVER_ABI_VERSION,
        sizeof(bk_driver_module_t),
        1U, 0U,
        "vmware_svga3d",
        "VMware SVGA3D: surfaces, composición, alfa y TinyGL",
        vmware_svga3d_driver_init,
        vmware_svga3d_driver_shutdown
    };
    return &module;
}
