#include "../include/startup_sound.h"
#include "../include/sound.h"
#include "../include/system_sounds.h"
#include "../include/task.h"
#include "../include/vga.h"
#include "../libc/include/string.h"

bool startup_sound_enabled(void) {
    return system_sounds_startup_enabled();
}

bool startup_sound_set_enabled(bool enabled) {
    return system_sounds_set_startup_enabled(enabled);
}

static void startup_sound_worker(void *argument UNUSED) {
    /*
     * La lectura de SOUND.INI y del WAV no forma parte de la ruta crítica
     * de entrada al escritorio. En WHPX una espera de E/S o un cambio de
     * tarea justo después de apagar el splash podía dejar la pantalla en
     * READY aunque el kernel ya hubiese terminado de iniciar la GUI.
     */
    if (!startup_sound_enabled()) {
        SOUND_LOG("[SOUND] Sonido de inicio desactivado\n");
        task_exit();
    }

    if (!sound_play_file(BK_STARTUP_SOUND_PATH)) {
        SOUND_LOG("[SOUND] Sonido de inicio no disponible\n");
    } else {
        /* sound_play_file() crea wav-player. Entregar CPU explícitamente evita
         * que en un PIII el lector FAT quede esperando hasta que otra E/S
         * incidental fuerce un cambio de tarea. */
        task_yield();
    }
    task_exit();
}

void startup_sound_play(void) {
    /*
     * No toque FAT ni cree tareas si el único backend es PC speaker.
     * sound_play_file() ya comprueba esto, pero lo hacía después de leer
     * SOUND.INI. Esa lectura síncrona era exactamente el último trabajo
     * ejecutado entre "DISABLE SPLASH" y gui_run().
     */
    if (!sound_pcm_available()) {
        SOUND_LOG("[SOUND] Sin PCM; se omite el sonido de inicio\n");
        return;
    }

    /* El audio de inicio es opcional: nunca debe bloquear la GUI. */
    if (task_create_kernel("startup-sound", startup_sound_worker, NULL) < 0) {
        SOUND_LOG("[SOUND] No se pudo crear la tarea de inicio\n");
        return;
    }

    /* La VM es tan rápida que el primer tick ocultaba esta carrera. En
     * silicio real, dar un único turno aquí inicia SOUND.INI/WAV de inmediato
     * sin convertir la lectura del archivo en trabajo síncrono de la GUI. */
    task_yield();
}
