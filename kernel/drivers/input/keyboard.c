#include "../../include/types.h"
#include "../../../sdk/include/bleskernos_input.h"
#include "../../include/keyboard.h"
#include "../../include/pic.h"
#include "../../include/vga.h"
#include "../../include/idt.h"

#define KBD_BUF_SIZE 256
static char kbd_buffer[KBD_BUF_SIZE];
static volatile uint32_t kbd_buf_head = 0;
static volatile uint32_t kbd_buf_tail = 0;
static kbd_key_event_t kbd_events[KBD_BUF_SIZE];
static volatile uint32_t kbd_evt_head = 0;
static volatile uint32_t kbd_evt_tail = 0;
/* Copia broadcast para consumidores interactivos Ring 3. El GUI conserva su
 * propia cola y ya no puede robarle a un juego los eventos key-up. */
static kbd_key_event_t kbd_app_events[KBD_BUF_SIZE];
static volatile uint32_t kbd_app_head = 0;
static volatile uint32_t kbd_app_tail = 0;
static volatile bool kbd_extended = false;
static volatile bool kbd_break = false;
static volatile kbd_modifiers_t mods = {false, false, false, false, false};

static bool buf_full(void) {
    return ((kbd_buf_head + 1) % KBD_BUF_SIZE) == kbd_buf_tail;
}

static bool buf_empty(void) {
    return kbd_buf_head == kbd_buf_tail;
}

static bool evt_full(void) {
    return ((kbd_evt_head + 1) % KBD_BUF_SIZE) == kbd_evt_tail;
}

static bool evt_empty(void) {
    return kbd_evt_head == kbd_evt_tail;
}

static bool app_evt_full(void) {
    return ((kbd_app_head + 1) % KBD_BUF_SIZE) == kbd_app_tail;
}

static bool app_evt_empty(void) {
    return kbd_app_head == kbd_app_tail;
}

static void buf_push(char c) {
    cli();
    if (!buf_full()) {
        kbd_buffer[kbd_buf_head] = c;
        kbd_buf_head = (kbd_buf_head + 1) % KBD_BUF_SIZE;
    }
    sti();
}

static char buf_pop(void) {
    cli();
    if (buf_empty()) {
        sti();
        return 0;
    }
    char c = kbd_buffer[kbd_buf_tail];
    kbd_buf_tail = (kbd_buf_tail + 1) % KBD_BUF_SIZE;
    sti();
    return c;
}

static void evt_push(uint8_t key, bool pressed) {
    cli();
    if (!evt_full()) {
        kbd_events[kbd_evt_head].key = key;
        kbd_events[kbd_evt_head].pressed = pressed;
        kbd_evt_head = (kbd_evt_head + 1) % KBD_BUF_SIZE;
    }
    if (!app_evt_full()) {
        kbd_app_events[kbd_app_head].key = key;
        kbd_app_events[kbd_app_head].pressed = pressed;
        kbd_app_head = (kbd_app_head + 1) % KBD_BUF_SIZE;
    }
    sti();
}

static bool evt_pop(kbd_key_event_t *event) {
    cli();
    if (evt_empty()) {
        sti();
        return false;
    }
    *event = kbd_events[kbd_evt_tail];
    kbd_evt_tail = (kbd_evt_tail + 1) % KBD_BUF_SIZE;
    sti();
    return true;
}

static bool app_evt_pop(kbd_key_event_t *event) {
    cli();
    if (app_evt_empty()) {
        sti();
        return false;
    }
    *event = kbd_app_events[kbd_app_tail];
    kbd_app_tail = (kbd_app_tail + 1) % KBD_BUF_SIZE;
    sti();
    return true;
}

#ifndef BLES_ARCH_ARM32_RPI3
/* USB-only PCs may have no 8042 at all (status reads as 0xff). Never block
 * boot indefinitely waiting for a legacy keyboard that does not exist. */
static bool kbd_wait_input(void) {
    for (uint32_t budget = 0; budget < 200000U; budget++) {
        uint8_t status = inb(KBD_STATUS_PORT);
        if (status == 0xffU) return false;
        if (!(status & 0x02U)) return true;
        __asm__ volatile("pause");
    }
    return false;
}
static bool kbd_wait_output(void) {
    for (uint32_t budget = 0; budget < 200000U; budget++) {
        uint8_t status = inb(KBD_STATUS_PORT);
        if (status == 0xffU) return false;
        if (status & 0x01U) return true;
        __asm__ volatile("pause");
    }
    return false;
}
static bool kbd_flush(void) {
    for (uint32_t budget = 0; budget < 256U; budget++) {
        uint8_t status = inb(KBD_STATUS_PORT);
        if (status == 0xffU) return false;
        if (!(status & 0x01U)) return true;
        (void)inb(KBD_DATA_PORT);
    }
    return false;
}
static bool kbd_send_cmd(uint8_t cmd) {
    for (unsigned attempt = 0; attempt < 3U; attempt++) {
        if (!kbd_wait_input()) return false;
        outb(KBD_DATA_PORT, cmd);
        if (!kbd_wait_output()) return false;
        uint8_t reply = inb(KBD_DATA_PORT);
        if (reply == 0xfaU) return true;
        if (reply != 0xfeU) return false;
    }
    return false;
}

#endif

static char kbd_map_scancode(uint8_t scancode, bool shifted, bool caps_lock) {
    char c = 0;

    if (bk_input_preference_get(BK_INPUT_LAYOUT)) {
        /* Spanish physical layout, Latin-1 for ñ/Ñ. */
        switch (scancode) {
            case 0x03: return shifted ? '"' : '2';
            case 0x04: return shifted ? 0xB7 : '3';
            case 0x07: return shifted ? '&' : '6';
            case 0x08: return shifted ? '/' : '7';
            case 0x09: return shifted ? '(' : '8';
            case 0x0A: return shifted ? ')' : '9';
            case 0x0B: return shifted ? '=' : '0';
            case 0x0C: return shifted ? '?' : '\'';
            case 0x0D: return shifted ? 0xBF : 0xA1;
            case 0x1A: return shifted ? '^' : '`';
            case 0x1B: return shifted ? '*' : '+';
            case 0x27: return (caps_lock!=shifted) ? (char)0xD1 : (char)0xF1;
            case 0x28: return shifted ? '"' : '\'';
            case 0x29: return shifted ? 0xAA : 0xBA;
            case 0x2B: return shifted ? 0xC7 : 0xE7;
            case 0x33: return shifted ? ';' : ',';
            case 0x34: return shifted ? ':' : '.';
            case 0x35: return shifted ? '_' : '-';
            case 0x56: return shifted ? '>' : '<';
            default: break;
        }
    }
    switch (scancode) {
        case 0x01: c = 0x1B; break; /* Esc */
        case 0x02: c = shifted ? '!' : '1'; break;
        case 0x03: c = shifted ? '@' : '2'; break;
        case 0x04: c = shifted ? '#' : '3'; break;
        case 0x05: c = shifted ? '$' : '4'; break;
        case 0x06: c = shifted ? '%' : '5'; break;
        case 0x07: c = shifted ? '^' : '6'; break;
        case 0x08: c = shifted ? '&' : '7'; break;
        case 0x09: c = shifted ? '*' : '8'; break;
        case 0x0A: c = shifted ? '(' : '9'; break;
        case 0x0B: c = shifted ? ')' : '0'; break;
        case 0x0C: c = shifted ? '_' : '-'; break;
        case 0x0D: c = shifted ? '+' : '='; break;
        case 0x0E: c = '\b'; break;
        case 0x0F: c = '\t'; break;
        case 0x10: c = 'q'; break;
        case 0x11: c = 'w'; break;
        case 0x12: c = 'e'; break;
        case 0x13: c = 'r'; break;
        case 0x14: c = 't'; break;
        case 0x15: c = 'y'; break;
        case 0x16: c = 'u'; break;
        case 0x17: c = 'i'; break;
        case 0x18: c = 'o'; break;
        case 0x19: c = 'p'; break;
        case 0x1A: c = '['; break;
        case 0x1B: c = ']'; break;
        case 0x1C: c = '\n'; break;
        case 0x1E: c = 'a'; break;
        case 0x1F: c = 's'; break;
        case 0x20: c = 'd'; break;
        case 0x21: c = 'f'; break;
        case 0x22: c = 'g'; break;
        case 0x23: c = 'h'; break;
        case 0x24: c = 'j'; break;
        case 0x25: c = 'k'; break;
        case 0x26: c = 'l'; break;
        case 0x27: c = ';'; break;
        case 0x28: c = '\''; break;
        case 0x29: c = '`'; break;
        case 0x2B: c = '\\'; break;
        case 0x2C: c = 'z'; break;
        case 0x2D: c = 'x'; break;
        case 0x2E: c = 'c'; break;
        case 0x2F: c = 'v'; break;
        case 0x30: c = 'b'; break;
        case 0x31: c = 'n'; break;
        case 0x32: c = 'm'; break;
        case 0x33: c = ','; break;
        case 0x34: c = '.'; break;
        case 0x35: c = '/'; break;
        case 0x39: c = ' '; break;
        default: return 0;
    }

    if (c >= 'a' && c <= 'z') {
        if ((caps_lock && !shifted) || (!caps_lock && shifted)) {
            c = (char)('A' + (c - 'a'));
        }
    } else if (c == '`') {
        c = shifted ? '~' : '`';
    } else if (c == '[') {
        c = shifted ? '{' : '[';
    } else if (c == ']') {
        c = shifted ? '}' : ']';
    } else if (c == '\\') {
        c = shifted ? '|' : '\\';
    } else if (c == ';') {
        c = shifted ? ':' : ';';
    } else if (c == '\'') {
        c = shifted ? '"' : '\'';
    } else if (c == ',') {
        c = shifted ? '<' : ',';
    } else if (c == '.') {
        c = shifted ? '>' : '.';
    } else if (c == '/') {
        c = shifted ? '?' : '/';
    }

    return c;
}

static void kbd_push_key_event(uint8_t scancode, bool pressed, bool extended) {
    uint8_t key = 0;

    if (extended) {
        switch (scancode) {
            case 0x1C: key = KEY_ENTER; break; /* keypad Enter */
            case 0x1D: key = KEY_CTRL; break; /* right Ctrl */
            case 0x38: key = KEY_ALT; break;  /* right Alt */
            case 0x48:
            case 0x75: key = KEY_UP; break;
            case 0x50:
            case 0x72: key = KEY_DOWN; break;
            case 0x4B:
            case 0x6B: key = KEY_LEFT; break;
            case 0x4D:
            case 0x74: key = KEY_RIGHT; break;
            case 0x47:
            case 0x6C: key = KEY_HOME; break;
            case 0x4F:
            case 0x69: key = KEY_END; break;
            case 0x49:
            case 0x7D: key = KEY_PGUP; break;
            case 0x51:
            case 0x7A: key = KEY_PGDN; break;
            case 0x52:
            case 0x70: key = KEY_INSERT; break;
            case 0x53:
            case 0x71: key = KEY_DELETE; break;
            default: break;
        }
    } else {
        switch (scancode) {
            case 0x1C: key = KEY_ENTER; break;
            case 0x2A:
            case 0x36: key = KEY_SHIFT; break;
            case 0x1D: key = KEY_CTRL; break;
            case 0x38: key = KEY_ALT; break;
            case 0x3B: key = KEY_F1; break;
            case 0x3C: key = KEY_F2; break;
            case 0x3D: key = KEY_F3; break;
            case 0x3E: key = KEY_F4; break;
            case 0x3F: key = KEY_F5; break;
            case 0x40: key = KEY_F6; break;
            case 0x41: key = KEY_F7; break;
            case 0x42: key = KEY_F8; break;
            case 0x43: key = KEY_F9; break;
            case 0x44: key = KEY_F10; break;
            case 0x57: key = KEY_F11; break;
            case 0x58: key = KEY_F12; break;
            default: {
                /* El evento GUI debe llevar el mismo carácter traducido que
                 * kbd_getchar: Shift y Caps Lock se aplican al presionar. */
                char c = kbd_map_scancode(scancode,
                    pressed ? mods.shift : false,
                    pressed ? mods.caps_lock : false);
                if (c != 0) key = (uint8_t)c;
                break;
            }
        }
    }

    if (key != 0) evt_push(key, pressed);
}

#ifndef BLES_ARCH_ARM32_RPI3
static void kbd_irq_handler(registers_t *regs UNUSED) {
    uint8_t status = inb(KBD_STATUS_PORT);
    uint8_t scancode;

    if ((status & 0x01) == 0) return;
    if (status & 0x20) return;

    scancode = inb(KBD_DATA_PORT);

    if (scancode == 0xE0) {
        kbd_extended = true;
        return;
    }

    if (scancode == 0xF0) {
        kbd_break = true;
        return;
    }

    bool released = kbd_break;
    kbd_break = false;

    if (scancode == 0xE1) {
        return;
    }

    /* El controlador PS/2 de PC normalmente traduce a Set 1, donde el bit
     * alto marca key-up (por ejemplo Ctrl: 0x1D -> 0x9D). También conservamos
     * el prefijo 0xF0 de Set 2 para teclados/controladores sin traducción. */
    if (scancode & 0x80) {
        released = true;
        scancode &= 0x7F;
    }

    if (released) {
        kbd_push_key_event(scancode, false, kbd_extended);
        switch (scancode) {
            case 0x2A:
            case 0x36: mods.shift = false; break;
            case 0x1D: mods.ctrl = false; break;
            case 0x38: mods.alt = false; break;
            case 0x3A: break;
            default: break;
        }
        kbd_extended = false;
        return;
    }

    switch (scancode) {
        case 0x2A:
        case 0x36: mods.shift = true; break;
        case 0x1D: mods.ctrl = true; break;
        case 0x38: mods.alt = true; break;
        case 0x3A:
            mods.caps_lock = !mods.caps_lock;
            break;
        case 0x48:
        case 0x75:
            if (kbd_extended) {
                buf_push(KEY_UP);
                evt_push(KEY_UP, true);
            }
            kbd_extended = false;
            return;
        case 0x50:
        case 0x72:
            if (kbd_extended) {
                buf_push(KEY_DOWN);
                evt_push(KEY_DOWN, true);
            }
            kbd_extended = false;
            return;
        case 0x4B:
        case 0x6B:
            if (kbd_extended) {
                buf_push(KEY_LEFT);
                evt_push(KEY_LEFT, true);
            }
            kbd_extended = false;
            return;
        case 0x4D:
        case 0x74:
            if (kbd_extended) {
                buf_push(KEY_RIGHT);
                evt_push(KEY_RIGHT, true);
            }
            kbd_extended = false;
            return;
        case 0x47:
        case 0x6C:
            if (kbd_extended) {
                buf_push(KEY_HOME);
                evt_push(KEY_HOME, true);
            }
            kbd_extended = false;
            return;
        case 0x4F:
        case 0x69:
            if (kbd_extended) {
                buf_push(KEY_END);
                evt_push(KEY_END, true);
            }
            kbd_extended = false;
            return;
        case 0x49:
        case 0x7D:
            if (kbd_extended) {
                buf_push(KEY_PGUP);
                evt_push(KEY_PGUP, true);
            }
            kbd_extended = false;
            return;
        case 0x51:
        case 0x7A:
            if (kbd_extended) {
                buf_push(KEY_PGDN);
                evt_push(KEY_PGDN, true);
            }
            kbd_extended = false;
            return;
        case 0x52:
        case 0x70:
            if (kbd_extended) {
                buf_push(KEY_INSERT);
                evt_push(KEY_INSERT, true);
            }
            kbd_extended = false;
            return;
        case 0x53:
        case 0x71:
            if (kbd_extended) {
                buf_push(KEY_DELETE);
                evt_push(KEY_DELETE, true);
            }
            kbd_extended = false;
            return;
        default:
            if (scancode >= 0x80) {
                kbd_extended = false;
                return;
            }
            break;
    }

    if (scancode < 0x80) {
        char c = kbd_map_scancode(scancode, mods.shift, mods.caps_lock);
        kbd_push_key_event(scancode, true, false);
        if (c != 0) {
            buf_push(c);
        }
    }

    kbd_extended = false;
}
#endif

void kbd_init(void) {
#ifdef BLES_ARCH_ARM32_RPI3
    kbd_buf_head = kbd_buf_tail = 0U;
    kbd_evt_head = kbd_evt_tail = 0U;
    kbd_app_head = kbd_app_tail = 0U;
    kbd_extended = false;
    kbd_break = false;
    mods = (kbd_modifiers_t){false, false, false, false, false};
#else
    if (!kbd_flush() || !kbd_send_cmd(0xF4)) {
        kprintf("[KBD] PS/2 no responde; entrada USB HID disponible\n");
        return;
    }
    irq_install_handler(1, kbd_irq_handler);
#endif
}

char kbd_getchar(void) {
    while (true) {
        cli();
        if (!buf_empty()) {
            char c = buf_pop();
            sti();
            return c;
        }
        sti();
#ifdef BLES_ARCH_ARM32_RPI3
        __asm__ volatile ("wfi");
#else
        __asm__ volatile ("hlt");
#endif
    }
}

bool kbd_haschar(void) {
    return !buf_empty();
}

void kbd_get_modifiers(kbd_modifiers_t *mods_out) {
    if (mods_out) {
        *mods_out = mods;
    }
}

bool kbd_has_event(void) {
    return !evt_empty();
}

bool kbd_next_event(kbd_key_event_t *event) {
    if (!event) return false;
    return evt_pop(event);
}

bool kbd_next_app_event(kbd_key_event_t *event) {
    if (!event) return false;
    return app_evt_pop(event);
}


void kbd_inject_key(uint8_t key, bool pressed) {
    if (key == KEY_NONE) return;
    evt_push(key, pressed);
    if (pressed && key < 0x80U) {
        if ((key >= 0x20U && key <= 0x7EU) || key == KEY_ENTER ||
            key == KEY_TAB || key == KEY_BACKSPACE || key == KEY_ESCAPE)
            buf_push((char)key);
    }
}


static uint8_t kbd_hid_usage_to_key(uint16_t usage) {
    if (usage >= 0x04U && usage <= 0x1DU)
        return (uint8_t)('a' + (usage - 0x04U));
    switch (usage) {
        case 0x1EU: return '1'; case 0x1FU: return '2';
        case 0x20U: return '3'; case 0x21U: return '4';
        case 0x22U: return '5'; case 0x23U: return '6';
        case 0x24U: return '7'; case 0x25U: return '8';
        case 0x26U: return '9'; case 0x27U: return '0';
        case 0x28U: return KEY_ENTER;
        case 0x29U: return KEY_ESCAPE;
        case 0x2AU: return KEY_BACKSPACE;
        case 0x2BU: return KEY_TAB;
        case 0x2CU: return ' ';
        case 0x2DU: return '-'; case 0x2EU: return '=';
        case 0x2FU: return '['; case 0x30U: return ']';
        case 0x31U: return '\\'; case 0x33U: return ';';
        case 0x34U: return '\''; case 0x35U: return '`';
        case 0x36U: return ','; case 0x37U: return '.';
        case 0x38U: return '/';
        case 0x3AU: return KEY_F1; case 0x3BU: return KEY_F2;
        case 0x3CU: return KEY_F3; case 0x3DU: return KEY_F4;
        case 0x3EU: return KEY_F5; case 0x3FU: return KEY_F6;
        case 0x40U: return KEY_F7; case 0x41U: return KEY_F8;
        case 0x42U: return KEY_F9; case 0x43U: return KEY_F10;
        case 0x44U: return KEY_F11; case 0x45U: return KEY_F12;
        case 0x49U: return KEY_INSERT;
        case 0x4AU: return KEY_HOME;
        case 0x4BU: return KEY_PGUP;
        case 0x4CU: return KEY_DELETE;
        case 0x4DU: return KEY_END;
        case 0x4EU: return KEY_PGDN;
        case 0x4FU: return KEY_RIGHT;
        case 0x50U: return KEY_LEFT;
        case 0x51U: return KEY_DOWN;
        case 0x52U: return KEY_UP;
        case 0xE0U: case 0xE4U: return KEY_CTRL;
        case 0xE1U: case 0xE5U: return KEY_SHIFT;
        case 0xE2U: case 0xE6U: return KEY_ALT;
        default: return KEY_NONE;
    }
}

static char kbd_hid_usage_to_char(uint16_t usage) {
    bool shifted = mods.shift;
    char c = 0;
    if (usage >= 0x04U && usage <= 0x1DU) {
        c = (char)('a' + (usage - 0x04U));
        if ((mods.caps_lock && !shifted) || (!mods.caps_lock && shifted))
            c = (char)('A' + (c - 'a'));
        return c;
    }
    if (bk_input_preference_get(BK_INPUT_LAYOUT)) {
        uint8_t scan=0;
        if (usage>=0x1EU && usage<=0x27U) scan=(uint8_t)(usage-0x1EU+2U);
        else switch (usage) {
            case 0x2D:scan=0x0C;break;case 0x2E:scan=0x0D;break;
            case 0x2F:scan=0x1A;break;case 0x30:scan=0x1B;break;
            case 0x31:scan=0x2B;break;case 0x33:scan=0x27;break;
            case 0x34:scan=0x28;break;case 0x35:scan=0x29;break;
            case 0x36:scan=0x33;break;case 0x37:scan=0x34;break;
            case 0x38:scan=0x35;break;case 0x64:scan=0x56;break;
            default:break;
        }
        if (scan) return kbd_map_scancode(scan,shifted,mods.caps_lock);
    }
    if (usage >= 0x1EU && usage <= 0x27U) {
        static const char plain[] = "1234567890";
        static const char upper[] = "!@#$%^&*()";
        uint32_t i = usage - 0x1EU;
        return shifted ? upper[i] : plain[i];
    }
    switch (usage) {
        case 0x28U: return '\n'; case 0x29U: return 0x1B;
        case 0x2AU: return '\b'; case 0x2BU: return '\t';
        case 0x2CU: return ' ';
        case 0x2DU: return shifted ? '_' : '-';
        case 0x2EU: return shifted ? '+' : '=';
        case 0x2FU: return shifted ? '{' : '[';
        case 0x30U: return shifted ? '}' : ']';
        case 0x31U: return shifted ? '|' : '\\';
        case 0x33U: return shifted ? ':' : ';';
        case 0x34U: return shifted ? '"' : '\'';
        case 0x35U: return shifted ? '~' : '`';
        case 0x36U: return shifted ? '<' : ',';
        case 0x37U: return shifted ? '>' : '.';
        case 0x38U: return shifted ? '?' : '/';
        default: return 0;
    }
}

void kbd_inject_hid_usage(uint16_t usage, bool pressed) {
    uint8_t key;
    char translated = 0;
    if (usage == 0U || usage > 0xE7U) return;
    switch (usage) {
        case 0xE0U: case 0xE4U: mods.ctrl = pressed; break;
        case 0xE1U: case 0xE5U: mods.shift = pressed; break;
        case 0xE2U: case 0xE6U: mods.alt = pressed; break;
        case 0x39U:
            if (pressed) mods.caps_lock = !mods.caps_lock;
            break;
        case 0x53U:
            if (pressed) mods.num_lock = !mods.num_lock;
            break;
        default: break;
    }
    key = kbd_hid_usage_to_key(usage);
    if (pressed) {
        translated = kbd_hid_usage_to_char(usage);
        if (translated) key = (uint8_t)translated;
    }
    if (key != KEY_NONE) evt_push(key, pressed);
    if (pressed && translated) buf_push(translated);
}
