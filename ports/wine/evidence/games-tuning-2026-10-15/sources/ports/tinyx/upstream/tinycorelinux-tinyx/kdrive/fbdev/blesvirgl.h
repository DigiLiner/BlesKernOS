/* BlesKernOS Gallium/VirGL transport. Fixed-width Linux i386 ABI. */
#ifndef BLES_VIRGL_H
#define BLES_VIRGL_H
#include <stdint.h>

#define BLES_GFX_VIRGL 0xB10CU
#define BLES_VIRGL_ABI 1U
#define BLES_VIRGL_MAX_DATA (64U * 1024U * 1024U)
#define BLES_VIRGL_MAX_COMMANDS (512U * 1024U)
#define BLES_VIRGL_FEATURE_ASYNC_SUBMIT 1U
#define BLES_VIRGL_FEATURE_ASYNC_UPLOAD 2U
enum {
    BLES_VIRGL_CAPS = 1, BLES_VIRGL_CONTEXT_CREATE, BLES_VIRGL_CONTEXT_DESTROY,
    BLES_VIRGL_RESOURCE_CREATE, BLES_VIRGL_RESOURCE_DESTROY,
    BLES_VIRGL_TRANSFER_WRITE, BLES_VIRGL_TRANSFER_READ,
    BLES_VIRGL_SUBMIT, BLES_VIRGL_WAIT, BLES_VIRGL_SUBMIT_ASYNC,
    BLES_VIRGL_TRANSFER_WRITE_ASYNC
};
/* The kernel supplies context from the open file description; callers cannot
 * select another owner's context. buffer is a userspace address at the ioctl
 * boundary and is never passed directly to the graphics driver.
 * CAPS: p[0..4] = ABI, capset ID, capset version, capset bytes, feature bits.
 * CREATE: p[0..10] = target, format, bind, width, height, depth, array_size,
 *                     last_level, samples, flags, backing bytes; returns p[0].
 * TRANSFER: p[0..11] = resource, level, stride, layer_stride, x,y,z,w,h,d,
 *                       backing offset, copied backing offset.
 * SUBMIT: buffer contains an aligned VirGL command stream.
 * SUBMIT_ASYNC: same stream, acknowledged after host decoding, before GPU
 * completion. Requires ASYNC_SUBMIT in CAPS; legacy SUBMIT remains fenced.
 * TRANSFER_WRITE_ASYNC: same payload as WRITE; requires ASYNC_UPLOAD. The
 * host has consumed the CPU source at acknowledgement; GPU work remains
 * pending. Commands and later transfers remain ordered in this context.
 * WAIT, READ and legacy WRITE complete all preceding GPU work.
 */
typedef struct {
    uint32_t abi, op, context, p[24], buffer, bytes;
} bles_virgl_t;
#endif
