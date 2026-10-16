# Drivers de audio

Los cuatro motores PCI nuevos tienen código de hardware independiente:

| Fuente | Dispositivo | Módulo |
|---|---|---|
| `es1370.c` | Ensoniq AudioPCI ES1370 | `ES1370.DVR` |
| `cmipci.c` | C-Media CMI8338/8738 | `CMIPCI.DVR` |
| `via686.c` | VIA VT82C686 | `VIA686.DVR` |
| `cs4281.c` | Cirrus CS4281 | `CS4281.DVR` |

Cada `.c` contiene `identify()`, inicialización de codec, parada, programación
DMA y selección de BAR. Para ajustar mixer/registros/compatibilidad de un chip,
edita ese archivo. ES1370 no implica ES1371, ni VIA686 implica VIA8233.

`legacy_pcm.h` comparte contexto privado, tamaños DMA y acceso a registros.
`legacy_pcm_runtime.inc` comparte conversión PCM, remuestreo, tonos, bloqueos,
finalización y ciclo de vida del módulo. Se incluye al final del `.c` para que
cada DVR tenga su propia memoria y callbacks. No usa `LEGACY_AUDIO_KIND`.

Los drivers anteriores `ac97.c`, `maestro3.c` y `sound.c`/SB16 conservan su
organización. La separación no cambia frecuencias, límites de clips ni los IDs
admitidos; esas restricciones están en `docs/platform/legacy-drivers.md`.

```
make build/system/drivers/ES1370.DVR
make legacy-drivers
python3 tests/drivers/test_legacy_engines.py
```
