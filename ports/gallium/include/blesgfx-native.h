/* SPDX-License-Identifier: MIT */
#ifndef BLES_GFX_NATIVE_H
#define BLES_GFX_NATIVE_H
/* Only the calling descriptor's native window; argument must be zero. */
#define BLES_GFX_NATIVE_FULLSCREEN 0xB111U
/* Request undecorated desktop-sized presentation atomically with GFX_OPEN.
 * Values 1,2,4,8 are the existing open flags; 16 belongs to the native saver. */
#define BLES_GFX_OPEN_SAVER 0x10U
/* Read the current task's cooperative stop request on the same GPU fd.
 * This lets native Linux ELF render loops honor bk_proc_request_exit(). */
#define BLES_GFX_NATIVE_EXIT_REQUEST 0xB112U
#endif
