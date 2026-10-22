# Native Gallium phase 1: accepted

Evidence: `build/native-gallium/validation2/result.json`, serial log, screenshots,
GPU activity samples and installed-byte SHA256 hashes in the same directory.
The accepted bootable copy is `build/bleskernos-gallium-phase1.img`.
The original user disk was not overwritten.

QEMU 8.2.2, KVM, one host CPU, 768 MiB guest RAM, VirtIO VGA GL / SDL.
Host: Celeron N4020, Intel UHD Graphics 600 (GLK 2).
Guest: Mesa 24.3.4 VirGL, OpenGL 4.3 compatibility.
Original glmark2 2023.01 scene code, 640 × 480:

| Scene | Duration | FPS |
| --- | ---: | ---: |
| build | 10 s | 235 |
| shading / phong | 10 s | 177 |
| jellyfish | 15 s | 54 |
| refract | 30 s | 44 |
| bump / normals | 10 s | 347 |

Score: 170. Every selected scene exceeds the declared 30 FPS threshold.
The native swap path reported zero presentation readbacks. GPU direct desktop
scanout was observed, with no CPU scanout fallback. This is a result for this
VM and host, not a performance promise for physical ATI or Intel legacy cards.

Independent GL readback and window screenshot checks verify red above blue.
Shader, FBO, depth, stencil, sharing and asynchronous VBO checks passed. After
glmark2 exited, the fixture reopened the pixel probe, rechecked the image,
waited for every native application to exit and confirmed desktop progress.
The no-3D VirtIO negative control (`build/native-gallium/no-3d/result.json`)
rejected context creation cleanly, with no hidden software renderer or panic.

The reusable application interface is ELF32/POSIX through libbkgfx, independent
of Wine and X11. It is not a freestanding LBS implementation. Gallium's VirGL
winsys is shared with the existing Wine integration. GLSL compilation runs on
CPU; the compiled shader executes on the host GPU through VirGL. Legacy fixed
function calls on this modern backend are implemented by GPU shader programs.

SVGA retains Mesa 3.5. Its current kernel POSITIONT interface does not expose
the contexts, shaders, resources, command queues and fences required by the
Gallium svga winsys. A real VMware SVGA3D transport and host validation remain
separate work; QEMU VMware VGA cannot validate that hardware path.
