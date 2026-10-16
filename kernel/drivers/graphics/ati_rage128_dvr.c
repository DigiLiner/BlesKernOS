/*
 * BlesKernOS external 2D graphics driver for ATI Rage Mobility M3 / Rage 128
 * PCI 1002:4C46 (RAGE128_LF).
 *
 * Version 2 keeps the firmware mode as its safe bootstrap, then adds a classic
 * 64x64 hardware cursor, packed-YUV overlay, and conservative direct CRTC/PPLL
 * mode changes among standard 60 Hz modes at the firmware pixel depth.  Every modeset is transactional
 * and restores the firmware state if register validation or engine restart fails.
 *
 * Register programming follows ATI's Rage 128 register reference and the
 * long-lived aty128fb / xf86-video-r128 acceleration paths.  This is original
 * integration code for BlesKernOS; no third-party source is copied verbatim.
 */

#include "../../include/types.h"
#include "../../include/perfmon.h"
#include "../../include/pit.h"
#include "../../include/rgb565.h"
#include "../../include/memory.h"
#include "../../include/driver.h"
#include "../../include/gfx.h"
#include "../../include/gfx_driver.h"
#include "../../include/pci.h"
#include "../../include/vesa.h"
#include "../../include/vga.h"
#include "../../include/rage128_engine.h"

#define ATI_VENDOR_ID              0x1002U
#define ATI_RAGE128_LF_DEVICE_ID    0x4C46U

#define R128_TIMEOUT                2000000U
#define R128_MAX_COORD              2048U
/* La GUI siempre compone en RAM cacheada. Esta reserva se usa únicamente
 * como staging: CPU sube dirty rects de forma secuencial y el motor 2D hace
 * el BitBlt final VRAM -> scanout. */
#define R128_VRAM_STAGING           1

/* Mobility M3 flat-panel table. XFree86/X11Libre searches the first 64 KiB
 * of the VGA option ROM for the literal "M3      ". */
#define R128_VBIOS_BASE             0x000C0000U
#define R128_VBIOS_SCAN_SIZE        0x00010000U
#define R128_VBIOS_PANEL_X_OFFSET   25U
#define R128_VBIOS_PANEL_Y_OFFSET   27U

/* MMIO / PLL registers used by the conservative 2D path. */
#define R128_CLOCK_CNTL_INDEX       0x0008U
#define R128_BIOS_0_SCRATCH         0x0010U
#define R128_CLOCK_CNTL_DATA        0x000CU
#define R128_PLL_WR_EN              (1U << 7)
#define R128_GEN_RESET_CNTL         0x00F0U
#define R128_SOFT_RESET_GUI         (1U << 0)
#define R128_CONFIG_MEMSIZE         0x00F8U
#define R128_PC_NGUI_CTLSTAT        0x0184U
#define R128_PC_FLUSH_ALL           0x000000FFU
#define R128_PC_BUSY                (1U << 31)
#define R128_PM4_BUFFER_CNTL        0x0704U
#define R128_PM4_MICRO_CNTL         0x07FCU
#define R128_PM4_NONPM4             0x00000000U
#define R128_MCLK_CNTL              0x000FU
#define R128_FORCE_GCP              (1U << 16)
#define R128_FORCE_PIPE3D_CP        (1U << 17)

#define R128_SRC_PITCH_OFFSET       0x1428U
#define R128_DST_PITCH_OFFSET       0x142CU
#define R128_SRC_Y_X                0x1434U
#define R128_DST_Y_X                0x1438U
#define R128_DST_HEIGHT_WIDTH       0x143CU
#define R128_DP_GUI_MASTER_CNTL     0x146CU
#define R128_DP_BRUSH_BKGD_CLR      0x1478U
#define R128_DP_BRUSH_FRGD_CLR      0x147CU
#define R128_CLR_CMP_CNTL           0x15C0U
#define R128_CLR_CMP_MASK           0x15CCU
#define R128_DP_SRC_FRGD_CLR        0x15D8U
#define R128_DP_SRC_BKGD_CLR        0x15DCU
#define R128_DST_WIDTH_HEIGHT       0x1598U
#define R128_AUX_SC_CNTL            0x1660U
#define R128_DP_CNTL                0x16C0U
#define R128_DP_DATATYPE            0x16C4U
#define R128_DP_WRITE_MASK          0x16CCU
#define R128_DEFAULT_OFFSET         0x16E0U
#define R128_DEFAULT_PITCH          0x16E4U
#define R128_DEFAULT_SC_BOTTOM_RIGHT 0x16E8U
#define R128_SC_TOP_LEFT            0x16ECU
#define R128_SC_BOTTOM_RIGHT        0x16F0U
#define R128_GUI_STAT               0x1740U
#define R128_SCALE_3D_CNTL          0x1A00U

/* Display controller / hardware cursor. */
#define R128_CRTC_GEN_CNTL          0x0050U
#define R128_CRTC_EXT_CNTL          0x0054U
#define R128_CRTC_H_TOTAL_DISP      0x0200U
#define R128_CRTC_H_SYNC_STRT_WID   0x0204U
#define R128_CRTC_V_TOTAL_DISP      0x0208U
#define R128_CRTC_V_SYNC_STRT_WID   0x020CU
#define R128_CRTC_OFFSET            0x0224U
#define R128_CRTC_OFFSET_CNTL       0x0228U
#define R128_CRTC_PITCH             0x022CU
#define R128_FP_CRTC_H_TOTAL_DISP   0x0250U
#define R128_FP_CRTC_V_TOTAL_DISP   0x0254U
#define R128_FP_GEN_CNTL            0x0284U
#define R128_FP_SEL_CRTC2           (1U << 13)
#define R128_FP_DONT_SHADOW_VPAR    (1U << 16)
#define R128_FP_DONT_SHADOW_HEND    (1U << 17)
#define R128_FP_USE_SHADOW_VEND     (1U << 18)
#define R128_FP_USE_SHADOW_ROWCUR   (1U << 19)
#define R128_FP_HORZ_DIV2_EN        (1U << 20)
#define R128_FP_HOR_CRT_DIV2_DIS    (1U << 21)
#define R128_FP_CRT_SYNC_SEL        (1U << 23)
#define R128_FP_USE_SHADOW_EN       (1U << 24)
#define R128_FP_HORZ_STRETCH        0x028CU
#define R128_FP_VERT_STRETCH        0x0290U
#define R128_FP_H_SYNC_STRT_WID     0x02C4U
#define R128_FP_V_SYNC_STRT_WID     0x02C8U
#define R128_LVDS_GEN_CNTL          0x02D0U
#define R128_FP_OUT_EN              (1U << 22)
#define R128_FP_ACTIVE              (1U << 23)
#define R128_LVDS_ON                (1U << 0)
#define R128_LVDS_EN                (1U << 7)
#define R128_LVDS_DIGON             (1U << 18)
#define R128_LVDS_SEL_CRTC2         (1U << 23)
#define R128_HORZ_STRETCH_RATIO_MASK 0x0000FFFFU
#define R128_HORZ_STRETCH_RATIO_MAX 4096U
#define R128_HORZ_PANEL_SIZE        0x00FF0000U
#define R128_HORZ_PANEL_SHIFT       16U
#define R128_HORZ_STRETCH_BLEND     (1U << 25)
#define R128_HORZ_STRETCH_ENABLE    (1U << 26)
#define R128_HORZ_AUTO_RATIO_FIX_EN (1U << 31)
#define R128_VERT_PANEL_SIZE        0x000007FFU
#define R128_VERT_PANEL_SHIFT       0U
#define R128_VERT_STRETCH_RATIO_MASK 0x000003FFU
#define R128_VERT_STRETCH_RATIO_SHIFT 11U
#define R128_VERT_STRETCH_RATIO_MAX 1024U
#define R128_VERT_STRETCH_ENABLE    (1U << 24)
#define R128_VERT_STRETCH_BLEND     (1U << 25)
#define R128_VERT_AUTO_RATIO_EN     (1U << 26)
#define R128_DDA_CONFIG             0x02E0U
#define R128_DDA_ON_OFF             0x02E4U
#define R128_DDA_XFER_MASK          0x00003FFFU
#define R128_DDA_PRECISION_MASK     0x000F0000U
#define R128_DDA_LOOP_MASK          0x01F00000U
#define R128_CRTC_PIX_WIDTH_MASK    (7U << 8)
#define R128_CRTC_DBL_SCAN_EN       (1U << 0)
#define R128_CRTC_INTERLACE_EN      (1U << 1)
#define R128_CRTC_CSYNC_EN          (1U << 4)
#define R128_CRTC_CUR_EN            (1U << 16)
#define R128_CRTC_EXT_DISP_EN       (1U << 24)
#define R128_CRTC_EN                (1U << 25)
#define R128_CRTC_H_SYNC_POL        (1U << 23)
#define R128_CRTC_V_SYNC_POL        (1U << 23)
#define R128_CRTC_HSYNC_DIS         (1U << 8)
#define R128_CRTC_VSYNC_DIS         (1U << 9)
#define R128_CRTC_DISPLAY_DIS       (1U << 10)
#define R128_CUR_OFFSET             0x0260U
#define R128_CUR_HORZ_VERT_POSN     0x0264U
#define R128_CUR_HORZ_VERT_OFF      0x0268U
#define R128_CUR_CLR0               0x026CU
#define R128_CUR_CLR1               0x0270U
#define R128_CUR_LOCK               (1U << 31)
#define R128_CURSOR_WIDTH           64U
#define R128_CURSOR_HEIGHT          64U
#define R128_CURSOR_BYTES           1024U

/* Pixel PLL. */
#define R128_PPLL_CNTL              0x0002U
#define R128_PPLL_REF_DIV           0x0003U
#define R128_PPLL_DIV_3             0x0007U
#define R128_VCLK_ECP_CNTL          0x0008U
#define R128_HTOTAL_CNTL            0x0009U
#define R128_PPLL_RESET             (1U << 0)
#define R128_PPLL_ATOMIC_UPDATE_EN  (1U << 16)
#define R128_PPLL_VGA_ATOMIC_UPDATE_EN (1U << 17)
#define R128_PPLL_ATOMIC_UPDATE     (1U << 15)
#define R128_PPLL_FB3_DIV_MASK      0x000007FFU
#define R128_PPLL_POST3_DIV_MASK    0x00070000U
#define R128_VCLK_SRC_SEL_MASK      0x00000003U
#define R128_VCLK_SRC_SEL_CPUCLK    0x00000000U
#define R128_VCLK_SRC_SEL_PPLLCLK   0x00000003U
#define R128_PLL_DIV_SEL            (3U << 8)

/* Rage 128 packed-YUV overlay (one unit). */
#define R128_OV0_Y_X_START          0x0400U
#define R128_OV0_Y_X_END            0x0404U
#define R128_OV0_EXCLUSIVE_HORZ     0x0408U
#define R128_OV0_REG_LOAD_CNTL      0x0410U
#define R128_OV0_SCALE_CNTL         0x0420U
#define R128_OV0_V_INC              0x0424U
#define R128_OV0_P1_V_ACCUM_INIT    0x0428U
#define R128_OV0_P23_V_ACCUM_INIT   0x042CU
#define R128_OV0_P1_BLANK_LINES_AT_TOP 0x0430U
#define R128_OV0_VID_BUF0_BASE_ADRS 0x0440U
#define R128_OV0_VID_BUF_PITCH0_VALUE 0x0460U
#define R128_OV0_AUTO_FLIP_CNTL     0x0470U
#define R128_OV0_H_INC              0x0480U
#define R128_OV0_STEP_BY            0x0484U
#define R128_OV0_P1_H_ACCUM_INIT    0x0488U
#define R128_OV0_P23_H_ACCUM_INIT   0x048CU
#define R128_OV0_P1_X_START_END     0x0494U
#define R128_OV0_P2_X_START_END     0x0498U
#define R128_OV0_P3_X_START_END     0x049CU
#define R128_OV0_FILTER_CNTL        0x04A0U
#define R128_OV0_COLOUR_CNTL        0x04E0U
#define R128_OV0_GRAPHICS_KEY_CLR   0x04ECU
#define R128_OV0_GRAPHICS_KEY_MSK   0x04F0U
#define R128_OV0_KEY_CNTL           0x04F4U
#define R128_GRAPHIC_KEY_FN_NE      0x00000050U
#define R128_OVERLAY_KEY            0x00FF00FFU
#define R128_OVERLAY_MIN_FREE       4096U

#define R128_GUI_FIFOCNT_MASK       0x00000FFFU
#define R128_GUI_ACTIVE             (1U << 31)
#define R128_HOST_BIG_ENDIAN_EN     (1U << 29)

#define R128_DEFAULT_SC_RIGHT_MAX   0x00001FFFU
#define R128_DEFAULT_SC_BOTTOM_MAX  0x1FFF0000U

/* DP_CNTL direction bits. */
#define R128_DST_X_LEFT_TO_RIGHT    (1U << 0)
#define R128_DST_Y_TOP_TO_BOTTOM    (1U << 1)

/* DP_GUI_MASTER_CNTL fields. */
#define R128_GMC_SRC_PITCH_OFFSET_CNTL (1U << 0)
#define R128_GMC_DST_PITCH_OFFSET_CNTL (1U << 1)
#define R128_GMC_BRUSH_SOLID_COLOR     (13U << 4)
#define R128_GMC_BRUSH_NONE            (15U << 4)
#define R128_GMC_DST_8BPP               (2U << 8)
#define R128_GMC_DST_16BPP              (4U << 8)
#define R128_GMC_DST_32BPP              (6U << 8)
#define R128_GMC_SRC_DATATYPE_COLOR     (3U << 12)
#define R128_DP_SRC_SOURCE_MEMORY       (2U << 24)
#define R128_GMC_CLR_CMP_CNTL_DIS       (1U << 28)
#define R128_GMC_AUX_CLIP_DIS           (1U << 29)

/* ROP3 encodings. */
#define R128_ROP3_SRCCOPY            0x00CC0000U
#define R128_ROP3_SRCAND             0x00880000U
#define R128_ROP3_SRCINVERT          0x00660000U
#define R128_ROP3_SRCPAINT           0x00EE0000U
#define R128_ROP3_DSTINVERT          0x00550000U
#define R128_ROP3_PATCOPY            0x00F00000U

typedef struct {
    uint16_t width, height;
    uint16_t h_total, h_sync_start, h_sync_end;
    uint16_t v_total, v_sync_start, v_sync_end;
    uint32_t pixel_khz;
    bool neg_hsync, neg_vsync;
} r128_mode_t;

static const r128_mode_t g_r128_modes[] = {
    {640, 480, 800, 656, 752, 525, 490, 492, 25175U, true, true},
    {800, 600, 1056, 840, 968, 628, 601, 605, 40000U, false, false},
    {1024, 768, 1344, 1048, 1184, 806, 771, 777, 65000U, true, true},
    {1280, 1024, 1688, 1328, 1440, 1066, 1025, 1028, 108000U, false, false},
    {1400, 1050, 1864, 1488, 1632, 1089, 1053, 1057, 121750U, true, false},
};

typedef struct {
    const pci_device_t *pci;
    volatile uint8_t *mmio;
    uint32_t fb_bar;
    uint32_t mmio_bar;
    uint32_t vram_size;
    uint32_t fb_offset;
    uint32_t pitch_offset;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint8_t bpp;
    bool detected;
    bool active;
    bool accel_ok;
    bool engine_busy;
    bool engine_saved;

    uint32_t stat_hw_fill;
    uint32_t stat_hw_blit;
    uint32_t stat_present_calls;
    uint32_t stat_present_rects;
    uint32_t stat_present_cpu_bytes;
    uint32_t stat_present_vram_calls;
    uint32_t stat_present_staging_calls;
    uint32_t stat_idle_waits;
    uint32_t stat_timeouts;

    bool surface_used;
    uint16_t surface_width;
    uint16_t surface_height;
    uint32_t surface_offset;
    uint32_t surface_size;
    uint32_t surface_pitch_offset;

    uint32_t saved_pm4_buffer_cntl;
    uint32_t saved_default_offset;
    uint32_t saved_default_pitch;
    uint32_t saved_default_sc_bottom_right;
    uint32_t saved_aux_sc_cntl;
    uint32_t saved_sc_top_left;
    uint32_t saved_sc_bottom_right;
    uint32_t saved_dp_gui_master_cntl;
    uint32_t saved_dp_cntl;
    uint32_t saved_dp_datatype;
    uint32_t saved_dp_write_mask;

    bool display_saved;
    bool direct_modeset_ok;
    bool firmware_info_valid;
    gfx_info_t firmware_info;
    uint16_t native_width;
    uint16_t native_height;
    uint32_t current_pixel_khz;
    uint32_t native_pixel_khz;
    uint32_t pll_min_khz,pll_max_khz;
    uint32_t saved_crtc_gen_cntl;
    uint32_t saved_crtc_ext_cntl;
    uint32_t saved_crtc_h_total_disp;
    uint32_t saved_crtc_h_sync_strt_wid;
    uint32_t saved_crtc_v_total_disp;
    uint32_t saved_crtc_v_sync_strt_wid;
    uint32_t saved_crtc_offset;
    uint32_t saved_crtc_offset_cntl;
    uint32_t saved_crtc_pitch;
    uint32_t saved_fp_crtc_h_total_disp;
    uint32_t saved_fp_gen_cntl;
    uint32_t saved_fp_crtc_v_total_disp;
    uint32_t saved_fp_h_sync_strt_wid;
    uint32_t saved_fp_v_sync_strt_wid;
    uint32_t saved_fp_horz_stretch;
    uint32_t saved_fp_vert_stretch;
    uint32_t saved_lvds_gen_cntl;
    bool panel_native_valid;
    bool panel_native_vbios;
    uint32_t saved_dda_config;
    uint32_t saved_dda_on_off;
    uint32_t saved_ppll_cntl;
    uint32_t saved_ppll_ref_div;
    uint32_t saved_ppll_div_3;
    uint32_t saved_ppll_active_div;
    uint32_t saved_clock_cntl_index;
    uint32_t panel_vbios_offset;
    uint32_t saved_vclk_ecp_cntl;
    uint32_t saved_htotal_cntl;

    bool cursor_ready;
    bool cursor_defined;
    bool cursor_visible;
    uint32_t cursor_offset;
    uint16_t cursor_hot_x;
    uint16_t cursor_hot_y;
    int cursor_x;
    int cursor_y;

    bool overlay_ready;
    bool overlay_active;
    uint32_t overlay_offset;
    uint32_t overlay_size;
    uint32_t saved_ov0_scale_cntl;
    uint32_t saved_ov0_key_cntl;
    uint32_t saved_ov0_graphics_key_clr;
    uint32_t saved_ov0_graphics_key_msk;
    uint32_t saved_ov0_filter_cntl;
    uint32_t saved_ov0_colour_cntl;
} r128_state_t;

static r128_state_t g_r128;

static uint32_t r128_align_up(uint32_t value, uint32_t alignment);
static uint32_t r128_align_down(uint32_t value, uint32_t alignment);

static void r128_stats_reset(void) {
    g_r128.stat_hw_fill = 0U;
    g_r128.stat_hw_blit = 0U;
    g_r128.stat_present_calls = 0U;
    g_r128.stat_present_rects = 0U;
    g_r128.stat_present_cpu_bytes = 0U;
    g_r128.stat_present_vram_calls = 0U;
    g_r128.stat_present_staging_calls = 0U;
    g_r128.stat_idle_waits = 0U;
    g_r128.stat_timeouts = 0U;
}

static void r128_stats_report(const char *reason) {
    kprintf("[ATIR128:PERF] %s 2D=%s HWfill=%u HWblit=%u "
            "present=%u VRAMpresent=%u staged=%u rects=%u CPUbytes=%u "
            "waits=%u timeouts=%u\n",
            reason ? reason : "estado",
            g_r128.accel_ok ? "activo" : "inactivo",
            g_r128.stat_hw_fill, g_r128.stat_hw_blit,
            g_r128.stat_present_calls, g_r128.stat_present_vram_calls,
            g_r128.stat_present_staging_calls, g_r128.stat_present_rects,
            g_r128.stat_present_cpu_bytes, g_r128.stat_idle_waits,
            g_r128.stat_timeouts);
}

static uint32_t r128_bytes_per_pixel(void) {
    return g_r128.bpp == 32U ? 4U : (g_r128.bpp == 16U ? 2U : 1U);
}

/* Rage 128 encodes 2D pitch in groups of eight pixels, independent of bpp. */
static uint32_t r128_pitch_units(void) {
    uint32_t bytes_per_pixel = r128_bytes_per_pixel();
    uint32_t pixels_per_line;
    if (!bytes_per_pixel || ((uint32_t)g_r128.pitch % bytes_per_pixel) != 0U)
        return 0U;
    pixels_per_line = (uint32_t)g_r128.pitch / bytes_per_pixel;
    if ((pixels_per_line & 7U) != 0U) return 0U;
    return pixels_per_line >> 3;
}

static uint32_t r128_visible_end(void) {
    return g_r128.fb_offset + (uint32_t)g_r128.pitch * g_r128.height;
}

static bool r128_surface_layout(uint32_t *offset_out, uint32_t *size_out) {
    uint32_t offset;
    uint32_t size;
    uint32_t limit;
    uint32_t pitch_units;
    if (!R128_VRAM_STAGING || !g_r128.active || !g_r128.accel_ok ||
        (g_r128.overlay_active && !g_r128.surface_used) ||
        g_r128.bpp != 32U ||
        !g_r128.width || !g_r128.height || !g_r128.pitch)
        return false;
    pitch_units = r128_pitch_units();
    if (!pitch_units) return false;
    offset = r128_align_up(r128_visible_end(), 32U);
    size = r128_align_up((uint32_t)g_r128.pitch * g_r128.height, 32U);
    limit = g_r128.cursor_ready ? g_r128.cursor_offset : g_r128.vram_size;
    if (offset >= limit || size > limit - offset) return false;
    if (offset_out) *offset_out = offset;
    if (size_out) *size_out = size;
    return true;
}

/* Private upload strip when VRAM cannot hold a second full-size scanout.
 * The bridge reserves it before the 3D allocator chooses its arena. */
static bool r128_staging_tile_layout(uint32_t *offset_out, uint32_t *rows_out) {
    uint32_t offset, limit, rows;
    if (!R128_VRAM_STAGING || !g_r128.active || !g_r128.accel_ok ||
        g_r128.overlay_active || (g_r128.bpp != 16U && g_r128.bpp != 32U) || !g_r128.pitch ||
        !g_r128.height || !r128_pitch_units()) return false;
    offset = r128_align_up(r128_visible_end(), 32U);
    limit = g_r128.cursor_ready ? g_r128.cursor_offset : g_r128.vram_size;
    if (offset >= limit) return false;
    rows = (limit - offset) / g_r128.pitch;
    if (rows > 128U) rows = 128U;
    if (rows > g_r128.height) rows = g_r128.height;
    if (!rows) return false;
    if (offset_out) *offset_out = offset;
    if (rows_out) *rows_out = rows;
    return true;
}

static uint32_t r128_gmc_dst_datatype(void) {
    return g_r128.bpp == 8U ? R128_GMC_DST_8BPP :
           (g_r128.bpp == 16U ? R128_GMC_DST_16BPP : R128_GMC_DST_32BPP);
}

static uint8_t r128_rgb_to_332(uint32_t rgb) {
    return (uint8_t)(((rgb >> 16) & 0xE0U) |
                     ((rgb >> 11) & 0x1CU) |
                     ((rgb >> 6) & 0x03U));
}

static uint32_t r128_native_color(uint32_t rgb) {
    if (g_r128.bpp == 8U) {
        uint32_t index = r128_rgb_to_332(rgb);
        return index | (index << 8) | (index << 16) | (index << 24);
    }
    if(g_r128.bpp==16U) {uint32_t c=bk_rgb565_encode(rgb);return c|(c<<16);}
    return rgb & 0x00FFFFFFU;
}

static bool r128_pointer_is_vram(const void *pointer) {
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t start = (uintptr_t)g_r128.fb_bar;
    uintptr_t end = start + (uintptr_t)g_r128.vram_size;
    return address >= start && address < end;
}

static void r128_copy_dwords_to_vram(volatile uint32_t *destination,
                                     const uint32_t *source,
                                     uint32_t count) {
    uint64_t started = perfmon_scope_begin();
    uint32_t bytes = count * 4U;
    uint32_t *dst = (uint32_t *)(uintptr_t)destination;
    const uint32_t *src = source;

    /* REP MOVSL produce una única ráfaga secuencial por fila. Es mucho menos
     * costoso que rasterizar cada primitiva GUI directamente sobre VRAM. */
    __asm__ volatile ("cld; rep movsl"
                      : "+D"(dst), "+S"(src), "+c"(count)
                      : : "memory");
    perfmon_scope_end(PERF_SCOPE_ATI_COPY_2D, started);
    perfmon_scope_note(PERF_SCOPE_ATI_COPY_2D, bytes, 0U, 0U);
}

static bool r128_engine_reset(void);

static uint32_t r128_read(uint32_t reg) {
    volatile uint32_t *p = (volatile uint32_t *)(g_r128.mmio + reg);
    uint32_t value = *p;
    __asm__ volatile ("" ::: "memory");
    return value;
}

static void r128_write(uint32_t reg, uint32_t value) {
    volatile uint32_t *p = (volatile uint32_t *)(g_r128.mmio + reg);
    *p = value;
    __asm__ volatile ("" ::: "memory");
}

static void r128_write8(uint32_t reg, uint8_t value) {
    volatile uint8_t *p = g_r128.mmio + reg;
    *p = value;
    __asm__ volatile ("" ::: "memory");
}

static uint32_t r128_pll_read(uint8_t index) {
    r128_write8(R128_CLOCK_CNTL_INDEX, (uint8_t)(index & 0x3FU));
    return r128_read(R128_CLOCK_CNTL_DATA);
}

static void r128_pll_write(uint8_t index, uint32_t value) {
    r128_write8(R128_CLOCK_CNTL_INDEX,
                (uint8_t)((index & 0x3FU) | R128_PLL_WR_EN));
    r128_write(R128_CLOCK_CNTL_DATA, value);
}

static uint32_t r128_align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint32_t r128_align_down(uint32_t value, uint32_t alignment) {
    return value & ~(alignment - 1U);
}

static void r128_spin_delay(uint32_t loops) {
    while (loops--) __asm__ volatile ("pause");
}

static void r128_pll_masked_write(uint8_t index, uint32_t value,
                                  uint32_t preserve_mask) {
    uint32_t current = r128_pll_read(index);
    r128_pll_write(index, (current & preserve_mask) | value);
}

static bool r128_pll_wait_update(void) {
    for (uint32_t i = 0; i < R128_TIMEOUT; i++) {
        if (!(r128_pll_read(R128_PPLL_REF_DIV) & R128_PPLL_ATOMIC_UPDATE))
            return true;
    }
    return false;
}

static bool r128_pll_commit(void) {
    if (!r128_pll_wait_update()) return false;
    r128_pll_masked_write(R128_PPLL_REF_DIV, R128_PPLL_ATOMIC_UPDATE,
                          ~R128_PPLL_ATOMIC_UPDATE);
    return r128_pll_wait_update();
}

static const r128_mode_t *r128_find_mode(uint16_t width, uint16_t height) {
    for (uint32_t i = 0; i < sizeof(g_r128_modes) / sizeof(g_r128_modes[0]); i++)
        if (g_r128_modes[i].width == width && g_r128_modes[i].height == height)
            return &g_r128_modes[i];
    return NULL;
}

static uint32_t r128_post_divisor(uint32_t bits) {
    static const uint8_t values[8] = {1, 2, 4, 8, 3, 0, 6, 12};
    return values[(bits >> 16) & 7U];
}

static uint32_t r128_post_bits(uint32_t divisor) {
    switch (divisor) {
        case 1: return 0U << 16;
        case 2: return 1U << 16;
        case 4: return 2U << 16;
        case 8: return 3U << 16;
        case 3: return 4U << 16;
        case 6: return 6U << 16;
        case 12: return 7U << 16;
        default: return 0xFFFFFFFFU;
    }
}

static bool r128_perf_poll(uint32_t reg, uint32_t free_mask, uint32_t slots,
                           uint32_t busy_mask, perf_scope_id_t scope) {
    uint64_t started = perfmon_scope_begin();
    uint32_t polls = 0U;
    /* Bound MMIO stalls in real time. Keep the graphics domain held: yielding
     * here could let another task alter an in-flight 2D command or staging tile.
     * The iteration limit also covers early boot with interrupts disabled. */
    uint32_t first_tick = pit_get_ticks();
    uint32_t deadline_ticks = pit_get_frequency_hz() / 10U;
    if (!deadline_ticks) deadline_ticks = 1U;
    bool ok = false;
    while (polls < R128_TIMEOUT) {
        uint32_t stat = r128_read(reg);
        polls++;
        if ((stat & free_mask) >= slots && !(stat & busy_mask)) {
            ok = true;
            break;
        }
        if ((polls & 4095U) == 0U) {
            if (pit_get_ticks() - first_tick >= deadline_ticks) break;
        }
        __asm__ volatile ("pause");
    }
    perfmon_scope_end(scope, started);
    perfmon_scope_note(scope, 0U, polls, !ok);
    return ok;
}

static bool r128_wait_fifo(uint32_t entries) {
    return r128_perf_poll(R128_GUI_STAT, R128_GUI_FIFOCNT_MASK, entries,
                          0U, PERF_SCOPE_ATI_FIFO_2D);
}

static bool r128_flush_pixel_cache(void) {
    uint32_t value = r128_read(R128_PC_NGUI_CTLSTAT);
    value = (value & ~R128_PC_FLUSH_ALL) | R128_PC_FLUSH_ALL;
    r128_write(R128_PC_NGUI_CTLSTAT, value);
    return r128_perf_poll(R128_PC_NGUI_CTLSTAT, 0U, 0U,
                          R128_PC_BUSY, PERF_SCOPE_ATI_CACHE_2D);
}

static bool r128_wait_idle_raw(void) {
    return r128_wait_fifo(64U) &&
           r128_perf_poll(R128_GUI_STAT, 0U, 0U, R128_GUI_ACTIVE,
                          PERF_SCOPE_ATI_IDLE_2D) && r128_flush_pixel_cache();
}

static bool r128_fail_acceleration(const char *where) {
    g_r128.stat_timeouts++;
    if (g_r128.accel_ok)
        kprintf("[ATIR128.DVR] timeout en %s; motor reiniciado y 2D desactivada\n",
                where);
    /* Abort any partially submitted operation before CPU framebuffer access. */
    (void)r128_engine_reset();
    g_r128.engine_busy = false;
    g_r128.accel_ok = false;
    r128_stats_report("timeout");
    return false;
}

static bool r128_wait_idle(void) {
    if (!g_r128.accel_ok) return false;
    if (!g_r128.engine_busy) return true;
    g_r128.stat_idle_waits++;
    if (r128_wait_idle_raw()) {
        g_r128.engine_busy = false;
        return true;
    }
    return r128_fail_acceleration("GUI");
}

static bool r128_engine_reset(void) {
    uint32_t clock_index;
    uint32_t mclk;
    uint32_t reset;

    /* A cache flush may time out when the engine was left wedged; reset anyway. */
    (void)r128_flush_pixel_cache();
    clock_index = r128_read(R128_CLOCK_CNTL_INDEX);
    mclk = r128_pll_read(R128_MCLK_CNTL);
    r128_pll_write(R128_MCLK_CNTL,
                   mclk | R128_FORCE_GCP | R128_FORCE_PIPE3D_CP);

    reset = r128_read(R128_GEN_RESET_CNTL);
    r128_write(R128_GEN_RESET_CNTL, reset | R128_SOFT_RESET_GUI);
    (void)r128_read(R128_GEN_RESET_CNTL);
    r128_write(R128_GEN_RESET_CNTL, reset & ~R128_SOFT_RESET_GUI);
    (void)r128_read(R128_GEN_RESET_CNTL);

    r128_pll_write(R128_MCLK_CNTL, mclk);
    r128_write(R128_CLOCK_CNTL_INDEX, clock_index);
    r128_write(R128_GEN_RESET_CNTL, reset);
    r128_write(R128_PM4_BUFFER_CNTL, R128_PM4_NONPM4);
    g_r128.engine_busy = false;
    return true;
}

static void r128_save_engine(void) {
    if (g_r128.engine_saved) return;
    g_r128.saved_pm4_buffer_cntl = r128_read(R128_PM4_BUFFER_CNTL);
    g_r128.saved_default_offset = r128_read(R128_DEFAULT_OFFSET);
    g_r128.saved_default_pitch = r128_read(R128_DEFAULT_PITCH);
    g_r128.saved_default_sc_bottom_right =
        r128_read(R128_DEFAULT_SC_BOTTOM_RIGHT);
    g_r128.saved_aux_sc_cntl = r128_read(R128_AUX_SC_CNTL);
    g_r128.saved_sc_top_left = r128_read(R128_SC_TOP_LEFT);
    g_r128.saved_sc_bottom_right = r128_read(R128_SC_BOTTOM_RIGHT);
    g_r128.saved_dp_gui_master_cntl = r128_read(R128_DP_GUI_MASTER_CNTL);
    g_r128.saved_dp_cntl = r128_read(R128_DP_CNTL);
    g_r128.saved_dp_datatype = r128_read(R128_DP_DATATYPE);
    g_r128.saved_dp_write_mask = r128_read(R128_DP_WRITE_MASK);
    g_r128.engine_saved = true;
}

static void r128_restore_engine(void) {
    if (!g_r128.engine_saved || !g_r128.mmio) return;
    (void)r128_wait_idle_raw();
    r128_write(R128_PM4_BUFFER_CNTL, g_r128.saved_pm4_buffer_cntl);
    r128_write(R128_DEFAULT_OFFSET, g_r128.saved_default_offset);
    r128_write(R128_DEFAULT_PITCH, g_r128.saved_default_pitch);
    r128_write(R128_DEFAULT_SC_BOTTOM_RIGHT,
               g_r128.saved_default_sc_bottom_right);
    r128_write(R128_AUX_SC_CNTL, g_r128.saved_aux_sc_cntl);
    r128_write(R128_SC_TOP_LEFT, g_r128.saved_sc_top_left);
    r128_write(R128_SC_BOTTOM_RIGHT, g_r128.saved_sc_bottom_right);
    r128_write(R128_DP_GUI_MASTER_CNTL,
               g_r128.saved_dp_gui_master_cntl);
    r128_write(R128_DP_CNTL, g_r128.saved_dp_cntl);
    r128_write(R128_DP_DATATYPE, g_r128.saved_dp_datatype);
    r128_write(R128_DP_WRITE_MASK, g_r128.saved_dp_write_mask);
    (void)r128_wait_idle_raw();
    g_r128.engine_saved = false;
}

static bool r128_program_2d_context(void) {
    const uint32_t scissor =
        R128_DEFAULT_SC_RIGHT_MAX | R128_DEFAULT_SC_BOTTOM_MAX;
    uint32_t datatype;
    uint32_t pitch_units = r128_pitch_units();

    if (!pitch_units || (g_r128.bpp != 8U && g_r128.bpp != 16U && g_r128.bpp != 32U))
        return false;

    /* A 3D client can leave PM4 in 192PIO. Stop the micro-engine without a
     * full GUI reset so the loaded CCE microcode survives the context switch. */
    r128_write(R128_PM4_MICRO_CNTL, 0U);
    r128_write(R128_PM4_BUFFER_CNTL, R128_PM4_NONPM4);
    r128_write(R128_SCALE_3D_CNTL, 0U);
    g_r128.engine_busy = false;

    if (!r128_wait_fifo(2U)) return false;
    r128_write(R128_DEFAULT_OFFSET, g_r128.fb_offset);
    r128_write(R128_DEFAULT_PITCH, pitch_units);

    if (!r128_wait_fifo(4U)) return false;
    r128_write(R128_AUX_SC_CNTL, 0U);
    r128_write(R128_DEFAULT_SC_BOTTOM_RIGHT, scissor);
    r128_write(R128_SC_TOP_LEFT, 0U);
    r128_write(R128_SC_BOTTOM_RIGHT, scissor);

    datatype = r128_gmc_dst_datatype() | R128_GMC_CLR_CMP_CNTL_DIS |
               R128_GMC_AUX_CLIP_DIS;
    if (!r128_wait_fifo(8U)) return false;
    r128_write(R128_DP_GUI_MASTER_CNTL,
               datatype | R128_GMC_BRUSH_SOLID_COLOR |
               R128_GMC_SRC_DATATYPE_COLOR);
    r128_write(R128_DP_BRUSH_FRGD_CLR, 0xFFFFFFFFU);
    r128_write(R128_DP_BRUSH_BKGD_CLR, 0x00000000U);
    r128_write(R128_DP_SRC_FRGD_CLR, 0xFFFFFFFFU);
    r128_write(R128_DP_SRC_BKGD_CLR, 0x00000000U);
    r128_write(R128_DP_WRITE_MASK, 0xFFFFFFFFU);
    r128_write(R128_CLR_CMP_CNTL, 0U);
    r128_write(R128_CLR_CMP_MASK, 0xFFFFFFFFU);
    r128_write(R128_DP_DATATYPE,
               r128_read(R128_DP_DATATYPE) & ~R128_HOST_BIG_ENDIAN_EN);
    return r128_wait_idle_raw();
}

static bool r128_engine_init(void) {
    uint32_t pitch_units = r128_pitch_units();

    if (!pitch_units || (g_r128.bpp != 8U && g_r128.bpp != 16U && g_r128.bpp != 32U)) {
        kprintf("[ATIR128:TRACE] ENGINE reject bpp=%u pitch=%u units=%u\n",
                (uint32_t)g_r128.bpp, (uint32_t)g_r128.pitch, pitch_units);
        return false;
    }

    r128_save_engine();
    r128_engine_reset();
    if (!r128_program_2d_context()) return false;
    kprintf("[ATIR128:TRACE] ENGINE OK bpp=%u pitch_bytes=%u pitch_units=%u datatype=%x\n",
            (uint32_t)g_r128.bpp, (uint32_t)g_r128.pitch,
            pitch_units, r128_gmc_dst_datatype());
    return true;
}

static bool r128_register_test(void) {
    uint32_t saved = r128_read(R128_BIOS_0_SCRATCH);
    bool ok = false;
    r128_write(R128_BIOS_0_SCRATCH, 0x55555555U);
    if (r128_read(R128_BIOS_0_SCRATCH) == 0x55555555U) {
        r128_write(R128_BIOS_0_SCRATCH, 0xAAAAAAAAU);
        ok = r128_read(R128_BIOS_0_SCRATCH) == 0xAAAAAAAAU;
    }
    r128_write(R128_BIOS_0_SCRATCH, saved);
    return ok;
}

static bool r128_find_device(void) {
    uint32_t count = pci_device_count();
    kprintf("[ATIR128:TRACE] PCI scan count=%u target=1002:4C46\n", count);
    for (uint32_t i = 0; i < count; i++) {
        const pci_device_t *dev = pci_device_at(i);
        uint32_t fb;
        uint32_t mmio;
        if (!dev || dev->vendor_id != ATI_VENDOR_ID ||
            dev->device_id != ATI_RAGE128_LF_DEVICE_ID)
            continue;
        kprintf("[ATIR128:TRACE] PCI MATCH index=%u bdf=%u:%u.%u command=%x BAR0=%x BAR1=%x BAR2=%x\n",
                i, (uint32_t)dev->bus, (uint32_t)dev->slot,
                (uint32_t)dev->function, (uint32_t)dev->command,
                dev->bars[0], dev->bars[1], dev->bars[2]);
        if (dev->bars[0] & 1U || dev->bars[2] & 1U) {
            kprintf("[ATIR128:TRACE] PCI REJECT BAR0/BAR2 marcado como I/O\n");
            continue;
        }
        fb = dev->bars[0] & 0xFFFFFFF0U;
        mmio = dev->bars[2] & 0xFFFFFFF0U;
        if (!fb || !mmio) {
            kprintf("[ATIR128:TRACE] PCI REJECT BAR vacio fb=%x mmio=%x\n",
                    fb, mmio);
            continue;
        }
        g_r128.pci = dev;
        g_r128.fb_bar = fb;
        g_r128.mmio_bar = mmio;
        g_r128.mmio = (volatile uint8_t *)(uintptr_t)mmio;
        g_r128.detected = true;
        kprintf("[ATIR128:TRACE] PCI ACCEPT fb=%x mmio=%x\n", fb, mmio);
        return true;
    }
    kprintf("[ATIR128:TRACE] PCI target no utilizable\n");
    return false;
}

static uint16_t r128_vbios_read16(const volatile uint8_t *rom,
                                    uint32_t offset) {
    return (uint16_t)rom[offset] |
           (uint16_t)((uint16_t)rom[offset + 1U] << 8);
}

static uint32_t r128_vbios_pixel_clock(void) {
    const volatile uint8_t *rom =
        (const volatile uint8_t *)(uintptr_t)R128_VBIOS_BASE;
    uint32_t header, pll, reference, divisor, feedback, post, clock;
    if (rom[0] != 0x55U || rom[1] != 0xAAU) return 0U;
    header = r128_vbios_read16(rom, 0x48U);
    if (!header || header + 0x31U >= R128_VBIOS_SCAN_SIZE) return 0U;
    pll = r128_vbios_read16(rom, header + 0x30U);
    if (!pll || pll + 0x0FU >= R128_VBIOS_SCAN_SIZE) return 0U;
    reference = (uint32_t)r128_vbios_read16(rom, pll + 0x0EU) * 10U;
    divisor = g_r128.saved_ppll_ref_div & 0x3FFU;
    feedback = g_r128.saved_ppll_active_div & R128_PPLL_FB3_DIV_MASK;
    post = r128_post_divisor(g_r128.saved_ppll_active_div);
    if (reference < 10000U || reference > 50000U ||
        !divisor || !feedback || !post) return 0U;
    clock = (reference * feedback + divisor * post / 2U) / (divisor * post);
    return clock >= 10000U && clock <= 200000U ? clock : 0U;
}

static void r128_vbios_pll_limits(void) {
    const volatile uint8_t *rom=(const volatile uint8_t *)(uintptr_t)R128_VBIOS_BASE;
    /* Conservative Rage128 limits also used by xf86-video-r128 without ROM. */
    g_r128.pll_min_khz=125000U;g_r128.pll_max_khz=250000U;
    if(rom[0]!=0x55U || rom[1]!=0xAAU)return;
    uint32_t header=r128_vbios_read16(rom,0x48U);
    if(!header || header+0x31U>=R128_VBIOS_SCAN_SIZE)return;
    uint32_t pll=r128_vbios_read16(rom,header+0x30U);
    if(!pll || pll+0x19U>=R128_VBIOS_SCAN_SIZE)return;
    uint32_t min=r128_vbios_read16(rom,pll+0x12U)|((uint32_t)r128_vbios_read16(rom,pll+0x14U)<<16);
    uint32_t max=r128_vbios_read16(rom,pll+0x16U)|((uint32_t)r128_vbios_read16(rom,pll+0x18U)<<16);
    if(min>=5000U && min<=30000U && max>min && max<=50000U) {
        g_r128.pll_min_khz=min*10U;g_r128.pll_max_khz=max*10U;
    }
}

static bool r128_detect_panel_vbios(uint16_t *width_out,
                                    uint16_t *height_out) {
    static const uint8_t signature[8] = {
        'M', '3', ' ', ' ', ' ', ' ', ' ', ' '
    };
    const volatile uint8_t *rom =
        (const volatile uint8_t *)(uintptr_t)R128_VBIOS_BASE;

    /* The VGA option ROM must start with the standard 55 AA signature. */
    if (rom[0] != 0x55U || rom[1] != 0xAAU) {
        kprintf("[ATIR128:TRACE] PANEL VBIOS sin firma 55AA en C0000\n");
        return false;
    }

    for (uint32_t i = 4U;
         i + sizeof(signature) <= R128_VBIOS_SCAN_SIZE; i++) {
        bool match = true;
        uint32_t table = 0U;
        uint32_t width;
        uint32_t height;

        for (uint32_t j = 0U; j < sizeof(signature); j++) {
            if (rom[i + j] != signature[j]) {
                match = false;
                break;
            }
        }
        if (!match) continue;

        /* M3 identifies a header, not the panel record itself. The header
         * starts two bytes earlier and contains a list of ROM offsets. */
        for (uint32_t slot = i - 2U + 20U;
             slot < i - 2U + 84U && slot + 1U < R128_VBIOS_SCAN_SIZE;
             slot += 2U) {
            table = r128_vbios_read16(rom, slot);
            if (table) break;
        }
        if (!table || table + R128_VBIOS_PANEL_Y_OFFSET + 1U >=
                      R128_VBIOS_SCAN_SIZE) continue;
        width = r128_vbios_read16(rom, table + R128_VBIOS_PANEL_X_OFFSET);
        height = r128_vbios_read16(rom, table + R128_VBIOS_PANEL_Y_OFFSET);

        /* Accept only a mode we actually know how to program and never let a
         * corrupt/shadowed ROM shrink the current firmware scanout. */
        if (width < g_r128.width || height < g_r128.height ||
            width > R128_MAX_COORD || height > R128_MAX_COORD ||
            !r128_find_mode((uint16_t)width, (uint16_t)height)) {
            kprintf("[ATIR128:TRACE] PANEL VBIOS M3 candidato invalido %ux%u @%x\n",
                    width, height, i);
            continue;
        }

        g_r128.panel_vbios_offset=table;
        if (width_out) *width_out = (uint16_t)width;
        if (height_out) *height_out = (uint16_t)height;
        kprintf("[ATIR128:TRACE] PANEL VBIOS M3 @%x -> %ux%u\n",
                i, width, height);
        return true;
    }

    kprintf("[ATIR128:TRACE] PANEL VBIOS: tabla M3 no encontrada\n");
    return false;
}

/* M3 panel mode records are indexed from panel_table+64. Decode bounded
 * records as described by xf86-video-r128 R128DoValidMode. Accept only records
 * whose scanout dimensions match our logical framebuffer; expanded records
 * for another geometry need a different scaler/CRTC integration. */
static bool r128_panel_mode(uint16_t width,uint16_t height,r128_mode_t *out) {
    const volatile uint8_t *rom=(const volatile uint8_t *)(uintptr_t)R128_VBIOS_BASE;
    uint32_t table=g_r128.panel_vbios_offset;
    if(!table || !out || rom[0]!=0x55U || rom[1]!=0xAAU)return false;
    for(uint32_t slot=0;slot<64U;slot++) {
        uint32_t at=table+64U+slot*2U;
        if(at+1U>=R128_VBIOS_SCAN_SIZE)break;
        uint32_t entry=r128_vbios_read16(rom,at);
        if(!entry)break;
        if(entry+8U>=R128_VBIOS_SCAN_SIZE)continue;
        if(r128_vbios_read16(rom,entry)!=width || r128_vbios_read16(rom,entry+2U)!=height)continue;
        uint32_t timing=r128_vbios_read16(rom,entry+5U);
        if(!timing)timing=entry+9U;
        if(timing+20U>=R128_VBIOS_SCAN_SIZE)continue;
        const r128_mode_t *fallback=r128_find_mode(width,height);
        if(!fallback)return false;
        r128_mode_t mode=*fallback;
        mode.pixel_khz=(uint32_t)r128_vbios_read16(rom,timing)*10U;
        mode.h_total=((r128_vbios_read16(rom,timing+8U)&511U)+1U)*8U;
        mode.width=((r128_vbios_read16(rom,timing+10U)&511U)+1U)*8U;
        mode.h_sync_start=((r128_vbios_read16(rom,timing+12U)&511U)+1U)*8U;
        /* BIOS horizontal widths are character-clock units, like CRTC. */
        mode.h_sync_end=mode.h_sync_start+(rom[timing+14U]&31U)*8U;
        mode.v_total=(r128_vbios_read16(rom,timing+15U)&2047U)+1U;
        mode.height=(r128_vbios_read16(rom,timing+17U)&2047U)+1U;
        uint32_t vsync=r128_vbios_read16(rom,timing+19U);
        mode.v_sync_start=(vsync&2047U)+1U;
        mode.v_sync_end=mode.v_sync_start+((vsync>>11)&31U);
        uint32_t pixels=(uint32_t)mode.h_total*mode.v_total;
        if(mode.width!=width || mode.height!=height || mode.pixel_khz<10000U ||
           mode.pixel_khz>200000U || mode.h_total<=width || mode.v_total<=height ||
           mode.h_sync_start<width || mode.h_sync_end<=mode.h_sync_start ||
           mode.h_sync_end>mode.h_total || mode.v_sync_start<height ||
           mode.v_sync_end<=mode.v_sync_start || mode.v_sync_end>mode.v_total ||
           mode.pixel_khz*1000U<pixels*40U || mode.pixel_khz*1000U>pixels*100U)continue;
        *out=mode;return true;
    }
    return false;
}

static bool r128_detect_panel_native(uint16_t *width_out,
                                      uint16_t *height_out) {
    uint32_t hreg = r128_read(R128_FP_HORZ_STRETCH);
    uint32_t vreg = r128_read(R128_FP_VERT_STRETCH);
    uint32_t lvds = r128_read(R128_LVDS_GEN_CNTL);
    uint32_t ext = r128_read(R128_CRTC_EXT_CNTL);
    uint32_t width;
    uint32_t height;

    /* Rage128 stores panel width as (pixels / 8) - 1 and panel height as
     * lines - 1. Only trust these fields while the flat-panel/LVDS path is
     * actually enabled; otherwise zeroed CRT registers decode as 8x1. */
    if (!(lvds & (R128_LVDS_ON | R128_LVDS_EN | R128_LVDS_DIGON)) &&
        !(ext & (R128_FP_OUT_EN | R128_FP_ACTIVE)))
        return false;

    width = ((((hreg & R128_HORZ_PANEL_SIZE) >>
               R128_HORZ_PANEL_SHIFT) + 1U) * 8U);
    height = (((vreg & R128_VERT_PANEL_SIZE) >>
               R128_VERT_PANEL_SHIFT) + 1U);

    /* Reject obviously stale/nonsensical register contents and never claim
     * that the physical panel is smaller than the firmware scanout. */
    if (width < 320U || height < 200U ||
        width > R128_MAX_COORD || height > R128_MAX_COORD ||
        width < g_r128.width || height < g_r128.height ||
        !r128_find_mode((uint16_t)width, (uint16_t)height))
        return false;

    if (width_out) *width_out = (uint16_t)width;
    if (height_out) *height_out = (uint16_t)height;
    kprintf("[ATIR128:TRACE] PANEL regs h=%x v=%x lvds=%x ext=%x -> %ux%u\n",
            hreg, vreg, lvds, ext, width, height);
    return true;
}

static bool r128_program_panel_scaler(const r128_mode_t *mode) {
    uint32_t h;
    uint32_t v;
    uint32_t hratio;
    uint32_t vratio;

    if (!g_r128.panel_native_valid || !mode ||
        !g_r128.native_width || !g_r128.native_height)
        return true;
    if (mode->width > g_r128.native_width ||
        mode->height > g_r128.native_height)
        return false;

    hratio = ((uint32_t)mode->width * R128_HORZ_STRETCH_RATIO_MAX +
              g_r128.native_width / 2U) / g_r128.native_width;
    vratio = ((uint32_t)mode->height * R128_VERT_STRETCH_RATIO_MAX +
              g_r128.native_height / 2U) / g_r128.native_height;
    if (!hratio || hratio > R128_HORZ_STRETCH_RATIO_MAX ||
        !vratio || vratio > R128_VERT_STRETCH_RATIO_MAX)
        return false;

    h = g_r128.saved_fp_horz_stretch;
    h &= ~(R128_HORZ_STRETCH_RATIO_MASK |
           R128_HORZ_STRETCH_BLEND | R128_HORZ_STRETCH_ENABLE |
           R128_HORZ_AUTO_RATIO_FIX_EN | R128_HORZ_PANEL_SIZE);
    h |= ((g_r128.native_width / 8U - 1U) << R128_HORZ_PANEL_SHIFT) &
         R128_HORZ_PANEL_SIZE;
    h |= hratio & R128_HORZ_STRETCH_RATIO_MASK;
    if (mode->width != g_r128.native_width)
        h |= R128_HORZ_STRETCH_BLEND | R128_HORZ_STRETCH_ENABLE;

    v = g_r128.saved_fp_vert_stretch;
    v &= ~((R128_VERT_STRETCH_RATIO_MASK << R128_VERT_STRETCH_RATIO_SHIFT) |
           R128_VERT_STRETCH_ENABLE | R128_VERT_STRETCH_BLEND |
           R128_VERT_AUTO_RATIO_EN | R128_VERT_PANEL_SIZE);
    v |= ((g_r128.native_height - 1U) << R128_VERT_PANEL_SHIFT) &
         R128_VERT_PANEL_SIZE;
    v |= (vratio & R128_VERT_STRETCH_RATIO_MASK) <<
         R128_VERT_STRETCH_RATIO_SHIFT;
    if (mode->height != g_r128.native_height)
        v |= R128_VERT_STRETCH_ENABLE | R128_VERT_STRETCH_BLEND;

    r128_write(R128_FP_HORZ_STRETCH, h);
    r128_write(R128_FP_VERT_STRETCH, v);

    /* Keep the FP shadow timing coherent with CRTC1. This mirrors the
     * long-standing Rage128 RMX path and avoids a stale 800x600 shadow when
     * the internal 1024x768 panel is selected. */
    r128_write(R128_FP_CRTC_H_TOTAL_DISP, r128_read(R128_CRTC_H_TOTAL_DISP));
    r128_write(R128_FP_CRTC_V_TOTAL_DISP, r128_read(R128_CRTC_V_TOTAL_DISP));
    r128_write(R128_FP_H_SYNC_STRT_WID, r128_read(R128_CRTC_H_SYNC_STRT_WID));
    r128_write(R128_FP_V_SYNC_STRT_WID, r128_read(R128_CRTC_V_SYNC_STRT_WID));
    /* All supported modes are progressive and driven by CRTC1. Firmware
     * half-clock/shadow selections belong to its boot mode, not the new one.
     * Preserve panel power/backlight bits while selecting coherent timings. */
    r128_write(R128_FP_GEN_CNTL,
        (g_r128.saved_fp_gen_cntl & ~(R128_FP_SEL_CRTC2 |
         R128_FP_USE_SHADOW_VEND | R128_FP_USE_SHADOW_ROWCUR |
         R128_FP_HORZ_DIV2_EN | R128_FP_HOR_CRT_DIV2_DIS |
         R128_FP_CRT_SYNC_SEL | R128_FP_USE_SHADOW_EN)) |
         R128_FP_DONT_SHADOW_VPAR | R128_FP_DONT_SHADOW_HEND);
    r128_write(R128_LVDS_GEN_CNTL,
               g_r128.saved_lvds_gen_cntl & ~R128_LVDS_SEL_CRTC2);
    return true;
}

static void r128_save_display(void) {
    const r128_mode_t *boot_mode;
    if (g_r128.display_saved) return;
    g_r128.saved_crtc_gen_cntl = r128_read(R128_CRTC_GEN_CNTL);
    g_r128.saved_crtc_ext_cntl = r128_read(R128_CRTC_EXT_CNTL);
    g_r128.saved_crtc_h_total_disp = r128_read(R128_CRTC_H_TOTAL_DISP);
    g_r128.saved_crtc_h_sync_strt_wid = r128_read(R128_CRTC_H_SYNC_STRT_WID);
    g_r128.saved_crtc_v_total_disp = r128_read(R128_CRTC_V_TOTAL_DISP);
    g_r128.saved_crtc_v_sync_strt_wid = r128_read(R128_CRTC_V_SYNC_STRT_WID);
    g_r128.saved_crtc_offset = r128_read(R128_CRTC_OFFSET);
    g_r128.saved_crtc_offset_cntl = r128_read(R128_CRTC_OFFSET_CNTL);
    g_r128.saved_crtc_pitch = r128_read(R128_CRTC_PITCH);
    g_r128.saved_fp_crtc_h_total_disp = r128_read(R128_FP_CRTC_H_TOTAL_DISP);
    g_r128.saved_fp_gen_cntl = r128_read(R128_FP_GEN_CNTL);
    g_r128.saved_fp_crtc_v_total_disp = r128_read(R128_FP_CRTC_V_TOTAL_DISP);
    g_r128.saved_fp_h_sync_strt_wid = r128_read(R128_FP_H_SYNC_STRT_WID);
    g_r128.saved_fp_v_sync_strt_wid = r128_read(R128_FP_V_SYNC_STRT_WID);
    g_r128.saved_fp_horz_stretch = r128_read(R128_FP_HORZ_STRETCH);
    g_r128.saved_fp_vert_stretch = r128_read(R128_FP_VERT_STRETCH);
    g_r128.saved_lvds_gen_cntl = r128_read(R128_LVDS_GEN_CNTL);
    g_r128.saved_dda_config = r128_read(R128_DDA_CONFIG);
    g_r128.saved_dda_on_off = r128_read(R128_DDA_ON_OFF);
    g_r128.saved_clock_cntl_index = r128_read(R128_CLOCK_CNTL_INDEX);
    g_r128.saved_ppll_active_div = r128_pll_read(4U +
        ((g_r128.saved_clock_cntl_index >> 8) & 3U));
    g_r128.saved_ppll_cntl = r128_pll_read(R128_PPLL_CNTL);
    g_r128.saved_ppll_ref_div = r128_pll_read(R128_PPLL_REF_DIV);
    g_r128.saved_ppll_div_3 = r128_pll_read(R128_PPLL_DIV_3);
    g_r128.saved_vclk_ecp_cntl = r128_pll_read(R128_VCLK_ECP_CNTL);
    g_r128.saved_htotal_cntl = r128_pll_read(R128_HTOTAL_CNTL);
    boot_mode = r128_find_mode(g_r128.width, g_r128.height);
    g_r128.current_pixel_khz = boot_mode ? boot_mode->pixel_khz : 0U;
    /* A scaled LCD boot mode may retain the panel's PLL clock. Derive the
     * saved clock from its actual dividers instead of assuming VGA timings. */
    r128_vbios_pll_limits();
    uint32_t firmware_clock = r128_vbios_pixel_clock();
    if (firmware_clock) g_r128.current_pixel_khz = firmware_clock;
    g_r128.native_pixel_khz = g_r128.current_pixel_khz;
    g_r128.native_width = g_r128.width;
    g_r128.native_height = g_r128.height;
    g_r128.panel_native_vbios = false;
    g_r128.panel_native_valid = r128_detect_panel_vbios(
        &g_r128.native_width, &g_r128.native_height);
    g_r128.panel_native_vbios = g_r128.panel_native_valid;
    if (!g_r128.panel_native_valid) {
        g_r128.panel_native_valid = r128_detect_panel_native(
            &g_r128.native_width, &g_r128.native_height);
    }
    g_r128.direct_modeset_ok = boot_mode != NULL &&
        (g_r128.bpp == 8U || g_r128.bpp == 16U || g_r128.bpp == 32U) &&
        (g_r128.saved_ppll_active_div & R128_PPLL_FB3_DIV_MASK) != 0U &&
        r128_post_divisor(g_r128.saved_ppll_active_div) != 0U &&
        (g_r128.saved_dda_config & R128_DDA_XFER_MASK) != 0U &&
        (g_r128.saved_dda_on_off & 0xFFFFU) != 0U;
    g_r128.display_saved = true;
    kprintf("[ATIR128:TRACE] DISPLAY saved crtc_gen=%x crtc_ext=%x pitch_reg=%u ppll_div3=%x dda=%x/%x native=%ux%u direct_modeset=%u\n",
            g_r128.saved_crtc_gen_cntl, g_r128.saved_crtc_ext_cntl,
            g_r128.saved_crtc_pitch, g_r128.saved_ppll_div_3,
            g_r128.saved_dda_config, g_r128.saved_dda_on_off,
            (uint32_t)g_r128.native_width, (uint32_t)g_r128.native_height,
            g_r128.direct_modeset_ok ? 1U : 0U);
}

static bool r128_program_pll(uint32_t pixel_khz) {
    uint32_t old_feedback = g_r128.saved_ppll_active_div & R128_PPLL_FB3_DIV_MASK;
    uint32_t old_post = r128_post_divisor(g_r128.saved_ppll_active_div);
    uint32_t target_post = old_post;
    uint32_t target_feedback;
    uint32_t post_bits;
    uint64_t native_vco;
    uint64_t best_error = ~(uint64_t)0;
    static const uint8_t post_candidates[] = {1, 2, 3, 4, 6, 8, 12};
    uint32_t vclk;
    uint32_t ppll;
    uint32_t clock_index;

    if (!g_r128.native_pixel_khz || !old_feedback || !old_post) return false;
    native_vco = (uint64_t)g_r128.native_pixel_khz * old_post;
    for (uint32_t i = 0; i < sizeof(post_candidates); i++) {
        uint64_t candidate_vco = (uint64_t)pixel_khz * post_candidates[i];
        uint32_t min=g_r128.pll_min_khz?g_r128.pll_min_khz:125000U;
        uint32_t max=g_r128.pll_max_khz?g_r128.pll_max_khz:250000U;
        if(candidate_vco<min || candidate_vco>max)continue;
        uint64_t error = candidate_vco > native_vco ?
                         candidate_vco - native_vco : native_vco - candidate_vco;
        if (error < best_error) { best_error = error; target_post = post_candidates[i]; }
    }
    if(best_error==~(uint64_t)0)return false;
    target_feedback = (uint32_t)(((uint64_t)old_feedback * pixel_khz * target_post +
                                 ((uint64_t)g_r128.native_pixel_khz * old_post) / 2U) /
                                ((uint64_t)g_r128.native_pixel_khz * old_post));
    if (!target_feedback || target_feedback > R128_PPLL_FB3_DIV_MASK) return false;
    post_bits = r128_post_bits(target_post);
    if (post_bits == 0xFFFFFFFFU) return false;

    vclk = r128_pll_read(R128_VCLK_ECP_CNTL);
    r128_pll_write(R128_VCLK_ECP_CNTL,
                   (vclk & ~R128_VCLK_SRC_SEL_MASK) | R128_VCLK_SRC_SEL_CPUCLK);
    ppll = r128_pll_read(R128_PPLL_CNTL);
    r128_pll_write(R128_PPLL_CNTL, ppll | R128_PPLL_RESET |
                   R128_PPLL_ATOMIC_UPDATE_EN | R128_PPLL_VGA_ATOMIC_UPDATE_EN);

    clock_index = r128_read(R128_CLOCK_CNTL_INDEX);
    r128_write(R128_CLOCK_CNTL_INDEX, clock_index | R128_PLL_DIV_SEL);
    r128_pll_masked_write(R128_PPLL_DIV_3, target_feedback,
                          ~R128_PPLL_FB3_DIV_MASK);
    r128_pll_masked_write(R128_PPLL_DIV_3, post_bits,
                          ~R128_PPLL_POST3_DIV_MASK);
    if (!r128_pll_commit()) return false;
    r128_pll_write(R128_HTOTAL_CNTL, 0U);
    r128_pll_write(R128_PPLL_CNTL, ppll & ~(R128_PPLL_RESET |
                   R128_PPLL_ATOMIC_UPDATE_EN | R128_PPLL_VGA_ATOMIC_UPDATE_EN));
    r128_spin_delay(500000U);
    r128_pll_write(R128_VCLK_ECP_CNTL,
                   (vclk & ~R128_VCLK_SRC_SEL_MASK) | R128_VCLK_SRC_SEL_PPLLCLK);
    /* DIV3 contains the new clock. Restoring the firmware divider selector
     * here could instead select an untouched DIV0/1/2 and corrupt scanout. */
    r128_write(R128_CLOCK_CNTL_INDEX, clock_index | R128_PLL_DIV_SEL);
    return true;
}

/* Recalcular DDA a partir del modo que programo la BIOS. Mientras la
 * profundidad no cambie (8->8 o 32->32), la latencia de memoria se conserva:
 * se mantiene precision y
 * loop-latency, y se escala el tiempo por transferencia y los umbrales con la
 * relacion entre el pixel clock nativo y el solicitado. */
static bool r128_program_dda(uint32_t pixel_khz) {
    uint32_t cfg = g_r128.saved_dda_config;
    uint32_t onoff = g_r128.saved_dda_on_off;
    uint32_t xfer = cfg & R128_DDA_XFER_MASK;
    uint32_t precision = (cfg & R128_DDA_PRECISION_MASK) >> 16;
    uint32_t loop = (cfg & R128_DDA_LOOP_MASK) >> 20;
    uint32_t old_on = onoff >> 16;
    uint32_t old_off = onoff & 0xFFFFU;
    uint32_t shift;
    uint32_t old_xfer_integer;
    uint32_t latency;
    uint32_t new_xfer;
    uint32_t new_xfer_integer;
    uint32_t new_on;
    uint32_t new_off;
    uint32_t new_cfg;
    uint32_t new_onoff;

    if (!pixel_khz || !g_r128.native_pixel_khz ||
        (g_r128.bpp != 8U && g_r128.bpp != 16U && g_r128.bpp != 32U) ||
        !xfer || !old_on || !old_off || precision > 11U)
        return false;
    shift = 11U - precision;
    old_xfer_integer = (xfer + (shift ? (1U << (shift - 1U)) : 0U)) >> shift;
    if (!old_xfer_integer || (old_on >> shift) < old_xfer_integer)
        return false;
    latency = (old_on >> shift) - old_xfer_integer;

    uint32_t saved_bytes=g_r128.firmware_info_valid ?
        (g_r128.firmware_info.bpp+7U)/8U :
        (((g_r128.saved_crtc_gen_cntl>>8)&7U)==2U?1U:
         (((g_r128.saved_crtc_gen_cntl>>8)&7U)==4U?2U:4U));
    uint64_t numerator=(uint64_t)g_r128.native_pixel_khz*saved_bytes;
    uint64_t denominator=(uint64_t)pixel_khz*r128_bytes_per_pixel();
    new_xfer_integer=(uint32_t)(((uint64_t)old_xfer_integer*numerator+denominator/2U)/denominator);
    if(!new_xfer_integer)return false;
    uint32_t new_precision=1U;
    for(uint32_t value=new_xfer_integer;value;value>>=1)new_precision++;
    if(new_precision>11U)return false;
    uint32_t new_shift=11U-new_precision;
    uint64_t divisor=denominator<<shift;
    new_xfer=(uint32_t)((((uint64_t)xfer*numerator<<new_shift)+divisor/2U)/divisor);
    new_off=(uint32_t)((((uint64_t)old_off*numerator<<new_shift)+divisor/2U)/divisor);
    if (!new_xfer || new_xfer > R128_DDA_XFER_MASK || !new_off ||
        new_off > 0xFFFFU)return false;
    new_on = (latency + new_xfer_integer) << new_shift;
    if (!new_on || new_on > 0xFFFFU || new_on + loop >= new_off)return false;
    new_cfg = (cfg & ~(R128_DDA_XFER_MASK|R128_DDA_PRECISION_MASK)) |
              new_xfer | (new_precision<<16);
    new_onoff = (new_on << 16) | new_off;
    r128_write(R128_DDA_CONFIG, new_cfg);
    r128_write(R128_DDA_ON_OFF, new_onoff);
    if (r128_read(R128_DDA_CONFIG) != new_cfg ||
        r128_read(R128_DDA_ON_OFF) != new_onoff)
        return false;
    kprintf("[ATIR128:TRACE] DDA pixel=%u config=%x onoff=%x\n",
            pixel_khz, new_cfg, new_onoff);
    return true;
}

static bool r128_write_mode_registers(const r128_mode_t *mode, uint16_t pitch) {
    uint32_t hsync_width;
    uint32_t hsync_start;
    uint32_t vsync_width;
    uint32_t gen;
    uint32_t bytes_per_pixel = r128_bytes_per_pixel();
    uint32_t pitch_pixels;
    uint32_t pitch_units;
    uint32_t pixel_width = r128_gmc_dst_datatype();
    if (!mode || !bytes_per_pixel ||
        pitch < mode->width * bytes_per_pixel || (pitch & 31U)) return false;
    pitch_pixels = pitch / bytes_per_pixel;
    if ((pitch_pixels & 7U) != 0U) return false;
    pitch_units = pitch_pixels >> 3;

    hsync_width = (mode->h_sync_end - mode->h_sync_start) / 8U;
    if (!hsync_width) hsync_width = 1U;
    if (hsync_width > 0x3FU) hsync_width = 0x3FU;
    hsync_start = mode->h_sync_start - 8U +
                  (g_r128.bpp == 16U ? 9U :
                   (g_r128.bpp == 8U && g_r128.panel_native_valid ? 17U : 5U));
    vsync_width = mode->v_sync_end - mode->v_sync_start;
    if (!vsync_width) vsync_width = 1U;
    if (vsync_width > 0x1FU) vsync_width = 0x1FU;

    gen = (g_r128.saved_crtc_gen_cntl & ~(R128_CRTC_PIX_WIDTH_MASK |
           R128_CRTC_DBL_SCAN_EN | R128_CRTC_INTERLACE_EN |
           R128_CRTC_CSYNC_EN)) |
          R128_CRTC_EXT_DISP_EN | R128_CRTC_EN | pixel_width;
    r128_write(R128_CRTC_GEN_CNTL, gen);
    r128_write(R128_CRTC_H_TOTAL_DISP,
               (((mode->h_total / 8U) - 1U) & 0xFFFFU) |
               ((((mode->width / 8U) - 1U) & 0xFFFFU) << 16));
    r128_write(R128_CRTC_H_SYNC_STRT_WID,
               (hsync_start & 0x0FFFU) | (hsync_width << 16) |
               (mode->neg_hsync ? R128_CRTC_H_SYNC_POL : 0U));
    r128_write(R128_CRTC_V_TOTAL_DISP,
               ((mode->v_total - 1U) & 0xFFFFU) |
               (((mode->height - 1U) & 0xFFFFU) << 16));
    r128_write(R128_CRTC_V_SYNC_STRT_WID,
               ((mode->v_sync_start - 1U) & 0x0FFFU) |
               (vsync_width << 16) |
               (mode->neg_vsync ? R128_CRTC_V_SYNC_POL : 0U));
    r128_write(R128_CRTC_OFFSET, g_r128.fb_offset);
    r128_write(R128_CRTC_OFFSET_CNTL, 0U);
    /* CRTC_PITCH se expresa en grupos de ocho pixeles, no en bytes. En 32
     * bpp coincide con pitch/32; en 8 bpp debe ser pitch/8. */
    r128_write(R128_CRTC_PITCH, pitch_units);
    return r128_read(R128_CRTC_PITCH) == pitch_units;
}

static bool r128_restore_display(gfx_info_t *info) {
    if (!g_r128.display_saved) return false;
    r128_write(R128_CRTC_EXT_CNTL, g_r128.saved_crtc_ext_cntl |
               R128_CRTC_HSYNC_DIS | R128_CRTC_VSYNC_DIS | R128_CRTC_DISPLAY_DIS);
    r128_pll_write(R128_VCLK_ECP_CNTL,
                   (g_r128.saved_vclk_ecp_cntl & ~R128_VCLK_SRC_SEL_MASK) |
                   R128_VCLK_SRC_SEL_CPUCLK);
    r128_pll_write(R128_PPLL_CNTL, g_r128.saved_ppll_cntl | R128_PPLL_RESET);
    r128_pll_write(R128_PPLL_REF_DIV, g_r128.saved_ppll_ref_div);
    r128_pll_write(R128_PPLL_DIV_3, g_r128.saved_ppll_div_3);
    (void)r128_pll_commit();
    r128_pll_write(R128_HTOTAL_CNTL, g_r128.saved_htotal_cntl);
    r128_pll_write(R128_PPLL_CNTL, g_r128.saved_ppll_cntl);
    r128_spin_delay(500000U);
    r128_pll_write(R128_VCLK_ECP_CNTL, g_r128.saved_vclk_ecp_cntl);
    r128_write(R128_DDA_CONFIG, g_r128.saved_dda_config);
    r128_write(R128_DDA_ON_OFF, g_r128.saved_dda_on_off);
    r128_write(R128_CRTC_H_TOTAL_DISP, g_r128.saved_crtc_h_total_disp);
    r128_write(R128_CRTC_H_SYNC_STRT_WID, g_r128.saved_crtc_h_sync_strt_wid);
    r128_write(R128_CRTC_V_TOTAL_DISP, g_r128.saved_crtc_v_total_disp);
    r128_write(R128_CRTC_V_SYNC_STRT_WID, g_r128.saved_crtc_v_sync_strt_wid);
    r128_write(R128_CRTC_OFFSET, g_r128.saved_crtc_offset);
    r128_write(R128_CRTC_OFFSET_CNTL, g_r128.saved_crtc_offset_cntl);
    r128_write(R128_CRTC_PITCH, g_r128.saved_crtc_pitch);
    r128_write(R128_FP_CRTC_H_TOTAL_DISP, g_r128.saved_fp_crtc_h_total_disp);
    r128_write(R128_FP_CRTC_V_TOTAL_DISP, g_r128.saved_fp_crtc_v_total_disp);
    r128_write(R128_FP_H_SYNC_STRT_WID, g_r128.saved_fp_h_sync_strt_wid);
    r128_write(R128_FP_V_SYNC_STRT_WID, g_r128.saved_fp_v_sync_strt_wid);
    r128_write(R128_FP_HORZ_STRETCH, g_r128.saved_fp_horz_stretch);
    r128_write(R128_FP_VERT_STRETCH, g_r128.saved_fp_vert_stretch);
    r128_write(R128_FP_GEN_CNTL, g_r128.saved_fp_gen_cntl);
    r128_write(R128_LVDS_GEN_CNTL, g_r128.saved_lvds_gen_cntl);
    r128_write(R128_CRTC_GEN_CNTL, g_r128.saved_crtc_gen_cntl);
    r128_write(R128_CRTC_EXT_CNTL, g_r128.saved_crtc_ext_cntl);
    r128_write(R128_CLOCK_CNTL_INDEX, g_r128.saved_clock_cntl_index);
    if (g_r128.firmware_info_valid) {
        if (info) {
            *info = g_r128.firmware_info;
            info->mode = GFX_MODE_ATI_RAGE128;
        }
        g_r128.fb_offset = g_r128.firmware_info.framebuffer - g_r128.fb_bar;
        g_r128.width = g_r128.firmware_info.width;
        g_r128.height = g_r128.firmware_info.height;
        g_r128.pitch = g_r128.firmware_info.pitch;
        g_r128.bpp = g_r128.firmware_info.bpp;
        g_r128.pitch_offset = (r128_pitch_units() << 21) |
                              (g_r128.fb_offset >> 5);
    } else {
        g_r128.width = g_r128.native_width;
        g_r128.height = g_r128.native_height;
        g_r128.bpp = 32U;
        g_r128.pitch = (uint16_t)(g_r128.saved_crtc_pitch * 32U);
        g_r128.pitch_offset = (r128_pitch_units() << 21) |
                              (g_r128.fb_offset >> 5);
    }
    g_r128.current_pixel_khz = g_r128.native_pixel_khz;
    return true;
}

static bool r128_validate_mode(const gfx_info_t *info) {
    uint64_t visible_end;
    uint64_t vram_end;
    uint32_t bytes_per_pixel;
    uint32_t pixels_per_line;
    uint32_t pitch_units;

    if (!info || info->mode != GFX_MODE_VESA_LFB ||
        (info->bpp != 8U && info->bpp != 16U && info->bpp != 32U) ||
        !info->framebuffer || !info->width || !info->height ||
        info->width > R128_MAX_COORD || info->height > R128_MAX_COORD)
        return false;

    bytes_per_pixel = (info->bpp + 7U) / 8U;
    if ((uint32_t)info->pitch < (uint32_t)info->width * bytes_per_pixel ||
        ((uint32_t)info->pitch % bytes_per_pixel) != 0U)
        return false;
    pixels_per_line = (uint32_t)info->pitch / bytes_per_pixel;
    if (pixels_per_line < info->width || (pixels_per_line & 7U) != 0U)
        return false;
    pitch_units = pixels_per_line >> 3;
    if (!pitch_units || pitch_units > 0x3FFU) return false;

    g_r128.vram_size = r128_read(R128_CONFIG_MEMSIZE) & 0x03FFFFFFU;
    if (!g_r128.vram_size) return false;
    if (info->framebuffer < g_r128.fb_bar) return false;
    g_r128.fb_offset = info->framebuffer - g_r128.fb_bar;
    if (g_r128.fb_offset & 31U) return false;

    visible_end = (uint64_t)g_r128.fb_offset +
                  (uint64_t)info->pitch * info->height;
    vram_end = (uint64_t)g_r128.vram_size;
    if (visible_end > vram_end) return false;

    g_r128.width = info->width;
    g_r128.height = info->height;
    g_r128.pitch = info->pitch;
    g_r128.bpp = info->bpp;
    g_r128.pitch_offset = (pitch_units << 21) |
                          (g_r128.fb_offset >> 5);
    g_r128.cursor_offset =
        r128_align_down(g_r128.vram_size - R128_CURSOR_BYTES, 16U);
    g_r128.cursor_ready = g_r128.cursor_offset >= visible_end;
    /* Color key and overlay programming are kept disabled in indexed mode. */
    g_r128.overlay_ready = info->bpp == 32U && g_r128.cursor_ready &&
        g_r128.cursor_offset > visible_end + R128_OVERLAY_MIN_FREE;
    return true;
}


static bool r128_prepare_firmware_bootstrap(const gfx_info_t *info) {
    uint64_t visible_end;

    if (!info || info->mode != GFX_MODE_VESA_LFB ||
        !info->framebuffer || !info->width || !info->height ||
        !info->pitch || !info->bpp ||
        info->width > R128_MAX_COORD || info->height > R128_MAX_COORD)
        return false;

    g_r128.vram_size = r128_read(R128_CONFIG_MEMSIZE) & 0x03FFFFFFU;
    if (!g_r128.vram_size || info->framebuffer < g_r128.fb_bar) return false;

    g_r128.fb_offset = info->framebuffer - g_r128.fb_bar;
    if (g_r128.fb_offset & 31U) return false;

    visible_end = (uint64_t)g_r128.fb_offset +
                  (uint64_t)info->pitch * info->height;
    if (visible_end > (uint64_t)g_r128.vram_size) return false;

    g_r128.firmware_info = *info;
    g_r128.firmware_info_valid = true;
    g_r128.width = info->width;
    g_r128.height = info->height;
    g_r128.pitch = info->pitch;
    g_r128.bpp = info->bpp;
    return true;
}





static bool r128_clip_rect(const gfx_info_t *info, int *x, int *y,
                           int *w, int *h) {
    if (!info || !x || !y || !w || !h || *w <= 0 || *h <= 0) return false;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > info->width) *w = info->width - *x;
    if (*y + *h > info->height) *h = info->height - *y;
    return *w > 0 && *h > 0;
}

static bool r128_clip_blit(const gfx_info_t *info, int *src_x, int *src_y,
                           int *dst_x, int *dst_y, int *w, int *h) {
    if (!info || !src_x || !src_y || !dst_x || !dst_y || !w || !h ||
        *w <= 0 || *h <= 0) return false;
    if (*src_x < 0) { int d = -*src_x; *src_x = 0; *dst_x += d; *w -= d; }
    if (*src_y < 0) { int d = -*src_y; *src_y = 0; *dst_y += d; *h -= d; }
    if (*dst_x < 0) { int d = -*dst_x; *dst_x = 0; *src_x += d; *w -= d; }
    if (*dst_y < 0) { int d = -*dst_y; *dst_y = 0; *src_y += d; *h -= d; }
    if (*src_x + *w > info->width) *w = info->width - *src_x;
    if (*dst_x + *w > info->width) *w = info->width - *dst_x;
    if (*src_y + *h > info->height) *h = info->height - *src_y;
    if (*dst_y + *h > info->height) *h = info->height - *dst_y;
    return *w > 0 && *h > 0;
}

static uint32_t r128_hw_rop(gfx_rop_t rop) {
    switch (rop) {
        case GFX_ROP_XOR: return R128_ROP3_SRCINVERT;
        case GFX_ROP_AND: return R128_ROP3_SRCAND;
        case GFX_ROP_OR: return R128_ROP3_SRCPAINT;
        case GFX_ROP_INVERT: return R128_ROP3_DSTINVERT;
        case GFX_ROP_COPY:
        default: return R128_ROP3_SRCCOPY;
    }
}

static bool r128_activate(gfx_info_t *info, uint16_t preferred_width,
                          uint16_t preferred_height) {
    kprintf("[ATIR128:TRACE] ACTIVATE begin detected=%u pci=%x mmio=%x preferred=%ux%u\n",
            g_r128.detected ? 1U : 0U, (uint32_t)(uintptr_t)g_r128.pci,
            (uint32_t)(uintptr_t)g_r128.mmio, (uint32_t)preferred_width,
            (uint32_t)preferred_height);
    if (!g_r128.detected || !g_r128.pci || !g_r128.mmio) {
        kprintf("[ATIR128:TRACE] ACTIVATE FAIL estado de deteccion incompleto\n");
        return false;
    }
    if (!pci_enable_command(g_r128.pci, PCI_COMMAND_MEMORY)) {
        kprintf("[ATIR128:TRACE] ACTIVATE FAIL no se pudo habilitar PCI MEMORY\n");
        return false;
    }
    kprintf("[ATIR128:TRACE] PCI MEMORY habilitado\n");
    if (!r128_register_test()) {
        kprintf("[ATIR128.DVR] BAR2 no responde como MMIO Rage 128\n");
        return false;
    }
    if (!vesa_init_from_bootinfo(info)) {
        kprintf("[ATIR128.DVR] no hay modo VESA LFB activo\n");
        return false;
    }
    kprintf("[ATIR128:TRACE] VESA bootstrap mode=%ux%ux%u pitch=%u fb=%x BAR0=%x delta=%x\n",
            (uint32_t)info->width, (uint32_t)info->height,
            (uint32_t)info->bpp, (uint32_t)info->pitch, info->framebuffer,
            g_r128.fb_bar, info->framebuffer - g_r128.fb_bar);
    if (!r128_prepare_firmware_bootstrap(info)) {
        kprintf("[ATIR128.DVR] bootstrap VESA fuera de la VRAM Rage 128\n");
        return false;
    }

    r128_save_display();
    g_r128.saved_ov0_scale_cntl = r128_read(R128_OV0_SCALE_CNTL);
    g_r128.saved_ov0_key_cntl = r128_read(R128_OV0_KEY_CNTL);
    g_r128.saved_ov0_graphics_key_clr = r128_read(R128_OV0_GRAPHICS_KEY_CLR);
    g_r128.saved_ov0_graphics_key_msk = r128_read(R128_OV0_GRAPHICS_KEY_MSK);
    g_r128.saved_ov0_filter_cntl = r128_read(R128_OV0_FILTER_CNTL);
    g_r128.saved_ov0_colour_cntl = r128_read(R128_OV0_COLOUR_CNTL);
    r128_write(R128_OV0_SCALE_CNTL, 0U);

    /*
     * Do not reinterpret a firmware 8bpp scanout as 32bpp. On Mobility M3 the
     * panel/DDA state is depth-dependent; changing only CRTC_PIX_WIDTH and
     * pitch causes four repeated columns and cyan separators. Keep the exact
     * BIOS mode and accelerate it using Rage 128's native indexed datatype.
     */
    if (!r128_validate_mode(info)) {
        kprintf("[ATIR128.DVR] bootstrap %ubpp invalido para Rage 128\n",
                (uint32_t)info->bpp);
        return false;
    }
    if (info->bpp == 8U) {
        g_r128.overlay_ready = false;
        kprintf("[ATIR128:TRACE] BOOT8 SAFE conserva profundidad/LVDS y habilita modos 8->8; palette RGB332; pitch_units=%u\n",
                r128_pitch_units());
    } else {
        kprintf("[ATIR128:TRACE] BOOT32 firmware nativo aceptado sin conversion\n");
    }

    r128_stats_reset();
    g_r128.engine_busy = false;
    g_r128.surface_used = false;
    g_r128.accel_ok = true;
    if (!r128_engine_init()) {
        g_r128.accel_ok = false;
        r128_restore_engine();
        /* Keep the valid firmware framebuffer active even without 2D. */
        kprintf("[ATIR128.DVR] motor 2D no respondio; continuando con framebuffer CPU\n");
    }
    g_r128.active = true;
    /* The scanout was bootstrapped by VESA, but ownership now belongs to the
     * Rage 128 backend.  Keeping GFX_MODE_VESA_LFB here made user-space report
     * VESA.DVR even though this driver was active. */
    info->mode = GFX_MODE_ATI_RAGE128;
    kprintf("[ATIR128:TRACE] MODE OWNER ati-rage128 (%ux%ux%u)\n",
            (uint32_t)info->width, (uint32_t)info->height,
            (uint32_t)info->bpp);
    kprintf("[ATIR128.DVR] Rage Mobility M3 1002:4C46, %ux%ux%u pitch=%u VRAM=%u KiB\n",
            (uint32_t)info->width, (uint32_t)info->height,
            (uint32_t)info->bpp, (uint32_t)info->pitch,
            g_r128.vram_size / 1024U);
    kprintf("[ATIR128.DVR] 2D=%s cursor=%s overlay=%s modeset=%s\n",
            g_r128.accel_ok ? "si" : "CPU fallback",
            g_r128.cursor_ready ? "si" : "no",
            g_r128.overlay_ready ? "YUY2/UYVY" : "no",
            g_r128.direct_modeset_ok ? "estandar a profundidad nativa" : "firmware protegido");
    kprintf("[ATIR128.DVR] compositor=RAM cacheada; present 32bpp=upload dirty + BitBlt VRAM->VRAM; Fill/BitBlt=%s\n",
            g_r128.accel_ok ? "GPU" : "inactivos");
    kprintf("[ATIR128.DVR] silicon panel fix: boot=%ux%ux%u panel=%ux%u fuente=%s\n",
            (uint32_t)info->width, (uint32_t)info->height,
            (uint32_t)info->bpp, (uint32_t)g_r128.native_width,
            (uint32_t)g_r128.native_height,
            g_r128.panel_native_vbios ? "VBIOS M3" :
            (g_r128.panel_native_valid ? "FP/LVDS regs" : "modo firmware"));
    kprintf("[ATIR128.DVR] silicon perf: cursor HW ATI permitido; present staging asincrono\n");
    return true;
}

static uint32_t r128_capabilities(void) {
    uint32_t caps;
    uint32_t surface_offset;
    uint32_t surface_size;
    if (!g_r128.active) return 0U;
    caps = GFX_CAP_DIRTY_RECTS;
    if (g_r128.bpp == 8U || g_r128.bpp == 16U || g_r128.bpp == 32U)
        caps |= GFX_CAP_PRESENT_BUFFER;
    if (g_r128.accel_ok) {
        caps |= GFX_CAP_BITBLT | GFX_CAP_FILL | GFX_CAP_FILL_HW |
                GFX_CAP_BITBLT_COPY_HW | GFX_CAP_BITBLT_ROP_HW;
        if (g_r128.surface_used) {
            caps |= GFX_CAP_PRESENT_VRAM_BLIT | GFX_CAP_OFFSCREEN_VRAM |
                    GFX_CAP_SURFACE_BLIT_HW;
        } else if (r128_surface_layout(&surface_offset, &surface_size)) {
            (void)surface_offset;
            (void)surface_size;
            caps |= GFX_CAP_PRESENT_VRAM_BLIT | GFX_CAP_OFFSCREEN_VRAM |
                    GFX_CAP_SURFACE_BLIT_HW;
        }
    }
    if (g_r128.accel_ok && r128_staging_tile_layout(NULL, NULL))
        caps |= GFX_CAP_PRESENT_VRAM_BLIT;
    /* Incluso con staging, los píxeles nacen en RAM y se suben por CPU. No
     * anunciar un mapa CPU de VRAM: en Mobility M3 sin write-combining eso
     * convierte todo el compositor en escrituras MMIO lentas. */
    caps |= GFX_CAP_PRESENT_CPU_COPY;
    if (g_r128.cursor_ready) caps |= GFX_CAP_HW_CURSOR;
    if (g_r128.overlay_ready && g_r128.bpp == 32U)
        caps |= GFX_CAP_VIDEO_OVERLAY;
    return caps;
}

static void r128_overlay_restore(void) {
    if (!g_r128.mmio) return;
    r128_write(R128_OV0_SCALE_CNTL, 0U);
    r128_write(R128_OV0_KEY_CNTL, g_r128.saved_ov0_key_cntl);
    r128_write(R128_OV0_GRAPHICS_KEY_CLR, g_r128.saved_ov0_graphics_key_clr);
    r128_write(R128_OV0_GRAPHICS_KEY_MSK, g_r128.saved_ov0_graphics_key_msk);
    r128_write(R128_OV0_FILTER_CNTL, g_r128.saved_ov0_filter_cntl);
    r128_write(R128_OV0_COLOUR_CNTL, g_r128.saved_ov0_colour_cntl);
    r128_write(R128_OV0_SCALE_CNTL, g_r128.saved_ov0_scale_cntl);
    g_r128.overlay_active = false;
}

static void r128_disable(void) {
    if (g_r128.active) r128_stats_report("disable");
    if (g_r128.mmio) {
        r128_write(R128_CRTC_GEN_CNTL,
                   r128_read(R128_CRTC_GEN_CNTL) & ~R128_CRTC_CUR_EN);
        r128_overlay_restore();
    }
    if (g_r128.active && g_r128.display_saved) (void)r128_restore_display(NULL);
    if (g_r128.active || g_r128.engine_saved) r128_restore_engine();
    g_r128.active = false;
    g_r128.accel_ok = false;
    g_r128.engine_busy = false;
    g_r128.surface_used = false;
    g_r128.cursor_defined = false;
    g_r128.cursor_visible = false;
}

static bool r128_mode_allowed(const r128_mode_t *mode) {
    uint64_t bytes;
    if (!mode || !g_r128.direct_modeset_ok) return false;
    if (mode->width > g_r128.native_width || mode->height > g_r128.native_height)
        return false;
    bytes = (uint64_t)r128_align_up((uint32_t)mode->width *
            r128_bytes_per_pixel(), 32U) * mode->height;
    return (uint64_t)g_r128.fb_offset + bytes <= g_r128.cursor_offset;
}

static bool r128_list_modes(gfx_display_mode_t *modes, uint32_t max_modes,
                            uint32_t *count) {
    uint32_t written = 0U;
    if (!count || !g_r128.active) return false;

    uint8_t previous_bpp=g_r128.bpp;
    static const uint8_t depths[]={8U,16U,32U};
    for(uint32_t d=0;d<sizeof(depths);d++) {
        /* Switching to indexed color also needs a palette setup, so expose it
         * only when the firmware booted indexed. True-color 16/32 can switch. */
        if(depths[d]==8U && previous_bpp!=8U)continue;
        g_r128.bpp=depths[d];
        for (uint32_t i=0;i<sizeof(g_r128_modes)/sizeof(g_r128_modes[0]);i++) {
            if(!r128_mode_allowed(&g_r128_modes[i]))continue;
            if(modes && written<max_modes) {
                modes[written].width=g_r128_modes[i].width;
                modes[written].height=g_r128_modes[i].height;
                modes[written].bpp=depths[d];
            }
            written++;
        }
    }
    g_r128.bpp=previous_bpp;
    if (!written) {
        if (modes && max_modes) {
            modes[0].width = g_r128.width;
            modes[0].height = g_r128.height;
            modes[0].bpp = g_r128.bpp;
        }
        written = 1U;
    }
    *count = written > max_modes && modes ? max_modes : written;
    return true;
}

/* Roll back to the last working mode, rather than always to boot geometry.
 * The generic graphics core retains its previous info after a failed modeset. */
static void r128_mode_checkpoint(r128_state_t *state,const gfx_info_t *info) {
    *state=g_r128;
    state->firmware_info=*info;state->firmware_info_valid=true;
    state->saved_crtc_gen_cntl=r128_read(R128_CRTC_GEN_CNTL);
    state->saved_crtc_ext_cntl=r128_read(R128_CRTC_EXT_CNTL);
    state->saved_crtc_h_total_disp=r128_read(R128_CRTC_H_TOTAL_DISP);
    state->saved_crtc_h_sync_strt_wid=r128_read(R128_CRTC_H_SYNC_STRT_WID);
    state->saved_crtc_v_total_disp=r128_read(R128_CRTC_V_TOTAL_DISP);
    state->saved_crtc_v_sync_strt_wid=r128_read(R128_CRTC_V_SYNC_STRT_WID);
    state->saved_crtc_offset=r128_read(R128_CRTC_OFFSET);
    state->saved_crtc_offset_cntl=r128_read(R128_CRTC_OFFSET_CNTL);
    state->saved_crtc_pitch=r128_read(R128_CRTC_PITCH);
    state->saved_fp_crtc_h_total_disp=r128_read(R128_FP_CRTC_H_TOTAL_DISP);
    state->saved_fp_crtc_v_total_disp=r128_read(R128_FP_CRTC_V_TOTAL_DISP);
    state->saved_fp_h_sync_strt_wid=r128_read(R128_FP_H_SYNC_STRT_WID);
    state->saved_fp_v_sync_strt_wid=r128_read(R128_FP_V_SYNC_STRT_WID);
    state->saved_fp_horz_stretch=r128_read(R128_FP_HORZ_STRETCH);
    state->saved_fp_vert_stretch=r128_read(R128_FP_VERT_STRETCH);
    state->saved_fp_gen_cntl=r128_read(R128_FP_GEN_CNTL);
    state->saved_lvds_gen_cntl=r128_read(R128_LVDS_GEN_CNTL);
    state->saved_dda_config=r128_read(R128_DDA_CONFIG);
    state->saved_dda_on_off=r128_read(R128_DDA_ON_OFF);
    state->saved_clock_cntl_index=r128_read(R128_CLOCK_CNTL_INDEX);
    state->saved_ppll_cntl=r128_pll_read(R128_PPLL_CNTL);
    state->saved_ppll_ref_div=r128_pll_read(R128_PPLL_REF_DIV);
    state->saved_ppll_div_3=r128_pll_read(R128_PPLL_DIV_3);
    state->saved_vclk_ecp_cntl=r128_pll_read(R128_VCLK_ECP_CNTL);
    state->saved_htotal_cntl=r128_pll_read(R128_HTOTAL_CNTL);
    state->native_pixel_khz=g_r128.current_pixel_khz;
}

static bool r128_set_mode(gfx_info_t *info, uint16_t width, uint16_t height,
                          uint8_t bpp) {
    const r128_mode_t *mode;
    uint16_t pitch;
    uint32_t old_ext;
    if (!g_r128.active || !info) return false;

    if(bpp!=8U && bpp!=16U && bpp!=32U)return false;
    if(bpp==8U && g_r128.bpp!=8U)return false;
    if(width==g_r128.width && height==g_r128.height && bpp==g_r128.bpp)return true;
    mode=r128_find_mode(width,height);
    r128_mode_t panel_mode;
    if(r128_panel_mode(width,height,&panel_mode)) {
        mode=&panel_mode;
        kprintf("[ATIR128.DVR] LCD timings BIOS %ux%u clock=%u\n",(uint32_t)width,(uint32_t)height,mode->pixel_khz);
    }
    uint8_t old_bpp=g_r128.bpp;
    g_r128.bpp=bpp;
    bool allowed=r128_mode_allowed(mode);
    pitch=(uint16_t)r128_align_up((uint32_t)width*r128_bytes_per_pixel(),32U);
    g_r128.bpp=old_bpp;
    if(!allowed)return false;
    (void)r128_wait_idle();
    r128_state_t previous=g_r128,checkpoint;
    uint32_t previous_overlay=r128_read(R128_OV0_SCALE_CNTL);
    r128_mode_checkpoint(&checkpoint,info);
    r128_write(R128_CRTC_GEN_CNTL,
               r128_read(R128_CRTC_GEN_CNTL) & ~R128_CRTC_CUR_EN);
    r128_write(R128_OV0_SCALE_CNTL, 0U);
    g_r128.overlay_active = false;
    old_ext = r128_read(R128_CRTC_EXT_CNTL);
    r128_write(R128_CRTC_EXT_CNTL, old_ext | R128_CRTC_HSYNC_DIS |
               R128_CRTC_VSYNC_DIS | R128_CRTC_DISPLAY_DIS);

    g_r128.bpp=bpp;
    if (!r128_program_pll(mode->pixel_khz) ||
        !r128_program_dda(mode->pixel_khz) ||
        !r128_write_mode_registers(mode, pitch) ||
        !r128_program_panel_scaler(mode)) {
        kprintf("[ATIR128.DVR] modeset %ux%u fallo; restaurando modo anterior\n",
                (uint32_t)width, (uint32_t)height);
        goto rollback;
    }
    r128_write(R128_CRTC_EXT_CNTL, old_ext & ~(R128_CRTC_HSYNC_DIS |
               R128_CRTC_VSYNC_DIS | R128_CRTC_DISPLAY_DIS));

    info->mode = GFX_MODE_ATI_RAGE128;
    info->framebuffer = g_r128.fb_bar + g_r128.fb_offset;
    info->width = width;
    info->height = height;
    info->pitch = pitch;
    info->bpp = bpp;
    g_r128.surface_used = false;
    g_r128.width = width;
    g_r128.height = height;
    g_r128.pitch = pitch;
    g_r128.bpp = bpp;
    g_r128.pitch_offset = (r128_pitch_units() << 21) |
                          (g_r128.fb_offset >> 5);
    g_r128.current_pixel_khz = mode->pixel_khz;
    g_r128.engine_busy = false;
    g_r128.accel_ok = true;
    if (!r128_engine_init()) {
        g_r128.accel_ok = false;
        goto rollback;
    }
    return true;
rollback:
    g_r128=checkpoint;
    (void)r128_restore_display(info);
    g_r128=previous;
    g_r128.engine_busy=false;
    if(!r128_engine_init())g_r128.accel_ok=false;
    r128_write(R128_OV0_SCALE_CNTL,previous_overlay);
    return false;
}

static bool r128_emit_bitblt(const gfx_info_t *info,
                              uint32_t source_pitch_offset,
                              int src_x, int src_y, int dst_x, int dst_y,
                              int w, int h, gfx_rop_t rop,
                              bool same_storage) {
    uint32_t master;
    uint32_t direction = 0U;
    bool reverse_x = false;
    bool reverse_y = false;

    if (!g_r128.active || !g_r128.accel_ok ||
        !r128_clip_blit(info, &src_x, &src_y, &dst_x, &dst_y, &w, &h))
        return false;

    if (same_storage) {
        reverse_y = dst_y > src_y && dst_y < src_y + h;
        reverse_x = dst_x > src_x && dst_x < src_x + w;
    }
    if (!reverse_x) direction |= R128_DST_X_LEFT_TO_RIGHT;
    if (!reverse_y) direction |= R128_DST_Y_TOP_TO_BOTTOM;
    if (reverse_x) { src_x += w - 1; dst_x += w - 1; }
    if (reverse_y) { src_y += h - 1; dst_y += h - 1; }

    master = R128_GMC_SRC_PITCH_OFFSET_CNTL |
             R128_GMC_DST_PITCH_OFFSET_CNTL |
             R128_GMC_BRUSH_NONE |
             r128_gmc_dst_datatype() |
             R128_GMC_SRC_DATATYPE_COLOR |
             r128_hw_rop(rop) |
             R128_DP_SRC_SOURCE_MEMORY |
             R128_GMC_CLR_CMP_CNTL_DIS |
             R128_GMC_AUX_CLIP_DIS;

    /* Se escriben doce registros antes de lanzar el rectángulo. */
    if (!r128_wait_fifo(12U))
        return r128_fail_acceleration("FIFO BitBlt");
    r128_write(R128_DP_GUI_MASTER_CNTL, master);
    r128_write(R128_DP_BRUSH_FRGD_CLR, 0xFFFFFFFFU);
    r128_write(R128_DP_BRUSH_BKGD_CLR, 0U);
    r128_write(R128_DP_SRC_FRGD_CLR, 0xFFFFFFFFU);
    r128_write(R128_DP_SRC_BKGD_CLR, 0U);
    r128_write(R128_DP_WRITE_MASK, 0xFFFFFFFFU);
    r128_write(R128_DP_CNTL, direction);
    r128_write(R128_DST_PITCH_OFFSET, g_r128.pitch_offset);
    r128_write(R128_SRC_PITCH_OFFSET, source_pitch_offset);
    r128_write(R128_SRC_Y_X, ((uint32_t)src_y << 16) | (uint32_t)src_x);
    r128_write(R128_DST_Y_X, ((uint32_t)dst_y << 16) | (uint32_t)dst_x);
    r128_write(R128_DST_HEIGHT_WIDTH,
               ((uint32_t)h << 16) | (uint32_t)w);
    g_r128.engine_busy = true;
    g_r128.stat_hw_blit++;
    return true;
}

static bool r128_prepare_staging_surface(void) {
    uint32_t offset;
    uint32_t size;

    if (g_r128.surface_used)
        return g_r128.surface_width == g_r128.width &&
               g_r128.surface_height == g_r128.height &&
               g_r128.surface_pitch_offset != 0U;
    if (!r128_surface_layout(&offset, &size)) return false;

    g_r128.surface_used = true;
    g_r128.surface_width = g_r128.width;
    g_r128.surface_height = g_r128.height;
    g_r128.surface_offset = offset;
    g_r128.surface_size = size;
    g_r128.surface_pitch_offset = (r128_pitch_units() << 21) |
                                  (offset >> 5);
    kprintf("[ATIR128.DVR] staging VRAM offset=%x size=%u KB; "
            "compositor permanece en RAM\n",
            offset, size / 1024U);
    return true;
}

static bool r128_present_tiles(const gfx_info_t *info, const uint32_t *pixels,
                               uint32_t source_pitch, const gfx_rect_t *rects,
                               uint32_t rect_count) {
    uint32_t offset, rows;
    uint32_t pitch = g_r128.pitch / sizeof(uint32_t);
    uint32_t pitch_offset;
    volatile uint32_t *staging;
    if (!r128_staging_tile_layout(&offset, &rows)) return false;
    pitch_offset = (r128_pitch_units() << 21) | (offset >> 5);
    staging = (volatile uint32_t *)(uintptr_t)(g_r128.fb_bar + offset);
    for (uint32_t i = 0; i < rect_count; i++) {
        int x = rects[i].x, y = rects[i].y, w = rects[i].w, h = rects[i].h;
        if (!r128_clip_rect(info, &x, &y, &w, &h)) continue;
        for (uint32_t done = 0; done < (uint32_t)h; done += rows) {
            uint32_t height = (uint32_t)h - done;
            if (height > rows) height = rows;
            /* The GPU must finish reading this strip before it is reused. */
            if (g_r128.engine_busy && !r128_wait_idle()) return false;
            for (uint32_t row = 0; row < height; row++) {
                const uint32_t *src=pixels+((uint32_t)y+done+row)*source_pitch+(uint32_t)x;
                if(g_r128.bpp==16U)
                    bk_rgb565_upload((volatile uint16_t *)(uintptr_t)
                        (g_r128.fb_bar+offset+row*g_r128.pitch)+(uint32_t)x,src,(uint32_t)w);
                else r128_copy_dwords_to_vram(staging+row*pitch+(uint32_t)x,src,(uint32_t)w);
            }
            __asm__ volatile ("" ::: "memory");
            g_r128.stat_present_cpu_bytes += (uint32_t)w * height * r128_bytes_per_pixel();
            if (!r128_emit_bitblt(info, pitch_offset, x, 0, x,
                                  y + (int)done, w, (int)height,
                                  GFX_ROP_COPY, false)) return false;
        }
    }
    g_r128.stat_present_vram_calls++;
    g_r128.stat_present_staging_calls++;
    return true;
}

static bool r128_present_buffer(const gfx_info_t *info,
                                const uint32_t *pixels,
                                uint32_t source_pitch,
                                const gfx_rect_t *rects,
                                uint32_t rect_count,
                                uint32_t *fence_out) {
    bool result = false;
    bool source_is_vram;
    bool source_is_staging;

    if (fence_out) *fence_out = 0U;
    if (!g_r128.active || !info || !pixels || !rects || !rect_count ||
        source_pitch < info->width ||
        (info->bpp != 8U && info->bpp != 16U && info->bpp != 32U))
        return false;
    uint64_t started = perfmon_scope_begin();
    if (!r128_engine_acquire_2d()) {
        perfmon_scope_end(PERF_SCOPE_ATI_PRESENT_2D, started);
        return false;
    }

    g_r128.stat_present_calls++;
    g_r128.stat_present_rects += rect_count;
    source_is_vram = r128_pointer_is_vram(pixels);
    source_is_staging = g_r128.surface_used && info->bpp == 32U &&
        source_pitch == (uint32_t)g_r128.pitch / sizeof(uint32_t) &&
        (uintptr_t)pixels ==
            (uintptr_t)(g_r128.fb_bar + g_r128.surface_offset);

    /* Mantener compatibilidad con una superficie VRAM ya existente, pero no
     * volver a elegirla para la GUI. Esta ruta nunca lee VRAM mediante CPU. */
    if (g_r128.accel_ok && source_is_staging) {
        bool submitted = true;
        for (uint32_t i = 0; i < rect_count; i++) {
            int x = rects[i].x, y = rects[i].y;
            int w = rects[i].w, h = rects[i].h;
            if (!r128_clip_rect(info, &x, &y, &w, &h)) continue;
            if (!r128_emit_bitblt(info, g_r128.surface_pitch_offset,
                                  x, y, x, y, w, h,
                                  GFX_ROP_COPY, false)) {
                submitted = false;
                break;
            }
        }
        if (submitted && r128_wait_idle()) {
            g_r128.stat_present_vram_calls++;
            result = true;
            goto out;
        }
    }

    /* Ruta normal 32 bpp: compositor en RAM cacheada. Subir dirty rects de
     * forma secuencial a una superficie staging y emitir todos los BitBlt bajo
     * una sola espera. Esto evita millones de escrituras pequeñas a VRAM. */
    if (!source_is_vram && info->bpp == 32U && g_r128.accel_ok &&
        r128_prepare_staging_surface()) {
        bool uploaded = true;
        bool submitted = true;
        uint32_t uploaded_bytes = 0U;
        uint32_t staging_pitch = (uint32_t)g_r128.pitch / sizeof(uint32_t);
        volatile uint32_t *staging = (volatile uint32_t *)(uintptr_t)
            (g_r128.fb_bar + g_r128.surface_offset);

        /* No sobrescribir staging mientras el motor aún lo lee. */
        if (g_r128.engine_busy && !r128_wait_idle()) uploaded = false;
        if (uploaded) {
            for (uint32_t i = 0; i < rect_count; i++) {
                int x = rects[i].x, y = rects[i].y;
                int w = rects[i].w, h = rects[i].h;
                if (!r128_clip_rect(info, &x, &y, &w, &h)) continue;
                for (int row = 0; row < h; row++) {
                    volatile uint32_t *dst = staging +
                        (uint32_t)(y + row) * staging_pitch + (uint32_t)x;
                    const uint32_t *src = pixels +
                        (uint32_t)(y + row) * source_pitch + (uint32_t)x;
                    r128_copy_dwords_to_vram(dst, src, (uint32_t)w);
                    uploaded_bytes += (uint32_t)w * sizeof(uint32_t);
                }
            }
            __asm__ volatile ("" ::: "memory");
            g_r128.stat_present_cpu_bytes += uploaded_bytes;
        }

        if (uploaded) {
            for (uint32_t i = 0; i < rect_count; i++) {
                int x = rects[i].x, y = rects[i].y;
                int w = rects[i].w, h = rects[i].h;
                if (!r128_clip_rect(info, &x, &y, &w, &h)) continue;
                if (!r128_emit_bitblt(info, g_r128.surface_pitch_offset,
                                      x, y, x, y, w, h,
                                      GFX_ROP_COPY, false)) {
                    submitted = false;
                    break;
                }
            }
            if (submitted) {
                /* Do not serialize CPU and 2D engine at the end of every GUI
                 * frame. r128_emit_bitblt() marks engine_busy; the next staging
                 * upload, gfx_flush(), fence, modeset or CPU fallback performs
                 * the required barrier before touching shared VRAM again. */
                g_r128.stat_present_vram_calls++;
                g_r128.stat_present_staging_calls++;
                result = true;
                goto out;
            }
        }
    }

    if (!source_is_vram && (info->bpp == 16U || info->bpp == 32U) && g_r128.accel_ok &&
        !g_r128.surface_used &&
        r128_present_tiles(info, pixels, source_pitch, rects, rect_count)) {
        result = true;
        goto out;
    }

    /* Nunca hacer fallback CPU desde un puntero VRAM: leer VRAM y volver a
     * escribirla es la peor ruta posible. Los consumidores normales usan RAM. */
    if (source_is_vram) goto out;

    /* Fallback seguro RAM -> scanout. Un timeout invalida la aceleración, pero
     * el compositor permanece en RAM y puede continuar sin reconfigurarse. */
    if (g_r128.engine_busy && g_r128.accel_ok && !r128_wait_idle()) {
        /* r128_wait_idle ya reseteó el motor y habilitó el fallback CPU. */
    }

    for (uint32_t i = 0; i < rect_count; i++) {
        int x = rects[i].x;
        int y = rects[i].y;
        int w = rects[i].w;
        int h = rects[i].h;
        if (!r128_clip_rect(info, &x, &y, &w, &h)) continue;

        if (info->bpp == 32U && x == 0 && w == info->width &&
            source_pitch == info->width &&
            info->pitch == (uint16_t)(info->width * sizeof(uint32_t))) {
            const uint32_t *src = pixels + (uint32_t)y * source_pitch;
            volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)
                (info->framebuffer + (uint32_t)y * info->pitch);
            uint32_t count = (uint32_t)w * (uint32_t)h;
            r128_copy_dwords_to_vram(dst, src, count);
            g_r128.stat_present_cpu_bytes += count * sizeof(uint32_t);
            continue;
        }

        for (int row = 0; row < h; row++) {
            const uint32_t *src = pixels +
                (uint32_t)(y + row) * source_pitch + (uint32_t)x;
            if (info->bpp == 16U) {
                volatile uint16_t *dst=(volatile uint16_t *)(uintptr_t)
                    (info->framebuffer+(uint32_t)(y+row)*info->pitch)+x;
                bk_rgb565_upload(dst,src,(uint32_t)w);
                g_r128.stat_present_cpu_bytes+=(uint32_t)w*2U;
            } else if (info->bpp == 8U) {
                volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)
                    (info->framebuffer + (uint32_t)(y + row) * info->pitch) + x;
                int col = 0;
                while (col < w && ((uintptr_t)&dst[col] & 3U)) {
                    dst[col] = r128_rgb_to_332(src[col]);
                    col++;
                }
                for (; col + 4 <= w; col += 4) {
                    uint32_t packed = (uint32_t)r128_rgb_to_332(src[col]) |
                        ((uint32_t)r128_rgb_to_332(src[col + 1]) << 8) |
                        ((uint32_t)r128_rgb_to_332(src[col + 2]) << 16) |
                        ((uint32_t)r128_rgb_to_332(src[col + 3]) << 24);
                    *(volatile uint32_t *)(uintptr_t)&dst[col] = packed;
                }
                for (; col < w; col++)
                    dst[col] = r128_rgb_to_332(src[col]);
                g_r128.stat_present_cpu_bytes += (uint32_t)w;
            } else {
                volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)
                    (info->framebuffer + (uint32_t)(y + row) * info->pitch) + x;
                r128_copy_dwords_to_vram(dst, src, (uint32_t)w);
                g_r128.stat_present_cpu_bytes +=
                    (uint32_t)w * (uint32_t)sizeof(uint32_t);
            }
        }
    }
    __asm__ volatile ("" ::: "memory");
    result = true;
out:
    r128_engine_release_2d();
    perfmon_scope_end(PERF_SCOPE_ATI_PRESENT_2D, started);
    perfmon_scope_note(PERF_SCOPE_ATI_PRESENT_2D, rect_count, 0U, 0U);
    return result;
}

static bool r128_update_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    return g_r128.active;
}

static bool r128_flush(uint32_t *fence_out) {
    bool ok;
    if (fence_out) *fence_out = 0U;
    if (!g_r128.active || !r128_engine_acquire_2d()) return false;
    ok = !g_r128.accel_ok || r128_wait_idle();
    r128_engine_release_2d();
    return ok;
}

static bool r128_wait_fence(uint32_t fence) {
    bool ok;
    (void)fence;
    if (!g_r128.active || !r128_engine_acquire_2d()) return false;
    ok = !g_r128.accel_ok || r128_wait_idle();
    r128_engine_release_2d();
    return ok;
}

static bool r128_fill_rect(const gfx_info_t *info, int x, int y,
                           int w, int h, uint32_t rgb,
                           uint32_t *fence_out) {
    uint32_t master;
    bool ok = false;
    if (fence_out) *fence_out = 0U;
    if (!g_r128.active || !info ||
        !r128_clip_rect(info, &x, &y, &w, &h) ||
        !r128_engine_acquire_2d())
        return false;
    if (!g_r128.accel_ok) goto out;

    master = R128_GMC_DST_PITCH_OFFSET_CNTL |
             R128_GMC_BRUSH_SOLID_COLOR |
             r128_gmc_dst_datatype() |
             R128_GMC_SRC_DATATYPE_COLOR |
             R128_ROP3_PATCOPY |
             R128_GMC_CLR_CMP_CNTL_DIS |
             R128_GMC_AUX_CLIP_DIS;

    if (!r128_wait_fifo(8U)) {
        (void)r128_fail_acceleration("FIFO fill");
        goto out;
    }
    r128_write(R128_DP_GUI_MASTER_CNTL, master);
    r128_write(R128_DP_BRUSH_FRGD_CLR, r128_native_color(rgb));
    r128_write(R128_DP_BRUSH_BKGD_CLR, 0U);
    r128_write(R128_DP_WRITE_MASK, 0xFFFFFFFFU);
    r128_write(R128_DP_CNTL,
               R128_DST_X_LEFT_TO_RIGHT | R128_DST_Y_TOP_TO_BOTTOM);
    r128_write(R128_DST_PITCH_OFFSET, g_r128.pitch_offset);
    r128_write(R128_DST_Y_X, ((uint32_t)y << 16) | (uint32_t)x);
    r128_write(R128_DST_WIDTH_HEIGHT,
               ((uint32_t)w << 16) | (uint32_t)h);
    g_r128.engine_busy = true;
    g_r128.stat_hw_fill++;
    /* El FIFO conserva el orden. La barrera real ocurre en gfx_flush(), antes
     * de acceso CPU, al presentar o al cambiar 2D/3D. */
    ok = true;
out:
    r128_engine_release_2d();
    return ok;
}

static bool r128_bitblt(const gfx_info_t *info,
                        int src_x, int src_y, int dst_x, int dst_y,
                        int w, int h, gfx_rop_t rop,
                        uint32_t *fence_out) {
    bool ok;
    if (fence_out) *fence_out = 0U;
    if (!r128_engine_acquire_2d()) return false;
    ok = r128_emit_bitblt(info, g_r128.pitch_offset,
                          src_x, src_y, dst_x, dst_y,
                          w, h, rop, true);
    /* No vaciar el motor por rectángulo: gfx_flush() y el coordinador son las
     * barreras de CPU/contexto. */
    r128_engine_release_2d();
    return ok;
}

static bool r128_surface_create(uint16_t width, uint16_t height,
                                gfx_surface_handle_t *handle_out) {
    if (!handle_out || width != g_r128.width || height != g_r128.height ||
        g_r128.bpp != 32U || !g_r128.accel_ok)
        return false;
    if (g_r128.overlay_active || !r128_prepare_staging_surface())
        return false;
    if (g_r128.surface_width != width || g_r128.surface_height != height)
        return false;
    *handle_out = g_r128.surface_offset - g_r128.fb_offset;
    return true;
}

static bool r128_surface_destroy(gfx_surface_handle_t handle) {
    bool ok = false;
    if (!g_r128.surface_used ||
        handle != g_r128.surface_offset - g_r128.fb_offset ||
        !r128_engine_acquire_2d())
        return false;
    if (g_r128.engine_busy && !r128_wait_idle()) goto out;
    g_r128.surface_used = false;
    g_r128.surface_width = 0U;
    g_r128.surface_height = 0U;
    g_r128.surface_offset = 0U;
    g_r128.surface_size = 0U;
    g_r128.surface_pitch_offset = 0U;
    ok = true;
out:
    r128_engine_release_2d();
    return ok;
}

static bool r128_surface_upload(gfx_surface_handle_t handle,
                                const uint32_t *pixels,
                                uint32_t source_pitch,
                                const gfx_rect_t *rect) {
    int x;
    int y;
    int w;
    int h;
    uint32_t *base;
    bool ok = false;
    if (!g_r128.surface_used || !pixels || !rect ||
        handle != g_r128.surface_offset - g_r128.fb_offset ||
        source_pitch < g_r128.surface_width)
        return false;
    x = rect->x; y = rect->y; w = rect->w; h = rect->h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_r128.surface_width) w = g_r128.surface_width - x;
    if (y + h > g_r128.surface_height) h = g_r128.surface_height - y;
    if (w <= 0 || h <= 0 || !r128_engine_acquire_2d()) return false;
    if (g_r128.engine_busy && !r128_wait_idle()) goto out;
    base = (uint32_t *)(uintptr_t)(g_r128.fb_bar + g_r128.surface_offset);
    for (int row = 0; row < h; row++) {
        uint32_t *dst = base + (uint32_t)(y + row) *
                        ((uint32_t)g_r128.pitch / sizeof(uint32_t)) + x;
        const uint32_t *src = pixels + (uint32_t)(y + row) * source_pitch + x;
        kmemcpy(dst, src, (size_t)w * sizeof(uint32_t));
    }
    ok = true;
out:
    r128_engine_release_2d();
    return ok;
}

static bool r128_surface_blit(const gfx_info_t *info,
                              gfx_surface_handle_t handle,
                              int src_x, int src_y, int dst_x, int dst_y,
                              int w, int h, uint32_t *fence_out) {
    bool ok;
    if (fence_out) *fence_out = 0U;
    if (!g_r128.surface_used ||
        handle != g_r128.surface_offset - g_r128.fb_offset ||
        !r128_engine_acquire_2d())
        return false;
    ok = r128_emit_bitblt(info, g_r128.surface_pitch_offset,
                          src_x, src_y, dst_x, dst_y,
                          w, h, GFX_ROP_COPY, false);
    if (ok) ok = r128_wait_idle();
    r128_engine_release_2d();
    return ok;
}

static bool r128_bridge_enter_2d(void) {
    if (!g_r128.active || !g_r128.mmio) return false;
    r128_write(R128_PM4_MICRO_CNTL, 0U);
    r128_write(R128_PM4_BUFFER_CNTL, R128_PM4_NONPM4);
    g_r128.engine_busy = false;
    if (!g_r128.accel_ok) return true;
    if (r128_program_2d_context()) return true;
    return r128_fail_acceleration("cambio 3D->2D");
}

static bool r128_bridge_wait_2d_idle(void) {
    if (!g_r128.active) return false;
    return !g_r128.accel_ok || r128_wait_idle();
}

static bool r128_bridge_blit_vram32(uint32_t source_offset,
                                    uint32_t source_pitch_bytes,
                                    int src_x, int src_y,
                                    int dst_x, int dst_y,
                                    int width, int height) {
    gfx_info_t info;
    uint32_t source_pitch_pixels;
    uint32_t source_pitch_offset;
    uint64_t source_end;

    if (!g_r128.active || !g_r128.accel_ok || g_r128.bpp != 32U ||
        !source_pitch_bytes || (source_pitch_bytes & 31U) ||
        (source_offset & 31U) || src_x < 0 || src_y < 0 ||
        width <= 0 || height <= 0)
        return false;
    source_pitch_pixels = source_pitch_bytes / sizeof(uint32_t);
    if (!source_pitch_pixels || (source_pitch_pixels & 7U)) return false;
    source_end = (uint64_t)source_offset +
        (uint64_t)(src_y + height - 1) * source_pitch_bytes +
        (uint64_t)(src_x + width) * sizeof(uint32_t);
    if (source_end > g_r128.vram_size) return false;

    info = (gfx_info_t){
        GFX_MODE_ATI_RAGE128,
        g_r128.fb_bar + g_r128.fb_offset,
        g_r128.width,
        g_r128.height,
        g_r128.pitch,
        g_r128.bpp
    };
    source_pitch_offset = ((source_pitch_pixels >> 3) << 21) |
                          (source_offset >> 5);
    return r128_emit_bitblt(&info, source_pitch_offset,
                            src_x, src_y, dst_x, dst_y,
                            width, height, GFX_ROP_COPY, false);
}

static uint32_t r128_bridge_reserved_vram_end(void) {
    uint32_t offset;
    uint32_t size;
    if (!g_r128.active) return 0U;
    if (r128_surface_layout(&offset, &size)) return offset + size;
    if (r128_staging_tile_layout(&offset, &size))
        return offset + size * g_r128.pitch;
    return r128_visible_end();
}

static const r128_engine_2d_ops_t g_r128_engine_2d_ops = {
    BK_R128_ENGINE_2D_ABI_VERSION,
    sizeof(r128_engine_2d_ops_t),
    "ati_rage128_2d",
    r128_bridge_enter_2d,
    r128_bridge_wait_2d_idle,
    r128_bridge_blit_vram32,
    r128_bridge_reserved_vram_end
};

static uint32_t r128_rgb_distance(uint32_t a, uint32_t b) {
    int ar = (int)((a >> 16) & 0xFFU), ag = (int)((a >> 8) & 0xFFU), ab = (int)(a & 0xFFU);
    int br = (int)((b >> 16) & 0xFFU), bg = (int)((b >> 8) & 0xFFU), bb = (int)(b & 0xFFU);
    int dr = ar - br, dg = ag - bg, db = ab - bb;
    return (uint32_t)(dr * dr + dg * dg + db * db);
}

static bool r128_cursor_define(const uint32_t *argb, uint16_t width,
                               uint16_t height, uint16_t hot_x, uint16_t hot_y) {
    uint8_t *dst;
    uint32_t dark = 0x00FFFFFFU, light = 0U;
    uint32_t min_luma = 0xFFFFFFFFU, max_luma = 0U;
    if (!g_r128.active || !g_r128.cursor_ready || !argb ||
        !width || !height || width > R128_CURSOR_WIDTH ||
        height > R128_CURSOR_HEIGHT || hot_x >= width || hot_y >= height)
        return false;

    for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) {
        uint32_t px = argb[y * width + x];
        uint32_t alpha = px >> 24;
        uint32_t rgb = px & 0x00FFFFFFU;
        uint32_t luma;
        if (alpha < 0x80U) continue;
        luma = ((rgb >> 16) & 0xFFU) * 77U + ((rgb >> 8) & 0xFFU) * 150U +
               (rgb & 0xFFU) * 29U;
        if (luma < min_luma) { min_luma = luma; dark = rgb; }
        if (luma > max_luma) { max_luma = luma; light = rgb; }
    }
    if (min_luma == 0xFFFFFFFFU) { dark = 0U; light = 0x00FFFFFFU; }
    dst = (uint8_t *)(uintptr_t)(g_r128.fb_bar + g_r128.cursor_offset);
    for (uint32_t i = 0; i < R128_CURSOR_BYTES; i++) dst[i] = 0U;

    /* Hardware format: 8 source bytes followed by 8 mask bytes per row.
     * Encodings after ATI's mask inversion/swap are 10 transparent,
     * 00 color0 and 01 color1, MSB first. */
    for (uint32_t y = 0; y < R128_CURSOR_HEIGHT; y++) {
        for (uint32_t x = 0; x < R128_CURSOR_WIDTH; x++) {
            uint32_t source_bit = 1U, mask_bit = 0U;
            if (x < width && y < height) {
                uint32_t px = argb[y * width + x];
                if ((px >> 24) >= 0x80U) {
                    uint32_t rgb = px & 0x00FFFFFFU;
                    source_bit = 0U;
                    mask_bit = r128_rgb_distance(rgb, light) <
                               r128_rgb_distance(rgb, dark) ? 1U : 0U;
                }
            }
            if (source_bit) dst[y * 16U + x / 8U] |= (uint8_t)(0x80U >> (x & 7U));
            if (mask_bit) dst[y * 16U + 8U + x / 8U] |= (uint8_t)(0x80U >> (x & 7U));
        }
    }
    r128_write(R128_CUR_CLR0, dark);
    r128_write(R128_CUR_CLR1, light);
    g_r128.cursor_hot_x = hot_x;
    g_r128.cursor_hot_y = hot_y;
    g_r128.cursor_defined = true;
    return true;
}

static bool r128_cursor_move(int x, int y) {
    int hw_x, hw_y;
    uint32_t xorigin = 0U, yorigin = 0U;
    if (!g_r128.active || !g_r128.cursor_ready || !g_r128.cursor_defined)
        return false;
    g_r128.cursor_x = x;
    g_r128.cursor_y = y;
    hw_x = x - (int)g_r128.cursor_hot_x;
    hw_y = y - (int)g_r128.cursor_hot_y;
    if (hw_x < 0) xorigin = (uint32_t)(-hw_x + 1);
    if (hw_y < 0) yorigin = (uint32_t)(-hw_y + 1);
    if (xorigin >= R128_CURSOR_WIDTH) xorigin = R128_CURSOR_WIDTH - 1U;
    if (yorigin >= R128_CURSOR_HEIGHT) yorigin = R128_CURSOR_HEIGHT - 1U;
    r128_write(R128_CUR_HORZ_VERT_OFF, R128_CUR_LOCK | (xorigin << 16) | yorigin);
    r128_write(R128_CUR_HORZ_VERT_POSN, R128_CUR_LOCK |
               ((uint32_t)(xorigin ? 0 : hw_x) << 16) |
               (uint32_t)(yorigin ? 0 : hw_y));
    r128_write(R128_CUR_OFFSET, g_r128.cursor_offset + yorigin * 16U);
    return true;
}

static bool r128_cursor_show(bool visible) {
    uint32_t gen;
    if (!g_r128.active || !g_r128.cursor_ready) return false;
    gen = r128_read(R128_CRTC_GEN_CNTL);
    if (visible && g_r128.cursor_defined) gen |= R128_CRTC_CUR_EN;
    else gen &= ~R128_CRTC_CUR_EN;
    r128_write(R128_CRTC_GEN_CNTL, gen);
    g_r128.cursor_visible = visible && g_r128.cursor_defined;
    return true;
}

static bool r128_overlay_stop_locked(void) {
    if (!g_r128.active || !g_r128.overlay_ready) return false;
    r128_write(R128_OV0_SCALE_CNTL, 0U);
    g_r128.overlay_active = false;
    return true;
}

static bool r128_overlay_wait_lock(void) {
    for (uint32_t i = 0; i < R128_TIMEOUT; i++)
        if (r128_read(R128_OV0_REG_LOAD_CNTL) & (1U << 3)) return true;
    return false;
}

static bool r128_overlay_put_locked(const gfx_info_t *info, const void *pixels,
                             uint32_t source_pitch, uint16_t source_width,
                             uint16_t source_height, gfx_overlay_format_t format,
                             int dst_x, int dst_y, int dst_w, int dst_h) {
    uint32_t pitch, size, offset, reserved_end;
    uint8_t *dst;
    const uint8_t *src = (const uint8_t *)pixels;
    uint32_t ecp_div, vclk, h_inc, v_inc, step_by = 1U;
    uint32_t tmp, p1_h_accum, p23_h_accum, p1_v_accum;
    if (!g_r128.active || !g_r128.overlay_ready || !info || info->bpp != 32U || !pixels ||
        source_width < 2U || (source_width & 1U) || !source_height ||
        source_width > 2048U || source_height > 2048U ||
        source_pitch < (uint32_t)source_width * 2U ||
        dst_w <= 0 || dst_h <= 0 || dst_x < 0 || dst_y < 0 ||
        dst_x + dst_w > info->width || dst_y + dst_h > info->height ||
        (format != GFX_OVERLAY_YUY2 && format != GFX_OVERLAY_UYVY))
        return false;

    pitch = r128_align_up((uint32_t)source_width * 2U, 16U);
    size = r128_align_up(pitch * source_height, 16U);
    if (size + R128_OVERLAY_MIN_FREE > g_r128.cursor_offset) return false;
    offset = r128_align_down(g_r128.cursor_offset - size, 16U);
    reserved_end = g_r128.fb_offset + (uint32_t)info->pitch * info->height;
    if (g_r128.surface_used &&
        g_r128.surface_offset + g_r128.surface_size > reserved_end)
        reserved_end = g_r128.surface_offset + g_r128.surface_size;
    {
        uint32_t tile_offset, tile_rows;
        if (r128_staging_tile_layout(&tile_offset, &tile_rows) &&
            tile_offset + tile_rows * g_r128.pitch > reserved_end)
            reserved_end = tile_offset + tile_rows * g_r128.pitch;
    }
    if (offset < reserved_end + R128_OVERLAY_MIN_FREE) return false;
    dst = (uint8_t *)(uintptr_t)(g_r128.fb_bar + offset);
    for (uint32_t y = 0; y < source_height; y++) {
        for (uint32_t x = 0; x < (uint32_t)source_width * 2U; x++)
            dst[y * pitch + x] = src[y * source_pitch + x];
        for (uint32_t x = (uint32_t)source_width * 2U; x < pitch; x++)
            dst[y * pitch + x] = 0U;
    }
    g_r128.overlay_offset = offset;
    g_r128.overlay_size = size;

    /* The overlay is selected where the graphics plane contains this key. */
    if (r128_fill_rect(info, dst_x, dst_y, dst_w, dst_h,
                       R128_OVERLAY_KEY, NULL)) {
        if (g_r128.engine_busy && !r128_wait_idle()) return false;
    } else {
        vesa_fill_rect_rgb(info, dst_x, dst_y, dst_w, dst_h, R128_OVERLAY_KEY);
    }
    r128_write(R128_OV0_SCALE_CNTL, 0x80000000U);
    r128_write(R128_OV0_EXCLUSIVE_HORZ, 0U);
    r128_write(R128_OV0_AUTO_FLIP_CNTL, 0U);
    r128_write(R128_OV0_FILTER_CNTL, 0x0000000FU);
    r128_write(R128_OV0_COLOUR_CNTL, (16U << 8) | (16U << 16));
    r128_write(R128_OV0_GRAPHICS_KEY_MSK, 0x00FFFFFFU);
    r128_write(R128_OV0_GRAPHICS_KEY_CLR, R128_OVERLAY_KEY);
    r128_write(R128_OV0_KEY_CNTL, R128_GRAPHIC_KEY_FN_NE);

    ecp_div = g_r128.current_pixel_khz < 12500U ? 0U :
              (g_r128.current_pixel_khz < 25000U ? 1U : 2U);
    vclk = r128_pll_read(R128_VCLK_ECP_CNTL);
    r128_pll_write(R128_VCLK_ECP_CNTL, (vclk & ~(3U << 8)) | (ecp_div << 8));
    v_inc = ((uint32_t)source_height << 20) / (uint32_t)dst_h;
    h_inc = ((uint32_t)source_width << (12U + ecp_div)) / (uint32_t)dst_w;
    while (h_inc >= (2U << 12)) { step_by++; h_inc >>= 1; }
    tmp = 0x00028000U + (h_inc << 3);
    p1_h_accum = ((tmp << 4) & 0x000F8000U) | ((tmp << 12) & 0xF0000000U);
    tmp = 0x00028000U + (h_inc << 2);
    p23_h_accum = ((tmp << 4) & 0x000F8000U) | ((tmp << 12) & 0x70000000U);
    p1_v_accum = (0x00018000U << 4 & 0x03FF8000U) | 1U;

    r128_write(R128_OV0_REG_LOAD_CNTL, 1U);
    if (!r128_overlay_wait_lock()) { r128_write(R128_OV0_SCALE_CNTL, 0U); return false; }
    r128_write(R128_OV0_H_INC, h_inc | ((h_inc >> 1) << 16));
    r128_write(R128_OV0_STEP_BY, step_by | (step_by << 8));
    r128_write(R128_OV0_Y_X_START, (uint32_t)dst_x | ((uint32_t)dst_y << 16));
    r128_write(R128_OV0_Y_X_END, (uint32_t)(dst_x + dst_w) |
               ((uint32_t)(dst_y + dst_h) << 16));
    r128_write(R128_OV0_V_INC, v_inc);
    r128_write(R128_OV0_P1_BLANK_LINES_AT_TOP, 0x00000FFFU |
               ((uint32_t)(source_height - 1U) << 16));
    r128_write(R128_OV0_VID_BUF_PITCH0_VALUE, pitch);
    r128_write(R128_OV0_P1_X_START_END, (uint32_t)source_width - 1U);
    r128_write(R128_OV0_P2_X_START_END, ((uint32_t)source_width / 2U) - 1U);
    r128_write(R128_OV0_P3_X_START_END, ((uint32_t)source_width / 2U) - 1U);
    r128_write(R128_OV0_VID_BUF0_BASE_ADRS, offset & 0xFFFFFFF0U);
    r128_write(R128_OV0_P1_V_ACCUM_INIT, p1_v_accum);
    r128_write(R128_OV0_P23_V_ACCUM_INIT, 0U);
    r128_write(R128_OV0_P1_H_ACCUM_INIT, p1_h_accum);
    r128_write(R128_OV0_P23_H_ACCUM_INIT, p23_h_accum);
    r128_write(R128_OV0_SCALE_CNTL, format == GFX_OVERLAY_UYVY ?
               0x41FF8C03U : 0x41FF8B03U);
    r128_write(R128_OV0_REG_LOAD_CNTL, 0U);
    g_r128.overlay_active = true;
    return true;
}

static bool r128_overlay_stop(void) {
    bool ok;
    if (!r128_engine_acquire_2d()) return false;
    ok = r128_overlay_stop_locked();
    r128_engine_release_2d();
    return ok;
}

static bool r128_overlay_put(const gfx_info_t *info, const void *pixels,
                             uint32_t source_pitch, uint16_t source_width,
                             uint16_t source_height, gfx_overlay_format_t format,
                             int dst_x, int dst_y, int dst_w, int dst_h) {
    bool ok;
    if (!r128_engine_acquire_2d()) return false;
    ok = r128_overlay_put_locked(info, pixels, source_pitch,
                                 source_width, source_height, format,
                                 dst_x, dst_y, dst_w, dst_h);
    r128_engine_release_2d();
    return ok;
}

static bool ati_rage128_driver_init(void) {
    static const gfx_driver_ops_t ops = {
        BK_GFX_DRIVER_ABI_VERSION,
        sizeof(gfx_driver_ops_t),
        "ati_rage128_mobility",
        190U,
        GFX_CAP_PRESENT_BUFFER | GFX_CAP_DIRTY_RECTS |
        GFX_CAP_PRESENT_CPU_COPY | GFX_CAP_PRESENT_VRAM_BLIT |
        GFX_CAP_BITBLT | GFX_CAP_FILL | GFX_CAP_FILL_HW |
        GFX_CAP_HW_CURSOR | GFX_CAP_VIDEO_OVERLAY |
        GFX_CAP_BITBLT_COPY_HW | GFX_CAP_BITBLT_ROP_HW |
        GFX_CAP_OFFSCREEN_VRAM | GFX_CAP_SURFACE_BLIT_HW,
        r128_activate,
        r128_capabilities,
        r128_disable,
        r128_list_modes,
        r128_set_mode,
        r128_present_buffer,
        r128_update_rect,
        r128_flush,
        r128_wait_fence,
        r128_fill_rect,
        r128_bitblt,
        r128_cursor_define, r128_cursor_move, r128_cursor_show,
        r128_surface_create, r128_surface_destroy,
        r128_surface_upload, r128_surface_blit,
        r128_overlay_put, r128_overlay_stop
    };

    kprintf("[ATIR128:TRACE] DRIVER init begin ops_size=%u gfx_abi=%u\n",
            (uint32_t)sizeof(ops), BK_GFX_DRIVER_ABI_VERSION);
    if (!r128_find_device()) {
        kprintf("[ATIR128.DVR] dispositivo ATI 1002:4C46 no detectado\n");
        return false;
    }
    if (!r128_engine_register_2d(&g_r128_engine_2d_ops)) {
        kprintf("[ATIR128:TRACE] DRIVER init FAIL coordinador Rage 128\n");
        return false;
    }
    if (!gfx_register_driver(&ops)) {
        r128_engine_unregister_2d(&g_r128_engine_2d_ops);
        kprintf("[ATIR128:TRACE] DRIVER init FAIL gfx_register_driver\n");
        return false;
    }
    kprintf("[ATIR128:TRACE] DRIVER init gfx_register_driver OK\n");
    kprintf("[ATIR128.DVR] ATI Rage Mobility M3 detectada, MMIO=%x FB=%x\n",
            g_r128.mmio_bar, g_r128.fb_bar);
    return true;
}

static void ati_rage128_driver_shutdown(void) {
    r128_disable();
    r128_engine_report("shutdown 2D");
    r128_engine_unregister_2d(&g_r128_engine_2d_ops);
}

const bk_driver_module_t *bleskernos_driver_query(void) {
    static const bk_driver_module_t module = {
        BK_DRIVER_ABI_VERSION,
        sizeof(bk_driver_module_t),
        1U, 0U,
        "ati_rage128_mobility",
        "ATI Rage Mobility M3/LF: 2D/cursor nativo 8/32bpp, overlay y modeset seguro",
        ati_rage128_driver_init,
        ati_rage128_driver_shutdown
    };
    return &module;
}
