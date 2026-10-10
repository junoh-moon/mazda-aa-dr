#ifndef MX5_SENSORS_VIM_CHANNELS_H
#define MX5_SENSORS_VIM_CHANNELS_H
// Field parsers for the VIM side channel (validation/VIM_CHANNEL_CAPTURE_
// 2026-10-10.md). LOGGING ONLY: raw integers, no scale, no sign, no zero.
// Offsets are relative to the VIMC callback data (the SPI payload, byte 0 is
// the VIP's sub-type byte), from the static analysis of the VIP image
// VIP_APP-MAZ150_10.13.012 (the SPI builders of 0x116, 0x169 and 0x15B):
//   0x116 (10 bytes): +1..+2 yaw sum (LE, motion path), +3 count (motion
//     path), +4 Qf of CAN 0x078 byte 0 (bits 7-6), +5..+6 longitudinal
//     acceleration (13 bits of CAN 0x078 bytes 0-1, LE u16), +7..+8 brake
//     pressure (13 bits of CAN 0x078 bytes 3-4, LE u16), +9 Qf of CAN 0x078
//     byte 5 (bits 7-6);
//   0x169 (4 bytes): +1 Qf of CAN 0x079 byte 0, +2..+3 lateral acceleration
//     (13 bits of CAN 0x079 bytes 0-1, LE u16);
//   0x15B (12 bytes): +1..+2 vehicle speed (CAN 0x202 bytes 2-3, LE u16),
//     +3..+4 engine rpm (13 bits of CAN 0x202 bytes 0-1, LE u16), +5..+6 and
//     +7 further CAN 0x202 fields (not logged), +11 CAN 0x202 byte 1 bits 1-0
//     (the VIP uses the speed only when this is 3).
// Header only; worker thread (never the tap).
#include <stdint.h>
#include <stddef.h>

namespace mx5 { namespace sensors {

enum ChannelParse { CHANNEL_PARSED=0, CHANNEL_SHORT, CHANNEL_ODD };
struct Vim116Extra { unsigned qf_a,qf_b,accel_long,brake; };
struct Vim169 { unsigned qf,accel_lat; };
struct Vim15b { unsigned speed,rpm; int status; };

inline unsigned channel_le16(const unsigned char* p) { return unsigned(p[0]) | unsigned(p[1])<<8; }
// CHANNEL_ODD: the fields are present but outside what the VIP builder can
// produce (Qf > 3 or a value wider than 13 bits); the values are still
// filled in for diagnostics but must not enter the statistics.
inline ChannelParse parse_vim116_extra(const unsigned char* d,size_t n,Vim116Extra* out) {
    if(!d || !out || n<10 || n>16)return CHANNEL_SHORT;
    out->qf_a=d[4];out->accel_long=channel_le16(d+5);out->brake=channel_le16(d+7);out->qf_b=d[9];
    return out->qf_a>3 || out->qf_b>3 || out->accel_long>0x1fff || out->brake>0x1fff?CHANNEL_ODD:CHANNEL_PARSED;
}
inline ChannelParse parse_vim169(const unsigned char* d,size_t n,Vim169* out) {
    if(!d || !out || n<4 || n>16)return CHANNEL_SHORT;
    out->qf=d[1];out->accel_lat=channel_le16(d+2);
    return out->qf>3 || out->accel_lat>0x1fff?CHANNEL_ODD:CHANNEL_PARSED;
}
// The speed/rpm need 5 bytes; status is -1 without byte 11.
inline ChannelParse parse_vim15b(const unsigned char* d,size_t n,Vim15b* out) {
    if(!d || !out || n<5 || n>16)return CHANNEL_SHORT;
    out->speed=channel_le16(d+1);out->rpm=channel_le16(d+3);out->status=n>=12?int(d[11]):-1;
    return out->rpm>0x1fff || out->status>3?CHANNEL_ODD:CHANNEL_PARSED;
}

} }
#endif
