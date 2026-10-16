#include "uart.h"
#include "mmio.h"

/* BCM2837 peripheral bus as seen by the ARM on Raspberry Pi 3. */
#define PERIPHERAL_BASE 0x3F000000U
#define GPIO_BASE       (PERIPHERAL_BASE + 0x200000U)
#define AUX_BASE        (PERIPHERAL_BASE + 0x215000U)

#define GPFSEL1         (GPIO_BASE + 0x04U)
#define GPPUD           (GPIO_BASE + 0x94U)
#define GPPUDCLK0       (GPIO_BASE + 0x98U)

#define AUX_ENABLES     (AUX_BASE + 0x04U)
#define AUX_MU_IO_REG   (AUX_BASE + 0x40U)
#define AUX_MU_IER_REG  (AUX_BASE + 0x44U)
#define AUX_MU_IIR_REG  (AUX_BASE + 0x48U)
#define AUX_MU_LCR_REG  (AUX_BASE + 0x4CU)
#define AUX_MU_MCR_REG  (AUX_BASE + 0x50U)
#define AUX_MU_LSR_REG  (AUX_BASE + 0x54U)
#define AUX_MU_CNTL_REG (AUX_BASE + 0x60U)
#define AUX_MU_BAUD_REG (AUX_BASE + 0x68U)

static void delay_cycles(uint32_t count) {
    while (count--) __asm__ volatile("nop");
}

void rpi3_uart_init(void) {
    uint32_t v;

    /* Mini UART on GPIO14/15 (ALT5). This avoids depending on PL011 routing. */
    mmio_write(AUX_ENABLES, mmio_read(AUX_ENABLES) | 1U);
    mmio_write(AUX_MU_CNTL_REG, 0U);
    mmio_write(AUX_MU_IER_REG, 0U);
    mmio_write(AUX_MU_LCR_REG, 3U); /* 8-bit mode */
    mmio_write(AUX_MU_MCR_REG, 0U);
    mmio_write(AUX_MU_IIR_REG, 0xC6U);

    /* 115200 baud when core_freq is fixed to 250 MHz in config.txt. */
    mmio_write(AUX_MU_BAUD_REG, 270U);

    v = mmio_read(GPFSEL1);
    v &= ~((7U << 12) | (7U << 15));
    v |=  ((2U << 12) | (2U << 15)); /* GPIO14/15 = ALT5 */
    mmio_write(GPFSEL1, v);

    mmio_write(GPPUD, 0U);
    delay_cycles(150U);
    mmio_write(GPPUDCLK0, (1U << 14) | (1U << 15));
    delay_cycles(150U);
    mmio_write(GPPUDCLK0, 0U);

    mmio_write(AUX_MU_CNTL_REG, 3U);
}

void rpi3_uart_putc(char c) {
    if (c == '\n') rpi3_uart_putc('\r');
    while ((mmio_read(AUX_MU_LSR_REG) & 0x20U) == 0U) { }
    mmio_write(AUX_MU_IO_REG, (uint32_t)(uint8_t)c);
}

void rpi3_uart_puts(const char *s) {
    if (!s) return;
    while (*s) rpi3_uart_putc(*s++);
}

void rpi3_uart_puthex(unsigned int value) {
    static const char hex[] = "0123456789ABCDEF";
    int shift;
    rpi3_uart_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4)
        rpi3_uart_putc(hex[(value >> (unsigned)shift) & 0xFU]);
}


bool rpi3_uart_try_getc(char *out) {
    uint32_t lsr;
    if (!out) return false;
    lsr = mmio_read(AUX_MU_LSR_REG);
    if ((lsr & 0x01U) == 0U) return false;
    *out = (char)(uint8_t)mmio_read(AUX_MU_IO_REG);
    return true;
}
