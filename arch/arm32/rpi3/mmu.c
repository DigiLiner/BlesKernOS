#include "mmu.h"

#define MB(x) ((uint32_t)(x) << 20)
#define L1_ENTRIES 4096U

/* ARMv7 short-descriptor 1 MiB section entries. */
#define DESC_SECTION       0x00000002U
#define DESC_B             (1U << 2)
#define DESC_C             (1U << 3)
#define DESC_XN            (1U << 4)
#define DESC_AP_PRIV_RW    (1U << 10)
#define DESC_AP_USER_RW    ((1U << 10) | (1U << 11))
#define DESC_TEX1          (1U << 12)
#define DESC_S             (1U << 16)

/* Inner/outer write-back, write-allocate normal memory. */
#define DESC_NORMAL_RAM (DESC_SECTION | DESC_AP_PRIV_RW | DESC_TEX1 | \
                         DESC_C | DESC_B | DESC_S)
#define DESC_NORMAL_USER (DESC_SECTION | DESC_AP_USER_RW | DESC_TEX1 | \
                          DESC_C | DESC_B | DESC_S)
/* Shared Device, execute-never. */
#define DESC_DEVICE     (DESC_SECTION | DESC_AP_PRIV_RW | DESC_B | DESC_S | DESC_XN)

#define SCTLR_M (1U << 0)
#define SCTLR_C (1U << 2)
#define SCTLR_Z (1U << 11)
#define SCTLR_I (1U << 12)

/* One first-level table covers the complete 32-bit virtual address space. */
static uint32_t g_l1[L1_ENTRIES]
    __attribute__((aligned(16384), section(".mmu_tables")));
static uint32_t g_user_first_mb = 0xFFFFFFFFU;
static uint32_t g_user_last_mb = 0U;

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static void isb(void) { __asm__ volatile("isb" ::: "memory"); }


static void clean_l1_entry(uint32_t mb) {
    uintptr_t address = (uintptr_t)&g_l1[mb];
    address &= ~(uintptr_t)63U;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1"
                     :: "r"((uint32_t)address) : "memory");
}

static void tlb_invalidate_all(void) {
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0U) : "memory");
    dsb();
    isb();
}

static bool window_to_mb(uint32_t base, uint32_t bytes,
                         uint32_t *first_out, uint32_t *last_out) {
    uint32_t end;
    if (!bytes || (base & 0x000FFFFFU)) return false;
    end = base + bytes - 1U;
    if (end < base || end >= 0x3F000000U) return false;
    if (first_out) *first_out = base >> 20;
    if (last_out) *last_out = end >> 20;
    return true;
}

bool rpi3_mmu_activate_user_window(uint32_t base, uint32_t bytes) {
    uint32_t first, last, mb;
    if (!window_to_mb(base, bytes, &first, &last)) return false;

    if (g_user_first_mb != 0xFFFFFFFFU) {
        for (mb = g_user_first_mb; mb <= g_user_last_mb; ++mb) {
            g_l1[mb] = (mb << 20) | DESC_NORMAL_RAM;
            clean_l1_entry(mb);
        }
    }
    for (mb = first; mb <= last; ++mb) {
        g_l1[mb] = (mb << 20) | DESC_NORMAL_USER;
        clean_l1_entry(mb);
    }
    g_user_first_mb = first;
    g_user_last_mb = last;
    dsb();
    tlb_invalidate_all();
    return true;
}

void rpi3_mmu_deactivate_user_window(void) {
    uint32_t mb;
    if (g_user_first_mb == 0xFFFFFFFFU) return;
    for (mb = g_user_first_mb; mb <= g_user_last_mb; ++mb) {
        g_l1[mb] = (mb << 20) | DESC_NORMAL_RAM;
        clean_l1_entry(mb);
    }
    g_user_first_mb = 0xFFFFFFFFU;
    g_user_last_mb = 0U;
    dsb();
    tlb_invalidate_all();
}

static uint32_t read_sctlr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

int rpi3_mmu_enabled(void) {
    return (read_sctlr() & SCTLR_M) != 0U;
}

uint32_t rpi3_mmu_ttbr0(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
    return v;
}

static void map_sections(uint32_t first_mb, uint32_t last_mb, uint32_t flags) {
    uint32_t mb;
    for (mb = first_mb; mb <= last_mb; ++mb)
        g_l1[mb] = (mb << 20) | flags;
}

int rpi3_mmu_init(void) {
    uint32_t i;
    uint32_t sctlr;
    uint32_t ttbr0 = (uint32_t)(uintptr_t)g_l1;

    /* Firmware normally enters kernel7.img with MMU off. Refuse to replace an
       unknown translation regime instead of corrupting a live address space. */
    if (rpi3_mmu_enabled())
        return -1;

    for (i = 0U; i < L1_ENTRIES; ++i)
        g_l1[i] = 0U; /* translation fault by default */

    /* Pi 3 RAM is below the peripheral aperture. A conservative identity map
       keeps Stage 3 simple while preserving the existing 32-bit kernel ABI. */
    map_sections(0x000U, 0x3EFU, DESC_NORMAL_RAM);

    /* BCM2837 peripheral bus window visible to ARM at 0x3f000000, plus the
       BCM2836/7 ARM-local peripheral page beginning at 0x40000000. */
    map_sections(0x3F0U, 0x3FFU, DESC_DEVICE);
    map_sections(0x400U, 0x400U, DESC_DEVICE);

    dsb();

    /* Short-descriptor format (TTBCR.EAE=0), all addresses via TTBR0. */
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 2" :: "r"(0U) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c2, c0, 0" :: "r"(ttbr0) : "memory");
    /* Domain 0 = Client, so AP permissions are checked. */
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(1U) : "memory");

    /* Invalidate unified TLB, I-cache and branch predictor before enabling. */
    __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0U) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0U) : "memory");
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 6" :: "r"(0U) : "memory");
    dsb();
    isb();

    sctlr = read_sctlr();
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_Z;
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(sctlr) : "memory");
    isb();

    return rpi3_mmu_enabled() ? 0 : -2;
}
