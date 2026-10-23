#!/usr/bin/env python3
"""Build a compact road-tunnel database from an OpenStreetMap extract (host only).

Input: a Geofabrik ``south-korea-latest.osm.pbf`` (or any .osm/.osm.pbf),
read twice with pyosmium in streaming mode (pass 1: tunnel ways, pass 2: only
their nodes), so the extract is never loaded whole. Output: a versioned,
gzip-compressed JSON database of directed tunnel chains plus a manifest.

The database is a derived database of OpenStreetMap data under the Open
Database License (ODbL) 1.0. It is NOT committed to the repository: the
default output directory ``private-data/osm/`` is git-ignored; regenerate it
on demand (tools/README_MAP_AID.md). Only this builder and its tests (on a
tiny synthetic fixture) are committed.

Chains: every tunnel way of the selected road classes is split at nodes it
shares with another selected tunnel way, made directed (one-way ways once,
two-way ways in both directions) and chained while the travel direction has
exactly one tunnel successor (a merge does not end a chain, a fork does; a
U-turn onto the same way's reverse never counts). Each chain is simplified
(Douglas-Peucker, default 2 m) and stored with its start kind (``portal``:
no tunnel predecessor, ``branch``: after a fork, ``loop``), end kind
(``portal``, ``fork``, ``loop``, ``cap``), the chain ids continuing after a
fork, one-way flag, length, road class, OSM way ids and the tunnel name.
"""
import argparse
import datetime
import gzip
import hashlib
import json
import math
import os
import sys

SCHEMA = "mx5dr-osm-tunnels"
VERSION = 1
ATTRIBUTION = ("Contains information from OpenStreetMap, (c) OpenStreetMap contributors, made available under the "
               "Open Database License (ODbL) 1.0 (https://www.openstreetmap.org/copyright, "
               "https://opendatacommons.org/licenses/odbl/1-0/). This file is a derived database: share it only "
               "under the ODbL with this notice.")
# Road classes kept by default: the classes both feasibility studies matched
# (validation/MAP_AIDED_TUNNEL_HEADING_2026-10-14.md section 1); service
# roads (mostly parking ramps) are excluded.
DEFAULT_CLASSES = ("motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link", "secondary",
                   "secondary_link", "tertiary", "tertiary_link", "unclassified", "residential", "living_street",
                   "road")
NOT_TUNNEL = (None, "", "no", "building_passage", "culvert")
IMPLIED_ONEWAY = ("motorway", "motorway_link")
DEFAULT_OUT = os.path.join("private-data", "osm", "tunnels-v1.json.gz")
EARTH_M_PER_DEG_LAT = 111132.954
MAX_CHAIN_M = 30000.0


def is_tunnel_way(tags, classes=DEFAULT_CLASSES):
    """tags: mapping. True for a road way of a kept class that is a tunnel."""
    return tags.get("highway") in classes and tags.get("tunnel") not in NOT_TUNNEL


def travel_directions(tags):
    """Allowed travel directions along the node order: (forward, reverse)."""
    oneway = (tags.get("oneway") or "").strip().lower()
    if oneway in ("-1", "reverse"):
        return False, True
    if oneway in ("yes", "true", "1"):
        return True, False
    if oneway in ("no", "false", "0", "alternating", "reversible"):
        return True, True
    if tags.get("highway") in IMPLIED_ONEWAY or tags.get("junction") in ("roundabout", "circular"):
        return True, False
    return True, True


def local_xy(origin, point):
    """Equirectangular metres of point (lat, lon) east/north of origin."""
    lat0 = math.radians(origin[0])
    m_lat = EARTH_M_PER_DEG_LAT - 559.822 * math.cos(2 * lat0)
    m_lon = 111412.84 * math.cos(lat0)
    return ((point[1] - origin[1]) * m_lon, (point[0] - origin[0]) * m_lat)


def douglas_peucker(xy, tol):
    """Indices of xy kept by Douglas-Peucker with tolerance tol (metres)."""
    n = len(xy)
    if n <= 2:
        return list(range(n))
    keep = [False] * n
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = xy[a]
        bx, by = xy[b]
        dx, dy = bx - ax, by - ay
        seg = math.hypot(dx, dy)
        best, best_i = -1.0, None
        for i in range(a + 1, b):
            px, py = xy[i]
            if seg < 1e-9:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs((px - ax) * dy - (py - ay) * dx) / seg
            if d > best:
                best, best_i = d, i
        if best_i is not None and best > tol:
            keep[best_i] = True
            stack.append((a, best_i))
            stack.append((best_i, b))
    return [i for i in range(n) if keep[i]]


def polyline_length(points):
    total = 0.0
    for a, b in zip(points, points[1:]):
        x, y = local_xy(a, b)
        total += math.hypot(x, y)
    return total


def read_osm(path, classes=DEFAULT_CLASSES, log=None):
    """Two streaming pyosmium passes. Returns (ways, coords, header_timestamp):
    ways {way_id: dict(tags=..., nodes=[ids])}, coords {node_id: (lat, lon)}."""
    import osmium  # optional dependency: tools/README_MAP_AID.md

    ways = {}
    processor = osmium.FileProcessor(path, osmium.osm.WAY).with_filter(osmium.filter.KeyFilter("tunnel"))
    for way in processor:
        tags = {k: way.tags.get(k) for k in ("highway", "tunnel", "oneway", "junction", "name", "tunnel:name",
                                             "layer", "ref")}
        if not is_tunnel_way(tags, classes):
            continue
        ways[way.id] = dict(tags={k: v for k, v in tags.items() if v is not None},
                            nodes=[n.ref for n in way.nodes])
    need = set()
    for way in ways.values():
        need.update(way["nodes"])
    if log:
        log("pass 1: %d tunnel ways, %d nodes needed" % (len(ways), len(need)))
    coords = {}
    if need:
        # Membership is checked in Python on purpose: osmium.filter.IdFilter
        # allocates an id bitmap sized by the largest node id (about 1.5 GB
        # RSS on the Korean extract); this loop stays near 50 MB (about 3 min).
        for node in osmium.FileProcessor(path, osmium.osm.NODE):
            if node.id in need and node.location.valid():
                coords[node.id] = (node.location.lat, node.location.lon)
    if log:
        log("pass 2: %d node coordinates" % len(coords))
    header = None
    try:
        reader = osmium.io.Reader(path, osmium.osm.osm_entity_bits.NOTHING)
        header = reader.header().get("osmosis_replication_timestamp") or None
        reader.close()
    except Exception:  # header timestamps are optional
        header = None
    return ways, coords, header


def _edges(ways, coords):
    """Directed edges between split nodes. Returns list of dict(nodes, way, twin)."""
    usage = {}
    for way in ways.values():
        nodes = [n for n in way["nodes"] if n in coords]
        for n in set(nodes):
            usage[n] = usage.get(n, 0) + 1
        seen = set()
        for n in nodes:
            if n in seen:
                usage[n] = usage.get(n, 0) + 1  # self-touching way: split there too
            seen.add(n)
    edges = []
    for wid in sorted(ways):
        way = ways[wid]
        nodes = [n for n in way["nodes"] if n in coords]
        clean = []
        for n in nodes:  # drop repeated consecutive references
            if not clean or clean[-1] != n:
                clean.append(n)
        if len(clean) < 2:
            continue
        fwd, rev = travel_directions(way["tags"])
        pieces, cur = [], [clean[0]]
        for n in clean[1:]:
            cur.append(n)
            if usage.get(n, 0) > 1 and n != clean[-1]:
                pieces.append(cur)
                cur = [n]
        pieces.append(cur)
        for piece in pieces:
            if len(piece) < 2:
                continue
            a = b = None
            if fwd:
                a = len(edges)
                edges.append(dict(nodes=piece, way=wid, twin=None))
            if rev:
                b = len(edges)
                edges.append(dict(nodes=piece[::-1], way=wid, twin=None))
            if a is not None and b is not None:
                edges[a]["twin"] = b
                edges[b]["twin"] = a
    return edges


CLASS_RANK = {c: i for i, c in enumerate(("motorway", "trunk", "primary", "secondary", "tertiary", "motorway_link",
                                          "trunk_link", "primary_link", "secondary_link", "tertiary_link",
                                          "unclassified", "residential", "living_street", "road"))}


def build(ways, coords, simplify_m=2.0, max_chain_m=MAX_CHAIN_M):
    """Chains (list of dicts, see module doc) from tunnel ways and node coordinates."""
    edges = _edges(ways, coords)
    out_at, in_at = {}, {}
    for i, e in enumerate(edges):
        out_at.setdefault(e["nodes"][0], []).append(i)
        in_at.setdefault(e["nodes"][-1], []).append(i)

    def succ(i):
        return [j for j in out_at.get(edges[i]["nodes"][-1], ()) if j != edges[i]["twin"]]

    def pred(i):
        return [j for j in in_at.get(edges[i]["nodes"][0], ()) if j != edges[i]["twin"]]

    starts = []
    for i in range(len(edges)):
        p = pred(i)
        if not p:
            starts.append((i, "portal"))
        elif any(len(succ(j)) > 1 for j in p):
            starts.append((i, "branch"))
    covered = set()
    raw = []

    def follow(first, start_kind):
        nodes = list(edges[first]["nodes"])
        seq = [first]
        seen = {first}
        length = polyline_length([coords[n] for n in nodes])
        cur = first
        while True:
            s = succ(cur)
            if not s:
                end = "portal"
                break
            if len(s) > 1:
                end = "fork"
                break
            if s[0] in seen:
                end = "loop"
                break
            if length >= max_chain_m:
                end = "cap"
                break
            cur = s[0]
            seen.add(cur)
            seq.append(cur)
            pts = edges[cur]["nodes"]
            length += polyline_length([coords[n] for n in pts])
            nodes.extend(pts[1:])
        covered.update(seq)
        raw.append(dict(edges=seq, nodes=nodes, start=start_kind, end=end, last=cur))

    for i, kind in starts:
        follow(i, kind)
    for i in range(len(edges)):  # pure cycles (no start): one chain per uncovered cycle
        if i not in covered:
            follow(i, "loop")
    first_edge_chain = {}
    for cid, c in enumerate(raw):
        first_edge_chain.setdefault(c["edges"][0], cid)
    chains = []
    for cid, c in enumerate(raw):
        pts = [coords[n] for n in c["nodes"]]
        dedup = []
        for p in pts:
            if not dedup or dedup[-1] != p:
                dedup.append(p)
        if len(dedup) < 2:
            continue
        xy = [local_xy(dedup[0], p) for p in dedup]
        kept = [dedup[i] for i in douglas_peucker(xy, simplify_m)]
        way_ids = []
        for e in c["edges"]:
            if not way_ids or way_ids[-1] != edges[e]["way"]:
                way_ids.append(edges[e]["way"])
        classes = [ways[w]["tags"].get("highway") for w in way_ids]
        names = [ways[w]["tags"].get("tunnel:name") or ways[w]["tags"].get("name") for w in way_ids]
        nxt = []
        if c["end"] == "fork":
            nxt = sorted(first_edge_chain[j] for j in succ(c["last"]) if j in first_edge_chain)
        chains.append(dict(id=cid, pts=[[int(round(p[0] * 1e7)), int(round(p[1] * 1e7))] for p in kept],
                           len_m=round(polyline_length(dedup), 1), oneway=all(edges[e]["twin"] is None
                                                                             for e in c["edges"]),
                           cls=min(classes, key=lambda k: CLASS_RANK.get(k, 99)), start=c["start"], end=c["end"],
                           next=nxt, ways=way_ids, name=next((n for n in names if n), None)))
    return chains


def summarize(chains):
    lengths = [c["len_m"] for c in chains]

    def count(th):
        sub = [x for x in lengths if x >= th]
        return dict(chains=len(sub), km=round(sum(sub) / 1000.0, 1))

    long = [c for c in chains if c["len_m"] >= 300]
    return dict(chains=len(chains), km=round(sum(lengths) / 1000.0, 1), ge_300m=count(300), ge_1km=count(1000),
                ge_5km=count(5000), max_m=round(max(lengths), 1) if lengths else 0.0,
                oneway_share_ge_300m=round(sum(1 for c in long if c["oneway"]) / len(long), 3) if long else None,
                fork_ends_ge_300m=sum(1 for c in long if c["end"] == "fork"),
                branch_starts=sum(1 for c in chains if c["start"] == "branch"),
                points=sum(len(c["pts"]) for c in chains))


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def make_db(chains, source, params):
    return dict(schema=SCHEMA, version=VERSION, attribution=ATTRIBUTION, source=source, params=params,
                counts=summarize(chains), chains=chains)


def write_db(db, out_path):
    """Write db (gzip JSON) and <out>.manifest.json; returns the manifest path."""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    tmp = out_path + ".tmp"
    with gzip.open(tmp, "wt", encoding="utf-8") as f:
        json.dump(db, f, ensure_ascii=False, separators=(",", ":"))
    os.replace(tmp, out_path)
    manifest = {k: v for k, v in db.items() if k != "chains"}
    manifest["database"] = dict(file=os.path.basename(out_path), bytes=os.path.getsize(out_path),
                                md5=md5_file(out_path))
    path = manifest_path(out_path)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return path


def manifest_path(out_path):
    base = out_path[:-len(".json.gz")] if out_path.endswith(".json.gz") else out_path
    return base + ".manifest.json"


def bin_path(out_path):
    base = out_path[:-len(".json.gz")] if out_path.endswith(".json.gz") else out_path
    return base + ".bin"


# Compact on-device database (src/navigation/tunnel_map.h). Little-endian:
#   char magic[8]="MX5TUN01"; u32 version; u32 nchains; u32 npoints; u32 pad;
#   chains[nchains] x 32 B: u32 first; u16 count; u16 flags(bit0 oneway);
#            f32 length_m; f32 lat_min,lat_max,lon_min,lon_max; u32 pad
#   points[npoints] x 8 B: f32 lat, lon   (degrees)
BIN_MAGIC = b"MX5TUN01"
BIN_VERSION = 1
BIN_CHAIN = 32


def bin_image(db):
    """bytes of the on-device database for a loaded/just-built db."""
    import struct
    chains = db["chains"]
    pts = []
    recs = []
    for c in chains:
        raw = c["pts"]
        if len(raw) < 2:
            continue
        if len(raw) > 0xFFFF:
            raise ValueError("chain %s has %d points (max 65535)" % (c.get("id"), len(raw)))
        first = len(pts)
        lats = [p[0] / 1e7 for p in raw]
        lons = [p[1] / 1e7 for p in raw]
        pts.extend(zip(lats, lons))
        # bit0 one-way; bit1 end is not a portal (fork/loop), so the device
        # must not extrapolate past the chain end (tunnel_map.h flags).
        flags = (1 if c.get("oneway") else 0) | (2 if c.get("end", "portal") != "portal" else 0)
        recs.append((first, len(raw), flags, float(c.get("len_m", 0.0)),
                     min(lats), max(lats), min(lons), max(lons)))
    out = bytearray()
    out += BIN_MAGIC
    out += struct.pack("<IIII", BIN_VERSION, len(recs), len(pts), 0)
    for rec in recs:
        out += struct.pack("<IHHfffffI", rec[0], rec[1], rec[2], rec[3], rec[4], rec[5], rec[6], rec[7], 0)
    for lat, lon in pts:
        out += struct.pack("<ff", lat, lon)
    return bytes(out)


def write_bin(db, out_path):
    """Write the compact on-device database; returns the path."""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(bin_image(db))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, out_path)
    return out_path


def load_db(path):
    with gzip.open(path, "rt", encoding="utf-8") as f:
        db = json.load(f)
    if db.get("schema") != SCHEMA or db.get("version") != VERSION:
        raise ValueError("%s: not a %s v%d database" % (path, SCHEMA, VERSION))
    return db


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("extract", help="OSM extract (.osm.pbf or .osm), e.g. Geofabrik south-korea-latest.osm.pbf")
    parser.add_argument("--out", default=DEFAULT_OUT, help="database path (default %(default)s, git-ignored)")
    parser.add_argument("--bin", default=None,
                        help="compact on-device database path (default: the --out path with .bin)")
    parser.add_argument("--no-bin", action="store_true", help="do not write the compact on-device database")
    parser.add_argument("--simplify-m", type=float, default=2.0, help="Douglas-Peucker tolerance (m)")
    parser.add_argument("--classes", default=",".join(DEFAULT_CLASSES), help="comma-separated highway classes")
    args = parser.parse_args(argv)
    classes = tuple(c for c in args.classes.split(",") if c)
    log = lambda msg: print(msg, file=sys.stderr, flush=True)
    ways, coords, header = read_osm(args.extract, classes, log)
    chains = build(ways, coords, args.simplify_m)
    stat = os.stat(args.extract)
    source = dict(file=os.path.basename(args.extract), bytes=stat.st_size, md5=md5_file(args.extract),
                  file_mtime_utc=datetime.datetime.fromtimestamp(stat.st_mtime, datetime.timezone.utc)
                  .strftime("%Y-%m-%dT%H:%M:%SZ"), osm_timestamp=header, tunnel_ways=len(ways))
    params = dict(classes=list(classes), simplify_m=args.simplify_m, max_chain_m=MAX_CHAIN_M,
                  built_utc=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
    db = make_db(chains, source, params)
    path = write_db(db, args.out)
    bin_out = None if args.no_bin else (args.bin or bin_path(args.out))
    if bin_out:
        write_bin(db, bin_out)
    log("wrote %s (%d chains, %.1f km), %s%s" % (
        args.out, db["counts"]["chains"], db["counts"]["km"], path,
        " and %s" % bin_out if bin_out else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
