/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exact comparison also observes direct/shared framebuffer writes which do
 * not pass through X Damage. Freeze uploads before ioctl; commit the reference
 * only after successful publication. */
#include <stdlib.h>
#include <string.h>

static void *blesFramePixels;
static size_t blesFrameSize;
static int blesFrameValid;

static void BlesFrameFini(void)
{
    free(blesFramePixels);
    blesFramePixels = NULL;
    blesFrameSize = 0;
    blesFrameValid = 0;
}

static int BlesFrameChanged(const void *pixels, size_t bytes)
{
    if (!bytes) return 0;
    if (bytes != blesFrameSize) {
        void *snapshot;
        blesFrameValid = 0;
        snapshot = realloc(blesFramePixels, bytes);
        if (!snapshot) return 1; /* OOM: publish conservatively. */
        blesFramePixels = snapshot;
        blesFrameSize = bytes;
    }
    return !blesFrameValid || memcmp(pixels, blesFramePixels, bytes) != 0;
}

static const void *BlesFramePrepare(const void *pixels, size_t bytes)
{
    blesFrameValid = 0;
    if (blesFramePixels && bytes == blesFrameSize) {
        memcpy(blesFramePixels, pixels, bytes);
        return blesFramePixels;
    }
    return pixels;
}

static void BlesFramePublished(const void *pixels, size_t bytes)
{
    /* The upload and the reference must describe the same frozen image.
     * A shared-memory writer may change the live buffer while ioctl sleeps. */
    if (blesFramePixels && pixels == blesFramePixels && bytes == blesFrameSize)
        blesFrameValid = 1;
}
