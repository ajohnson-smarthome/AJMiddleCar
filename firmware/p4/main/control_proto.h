#ifndef CONTROL_PROTO_H
#define CONTROL_PROTO_H

// Parse a JSON control frame {"t":<num>,"y":<num>} into throttle and yaw.
// Zero-alloc, fixed-shape scan (no JSON library) for the 10 Hz hot path: finds the
// "t"/"y" keys (order-independent) and reads the number after each. Whitespace is
// tolerated. Non-finite values (NaN/inf) are rejected. Returns 0 on success, -1 on
// malformed input or a missing key. Does NOT range-check finite values (car_drive
// clamps them). On failure, *throttle/*yaw are unchanged.
int control_parse_json(const char *msg, float *throttle, float *yaw);

#endif // CONTROL_PROTO_H
