/* Original BlesKernOS integration of the ACPI PM timer. The FADT layout is
 * defined by ACPI; verified reads and PIT rate checks follow the approach in
 * Linux drivers/clocksource/acpi_pm.c (see docs/platform/c600-performance.md).
 * No Linux code is copied. PIIX4 counters can return torn values while ticking. */
#include "../../include/acpi_pm.h"
#include "../../include/memory.h"
#include "../../include/pit.h"
#include "../../include/vga.h"
#include "../../include/pic.h"
static uint16_t g_pm_port;
static uint32_t g_pm_mask = 0xFFFFFFU;
static bool g_pm_checked;
static uint32_t pm_u32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static bool pm_checksum(const uint8_t *p, uint32_t size) {
    uint8_t sum=0;
    for(uint32_t i=0;i<size;i++)sum+=p[i];
    return sum==0;
}
static const uint8_t *pm_table(uint32_t address) {
    if(!mm_firmware_range_valid(address,36U))return NULL;
    const uint8_t *p=(const uint8_t *)(uintptr_t)address;
    uint32_t len=pm_u32(p+4);
    if(len<36U || len>65536U || !mm_firmware_range_valid(address,len) || !pm_checksum(p,len))return NULL;
    return p;
}
static const uint8_t *pm_rsdp(uint32_t begin,uint32_t end) {
    for(uint32_t a=(begin+15U)&~15U;a+20U<=end;a+=16U) {
        const uint8_t *p=(const uint8_t *)(uintptr_t)a;
        if(pm_u32(p)==0x20445352U && pm_u32(p+4)==0x20525450U && pm_checksum(p,20U))return p;
    }
    return NULL;
}
static bool pm_fadt(const uint8_t *p) {
    uint32_t len=pm_u32(p+4),port;
    if(len<116U || pm_u32(p)!=0x50434146U)return false;
    port=pm_u32(p+76U);
    /* Extended GAS takes precedence when present, but only System I/O is supported. */
    if(len>=220U && (pm_u32(p+212U) || pm_u32(p+216U))) {
        if(p[208U]!=1U || (p[209U]!=24U && p[209U]!=32U) || p[210U]!=0U || pm_u32(p+216U))return false;
        port=pm_u32(p+212U);
    } else if(p[91U]!=4U)return false;
    if(!port || port>65532U)return false;
    g_pm_port=(uint16_t)port;
    g_pm_mask=(pm_u32(p+112U)&(1U<<8))?0xFFFFFFFFU:0xFFFFFFU;
    return true;
}
bool acpi_pm_read(uint32_t *counter) {
    if(!g_pm_port || !counter)return false;
    /* Three successive samples allow a wrap but reject an inconsistent middle
     * sample. Bounded retries avoid hanging on a broken or absent device. */
    for(uint32_t i=0;i<8U;i++) {
        uint32_t a=inl(g_pm_port)&g_pm_mask,b=inl(g_pm_port)&g_pm_mask,c=inl(g_pm_port)&g_pm_mask;
        if(((b-a)&g_pm_mask)<=((c-a)&g_pm_mask) && ((c-a)&g_pm_mask)<ACPI_PM_HZ/10U) {
            *counter=b;return true;
        }
    }
    return false;
}
uint32_t acpi_pm_mask(void) {
    return g_pm_mask;
}
bool acpi_pm_init(void) {
    if(g_pm_checked)return g_pm_port!=0;
    uint32_t hz=pit_get_frequency_hz(),begin=pit_get_ticks();
    if(!hz)return false;
    /* Do not initialize in early boot with a stopped PIT/interrupts disabled. */
    uint32_t flags;__asm__ volatile("pushfl; popl %0":"=r"(flags));
    if(!(flags&0x200U))return false;
    g_pm_checked=true;
    const uint8_t *rsdp=NULL;
    uint32_t ebda=(uint32_t)mm_boot_ebda_segment()<<4;
    if(ebda>=0x80000U && ebda<0xA0000U)rsdp=pm_rsdp(ebda,ebda+1024U);
    if(!rsdp)rsdp=pm_rsdp(0xE0000U,0x100000U);
    if(!rsdp)return false;
    const uint8_t *rsdt=pm_table(pm_u32(rsdp+16U));
    if(!rsdt || pm_u32(rsdt)!=0x54445352U)return false;
    for(uint32_t i=36U;i+4U<=pm_u32(rsdt+4U);i+=4U) {
        const uint8_t *table=pm_table(pm_u32(rsdt+i));
        if(table && pm_fadt(table))break;
    }
    uint32_t a,b;
    if(!acpi_pm_read(&a)) {g_pm_port=0;return false;}
    begin=pit_get_ticks();
    uint32_t ticks=hz/10U;if(ticks<3U)ticks=3U;
    for(uint32_t spins=0;pit_get_ticks()-begin<ticks;spins++) {
        if(spins>=20000000U){g_pm_port=0;return false;}
        __asm__ volatile("pause");
    }
    uint32_t elapsed=pit_get_ticks()-begin;
    if(!acpi_pm_read(&b)){g_pm_port=0;return false;}
    uint32_t delta=(b-a)&g_pm_mask;
    uint32_t expected=(uint32_t)((uint64_t)elapsed*ACPI_PM_HZ/hz);
    if(!delta || delta<expected*9U/10U || delta>expected*11U/10U) {
        g_pm_port=0;return false;
    }
    kprintf("[CLOCK] ACPI PM verificado port=%x mask=%x\n",(uint32_t)g_pm_port,g_pm_mask);
    return true;
}
