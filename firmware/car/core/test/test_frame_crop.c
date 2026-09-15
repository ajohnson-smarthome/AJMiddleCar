#include "../main/frame_crop.h"
#include "contract.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* The packing: 4:2:0 as O_UYY_E_VYY, three bytes per two pixels in every row. */
    assert(frame_crop_row_bytes(1280) == 1920);
    assert(frame_crop_bytes(1280, 720) == 1280u * 720u * 3u / 2u);

    /* The contract's own numbers: the wire picture is the middle 720 of 960 rows. The
       offset lands on an even row (the U/V row pairing survives), and on a cache line
       (the encoder's DMA reads from the offset, not from the buffer's start). */
    size_t off = frame_crop_offset(VIDEO_WIDTH, VIDEO_SENSOR_HEIGHT, VIDEO_HEIGHT);
    assert(off == 120u * 1920u);
    assert(off % frame_crop_row_bytes(VIDEO_WIDTH) == 0);
    assert((off / frame_crop_row_bytes(VIDEO_WIDTH)) % 2 == 0);
    assert(off % 64 == 0);
    assert(off + frame_crop_bytes(VIDEO_WIDTH, VIDEO_HEIGHT) <= frame_crop_bytes(VIDEO_WIDTH, VIDEO_SENSOR_HEIGHT));

    /* No crop is offset zero, not a special case. */
    assert(frame_crop_offset(1280, 960, 960) == 0);

    puts("frame_crop: ok");
    return 0;
}
