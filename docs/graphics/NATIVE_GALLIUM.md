# Native Gallium integration

The new user-space entry point is `ports/gallium/include/bleskernos_gallium.h`.
Its shared client library is `/SYSTEM/LIBS/GALLIUM/libbkgfx.so.1`; the header
is installed in `/SYSTEM/SDK/GALLIUM/INCLUDE`.
It creates an OpenGL compatibility context and presents into a native window.
It does not start Wine, TinyX, EGL, or an X server. It uses the OS-supported
ELF32/POSIX interpreter and its real TLS/C++ runtime. It is **not** a
freestanding `.LBS` plugin: the native LBS loader currently rejects PT_TLS.

The existing Mesa 24.3.4 source pin and frontend patches are reused. The build
uses the common winsys in `ports/gallium/virgl-winsys.c`; the Wine source path
is a compatibility include. It packages a separate runtime in
`/SYSTEM/LIBS/GALLIUM`, glmark2 2023.01 in
`/SYSTEM/PROGRAMS/GLMARK2.ELF`, and its original data in `/SYSTEM/SHARE/GLMARK2`.
The upstream benchmark scenes are retained. The window/context adapter is in
`ports/gallium/glmark2-bles.h`. Neither the benchmark nor the API claims that
an unavailable driver is a GPU renderer.

The native drawable is created and a blank frame is attached during context
setup, before benchmark timing begins. Each screen currently owns one native
window at a fixed size; multiple windows in one process are not implemented.
`--bkg-trace` enables the optional winsys and draw/present timing diagnostics;
performance acceptance runs leave this tracing disabled.

## Presentation and resource lifetime

`OSMesaBleskNativeMode` explicitly disables the OSMesa frontend's implicit
readback. `glReadPixels` remains an explicit GPU read. On presentation Mesa
flushes the selected front resource. The winsys submits `BLES_VIRGL_PRESENT`
through the same private descriptor that owns the resource.

The kernel does not accept a user-supplied destination handle. It allocates a
GFX3D target owned by the description. The driver verifies the source owner,
target, format, sample count, and dimensions. Draws and capture run in the
producer's context under one completion fence. The kernel rotates four owned
snapshots. After Vision queues its copy, a desktop fence is submitted
asynchronously by the same exact RGBA GPU blit into Vision's viewport. This
avoids clearing and drawing a textured quad just to copy a frame. A snapshot
cannot be overwritten until its reader fence has
completed. The compositor receives
the copied image through the established native viewport publication API.
Presentation returns after enqueueing the capture. Interactive windows default
to one pending capture. Gallium clients can explicitly choose throughput with
`BKG_PRESENT_AHEAD=3` before context creation; glmark2 does so itself. This sets
`BLES_GFX_OPEN_PIPELINED` at window creation. At most three captures may await
consumption, leaving pool capacity for
desktop readers. Producer completion alone never permits recycling an unread
capture. Vision polls actual GPU
completion and consumes the newest ready image; completed older captures may
be dropped. The producer receives backpressure when every snapshot is in use.
It never waits synchronously for Vision's frame RPC. Kernel notifications wake
the server, and a pending bit preserves progress if a notification arrives
before the server arms its wait.

The consumer API is private to Vision. It pins the description during each
kernel transaction, including scheduler safe points, so last close cannot
destroy an image in use by the consumer. No lease escapes into Ring 3. Last
close clears the viewport and destroys resources after pending GPU work.
Only verified desktop fences retire desktop VBOs; raw-context completion is
tracked separately. Asynchronous fence packets remain owned by the driver
until the matching used-ring descriptor is returned.

This path copies on GPU to preserve a stable published frame. It avoids a
GPU-to-CPU-to-GPU image transfer; it does not claim that no GPU copies occur.

## Drivers

VirGL has the existing programmable transport and is the first implementation.
The native client interface is independent of a window-system API, but adding
another hardware driver still requires its Gallium winsys and kernel contract.

The current VMware GFX3D driver accepts a fixed-function POSITIONT subset.
Its public transport exposes no shader/context/surface management API suitable
for the upstream Gallium `svga` winsys. Upstream's winsys requires resource and
buffer allocation/mapping, relocations, context command queues, caps and fences;
the present `draw_triangles` callback cannot substitute for it. Merely enabling
`-Dgallium-drivers=svga` would not create a working port. VMware therefore keeps
Mesa 3.5 until that transport is implemented and verified on VMware itself.
QEMU's VMware VGA emulation is not a substitute for a real SVGA3D host.

## Build and validation

```
make gallium-native
python3 tests/graphics/run_native_gallium.py
```

The runner copies the disk, installs the package and current kernel/driver,
then starts the pixel probe and unmodified glmark2 scenes in a disposable
KVM/VirGL VM. It saves the command, serial, screenshots, and parsed results.
The pixel probe checks explicit reads from both colored halves; the captured
window must independently show red above blue. The default performance gate
requires 30 FPS in each selected scene at 640×480.
`make gallium-native-test` builds and runs both steps.

The transport host test checks ownership, dimensions, fenced copies, transfer
bounds, async barriers and cleanup. Actual pixel/GLSL/performance evidence must
come from the guest; compilation or a host queue mock is insufficient.

To install in another disposable image after reviewing measurements:

```
python3 tools/install_native_gallium.py --base build/bleskernos-ata-user.img \
  --output build/bleskernos-native-gallium.img
```

No performance pass is inferred by this document. The measured results and
remaining integration work belong in the validation report.

Phase 1 measured acceptance is recorded in
[GALLIUM_PHASE1_VALIDATION_2026-10-21.md](GALLIUM_PHASE1_VALIDATION_2026-10-21.md).
The native Mesa 3.5 comparison port is described in
[NATIVE_MESA_DEMOS.md](NATIVE_MESA_DEMOS.md).
When the package includes Mesa demos, the installer additionally requires a
passing `--mesa-validation` report for the same package and core hashes.
