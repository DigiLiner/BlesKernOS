#ifndef PIC_H
#define PIC_H

#include "types.h"
#include "idt.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC1_OFFSET 0x20
#define PIC2_OFFSET 0x28

#define PIC_EOI     0x20
#define ICW1_ICW4   0x01
#define ICW1_INIT   0x10
#define ICW4_8086   0x01

typedef void (*irq_handler_t)(registers_t *regs);

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void irq_install_handler(uint8_t irq, irq_handler_t handler);
void irq_uninstall_handler(uint8_t irq);
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);
registers_t *irq_handler(registers_t *regs);

#ifdef BLES_ARCH_ARM32_RPI3
/* x86 port-I/O has no meaning on BCM283x. Architecture-neutral users of
 * pic.h (notably the desktop power menu during early ARM bring-up) still
 * need interrupt enable/disable primitives, so map those to CPSR while
 * leaving legacy port accesses inert until their drivers are ported. */
static inline void sti(void) { __asm__ volatile ("cpsie i" ::: "memory"); }
static inline void cli(void) { __asm__ volatile ("cpsid i" ::: "memory"); }
static inline uint8_t inb(uint16_t port) { (void)port; return 0xFFU; }
static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline void pic_outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0xFFFFFFFFU; }
static inline void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
static inline void io_wait(void) { __asm__ volatile ("nop"); }
#else
static inline void sti(void) { __asm__ volatile ("sti"); }
static inline void cli(void) { __asm__ volatile ("cli"); }
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void pic_outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "d"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline void io_wait(void) {
    outb(0x80, 0);
}
#endif

#endif
