# Map-aided tunnel counterfactual (host tools)

Record-only evidence for the map-aided tunnel heading idea
(`validation/MAP_AID_EVALUATOR_2026-10-14.md`) and the database of the BETA
map-constrained tunnel replacement
(`validation/TUNNEL_MAP_CONSTRAINT_2026-10-22.md`). Nothing here runs on the
CMU; the `.bin` this document describes is data, not code.

## 1. Tunnel database (`tools/osm_tunnels.py`)

The database is an ODbL derived database of OpenStreetMap data. It is not
committed: it goes to the git-ignored `private-data/osm/` and is rebuilt on
demand.

```sh
python3 -m venv /tmp/osm-venv
/tmp/osm-venv/bin/pip install osmium          # pyosmium 4.x (tested 4.3.1)
curl -LO https://download.geofabrik.de/asia/south-korea-latest.osm.pbf
/tmp/osm-venv/bin/python tools/osm_tunnels.py south-korea-latest.osm.pbf
# -> private-data/osm/tunnels-v1.json.gz, tunnels-v1.manifest.json
#    and tunnels-v1.bin (compact on-device database)
```

By default the tool also writes the compact on-device database next to the
`json.gz`: `tunnels-v1.bin` (little-endian `MX5TUN01` v1 format described in
`src/navigation/tunnel_map.h`). Use `--bin PATH` to place it elsewhere or
`--no-bin` to skip it. About 1 MB per national build.

The `.bin` is not committed and must be generated for a release bundle and
copied into it: the BETA map-constrained tunnel replacement
(`src/runtime/tunnel_map_switch.h`, `validation/TUNNEL_MAP_CONSTRAINT_2026-10-22.md`)
loads `tunnels-v1.bin` from the package root, and a missing or corrupt file
fails closed to the ordinary BETA DR. Record its md5 in the release record.

Two streaming passes (tunnel ways, then only their nodes): about 3 minutes
and under 100 MB RSS for the 2026-10-13 extract (276 MB). Do not replace the
node pass by `osmium.filter.IdFilter`: its id bitmap needs about 1.5 GB.
The manifest records the source file name, OSM timestamp, md5, counts and the
ODbL attribution (it describes the `json.gz`; the `.bin` digest is not in it,
so record it separately in the release verification list). Refresh it about
once a year (tunnel ways change; a missing tunnel only means "no match").

Legal: the `json.gz`, the manifest and the `.bin` are an ODbL 1.0 derived
database of OpenStreetMap data. Any distribution must carry the ODbL notice
("Contains information from OpenStreetMap, © OpenStreetMap contributors"),
point to the machine-readable provenance, and declare the derived-database
status of the `.bin` (attribution string in `tunnels-v1.manifest.json`).
The extracted `.osm.pbf` source itself is not redistributed.

## 2. Evaluator (`tools/map_aid_eval.py`, standard library only)

```sh
python3 tools/map_aid_eval.py --db private-data/osm/tunnels-v1.json.gz \
    mx5dr-logs-*.tar            # or the uploaded .zip, a directory, JSONL
python3 tools/analyze_logs.py --map-db private-data/osm/tunnels-v1.json.gz mx5dr-logs-*.tar
```

The evaluator reads the `json.gz` (it re-derives the on-device view where
needed); the CMU product reads only the `.bin`. Both must come from the same
`osm_tunnels.py` run, so a rebuilt database replaces both files together.

Archives are read in memory (no extraction); overlapping exports are
deduplicated per boot. The default text and `--json` outputs contain no
coordinates, tunnel names or OSM ids (episodes are `<boot>@<return time>`);
`--private` prints those to the terminal only. Never paste `--private`
output into the repository or a public place.

Tests: `tests/tools/test_map_aid.py` (synthetic fixtures; the pyosmium
fixture test is skipped without pyosmium). Private regression:
`MX5DR_MAP_AID_LOGS=private-data/vehicle-logs MX5DR_MAP_AID_DB=private-data/osm/tunnels-v1.json.gz make test-tools`.
