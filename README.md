# BlesKernOS 0.10 

**A hobby operating system for 32-bit x86 computers, built with C, Assembly.**

BlesKernOS is an  operating system featuring its own bootloader, kernel, graphical desktop, native applications, device drivers, and compatibility layers.

Inspired by the desktop operating systems of the 1990s, the project aims to combine a classic computing experience with more advanced operating-system features.

**Status:** Alpha — actively developed. Not intended for production use.

## Screenshots

<img width="1366" alt="BlesKernOS 0.9 Alpha screenshot 1" src="https://github.com/user-attachments/assets/3a786152-6267-45a7-81fd-afa594d82fcb" />

<img width="1366" alt="BlesKernOS 0.9 Alpha screenshot 2" src="https://github.com/user-attachments/assets/b11d4967-e81b-408b-af15-44dc803e4189" />

<img width="800" height="601" alt="imagen" src="https://github.com/user-attachments/assets/2df3826f-165a-4356-bb1e-f134f7384d7e" />

<img width="1366" alt="BlesKernOS 0.9 Alpha screenshot 4" src="https://github.com/user-attachments/assets/0966cb7a-3698-4d9f-8c7a-e385f7ca6980" />

## What's new in 0.10?

Version 0.10 is currently under development, introducing major improvements to graphics acceleration, native libraries, and the desktop environment.

- **3D acceleration:** Expanded Gallium integration, VirtIO/VirGL GPU acceleration, and improvements to Mesa 3.5.
- **LBS shared libraries:** New native shared-library infrastructure, SDK interfaces, and reusable components for applications.
- **Desktop:** Improvements to the Vision compositor, window management, rendering, and screensavers.
- **Jelly Aquarium:** New experimental 3D screensaver featuring animated jellyfish.
- **Multimedia:** Reorganized native multimedia libraries and improvements to video and image rendering.
- **Kernel:** Continued improvements to memory handling, IPC, system interfaces, and graphics drivers.
- **Wine:** Further development of Wine 11 and TinyX graphics integration.
- **Testing:** Expanded automated tests, graphics diagnostics, and development tools.

This is the first development build of 0.10. Additional features, optimizations, and stability improvements are planned before the final release.

## Features

| Component | Capabilities |
|---|---|
| Architecture | 32-bit x86 |
| Kernel | Protected mode, SMP, preemptive scheduling, Ring 3, paging |
| Desktop | Window manager, taskbar, graphical applications |
| Storage | ATA/IDE, FAT12/16/32, ISO9660, USB storage |
| Graphics | VGA/VESA, VMware SVGA-II, VirtIO GPU, ATI Rage 128 |
| 3D | TinyGL, Mesa 3.5, experimental GPU acceleration |
| Networking | Ethernet, IPv4, HTTP, HTTPS/TLS 1.2 |
| Audio | Sound Blaster 16, AC'97, PC speaker, MIDI |
| Compatibility | Experimental Wine 11 port |
| Drivers | Loadable `.DVR` modules |
| Development | Native SDK, ELF32 applications, system utilities |

### Included software

BlesKernOS includes a File Browser, Text Editor, Calculator, Calendar, Paint, Image Viewer, Control Panel, Process Manager, MIDI player, system utilities, and various graphical demos.

Ports and compatibility projects include NetSurf, Doom-related support, and Wine.

### Windows applications

BlesKernOS 0.10 includes an experimental port of **Wine 11**, running on an i386 Linux-compatible ABI implemented within the operating system.

This allows selected Windows applications to run without Windows itself.

Compatibility, performance, and application startup times are still being improved.

More information: [Wine port documentation](ports/wine/README.md).

## Building and running

### Requirements

- GCC with 32-bit x86 support
- NASM and Binutils
- Python 3
- QEMU

Build the User edition:

```bash
make user
```

Run it in QEMU:

```bash
make EDITION=user run
```

Build the Developer edition:

```bash
make developer
```

### Installer

Build and launch the installer ISO:

```bash
make iso-user
make EDITION=user run-iso
```

For additional build targets and development information, see the [repository guide](docs/README.md).

## Project structure

```text
boot/       Bootloader
kernel/     Kernel and low-level subsystems
gui/        Graphics and windowing
system/     Desktop, drivers, services and utilities
programs/   Native applications
ports/      Ported third-party software
libs/       System libraries
sdk/        Native development SDK
docs/       Technical documentation
tests/      Tests and validation
```

## Current status

BlesKernOS is an **Alpha-stage research and hobby project**.

Although many features are functional, hardware compatibility varies, and several components remain experimental.

Current development priorities include kernel stability, memory protection, driver reliability, graphics performance, Wine compatibility, and broader testing on real hardware.

## Development approach

BlesKernOS is developed through an AI-assisted workflow involving architectural planning, code generation, debugging, integration, and testing.

The project combines original operating-system components with ports of existing open-source software. It is not a claim that every included line of code was written from scratch.

## License and credits

Original BlesKernOS code is distributed under the MIT License unless otherwise indicated.

Third-party software and assets retain their respective licenses. Some bundled components have unresolved provenance or redistribution restrictions and require further review.

See [NOTICE.md](NOTICE.md), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), and [LICENSES](LICENSES/README.md) for details.

The project incorporates or draws upon work from Wine, TinyGL, Mesa, BearSSL, DoomGeneric, NetSurf, and other open-source projects and technical references.

---
