// Host tests for the on-device OSM tunnel-centreline matcher
// (src/navigation/tunnel_map.h). The fixtures write the binary format that
// tools/osm_tunnels.py bin_image() documents: magic "MX5TUN01", u32
// version/nchains/npoints/pad, nchains x 32-byte chain records (u32 first;
// u16 count; u16 flags; f32 length; f32 lat_min,lat_max,lon_min,lon_max; u32
// pad), then npoints x (f32 lat, f32 lon); every field is written explicitly
// with fwrite (the host is little-endian). Synthetic coordinates only:
// 37.0 N / 127.0 E, no private data. Style follows
// tests/runtime/test_acc_cap.cpp: C++11, asserts, no STL containers, no
// exceptions, and one "ok" line from main.
#include "navigation/tunnel_map.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace N = mx5::navigation;

namespace {

const double START_LAT = 37.0;
const double START_LON = 127.0;

struct Pt { double lat, lon; };

struct ChainSpec {
    double lat0, lon0;       // start point
    double north_m, east_m;  // metre offset from the start to the end point
    float length_m;          // stored chain length (drives the s gate)
};

struct RawChain {
    Pt pts[3];
    uint32_t npts;
    float length_m;
    uint16_t flags;          // bit0: one-way
};

// A chain record with deliberately choosable fields, for load-failure cases.
struct BadRecord {
    uint32_t first;
    uint16_t count;
    uint16_t flags;
    float length_m;
};

void put_u16(FILE* f, uint16_t v) { assert(fwrite(&v, sizeof v, 1, f) == 1); }
void put_u32(FILE* f, uint32_t v) { assert(fwrite(&v, sizeof v, 1, f) == 1); }
void put_f32(FILE* f, double v) {
    const float fv = static_cast<float>(v);
    assert(fwrite(&fv, sizeof fv, 1, f) == 1);
}

// Chain bounds over the written points, as bin_image() computes them.
void write_chain_record(FILE* f, const RawChain& c, uint32_t first) {
    double lat_min = c.pts[0].lat, lat_max = c.pts[0].lat;
    double lon_min = c.pts[0].lon, lon_max = c.pts[0].lon;
    for (uint32_t i = 1; i < c.npts; ++i) {
        if (c.pts[i].lat < lat_min) lat_min = c.pts[i].lat;
        if (c.pts[i].lat > lat_max) lat_max = c.pts[i].lat;
        if (c.pts[i].lon < lon_min) lon_min = c.pts[i].lon;
        if (c.pts[i].lon > lon_max) lon_max = c.pts[i].lon;
    }
    put_u32(f, first);
    put_u16(f, static_cast<uint16_t>(c.npts));
    put_u16(f, c.flags);
    put_f32(f, c.length_m);
    put_f32(f, lat_min);
    put_f32(f, lat_max);
    put_f32(f, lon_min);
    put_f32(f, lon_max);
    put_u32(f, 0);
}

// All chain records first, then all points (the format is not interleaved).
void write_db_raw(const char* path, const RawChain* chains, uint32_t nchains) {
    uint32_t npoints = 0;
    for (uint32_t i = 0; i < nchains; ++i) npoints += chains[i].npts;
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(fwrite("MX5TUN01", 1, 8, f) == 8);
    put_u32(f, 1);   // version
    put_u32(f, nchains);
    put_u32(f, npoints);
    put_u32(f, 0);   // pad
    uint32_t first = 0;
    for (uint32_t i = 0; i < nchains; ++i) {
        write_chain_record(f, chains[i], first);
        first += chains[i].npts;
    }
    for (uint32_t i = 0; i < nchains; ++i) {
        for (uint32_t j = 0; j < chains[i].npts; ++j) {
            put_f32(f, chains[i].pts[j].lat);
            put_f32(f, chains[i].pts[j].lon);
        }
    }
    assert(fclose(f) == 0);
}

// The requested helper: every chain is a start position plus a north/east
// metre offset to its end point; two points per chain are written.
void write_db(const char* path, const ChainSpec* chains, uint32_t nchains) {
    RawChain* raw = static_cast<RawChain*>(malloc(nchains * sizeof(RawChain)));
    assert(raw);
    for (uint32_t i = 0; i < nchains; ++i) {
        const double mLat = N::tunnel_map_m_per_deg_lat(chains[i].lat0);
        const double mLon = N::tunnel_map_m_per_deg_lon(chains[i].lat0);
        raw[i].pts[0].lat = chains[i].lat0;
        raw[i].pts[0].lon = chains[i].lon0;
        raw[i].pts[1].lat = chains[i].lat0 + chains[i].north_m / mLat;
        raw[i].pts[1].lon = chains[i].lon0 + chains[i].east_m / mLon;
        raw[i].npts = 2;
        raw[i].length_m = chains[i].length_m;
        raw[i].flags = 1;
    }
    write_db_raw(path, raw, nchains);
    free(raw);
}

// Corrupt-header writer for load-failure cases: valid-looking records that
// may violate the bounds, then zeros for the npoints 8-byte point slots.
void write_corrupt_db(const char* path, const char* magic, uint32_t version, uint32_t pad,
                      const BadRecord* recs, uint32_t nrecs, uint32_t npoints) {
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(fwrite(magic, 1, 8, f) == 8);
    put_u32(f, version);
    put_u32(f, nrecs);
    put_u32(f, npoints);
    put_u32(f, pad);
    for (uint32_t i = 0; i < nrecs; ++i) {
        put_u32(f, recs[i].first);
        put_u16(f, recs[i].count);
        put_u16(f, recs[i].flags);
        put_f32(f, recs[i].length_m);
        put_f32(f, 0.0); put_f32(f, 0.0); put_f32(f, 0.0); put_f32(f, 0.0);
        put_u32(f, 0);
    }
    for (uint32_t i = 0; i < npoints; ++i) {
        put_f32(f, 0.0);
        put_f32(f, 0.0);
    }
    assert(fclose(f) == 0);
}

struct Fix { double lat, lon; };

// Fix at (north, east) metres of the synthetic origin.
Fix fix_at(double north_m, double east_m) {
    const double mLat = N::tunnel_map_m_per_deg_lat(START_LAT);
    const double mLon = N::tunnel_map_m_per_deg_lon(START_LAT);
    Fix fx;
    fx.lat = START_LAT + north_m / mLat;
    fx.lon = START_LON + east_m / mLon;
    return fx;
}

void path_join(char* out, size_t cap, const char* root, const char* name) {
    const int n = snprintf(out, cap, "%s/%s", root, name);
    assert(n > 0 && static_cast<size_t>(n) < cap);
}

N::TunnelMap* must_load(const char* path) {
    N::TunnelMap* m = N::tunnel_map_load(path);
    assert(m);
    return m;
}

// One straight 800 m chain due north plus a decoy chain 5 km away.
void straight_chain_cases(const char* root) {
    char path[512];
    path_join(path, sizeof path, root, "straight.bin");
    ChainSpec spec[2];
    const ChainSpec main_chain = {START_LAT, START_LON, 800.0, 0.0, 800.0f};
    const ChainSpec decoy = {START_LAT + 5.0e-2, START_LON, 800.0, 0.0, 800.0f};
    spec[0] = main_chain;
    spec[1] = decoy;
    write_db(path, spec, 2);

    N::TunnelMap* m = must_load(path);
    assert(m->nchains == 2);
    assert(m->npoints == 4);

    // Unique match 5 m east of the portal, entering due north at s ~ 0.
    const Fix portal = fix_at(0.0, 5.0);
    N::TunnelMatch mt;
    assert(N::tunnel_map_match(m, portal.lat, portal.lon, 0.0, 50.0, 0.5, &mt));
    assert(mt.chain_index == 0);
    assert(fabs(mt.s_m) < 1.0);                 // s ~ 0
    assert(fabs(mt.lateral_m - 5.0) < 0.5);     // lateral ~ 5 m
    assert(fabs(mt.dh_deg) < 1.0);              // course == tangent

    // The before-start extension reaches s = -60 m: the entry gate's lower
    // bound. The bbox pre-filter pads by the 60 m extension plus the 15 m
    // lateral gate, so portal-adjacent fixes up to 75 m before the first point
    // can still be tested (a fix 75 m south projects to s = -60 m, lateral
    // 15 m and passes both inclusive boundaries; 76 m south fails laterally).
    const Fix before = fix_at(-12.0, 0.0);
    assert(N::tunnel_map_match(m, before.lat, before.lon, 0.0, 50.0, 0.5, &mt));
    assert(fabs(mt.s_m + 12.0) < 0.5);
    assert(mt.lateral_m < 1.0);
    const Fix before50 = fix_at(-50.0, 0.0);
    assert(N::tunnel_map_match(m, before50.lat, before50.lon, 0.0, 50.0, 0.5, &mt));
    assert(fabs(mt.s_m + 50.0) < 0.5);
    assert(mt.lateral_m < 1.0);
    const Fix before75 = fix_at(-75.0, 0.0);
    assert(N::tunnel_map_match(m, before75.lat, before75.lon, 0.0, 50.0, 0.5, &mt));
    assert(fabs(mt.s_m + 60.0) < 0.5);          // clamped at the gate
    assert(fabs(mt.lateral_m - 15.0) < 0.5);   // lateral boundary
    const Fix before76 = fix_at(-76.0, 0.0);
    assert(!N::tunnel_map_match(m, before76.lat, before76.lon, 0.0, 50.0, 0.5, &mt));

    // Tangent and point at s = 500 m on the straight chain. The stored
    // points are f32 (about 0.4 m quantization at this latitude), so the
    // expectations are derived from the loaded (quantized) points, exactly
    // as the header computes them.
    const double mLat = N::tunnel_map_m_per_deg_lat(portal.lat);
    const double mLon = N::tunnel_map_m_per_deg_lon(portal.lat);
    assert(fabs(N::tunnel_map_tangent_deg(m, 0, 500.0, mLat, mLon)) < 1e-6);
    double plat = 0.0, plon = 0.0;
    assert(N::tunnel_map_point(m, 0, 500.0, mLat, mLon, &plat, &plon));
    const double seg_m =
        (static_cast<double>(m->points[1].lat) - START_LAT) * mLat;
    const double t500 = 500.0 / seg_m;
    assert(fabs(plat - (START_LAT + t500 * (static_cast<double>(m->points[1].lat) - START_LAT))) < 1e-9);
    assert(fabs(plon - START_LON) < 1e-9);

    // Gates: s inside the chain but > 150 m -> no match.
    const Fix inside = fix_at(170.0, 0.0);
    assert(!N::tunnel_map_match(m, inside.lat, inside.lon, 0.0, 50.0, 0.5, &mt));
    // s < -60 m: the projection clamps at s = -60 m, so 100 m south leaves a
    // 40 m lateral distance and fails the lateral gate (and the bbox gate).
    const Fix far_before = fix_at(-100.0, 0.0);
    assert(!N::tunnel_map_match(m, far_before.lat, far_before.lon, 0.0, 50.0, 0.5, &mt));
    // Speed 3 km/h < 10 km/h; age 3 s > 2 s; 45 deg heading error; 20 m lateral.
    const Fix east = fix_at(0.0, 5.0);
    assert(!N::tunnel_map_match(m, east.lat, east.lon, 0.0, 3.0, 0.5, &mt));
    assert(!N::tunnel_map_match(m, east.lat, east.lon, 0.0, 9.0, 0.5, &mt));
    assert(!N::tunnel_map_match(m, east.lat, east.lon, 0.0, 50.0, 3.0, &mt));
    assert(!N::tunnel_map_match(m, east.lat, east.lon, 45.0, 50.0, 0.5, &mt));
    const Fix wide = fix_at(0.0, 20.0);
    assert(!N::tunnel_map_match(m, wide.lat, wide.lon, 0.0, 50.0, 0.5, &mt));
    // The speed and age gates are inclusive at the limits (speed 10 km/h).
    assert(N::tunnel_map_match(m, east.lat, east.lon, 0.0, 10.0, 0.5, &mt));
    assert(N::tunnel_map_match(m, east.lat, east.lon, 0.0, 50.0, 2.0, &mt));

    // Out-of-range chain index: defined, harmless results.
    assert(N::tunnel_map_tangent_deg(m, 2, 0.0, mLat, mLon) == 0.0);
    assert(!N::tunnel_map_point(m, 2, 0.0, mLat, mLon, &plat, &plon));

    N::tunnel_map_free(m);
}

// A 3-point curved chain (600 m north + 200 m east, then 400 m north +
// 600 m east): a right turn.
void curved_chain_cases(const char* root) {
    char path[512];
    path_join(path, sizeof path, root, "curved.bin");
    const double mLat = N::tunnel_map_m_per_deg_lat(START_LAT);
    const double mLon = N::tunnel_map_m_per_deg_lon(START_LAT);
    RawChain chain;
    chain.pts[0].lat = START_LAT;
    chain.pts[0].lon = START_LON;
    chain.pts[1].lat = START_LAT + 600.0 / mLat;
    chain.pts[1].lon = START_LON + 200.0 / mLon;
    chain.pts[2].lat = chain.pts[1].lat + 400.0 / mLat;
    chain.pts[2].lon = chain.pts[1].lon + 600.0 / mLon;
    chain.npts = 3;
    chain.length_m = 1400.0f;
    chain.flags = 1;
    write_db_raw(path, &chain, 1);

    N::TunnelMap* m = must_load(path);
    assert(m->nchains == 1);
    assert(m->npoints == 3);

    // Metric frame of the portal. The stored points are f32 (about 0.4 m
    // quantization at this latitude), so every expectation below is derived
    // from the loaded (quantized) points, exactly as the header computes it.
    const double p0_lat = m->points[0].lat, p0_lon = m->points[0].lon;
    const double p1_lat = m->points[1].lat, p1_lon = m->points[1].lon;
    const double p2_lat = m->points[2].lat, p2_lon = m->points[2].lon;
    const double dx1 = (p1_lon - p0_lon) * mLon, dy1 = (p1_lat - p0_lat) * mLat;
    const double dx2 = (p2_lon - p1_lon) * mLon, dy2 = (p2_lat - p1_lat) * mLat;
    const double seg1 = sqrt(dx1 * dx1 + dy1 * dy1);
    const double seg2 = sqrt(dx2 * dx2 + dy2 * dy2);
    const double tan0 = N::tunnel_map_tangent_deg(m, 0, 0.0, mLat, mLon);
    const double tan_end = N::tunnel_map_tangent_deg(m, 0, seg1 + seg2, mLat, mLon);
    assert(fabs(tan0 - N::tunnel_map_bearing_deg(dx1, dy1)) < 1e-9);      // ~18.5 deg
    assert(fabs(tan_end - N::tunnel_map_bearing_deg(dx2, dy2)) < 1e-9);   // ~56.3 deg
    // The turn is to the right (clockwise): the tangent grows.
    assert(tan_end > tan0 + 30.0);

    // The point at the mid arc length lies strictly between the endpoints.
    const double s_mid = 0.5 * (seg1 + seg2);
    double mlat = 0.0, mlon = 0.0;
    assert(N::tunnel_map_point(m, 0, s_mid, mLat, mLon, &mlat, &mlon));
    const double t_mid = (s_mid - seg1) / seg2;
    const double exp_lat = p1_lat + t_mid * (p2_lat - p1_lat);
    const double exp_lon = p1_lon + t_mid * (p2_lon - p1_lon);
    assert(fabs(mlat - exp_lat) < 1e-9);
    assert(fabs(mlon - exp_lon) < 1e-9);
    assert(mlat > p0_lat && mlat < p2_lat);
    assert(mlon > p0_lon && mlon < p2_lon);

    N::tunnel_map_free(m);
}

// Two coincident chains: both pass the gates, so the match is ambiguous.
void ambiguous_cases(const char* root) {
    char path[512];
    path_join(path, sizeof path, root, "ambiguous.bin");
    ChainSpec spec[2];
    const ChainSpec twin = {START_LAT, START_LON, 800.0, 0.0, 800.0f};
    spec[0] = twin;
    spec[1] = twin;
    write_db(path, spec, 2);

    N::TunnelMap* m = must_load(path);
    assert(m->nchains == 2);
    assert(m->npoints == 4);
    const Fix portal = fix_at(0.0, 5.0);
    N::TunnelMatch mt;
    assert(!N::tunnel_map_match(m, portal.lat, portal.lon, 0.0, 50.0, 0.5, &mt));
    N::tunnel_map_free(m);
}

void load_failure_cases(const char* root) {
    char path[512];
    const BadRecord valid = {0, 2, 1, 800.0f};
    const BadRecord short_count = {0, 1, 1, 800.0f};
    const BadRecord overshoot = {1, 2, 1, 800.0f};

    path_join(path, sizeof path, root, "bad-magic.bin");
    write_corrupt_db(path, "MX5TUN02", 1, 0, &valid, 1, 2);
    assert(!N::tunnel_map_load(path));

    path_join(path, sizeof path, root, "bad-version.bin");
    write_corrupt_db(path, "MX5TUN01", 2, 0, &valid, 1, 2);
    assert(!N::tunnel_map_load(path));

    path_join(path, sizeof path, root, "short-count.bin");
    write_corrupt_db(path, "MX5TUN01", 1, 0, &short_count, 1, 2);   // count < 2
    assert(!N::tunnel_map_load(path));

    path_join(path, sizeof path, root, "overshoot.bin");
    write_corrupt_db(path, "MX5TUN01", 1, 0, &overshoot, 1, 2);     // first+count > npoints
    assert(!N::tunnel_map_load(path));

    // Truncated inside the header (only magic + version present).
    path_join(path, sizeof path, root, "short-file.bin");
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(fwrite("MX5TUN01", 1, 8, f) == 8);
    put_u32(f, 1);
    assert(fclose(f) == 0);
    assert(!N::tunnel_map_load(path));

    // Truncated inside the chain records.
    path_join(path, sizeof path, root, "truncated.bin");
    write_corrupt_db(path, "MX5TUN01", 1, 0, &valid, 1, 2);
    assert(truncate(path, 40) == 0);
    assert(!N::tunnel_map_load(path));

    // Nonexistent and null paths.
    path_join(path, sizeof path, root, "missing.bin");
    assert(!N::tunnel_map_load(path));
    assert(!N::tunnel_map_load(0));
}

void null_safety_cases() {
    N::TunnelMatch mt;
    assert(!N::tunnel_map_match(0, START_LAT, START_LON, 0.0, 50.0, 0.5, &mt));
    assert(N::tunnel_map_tangent_deg(0, 0, 0.0, 111132.954, 88979.0) == 0.0);
    double lat = 1.0, lon = 2.0;
    assert(!N::tunnel_map_point(0, 0, 0.0, 111132.954, 88979.0, &lat, &lon));
    N::tunnel_map_free(0);
}

}  // namespace

// Deterministic mutation fuzz for the on-device parser: a 1 MiB flash file can
// be corrupted independently of the release checksum. Every mutated or
// truncated input must either fail the load or satisfy the same structural
// contract the loader enforces (2 <= count, first+count <= npoints). This is a
// crash/over-read guard, not a match-quality test.
void load_fuzz_cases(const char* root) {
    char src[512], dst[512];
    path_join(src, sizeof src, root, "fuzz-src.bin");
    path_join(dst, sizeof dst, root, "fuzz-mut.bin");
    ChainSpec spec[2];
    const ChainSpec main_chain = {START_LAT, START_LON, 800.0, 0.0, 800.0f};
    const ChainSpec decoy = {START_LAT + 5.0e-2, START_LON, 800.0, 0.0, 800.0f};
    spec[0] = main_chain; spec[1] = decoy;
    write_db(src, spec, 2);

    FILE* f = fopen(src, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    const long end = ftell(f);
    assert(end > 24);
    assert(fseek(f, 0, SEEK_SET) == 0);
    const size_t size = static_cast<size_t>(end);
    unsigned char* base = static_cast<unsigned char*>(malloc(size));
    unsigned char* work = static_cast<unsigned char*>(malloc(size));
    assert(base && work);
    assert(fread(base, 1, size, f) == size);
    assert(fclose(f) == 0);

    uint32_t state = 0x5eed1234u;
    for (int iter = 0; iter < 400; ++iter) {
        memcpy(work, base, size);
        const int flips = 1 + static_cast<int>(state % 8u);
        for (int k = 0; k < flips; ++k) {
            state = state * 1664525u + 1013904223u;
            work[state % size] ^= static_cast<unsigned char>(1u << ((state >> 16) & 7u));
        }
        size_t len = size;
        if ((iter % 5) == 0) {
            state = state * 1664525u + 1013904223u;
            len = static_cast<size_t>(state) % (size + 1);
        }
        f = fopen(dst, "wb");
        assert(f);
        assert(fwrite(work, 1, len, f) == len);
        assert(fclose(f) == 0);
        N::TunnelMap* m = N::tunnel_map_load(dst);
        if (m) {
            assert(m->nchains > 0 && m->npoints > 0);
            for (uint32_t i = 0; i < m->nchains; ++i) {
                const N::TunnelMapChain& c = m->chains[i];
                assert(c.count >= 2);
                assert(c.count <= m->npoints);
                assert(c.first <= m->npoints - c.count);
            }
            N::tunnel_map_free(m);
        }
    }
    // Exhaustive truncation of the valid file at byte granularity around the
    // header and at a coarse stride over the body.
    for (long len = 0; len < end; len += (len < 32 ? 1 : 37)) {
        f = fopen(dst, "wb");
        assert(f);
        assert(fwrite(base, 1, static_cast<size_t>(len), f) == static_cast<size_t>(len));
        assert(fclose(f) == 0);
        N::TunnelMap* m = N::tunnel_map_load(dst);
        if (m) N::tunnel_map_free(m);
    }
    free(work);
    free(base);
    unlink(src);
    unlink(dst);
}

int main() {
    char tmpl[] = "/tmp/mx5dr-tunnelmap-XXXXXX";
    assert(mkdtemp(tmpl));
    straight_chain_cases(tmpl);
    curved_chain_cases(tmpl);
    ambiguous_cases(tmpl);
    load_failure_cases(tmpl);
    load_fuzz_cases(tmpl);
    null_safety_cases();
    static const char* const kFiles[] = {
        "straight.bin", "curved.bin", "ambiguous.bin",
        "bad-magic.bin", "bad-version.bin", "short-count.bin", "overshoot.bin",
        "short-file.bin", "truncated.bin", "missing.bin",
    };
    char path[512];
    for (size_t i = 0; i < sizeof kFiles / sizeof kFiles[0]; ++i) {
        path_join(path, sizeof path, tmpl, kFiles[i]);
        unlink(path);   // missing.bin does not exist; failures are ignored
    }
    assert(rmdir(tmpl) == 0);
    puts("tunnel_map tests passed");
    return 0;
}
