#ifndef IDENTITY_H
#define IDENTITY_H

// Which car this is — as opposed to board.h, which is about which board it runs on.
//
// Both cars in this family are a softAP serving the same API at 192.168.4.1. A distinct
// SSID is necessary but not sufficient: join the wrong network and a pult finds a car
// exactly where it expects one. CAR_DEVICE_ID is what lets it refuse.
#define CAR_DEVICE_ID  "ajmiddlecar"
#define CAR_AP_SSID    "AJMiddleCar"
#define CAR_AP_PASS    "drive1234"   // >= 8 chars for WPA2; "" for open

#endif // IDENTITY_H
