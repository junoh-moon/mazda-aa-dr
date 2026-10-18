#ifndef MX5_COLLECTOR_BUS_REGISTER_H
#define MX5_COLLECTOR_BUS_REGISTER_H
// Bounded replacement of dbus_bus_register(), shared by the collector's main
// thread (service bus, position polling) and the phone IMU receiver thread
// (HMI bus). libdbus waits without any limit in the SASL handshake when the
// socket accepts but never answers (a stuck daemon; measured > 140 s,
// validation/BCA_HMI_BUS_FIX_REVIEW_FABLE_2026-10-17.md 1-A); its 25 s default
// covers only the Hello call. Here the handshake gets AUTH_NS in 50 ms steps
// and Hello gets HELLO_MS; then the unique name is stored exactly like
// dbus_bus_register does. On false the caller closes the connection (never
// half-registered) and e carries the error name.
// Policy P: bool stopping() — checked after each 50 ms step that did not
// complete the handshake (true aborts with stop_text); void idle(uint64_t now)
// — called between the steps (the receiver drains its PTY there).
// All calls exist in the CMU's libdbus 1.7.2 (dynamic symbols checked) and in
// the toolchain's 1.6.30 headers.
#include <dbus/dbus.h>
#include <stdint.h>
#include <time.h>

namespace mx5 { namespace collector {

static const uint64_t BUS_AUTH_NS = 2000000000ULL;
static const int BUS_HELLO_MS = 1000;

inline uint64_t bus_mono_ns() {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return uint64_t(t.tv_sec) * 1000000000ULL + uint64_t(t.tv_nsec);
}

template <class P>
bool bounded_bus_register(DBusConnection* conn, DBusError* e, P& policy, const char* stop_text) {
    const uint64_t deadline = bus_mono_ns() + BUS_AUTH_NS;
    while (!dbus_connection_get_is_authenticated(conn)) {
        if (!dbus_connection_read_write(conn, 50)) {
            dbus_set_error_const(e, "mx5dr.AuthFailed", "disconnected during authentication");
            return false;
        }
        if (dbus_connection_get_is_authenticated(conn)) break;
        const uint64_t t = bus_mono_ns();
        const bool stopping = policy.stopping();
        if (stopping || t >= deadline) {
            dbus_set_error_const(e, "mx5dr.AuthTimeout", stopping ? stop_text : "no SASL answer within 2000 ms");
            return false;
        }
        policy.idle(t);
    }
    DBusMessage* m = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "Hello");
    if (!m) { dbus_set_error_const(e, DBUS_ERROR_NO_MEMORY, "Hello"); return false; }
    DBusMessage* r = dbus_connection_send_with_reply_and_block(conn, m, BUS_HELLO_MS, e);  // NoReply on timeout
    dbus_message_unref(m);
    if (!r) {
        if (!dbus_error_is_set(e)) dbus_set_error_const(e, "mx5dr.HelloFailed", "no reply to Hello");
        return false;
    }
    const char* name = 0;
    bool ok = dbus_message_get_args(r, e, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) && name && name[0];
    if (ok) ok = dbus_bus_set_unique_name(conn, name);  // copies the name before r is freed
    if (!ok && !dbus_error_is_set(e)) dbus_set_error_const(e, "mx5dr.HelloFailed", "no unique name");
    dbus_message_unref(r);
    return ok;
}

} }
#endif
