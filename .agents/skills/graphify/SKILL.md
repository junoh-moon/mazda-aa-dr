---
name: graphify
description: Build or query the local Graphify code graph for mazda-aa-dr. Use for code architecture, symbol relationships, call paths, and change impact in this repository, or when the user invokes graphify.
---

# Graphify for mazda-aa-dr

Use this skill only in this repository. Run commands from its root through
`./tools/graphify`; it uses uv's isolated tool environment and permits upgrades.
Do not install Graphify or assistant integrations in the user profile.

For a codebase question, query an existing graph before broad source searches:

```sh
./tools/graphify query "G3 heading carry and NO_FIX bearing" --budget 1500
./tools/graphify explain "G3Motion"
./tools/graphify path "symbol A" "symbol B"
./tools/graphify affected "symbol" --depth 2
```

Replace examples with the requested concepts. Query results locate relevant
source; verify behavior in those files. Ambiguous names exit 3 with candidates;
no match exits 4. Refine the name or use a bounded source search in those cases.
Missing, stale, or incomplete graphs must not prevent source inspection.

For initial creation, an explicit rebuild, or after relevant code changes:

```sh
./tools/graphify extract . --code-only --max-workers 2
./tools/graphify cluster-only . --no-label
```

Extraction incrementally refreshes an existing graph; for a requested full
rescan, add `--force` to `extract`. Clustering writes the report and interactive
HTML without LLM labeling. To refresh only the visualization:

```sh
./tools/graphify export html
```

Keep `.graphifyignore` and `.gitignore` active. They exclude private data and
limit traversal to eight path components; do not scan parent directories,
other checkouts, or override these exclusions. Generated `graphify-out/` files
are local and ignored by Git.

The launcher disables automatic LLM backend selection. This setup indexes code
with local AST parsing; documents are not semantically indexed. Read the current
status/review documents required by `AGENTS.md` directly. Graph edges, especially
inferred ones, are navigation aids, not proof of behavior or vehicle validation.

See `docs/GRAPHIFY_KO.md` for setup, upgrades, output files, and removal.
