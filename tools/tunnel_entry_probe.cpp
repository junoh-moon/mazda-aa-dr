// Host-only offline evaluator bridge; all map operations use product helpers.
#include "navigation/tunnel_map.h"
namespace N = mx5::navigation;
extern "C" {
void* entry_load(const char* path) { return N::tunnel_map_load(path); }
void entry_free(void* p) { N::tunnel_map_free(static_cast<N::TunnelMap*>(p)); }
int entry_match(void* p, double lat, double lon, double h, double speed,
                double age, double* out) {
    N::TunnelMap* map = static_cast<N::TunnelMap*>(p);
    N::TunnelMatch match;
    if (!N::tunnel_map_match(map, lat, lon, h, speed, age, &match)) return 0;
    out[0] = match.chain_index; out[1] = match.s_m;
    out[2] = match.lateral_m; out[3] = match.dh_deg;
    out[4] = map->chains[match.chain_index].length_m;
    out[5] = map->chains[match.chain_index].flags;
    out[6] = N::tunnel_map_m_per_deg_lat(lat);
    out[7] = N::tunnel_map_m_per_deg_lon(lat);
    out[8] = lat; out[9] = lon;
    return 1;
}
int entry_point(void* p, unsigned idx, double s, double ml, double mn, double* out) {
    return N::tunnel_map_point(static_cast<N::TunnelMap*>(p), idx, s, ml, mn, out, out + 1);
}
double entry_tangent(void* p, unsigned idx, double s, double ml, double mn) {
    return N::tunnel_map_tangent_deg(static_cast<N::TunnelMap*>(p), idx, s, ml, mn);
}
}
