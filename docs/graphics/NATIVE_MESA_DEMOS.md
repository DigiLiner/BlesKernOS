# Mesa demos and native Mesa 3.5

The port builds the original `gears`, `gloss` and `morph3d` scenes from
[Mesa demos 9.0.0](https://archive.mesa3d.org/demos/mesa-demos-9.0.0.tar.xz).
The archive is pinned by SHA256 in `ports/mesa-demos/build.py`. Scene sources
are unchanged. The native adapter replaces the GLUT window/context/event loop.
GLU quadrics, projection and mipmaps use this repository's actual Mesa GLU
sources. The SGI/Mark Kilgard teapot geometry retains its license in
`ports/mesa-demos/vendor/glut_teapot.c` and the installed package.

Build: `make mesa-demos-native`. Tests: `make mesa-demos-native-test`.
Installed launchers: `MDGEARS.BEX`, `MDGLOSS.BEX`, `MDMORPH3D.BEX`.
Default arguments: `--backend gallium --size 640x480`.
Arguments can instead be passed directly to the corresponding `.ELF` program:

```
--backend mesa35 --size 640x480 --seconds 10
--backend software --size 640x480 --seconds 5
```

Backends:

| Option | Rendering | Presentation |
| --- | --- | --- |
| gallium | Mesa 24.3.4 VirGL; shader pipeline on host GPU | GPU snapshots / Vision |
| mesa35 | Mesa 3.5 CPU TNL; strict GPU rasterization | GPU snapshots / Vision on VirGL; GPU viewport on SVGA |
| auto | Mesa 3.5 hardware subset with reported software fallback | coherent GPU target when supported |
| software | Mesa 3.5 software rasterization, explicit comparison | CPU image / native window |

Strict mode rejects unsupported state; it does not silently substitute software.
On devices without the GPU window-surface capability, a hardware raster target
must still be resolved to RAM for presentation. This is reported and prevents
passing the no-readback acceptance criterion used for VirGL. Physical ATI/Intel
legacy performance requires their own hardware tests.

Mesa 3.5 has no GLSL API. Transformations, lighting, clipping and triangle setup
remain on CPU in its legacy backend. GLSL source compilation also belongs on
CPU in the modern backend; compiled shader programs execute on GPU. Applications
can select Gallium to move supported legacy fixed-function work to GPU shaders.
A Gallium runtime existing on disk cannot add programmable hardware to old cards.

Controls retain the original scene callbacks. Escape closes; gears uses arrow
keys; morph3d uses 1–5, Space and Return. Gloss supports mouse dragging and its
right-click menu (click an entry or use its number; Escape dismisses). The
adapter supports one fixed-size native window per process. Unsupported GLUT
API is a link error, and unsupported fullscreen requests fail explicitly.

For deterministic comparisons, `--fixed-time --snapshot /SYSTEM/USER/test.ppm`
renders three frames at a fixed GLUT clock and writes an actual GL readback.
Snapshot readbacks are outside the measured benchmark interval. The acceptance
runner measures each GPU scene for 20 seconds, saves screenshots outside that
interval, compares strict-GPU readbacks to software, checks that all
applications close (including clicks on the native close button), requires no
strict-mode fallback or presentation readback,
and applies a 30 FPS floor to the measured GPU runs at 640 × 480.

Native presentation defaults to one capture ahead of Vision for interactive
clients, including both Mesa demo backends. Gallium clients can explicitly
request `BKG_PRESENT_AHEAD=3` before creating a context, allowing GPU draws to
overlap host fence polling. Glmark2 requests this throughput mode itself.
Four GPU surfaces protect outstanding captures/readers; a completed
capture remains owned by the consumer until it has been copied. Benchmark
FPS therefore include presentation backpressure and should not be compared
directly with earlier uncapped submission scores.

For desktop submission timing, run the acceptance harness in a new output
directory with `--pacing-trace`. It buffers interval histograms until each
window closes and records p50/p95/p99, the maximum and its interval index,
and counts above 50/100 ms. These timestamps describe successful compositor
submissions at PIT resolution (3.33 ms in the tested VM), not physical monitor
refreshes. Failed/no-op repaint attempts are excluded. Startup intervals are
included. The pacing gate additionally requires p95 <= 50 ms, p99 <= 83.334 ms
and fewer than 1% of intervals above 100 ms in all six GPU runs. This is a
minimum gate; it does not certify constant 60 Hz.

```sh
python3 tests/graphics/run_native_gallium.py \
  --base build/bleskernos-native-3d.img --mesa-demos --pacing-trace \
  --out build/graphics-pacing/recheck --seconds 400
```

`--pacing-only` skips software/image references for diagnosis and cannot
approve an installation. `--driver-pacing-trace` additionally logs long
virtqueue waits. Both trace build options have configuration stamps so a
subsequent ordinary build removes their instrumentation correctly.

Mesa 3.5 changes preserve cached textures across drawable rebinding, evict them
only when a new target allocation requires it, upload software-dirty color before
GPU publication, reject failed flushes, select back-face TNL colors for GPU rasterization, normalize polygon-offset
units to GPU depth range, and preserve pixel-center coordinates. Exact ONE/ONE
additive blend is gated by a separate VirGL capability. Explicit GL readback
resolves without marking a rendering fallback, and large pixel-packing arrays
use heap storage within native stack limits. Header changes invalidate the
legacy build objects.
The public ELF/POSIX graphics client and device protocol are reusable by other
native applications; the Mesa demos port does not start Wine or X11.

The package also updates `/SYSTEM/NGS3D.LBS` for existing freestanding native
clients. The validation fixture launches the existing `LBSTRI.BEX` example
against that module after the demo suite. Gallium's public client uses the
ELF32/POSIX ABI; existing LBS clients continue to use the legacy Mesa backend.
The legacy ELF build enables SSE and conservative optimization of CPU geometry
and lighting, retaining IEEE comparisons and avoiding fast-math assumptions.

The legacy ELF geometry build uses `-mfpmath=sse` with SSE2 disabled, matching
its Pentium III baseline. The mixed SSE/x87 register allocation option is
excluded; GCC documents its performance as unstable in the
[x86 options manual](https://gcc.gnu.org/onlinedocs/gcc-13.4.0/gcc/x86-Options.html).
Compile argument stamps also invalidate affected objects when flags change.
