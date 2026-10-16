#ifndef BK_SYSTEM_SOUNDS_H
#define BK_SYSTEM_SOUNDS_H

#include "types.h"

#define BK_SYSTEM_SOUND_OPEN_PATH      "/SYSTEM/SOUNDS/WINOPEN.WAV"
#define BK_SYSTEM_SOUND_CLOSE_PATH     "/SYSTEM/SOUNDS/WINCLOSE.WAV"
#define BK_SYSTEM_SOUND_MINIMIZE_PATH  "/SYSTEM/SOUNDS/MINIMIZE.WAV"
#define BK_SYSTEM_SOUND_RESTORE_PATH   "/SYSTEM/SOUNDS/RESTORE.WAV"
#define BK_SYSTEM_SOUND_RESIZE_PATH    "/SYSTEM/SOUNDS/RESIZE.WAV"
#define BK_SYSTEM_SOUND_BUTTON_PATH    "/SYSTEM/SOUNDS/BOTTON.WAV"
#define BK_SYSTEM_SOUND_SCROLL_PATH    "/SYSTEM/SOUNDS/SCROLL.WAV"

typedef enum {
    SYSTEM_SOUND_WINDOW_OPEN = 0,
    SYSTEM_SOUND_WINDOW_CLOSE,
    SYSTEM_SOUND_WINDOW_MINIMIZE,
    SYSTEM_SOUND_WINDOW_RESTORE,
    SYSTEM_SOUND_WINDOW_RESIZE,
    SYSTEM_SOUND_BUTTON,
    SYSTEM_SOUND_SCROLL,
    SYSTEM_SOUND_EVENT_COUNT
} system_sound_event_t;

/* Carga SOUND.INI y precarga los WAV cortos en RAM. Es seguro llamarla mas
 * de una vez; tras el primer intento no vuelve a tocar FAT durante el GUI. */
void system_sounds_init(void);

bool system_sounds_enabled(void);
bool system_sounds_set_enabled(bool enabled);
uint8_t system_sounds_master_volume(void);
bool system_sounds_set_master_volume(uint8_t percent);

bool system_sounds_startup_enabled(void);
bool system_sounds_set_startup_enabled(bool enabled);

bool system_sound_event_enabled(system_sound_event_t event);
bool system_sound_event_set_enabled(system_sound_event_t event, bool enabled);
bool system_sound_event_available(system_sound_event_t event);
bool system_sound_event_play(system_sound_event_t event);

/* RESIZE.WAV es una textura de audio corta. Vision la reinyecta con solape
 * mientras el usuario mantiene un borde, dando un sonido continuo sin hacer
 * I/O de disco por cada movimiento del mouse. */
void system_sound_resize_begin(void);
void system_sound_resize_end(void);

#endif
