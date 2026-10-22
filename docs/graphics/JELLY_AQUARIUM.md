# Jelly Aquarium — BlesKernOS 0.10 (screensaver)

Jelly's multi-medusa mode is used **only** when glmark2 starts with
`--bkg-screensaver`. It preserves the pinned glmark2 2023.01 benchmark's
original single-medusa transformation and source tree.

Configure via **Propiedades de pantalla -> Protector -> Medusa (Gallium)
-> Configurar**:

- `Cantidad de medusas`: 1 to 10, default 4.
- `Variedad de tamanos`: uniform, low, medium or high, default medium.
- Preferences persist in `/SYSTEM/USER/CONFIG/JELLY.INI` and are loaded
  by `SSJELLY.SCV` each time it starts. The window stays compatible with
  non-Jelly screensavers.

Each medusa ascends continuously. Starting elevations are staggered; each
uses an independent speed, horizontal wobble, animation phase and scale.
After moving entirely beyond the upper boundary, it reappears below the
lower boundary. All meshes share the same OpenGL program, vertex/index
buffers, textures and Gallium context. The saver remains capped at 30 FPS.

The build modifies the **pinned build copy** of `scene-jellyfish.cpp` through
`ports/gallium/jelly_aquarium.py`; `build.py` resets it from git on every
build. The glmark2 benchmark has no `BKG_JELLY_ACTIVE` environment marker,
so it continues to draw its unmodified single-medusa scene.

Test procedure: build with `make -j3 user-update` (QEMU stopped), start
BlesKernOS with VirtIO/VirGL, select 1/4/10 medusas and variety modes,
preview, ensure the medusas rise and loop, dismiss with mouse/keyboard,
restart, and ensure preferences persist. Compare performance at each count.
These visual and performance tests must be done in an accelerated guest;
source-level syntax checks alone are not proof of runtime correctness.
