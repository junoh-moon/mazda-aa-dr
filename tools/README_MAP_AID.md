# Map-aided tunnel counterfactual (host tools)

Record-only evidence for the map-aided tunnel heading idea
(`validation/MAP_AID_EVALUATOR_2026-10-14.md`). Nothing here changes the
product or runs on the CMU.

## 1. Tunnel database (`tools/osm_tunnels.py`)

The database is an ODbL derived database of OpenStreetMap data. It is not
committed: it goes to the git-ignored `private-data/osm/` and is rebuilt on
demand.

```sh
python3 -m venv /tmp/osm-venv
/tmp/osm-venv/bin/pip install osmium          # pyosmium 4.x (tested 4.3.1)
curl -LO https://download.geofabrik.de/asia/south-korea-latest.osm.pbf
/tmp/osm-venv/bin/python tools/osm_tunnels.py south-korea-latest.osm.pbf
# -> private-data/osm/tunnels-v1.json.gz and tunnels-v1.manifest.json
```

Two streaming passes (tunnel ways, then only their nodes): about 3 minutes
and under 100 MB RSS for the 2026-10-13 extract (276 MB). Do not replace the
node pass by `osmium.filter.IdFilter`: its id bitmap needs about 1.5 GB.
The manifest records the source file name, OSM timestamp, md5, counts and the
ODbL attribution. Refresh it about once a year (tunnel ways change; a missing
tunnel only means "no match").

## 2. Evaluator (`tools/map_aid_eval.py`, standard library only)

```sh
python3 tools/map_aid_eval.py --db private-data/osm/tunnels-v1.json.gz \
    mx5dr-logs-*.tar            # or the uploaded .zip, a directory, JSONL
python3 tools/analyze_logs.py --map-db private-data/osm/tunnels-v1.json.gz mx5dr-logs-*.tar
```

Archives are read in memory (no extraction); overlapping exports are
deduplicated per boot. The default text and `--json` outputs contain no
coordinates, tunnel names or OSM ids (episodes are `<boot>@<return time>`);
`--private` prints those to the terminal only. Never paste `--private`
output into the repository or a public place.

Tests: `tests/tools/test_map_aid.py` (synthetic fixtures; the pyosmium
fixture test is skipped without pyosmium). Private regression:
`MX5DR_MAP_AID_LOGS=private-data/vehicle-logs MX5DR_MAP_AID_DB=private-data/osm/tunnels-v1.json.gz make test-tools`.
