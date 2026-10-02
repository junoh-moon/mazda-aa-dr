#ifndef MX5_RUNTIME_LDS_ASSOCIATION_PROTOCOL_H
#define MX5_RUNTIME_LDS_ASSOCIATION_PROTOCOL_H
#include <atomic>
#include <stdint.h>

namespace mx5 { namespace runtime { namespace lds_association { namespace protocol {
// Every shared location, including payload, is an aligned lock-free32 word.
// The reader never stores/CASes in its PROT_READ mapping. A stable even sequence
// plus an unchanged loss epoch bounds the copy; it is not a physical lease.
enum { VERSION=3,CAPACITY=64,MAP_BYTES=45056,WORDS=MAP_BYTES/4,HEADER=64,
       RECORD_WORDS=160,OFFER_BYTES=32,READ_ATTEMPTS=2,DRAIN_LIMIT=2,MAX_RIGHTS=253 };
enum Header { SEQUENCE=0,MAGIC=1,LAYOUT=2,SIZE=3,SLOTS=4,ENABLED=5,LOSS=6,
              INSTANCE=8,PID=10,LAST_RECORD=11,FLOOR=12,COUNT=14,CREATED=16 };
enum Record { STATE=0,RECORD_SEQUENCE=1,OBSERVED=2,RECORD_LOSS=4,REPLY_TYPE=5,
              REQUEST_SERIAL=6,RESPONSE_SERIAL=7,REPLY_SERIAL=8,
              GUID=9,CLIENT=25,SERVER=41,DESTINATION=57,LIFETIME=73,WRITE=75,
              ORIGINS=77,POSITION=113,HEADING_PRESENCE=129,HEADING_RMC_STATUS=130 };
enum { EMPTY=0,LOCKED=1,CONTRADICTORY=2 };
static const uint32_t MAP_MAGIC=0x4d58414dU,OFFER_MAGIC=0x4d584146U;
static const uint32_t CLOSED=0x80000000U;
struct Map { std::atomic<uint32_t> words[WORDS]; };
static_assert(ATOMIC_INT_LOCK_FREE==2,"association requires lock-free32 atomics");
static_assert(sizeof(std::atomic<uint32_t>)==4,"fixed atomic word layout");
static_assert(sizeof(Map)==MAP_BYTES,"fixed mapping size");
static_assert(HEADER+CAPACITY*RECORD_WORDS<=WORDS,"bounded shared records");
} } } }
#endif
