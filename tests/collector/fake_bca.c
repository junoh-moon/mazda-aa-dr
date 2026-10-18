/* Host stand-in for the stock BCA D-Bus interface used by the phone IMU
 * receiver (validation/PHONE_IMU_CHANNEL_2026-10-14.md). Not OEM code: the
 * signatures come from the public com.jci.bca introspection
 * (ConnectionStatusResp(u,u,u,u,(ay)), QueryConnectStatus(u) -> (iiii(ay))).
 * Usage: fake_bca <bus-address> [--no-name | --no-object | --lds <lat> <lon>]
 *   --lds: instead of com.jci.bca, owns com.jci.lds.data and answers
 *   GetPosition with mode 2 and the given synthetic coordinates (signature
 *   i t d d i d d d d as the collector reads it; validation/V1_A_PRODUCT_HARDENING_2026-10-18.md).
 *   --no-object: owns com.jci.bca but registers no /com/jci/bca object, like
 *   the stock BCA's SERVICE-bus connection (the object and the signal exist
 *   only on its HMI-bus connection, validation/BCA_QUERY_CONNECT_STATUS_RE_2026-10-17.md):
 *   libdbus itself answers every method call with UnknownMethod; stdin
 *   commands other than quit are ignored.
 * stdin commands, one per line:
 *   state <svc> <connStatus> <status> <path|->   reply of QueryConnectStatus(svc)
 *   raw <svc> <connStatus> <status> <path|->     reply exactly these values whatever svc is asked
 *                                                 (the stock BCA answers 0,6,... for a service missing from its table)
 *   error [<name> [<message...>]]                 QueryConnectStatus replies an error (default com.jci.bca.Error "test")
 *   signal <svc> <connStatus> <btDeviceId> <status> <path|->
 *   quit
 * stdout: "ready <unique-name>", "query <svc>" per method call, "sent" per signal. */
#include <dbus/dbus.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int st_svc = -1, st_cs, st_st, reply_error, raw_reply, no_object, lds;
static double lds_lat, lds_lon;
static char st_path[64], err_name[128] = "com.jci.bca.Error", err_msg[200] = "test";

/* --no-object: print the call, then leave it unhandled so libdbus's default
 * handling replies UnknownMethod exactly as for a connection without the object. */
static DBusHandlerResult note_call(DBusConnection *c, DBusMessage *m, void *data) {
    (void)c; (void)data;
    if (dbus_message_is_method_call(m, "com.jci.bca", "QueryConnectStatus")) {
        dbus_uint32_t svc = 0;
        dbus_message_get_args(m, 0, DBUS_TYPE_UINT32, &svc, DBUS_TYPE_INVALID);
        printf("query %u\n", svc); fflush(stdout);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void append_path(DBusMessageIter *it, const char *path) {
    DBusMessageIter inner, arr;
    const char *bytes = path;
    int n = (int)strlen(path) + (*path ? 1 : 0); /* include the NUL like a C char[21] */
    dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, 0, &inner);
    dbus_message_iter_open_container(&inner, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &arr);
    dbus_message_iter_append_fixed_array(&arr, DBUS_TYPE_BYTE, &bytes, n);
    dbus_message_iter_close_container(&inner, &arr);
    dbus_message_iter_close_container(it, &inner);
}
static void append_status(DBusMessageIter *it, int a, int b, int c, int d, const char *path, int as_struct) {
    if (as_struct) { /* (iiii(ay)) */
        DBusMessageIter st;
        dbus_int32_t v[4] = {a, b, c, d};
        dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, 0, &st);
        for (int i = 0; i < 4; ++i) dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &v[i]);
        append_path(&st, path);
        dbus_message_iter_close_container(it, &st);
    } else { /* u u u u (ay) */
        dbus_uint32_t v[4] = {(dbus_uint32_t)a, (dbus_uint32_t)b, (dbus_uint32_t)c, (dbus_uint32_t)d};
        for (int i = 0; i < 4; ++i) dbus_message_iter_append_basic(it, DBUS_TYPE_UINT32, &v[i]);
        append_path(it, path);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 64;
    int named = !(argc > 2 && !strcmp(argv[2], "--no-name"));
    no_object = argc > 2 && !strcmp(argv[2], "--no-object");
    lds = argc > 4 && !strcmp(argv[2], "--lds");
    if (lds) { lds_lat = strtod(argv[3], 0); lds_lon = strtod(argv[4], 0); }
    DBusError e; dbus_error_init(&e);
    DBusConnection *c = dbus_connection_open_private(argv[1], &e);
    if (!c || !dbus_bus_register(c, &e)) { fprintf(stderr, "bus: %s\n", e.message ? e.message : "?"); return 1; }
    if (named && dbus_bus_request_name(c, lds ? "com.jci.lds.data" : "com.jci.bca", DBUS_NAME_FLAG_DO_NOT_QUEUE, &e) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) return 2;
    setvbuf(stdin, 0, _IONBF, 0); /* poll() must see every pending command line */
    if (no_object) dbus_connection_add_filter(c, note_call, 0, 0);
    printf("ready %s\n", dbus_bus_get_unique_name(c)); fflush(stdout);
    char line[256];
    for (;;) {
        struct pollfd p = {0, POLLIN, 0};
        if (poll(&p, 1, 20) > 0) {
            if (!fgets(line, sizeof line, stdin) || !strncmp(line, "quit", 4)) break;
            char path[64], name[128]; int a, b, cc, d, used = 0;
            if (no_object) { /* the service-bus connection has no object and sends no signal */ }
            else if (sscanf(line, "state %d %d %d %63s", &a, &b, &cc, path) == 4) {
                st_svc = a; st_cs = b; st_st = cc; strcpy(st_path, strcmp(path, "-") ? path : ""); reply_error = 0; raw_reply = 0;
            } else if (sscanf(line, "raw %d %d %d %63s", &a, &b, &cc, path) == 4) {
                st_svc = a; st_cs = b; st_st = cc; strcpy(st_path, strcmp(path, "-") ? path : ""); reply_error = 0; raw_reply = 1;
            } else if (!strncmp(line, "error", 5)) {
                reply_error = 1;
                if (sscanf(line, "error %127s %n", name, &used) == 1) {
                    strcpy(err_name, name);
                    snprintf(err_msg, sizeof err_msg, "%s", used ? line + used : "");
                    err_msg[strcspn(err_msg, "\n")] = 0;
                }
            }
            else if (sscanf(line, "signal %d %d %d %d %63s", &a, &b, &cc, &d, path) == 5) {
                DBusMessage *m = dbus_message_new_signal("/com/jci/bca", "com.jci.bca", "ConnectionStatusResp");
                DBusMessageIter it; dbus_message_iter_init_append(m, &it);
                append_status(&it, a, b, cc, d, strcmp(path, "-") ? path : "", 0);
                dbus_connection_send(c, m, 0); dbus_connection_flush(c); dbus_message_unref(m);
                printf("sent\n"); fflush(stdout);
            }
        }
        if (no_object) { /* libdbus dispatch: filter, then the default UnknownMethod reply */
            dbus_connection_read_write(c, 0);
            while (dbus_connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) {}
            continue;
        }
        dbus_connection_read_write(c, 0);
        DBusMessage *m;
        while ((m = dbus_connection_pop_message(c))) {
            if (lds && dbus_message_is_method_call(m, "com.jci.lds.data", "GetPosition")) {
                dbus_int32_t mode = 2, alt = 31;
                dbus_uint64_t utc = 1760000000ULL;
                double head = 87.25, vel = 42.5, h = 3.5, v = 5.0;
                DBusMessage *r = dbus_message_new_method_return(m);
                dbus_message_append_args(r, DBUS_TYPE_INT32, &mode, DBUS_TYPE_UINT64, &utc, DBUS_TYPE_DOUBLE, &lds_lat,
                                         DBUS_TYPE_DOUBLE, &lds_lon, DBUS_TYPE_INT32, &alt, DBUS_TYPE_DOUBLE, &head,
                                         DBUS_TYPE_DOUBLE, &vel, DBUS_TYPE_DOUBLE, &h, DBUS_TYPE_DOUBLE, &v, DBUS_TYPE_INVALID);
                dbus_connection_send(c, r, 0); dbus_connection_flush(c); dbus_message_unref(r);
                printf("position\n"); fflush(stdout);
            } else if (dbus_message_is_method_call(m, "com.jci.bca", "QueryConnectStatus")) {
                dbus_uint32_t svc = 0;
                dbus_message_get_args(m, 0, DBUS_TYPE_UINT32, &svc, DBUS_TYPE_INVALID);
                printf("query %u\n", svc); fflush(stdout);
                DBusMessage *r;
                if (reply_error) r = dbus_message_new_error(m, err_name, err_msg);
                else if (raw_reply) {
                    r = dbus_message_new_method_return(m);
                    DBusMessageIter it; dbus_message_iter_init_append(r, &it);
                    append_status(&it, st_svc, st_cs, 0, st_st, st_path, 1);
                } else {
                    r = dbus_message_new_method_return(m);
                    DBusMessageIter it; dbus_message_iter_init_append(r, &it);
                    int match = (int)svc == st_svc;
                    append_status(&it, (int)svc, match ? st_cs : 0, 0, match ? st_st : 0, match ? st_path : "", 1);
                }
                dbus_connection_send(c, r, 0); dbus_connection_flush(c); dbus_message_unref(r);
            }
            dbus_message_unref(m);
        }
    }
    dbus_connection_close(c); dbus_connection_unref(c);
    return 0;
}
