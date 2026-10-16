#include "sd.h"
#include "mailbox.h"
#include "mmio.h"
#include "timer.h"

#define PERIPHERAL_BASE 0x3F000000U
#define EMMC_BASE       (PERIPHERAL_BASE + 0x300000U)

#define EMMC_BLKSIZECNT (EMMC_BASE + 0x04U)
#define EMMC_ARG1       (EMMC_BASE + 0x08U)
#define EMMC_CMDTM      (EMMC_BASE + 0x0CU)
#define EMMC_RESP0      (EMMC_BASE + 0x10U)
#define EMMC_RESP1      (EMMC_BASE + 0x14U)
#define EMMC_RESP2      (EMMC_BASE + 0x18U)
#define EMMC_RESP3      (EMMC_BASE + 0x1CU)
#define EMMC_DATA       (EMMC_BASE + 0x20U)
#define EMMC_STATUS     (EMMC_BASE + 0x24U)
#define EMMC_CONTROL0   (EMMC_BASE + 0x28U)
#define EMMC_CONTROL1   (EMMC_BASE + 0x2CU)
#define EMMC_INTERRUPT  (EMMC_BASE + 0x30U)
#define EMMC_IRPT_MASK  (EMMC_BASE + 0x34U)
#define EMMC_IRPT_EN    (EMMC_BASE + 0x38U)
#define EMMC_SLOTISR_VER (EMMC_BASE + 0xFCU)

#define SRST_HC         (1U << 24)
#define SRST_CMD        (1U << 25)
#define SRST_DATA       (1U << 26)
#define CLK_INTLEN      (1U << 0)
#define CLK_STABLE      (1U << 1)
#define CLK_EN          (1U << 2)
#define DATA_TOUNIT_MAX (0xEU << 16)

#define STATUS_CMD_INHIBIT (1U << 0)
#define STATUS_DAT_INHIBIT (1U << 1)

#define INT_CMD_DONE    (1U << 0)
#define INT_DATA_DONE   (1U << 1)
#define INT_WRITE_RDY   (1U << 4)
#define INT_READ_RDY    (1U << 5)
#define INT_ERROR       (1U << 15)
#define INT_ERROR_MASK  0xFFFF8000U

#define CMD_RSP_NONE    (0U << 16)
#define CMD_RSP_136     (1U << 16)
#define CMD_RSP_48      (2U << 16)
#define CMD_RSP_48_BUSY (3U << 16)
#define CMD_CRCCHK      (1U << 19)
#define CMD_IXCHK       (1U << 20)
#define CMD_ISDATA      (1U << 21)
#define TM_DAT_READ     (1U << 4)

#define CMD_R1          (CMD_RSP_48 | CMD_CRCCHK | CMD_IXCHK)
#define CMD_R1B         (CMD_RSP_48_BUSY | CMD_CRCCHK | CMD_IXCHK)
#define CMD_R2          (CMD_RSP_136 | CMD_CRCCHK)
#define CMD_R3          CMD_RSP_48
#define CMD_R6          CMD_R1
#define CMD_R7          CMD_R1

#define OCR_BUSY        (1U << 31)
#define OCR_CCS         (1U << 30)
#define OCR_HCS         (1U << 30)
#define OCR_VOLTAGE     0x00FF8000U

#define EMMC_TIMEOUT_US 1000000U
#define SD_INIT_RETRIES 100U
#define SD_BLOCK_SIZE   512U

static rpi3_sd_info_t g_sd;

static int elapsed(uint32_t start, uint32_t usec) {
    return (uint32_t)(rpi3_timer_now_us() - start) >= usec;
}

static int wait_clear(uint32_t reg, uint32_t mask, uint32_t timeout_us) {
    uint32_t start = rpi3_timer_now_us();
    while ((mmio_read(reg) & mask) != 0U)
        if (elapsed(start, timeout_us)) return -1;
    return 0;
}

static int wait_set(uint32_t reg, uint32_t mask, uint32_t timeout_us) {
    uint32_t start = rpi3_timer_now_us();
    while ((mmio_read(reg) & mask) != mask)
        if (elapsed(start, timeout_us)) return -1;
    return 0;
}

static void clear_interrupts(uint32_t mask) {
    mmio_write(EMMC_INTERRUPT, mask);
    arm_dmb();
}

static int reset_line(uint32_t bit) {
    uint32_t v = mmio_read(EMMC_CONTROL1);
    mmio_write(EMMC_CONTROL1, v | bit);
    return wait_clear(EMMC_CONTROL1, bit, EMMC_TIMEOUT_US);
}

/* SDHCI v3 divider: base/(2*divisor), divisor=0 means base clock. The 10-bit
   divisor is split across CONTROL1[15:8] and [7:6]. */
static int set_clock(uint32_t target_hz) {
    uint32_t base = g_sd.base_clock_hz;
    uint32_t div;
    uint32_t encoded;
    uint32_t c1;

    if (base == 0U || target_hz == 0U) return -1;
    c1 = mmio_read(EMMC_CONTROL1);
    c1 &= ~CLK_EN;
    mmio_write(EMMC_CONTROL1, c1);
    rpi3_timer_delay_us(10U);

    if (target_hz >= base) div = 0U;
    else {
        uint32_t denom = target_hz * 2U;
        /* Current Pi 3 EMMC clocks are far below 2^32; keep this strictly
           32-bit so the freestanding bootstrap needs no __aeabi_uldivmod. */
        if (denom == 0U || base > 0xFFFFFFFFU - (denom - 1U)) return -3;
        div = (base + denom - 1U) / denom;
        if (div == 0U) div = 1U;
        if (div > 0x3FFU) div = 0x3FFU;
    }

    encoded = ((div & 0xFFU) << 8) | ((div & 0x300U) >> 2);
    c1 &= ~((0xFFU << 8) | (3U << 6) | (0xFU << 16));
    c1 |= encoded | DATA_TOUNIT_MAX | CLK_INTLEN;
    mmio_write(EMMC_CONTROL1, c1);
    if (wait_set(EMMC_CONTROL1, CLK_STABLE, 100000U) != 0) return -2;
    mmio_write(EMMC_CONTROL1, c1 | CLK_EN);
    rpi3_timer_delay_us(1000U);
    g_sd.bus_clock_hz = div == 0U ? base : base / (2U * div);
    return 0;
}

static int wait_interrupt(uint32_t wanted, uint32_t timeout_us) {
    uint32_t start = rpi3_timer_now_us();
    for (;;) {
        uint32_t irpt = mmio_read(EMMC_INTERRUPT);
        g_sd.last_interrupt = irpt;
        if ((irpt & INT_ERROR_MASK) != 0U) return -2;
        if ((irpt & wanted) != 0U) return 0;
        if (elapsed(start, timeout_us)) return -1;
    }
}

static int send_cmd(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *resp0) {
    uint32_t inhibit = STATUS_CMD_INHIBIT;
    uint32_t cmd = (index << 24) | flags;
    int rc;

    if ((flags & CMD_ISDATA) != 0U || (flags & CMD_RSP_48_BUSY) == CMD_RSP_48_BUSY)
        inhibit |= STATUS_DAT_INHIBIT;
    if (wait_clear(EMMC_STATUS, inhibit, EMMC_TIMEOUT_US) != 0) return -10;

    clear_interrupts(0xFFFFFFFFU);
    mmio_write(EMMC_ARG1, arg);
    mmio_write(EMMC_CMDTM, cmd);
    arm_dmb();

    rc = wait_interrupt(INT_CMD_DONE, EMMC_TIMEOUT_US);
    if (rc != 0) {
        (void)reset_line(SRST_CMD);
        clear_interrupts(0xFFFFFFFFU);
        return rc;
    }
    clear_interrupts(INT_CMD_DONE);
    if (resp0) *resp0 = mmio_read(EMMC_RESP0);

    if ((flags & CMD_RSP_48_BUSY) == CMD_RSP_48_BUSY &&
        wait_clear(EMMC_STATUS, STATUS_DAT_INHIBIT, EMMC_TIMEOUT_US) != 0)
        return -11;
    return 0;
}

static int app_cmd(uint32_t rca, uint32_t acmd, uint32_t arg,
                   uint32_t flags, uint32_t *resp) {
    int rc = send_cmd(55U, rca, CMD_R1, (uint32_t *)0);
    if (rc != 0) return rc;
    return send_cmd(acmd, arg, flags, resp);
}

int rpi3_sd_init(void) {
    uint32_t c1;
    uint32_t resp;
    uint32_t retry;
    uint32_t clock = 0U;
    int rc;

    g_sd.rca = 0U; g_sd.ocr = 0U; g_sd.base_clock_hz = 0U;
    g_sd.bus_clock_hz = 0U; g_sd.last_interrupt = 0U;
    g_sd.initialized = false; g_sd.high_capacity = false; g_sd.cmd8_supported = false;

    if (rpi3_mailbox_get_clock_rate(RPI3_CLOCK_EMMC, &clock) != 0 || clock == 0U)
        clock = 100000000U; /* documented controller source range fallback */
    g_sd.base_clock_hz = clock;

    c1 = mmio_read(EMMC_CONTROL1);
    mmio_write(EMMC_CONTROL1, c1 | SRST_HC);
    if (wait_clear(EMMC_CONTROL1, SRST_HC, EMMC_TIMEOUT_US) != 0) return -1;

    mmio_write(EMMC_CONTROL0, mmio_read(EMMC_CONTROL0) & ~(1U << 1)); /* 1-bit bus */
    mmio_write(EMMC_IRPT_EN, 0U);       /* Stage 5 polls the controller. */
    mmio_write(EMMC_IRPT_MASK, 0xFFFFFFFFU);
    clear_interrupts(0xFFFFFFFFU);

    rc = set_clock(400000U);
    if (rc != 0) return -2;
    rpi3_timer_delay_us(2000U);

    if (send_cmd(0U, 0U, CMD_RSP_NONE, (uint32_t *)0) != 0) return -3;
    rc = send_cmd(8U, 0x000001AAU, CMD_R7, &resp);
    if (rc == 0) {
        if ((resp & 0xFFFU) != 0x1AAU) return -4;
        g_sd.cmd8_supported = true;
    } else {
        /* SD v1.x cards may legally time out CMD8. Reset the command line and
           continue ACMD41 without HCS. */
        (void)reset_line(SRST_CMD);
        clear_interrupts(0xFFFFFFFFU);
    }

    for (retry = 0U; retry < SD_INIT_RETRIES; ++retry) {
        uint32_t arg = OCR_VOLTAGE | (g_sd.cmd8_supported ? OCR_HCS : 0U);
        rc = app_cmd(0U, 41U, arg, CMD_R3, &resp);
        if (rc == 0 && (resp & OCR_BUSY) != 0U) {
            g_sd.ocr = resp;
            break;
        }
        rpi3_timer_delay_us(10000U);
    }
    if (retry == SD_INIT_RETRIES) return -5;
    g_sd.high_capacity = (g_sd.ocr & OCR_CCS) != 0U;

    if (send_cmd(2U, 0U, CMD_R2, (uint32_t *)0) != 0) return -6;
    if (send_cmd(3U, 0U, CMD_R6, &resp) != 0) return -7;
    g_sd.rca = resp & 0xFFFF0000U;
    if (g_sd.rca == 0U) return -8;
    if (send_cmd(7U, g_sd.rca, CMD_R1B, &resp) != 0) return -9;

    if (set_clock(25000000U) != 0) return -10;
    if (!g_sd.high_capacity && send_cmd(16U, SD_BLOCK_SIZE, CMD_R1, &resp) != 0)
        return -11;

    g_sd.initialized = true;
    return 0;
}

int rpi3_sd_read_block(uint32_t lba, void *buffer512) {
    uint32_t arg;
    uint32_t i;
    uint32_t *out = (uint32_t *)buffer512;
    int rc;

    if (!g_sd.initialized || !buffer512) return -1;
    if (!g_sd.high_capacity && lba > (0xFFFFFFFFU / SD_BLOCK_SIZE)) return -2;
    arg = g_sd.high_capacity ? lba : lba * SD_BLOCK_SIZE;

    mmio_write(EMMC_BLKSIZECNT, (1U << 16) | SD_BLOCK_SIZE);
    rc = send_cmd(17U, arg, CMD_R1 | CMD_ISDATA | TM_DAT_READ, (uint32_t *)0);
    if (rc != 0) return -3;

    rc = wait_interrupt(INT_READ_RDY, EMMC_TIMEOUT_US);
    if (rc != 0) {
        (void)reset_line(SRST_DATA);
        return -4;
    }
    clear_interrupts(INT_READ_RDY);
    for (i = 0U; i < SD_BLOCK_SIZE / 4U; ++i)
        out[i] = mmio_read(EMMC_DATA);

    rc = wait_interrupt(INT_DATA_DONE, EMMC_TIMEOUT_US);
    if (rc != 0) {
        (void)reset_line(SRST_DATA);
        return -5;
    }
    clear_interrupts(INT_DATA_DONE);
    return 0;
}

int rpi3_sd_read_blocks(uint32_t lba, uint32_t count, void *buffer) {
    uint32_t i;
    uint8_t *out = (uint8_t *)buffer;
    if (count == 0U || !buffer) return -1;
    for (i = 0U; i < count; ++i) {
        int rc = rpi3_sd_read_block(lba + i, out + i * SD_BLOCK_SIZE);
        if (rc != 0) return rc;
    }
    return 0;
}

const rpi3_sd_info_t *rpi3_sd_info(void) {
    return &g_sd;
}
