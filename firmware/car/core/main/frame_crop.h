#ifndef FRAME_CROP_H
#define FRAME_CROP_H

#include <stddef.h>

// Pure: where the encoded picture sits inside the sensor's frame.
//
// The sensor delivers VIDEO_SENSOR_HEIGHT rows and the wire carries VIDEO_HEIGHT of them —
// the middle ones. The HUD shows a 16:9 window onto the fisheye's 4:3 frame, so the rows
// outside it were being encoded, paced onto the wire and then cropped by the viewer: a
// quarter of every keyframe spent on pixels nobody saw. Cropping at the encoder's input
// costs nothing — the ISP's O_UYY_E_VYY packing keeps every row contiguous at three bytes
// per two pixels, so a vertical crop is a pointer offset, no copy and no PPA.
//
// Two things the offset must respect, both checked by test_frame_crop.c against the
// contract's numbers: rows come in U/V pairs (odd rows carry U, even rows V), so the crop
// starts on an even row or every colour would swap; and the encoder reads the frame by
// DMA from wherever it is pointed, so the offset lands on a cache line.

static inline size_t frame_crop_row_bytes(unsigned width) { return (size_t)width * 3 / 2; }

static inline size_t frame_crop_bytes(unsigned width, unsigned height) {
    return frame_crop_row_bytes(width) * height;
}

static inline size_t frame_crop_offset(unsigned width, unsigned sensor_height, unsigned height) {
    unsigned top = (sensor_height - height) / 2;
    top &= ~1u;   /* an even row: the U/V pairing survives the crop */
    return frame_crop_row_bytes(width) * top;
}

#endif
