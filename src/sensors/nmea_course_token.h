#ifndef MX5_NMEA_COURSE_TOKEN_H
#define MX5_NMEA_COURSE_TOKEN_H

#include <stddef.h>
#include <stdint.h>

namespace mx5 { namespace sensors { namespace nmea_course_token {

// Lexical presence only. PRESENT says nothing about numeric validity, receiver
// quality, measurement time, freshness or whether the original parser accepted
// the sentence. Cache metadata needs a separate actual callback/commit binding.
enum Presence : uint32_t { UNKNOWN=0, EMPTY=1, PRESENT=2 };
// Exact lexical second RMC data token, not quality or a measurement status.
enum RmcStatus : uint32_t {
    RMC_UNKNOWN=0, RMC_EMPTY=1, RMC_A=2, RMC_V=3, RMC_OTHER=4
};
struct Tokens { Presence course; RmcStatus status; };
enum { SCAN_LIMIT=256 }; // Includes the terminating NUL.

// input must expose accessible readable bytes. Inspect at most SCAN_LIMIT and
// stop at the first NUL. Accept one $??RMC,...*HH sentence (uppercase ASCII
// talker), optionally followed by CR LF. The checksum's syntax, not its value,
// is checked. Course is data field eight and may end at comma or '*'.
// No allocation, I/O, locks, waits, numeric parsing or input mutation.
Presence classify(const char* input, size_t accessible) noexcept;
// Same complete-frame/buffer contract. Reached second/eighth tokens classify
// independently; missing course does not erase a reached lexical status.
Tokens classify_rmc(const char* input, size_t accessible) noexcept;

} } }
#endif
