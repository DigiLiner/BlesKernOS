/* BlesKernOS KDrive host integration. Copyright (c) 2026 BlesKernOS.
 * Distributed under GPL-3.0-or-later with the TinyCore TinyX port. */
#ifdef BLESK
#ifdef HAVE_CONFIG_H
#include <kdrive-config.h>
#endif
#include "fbdev.h"
#include "blesgfx.h"
#include "kkeymap.h"
#include <X11/keysym.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include "bles_present.h"

/* Values exported by BlesKernOS kernel/include/keyboard.h.  Keep the X
 * keycodes stable: printable characters retain their original positions. */
#define BLES_KEY_F1       0x80
#define BLES_KEY_UP       0x90
#define BLES_KEY_SHIFT    0x98
#define BLES_KEY_CTRL     0x99
#define BLES_KEY_ALT      0x9a
#define BLES_KEY_INSERT   0x9b

static int blesInputType;
static int blesInputFd = -1;
static void BlesPoll(void);
static void BlesReadInput(int fd, void *closure)
{
    (void)fd; (void)closure;
    BlesPoll();
}

static int BlesInit(void) { return 1; }
static void BlesEnable(void) { }
static Bool BlesSpecialKey(KeySym key) { (void)key; return FALSE; }
static void BlesDisable(void) { }
static void BlesFini(void)
{
    if (blesInputFd >= 0) KdUnregisterFds(blesInputType, FALSE);
    blesInputFd = -1;
    blesInputType = 0;
    BlesFrameFini();
}

/* The GUI supplies translated characters, while X11 and Wine expect a
 * stable keycode for each physical key plus modifier state. */
static unsigned char BlesBaseKey(unsigned char key)
{
    if (key == 10) return 13;
    if (key >= 'A' && key <= 'Z') return key + ('a' - 'A');
    switch (key) {
    case '!': return '1'; case '@': return '2'; case '#': return '3';
    case '$': return '4'; case '%': return '5'; case '^': return '6';
    case '&': return '7'; case '*': return '8'; case '(': return '9';
    case ')': return '0'; case '_': return '-'; case '+': return '=';
    case '{': return '['; case '}': return ']'; case '|': return '\\';
    case ':': return ';'; case '"': return '\''; case '<': return ',';
    case '>': return '.'; case '?': return '/'; case '~': return '`';
    default: return key;
    }
}

static KeySym BlesShiftedKey(unsigned char key)
{
    if (key >= 'a' && key <= 'z') return key - ('a' - 'A');
    switch (key) {
    case '1': return '!'; case '2': return '@'; case '3': return '#';
    case '4': return '$'; case '5': return '%'; case '6': return '^';
    case '7': return '&'; case '8': return '*'; case '9': return '(';
    case '0': return ')'; case '-': return '_'; case '=': return '+';
    case '[': return '{'; case ']': return '}'; case '\\': return '|';
    case ';': return ':'; case '\'': return '"'; case ',': return '<';
    case '.': return '>'; case '/': return '?'; case '`': return '~';
    default: return key;
    }
}

static void BlesPoll(void)
{
    KdCardInfo *card = kdCardInfo;
    FbdevPriv *priv = card ? card->driver : NULL;
    if (!priv || priv->fd < 0) return;
    if (blesInputFd != priv->fd) {
        if (blesInputFd >= 0) KdUnregisterFds(blesInputType, FALSE);
        if (!blesInputType) blesInputType = KdAllocInputType();
        blesInputFd = KdRegisterFd(blesInputType, priv->fd,
                                  BlesReadInput, NULL) ? priv->fd : -1;
    }
    bles_gfx_event_t event;
    for (unsigned i = 0; i < 32; ++i) {
        int result = syscall(SYS_ioctl, priv->fd, BLES_GFX_EVENT, &event);
        if (result <= 0) break;
        if (event.type == BLES_GFX_MOUSE_MOVE ||
            event.type == BLES_GFX_MOUSE_DOWN ||
            event.type == BLES_GFX_MOUSE_UP) {
            if (kdMouseInfo) KdEnqueueMouseEvent(kdMouseInfo,
                event.buttons & (KD_BUTTON_1 | KD_BUTTON_2 | KD_BUTTON_3),
                event.x, event.y);
        } else if (event.type == BLES_GFX_KEY ||
                   event.type == BLES_GFX_KEY_UP) {
            KdEnqueueKeyboardEvent((unsigned char)(BlesBaseKey(event.key) + 80),
                                   event.type == BLES_GFX_KEY_UP);
        }
    }
    static CARD32 last_present;
    CARD32 now = GetTimeInMillis();
    if (now - last_present >= 16) {
        /* libc compares idle images without the serial FNV dependency on
         * every pixel. Exact comparison retains direct framebuffer writes. */
        size_t bytes = (size_t)priv->var.xres * priv->var.yres * 4U;
        Bool changed = BlesFrameChanged(priv->fb, bytes);
        if (changed || syscall(SYS_ioctl, priv->fd, BLES_GFX_NEEDS_PRESENT, 0) > 0) {
            const void *pixels = BlesFramePrepare(priv->fb, bytes);
            bles_gfx_present_t present = {
                (uint32_t)(uintptr_t)pixels,
                priv->var.xres, priv->var.yres
            };
            if (syscall(SYS_ioctl, priv->fd, BLES_GFX_PRESENT, &present) >= 0) {
                BlesFramePublished(pixels, bytes);
            }
        }
        last_present = now;
    }
}

static const KdOsFuncs BlesFuncs = {
    BlesInit, BlesEnable, BlesSpecialKey,
    BlesDisable, BlesFini, BlesPoll
};
void OsVendorInit(void) { KdOsInit(&BlesFuncs); }

static Bool BlesMouseInit(void) { return TRUE; }
static void BlesMouseFini(void) { }
const KdMouseFuncs BlesMouseFuncs = { BlesMouseInit, BlesMouseFini };

static void BlesKeyboardLoad(void)
{
    memset(kdKeymap, 0, sizeof(KeySym) * KD_MAX_LENGTH * KD_MAX_WIDTH);
    kdMinScanCode = 80;
    kdMaxScanCode = 80 + BLES_KEY_INSERT;
    for (int ch = 0; ch < 128; ++ch) {
        if (BlesBaseKey(ch) != ch) continue;
        KeySym symbol = (KeySym)ch;
        if (ch == 8) symbol = XK_BackSpace;
        else if (ch == 9) symbol = XK_Tab;
        else if (ch == 10 || ch == 13) symbol = XK_Return;
        else if (ch == 27) symbol = XK_Escape;
        else if (ch == 127) symbol = XK_Delete;
        if (ch >= 32 || symbol != (KeySym)ch) {
            kdKeymap[ch * KD_MAX_WIDTH] = symbol;
            kdKeymap[ch * KD_MAX_WIDTH + 1] =
                ch >= 32 && ch < 127 ? BlesShiftedKey(ch) : symbol;
        }
    }
    for (int ch = BLES_KEY_F1; ch <= BLES_KEY_F1 + 11; ++ch)
        kdKeymap[ch * KD_MAX_WIDTH] = XK_F1 + ch - BLES_KEY_F1;
    static const KeySym navigation[] = {
        XK_Up, XK_Down, XK_Left, XK_Right,
        XK_Home, XK_End, XK_Prior, XK_Next
    };
    for (unsigned i = 0; i < sizeof(navigation) / sizeof(navigation[0]); ++i)
        kdKeymap[(BLES_KEY_UP + i) * KD_MAX_WIDTH] = navigation[i];
    kdKeymap[BLES_KEY_SHIFT * KD_MAX_WIDTH] = XK_Shift_L;
    kdKeymap[BLES_KEY_CTRL * KD_MAX_WIDTH] = XK_Control_L;
    kdKeymap[BLES_KEY_ALT * KD_MAX_WIDTH] = XK_Alt_L;
    kdKeymap[BLES_KEY_INSERT * KD_MAX_WIDTH] = XK_Insert;
}
static int BlesKeyboardInit(void) { return 1; }
static void BlesKeyboardLeds(int state) { (void)state; }
static void BlesKeyboardBell(int volume,int pitch,int duration)
{ (void)volume; (void)pitch; (void)duration; }
static void BlesKeyboardFini(void) { }
const KdKeyboardFuncs BlesKeyboardFuncs = {
    BlesKeyboardLoad, BlesKeyboardInit, BlesKeyboardLeds,
    BlesKeyboardBell, BlesKeyboardFini, 0
};
#endif
