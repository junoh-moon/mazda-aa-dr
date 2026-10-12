#!/usr/bin/env python3
"""Generate DHU command files for experiment 12 (accuracy-threshold test, H-ACC30).

Question. A static reading of Naver Map 6.8.0.5 (validation/NAVER_MAP_APP_STATIC_RE_2026-10-12.md) found that a head-unit location is
valid only if its reported accuracy is < 30 m (30-50 m -> INVALID_ACCURACY). Experiment 11 V4 (accuracy ramp 30 -> 40 m, cross-track
drift to 119 m over 111 s) turned grey at tau 65-85 s in 3 of 3 runs (V4-r1 with the experiment 4 ini, V4R-r1/-r2), while V3 (20 m, the
same kind of drift) and V2 (40 m held, no drift) stayed normal. H-ACC30: the boundary is exactly 30 m.

Single variable. Every run here is experiment 11 commands/V4-r1.txt BYTE FOR BYTE except
  (1) the screenshot tag (V4-r1 -> the run tag; same experiment11/ directory, so no earlier photo is overwritten) and
  (2) the accuracy field (4th word) of the 111 tunnel location lines (t = 20..130, tau = 0..110).
Approach and recovery fixes stay at 1 m; positions, speeds, bearings, gps_satellite lines and sleeps are untouched.
  V4A29: 29 m constant    V4A31: 31 m constant    V4REF: the original ramp (same-session control)    V4A25: 25 m constant
Tier 0 is experiment 11 T0-NOSAT.txt with only its screenshot tag changed (T0-E12). The preparation files are byte copies of
experiment 11 commands/prep (themselves byte copies of experiment 10 v5).
"""
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXP11 = os.path.normpath(os.path.join(HERE, "..", "DHU_NAVER_EXP11_PLAN_2026-10-11"))
EXP4_INI = os.path.normpath(os.path.join(HERE, "..", "DHU_NAVER_EXP4_2026-10-03", "dhu.ini"))

SOURCE_TAG = "V4-r1"
SOURCE_SHA256 = "a536d66458d5374f15962991bd3f4656a1c14fdccbe5a9fc043489f6d4917130"  # body_file_sha256 of exp 11 V4-r1 (run-log.csv)
T0_SOURCE_TAG = "T0-NOSAT"
T0_TAG = "T0-E12"
EXP4_INI_SHA256 = "5e57e7a2211cd0826c5e2df2ddeacdb98b8fe5e035f7587f1b8d637ffe1c766e"  # ini of exp 11 V4-r1 (progress.json)
SHOT = "../../../../experiment11/"
TUNNEL_T = (20, 130)  # first and last body second of the GPS-loss episode (tau = t - 20)
ACC_FIX = "1.000"
ACC_WORD = 3  # location <lat> <lon> <accuracy> <altitude> <speed> <bearing>
PREP_FILES = ("B2-search", "B2-route-y130", "B2-route-y225", "B2-route-y318", "B2-route-retry", "B2-gate", "B2-gate-nopopup",
              "search-again", "end-guidance")

# order by information: the bracket first, then the same-session control, then the margin run
RUNS = [
    ("V4A29-r1", 29.0, "just below the 30 m boundary: predicted no grey (or much later than V4)"),
    ("V4A31-r1", 31.0, "just above the 30 m boundary: predicted grey with onset like V4 (tau 65-85 s)"),
    ("V4REF-r1", None, "original V4 ramp 30 -> 40 m: same-session control (V4 grey onset varied 65-85 s)"),
    ("V4A25-r1", 25.0, "margin below the boundary (only if time allows)"),
]
TIER1_REQUIRED = ["V4A29-r1", "V4A31-r1"]
BUDGET_MIN = 25
ENV_MIN = 2.0
TIER0_MIN = (5, 7)
RUN_MIN = 5.0  # exp 11 / exp 11 v4 measured start-to-start 4.6-5.3 min for these 151 s bodies (run-log.csv)
GREY_ONSET_V4 = [65, 85]  # V4R-r1 65, V4-r1 85, V4R-r2 85 (first IND-GREY tau)
GREY_ONSET_LIKE_V4 = [55, 95]  # pre-registered window for 'onset about like V4' (one photo step of 5 s plus 5 s margin)
LATE_FROM_TAU = 100  # first IND-GREY at tau >= 100 counts as 'much later' (>= 15 s after the latest V4 onset)


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def read_lines(path):
    with open(path) as f:
        return f.read().split("\n")[:-1]


def fmt_acc(a):
    return f"{a:.3f}"


def retag(lines, old, new):
    return [x.replace(f"{SHOT}{old}-", f"{SHOT}{new}-") if x.startswith("screenshot ") else x for x in lines]


def build_run(src, tag, acc):
    """Experiment 11 V4-r1 with the tag and (if acc is not None) the tunnel accuracy replaced."""
    out, t = [], -1
    for x in retag(src, SOURCE_TAG, tag):
        if x.startswith("location "):
            t += 1
            w = x.split(" ")
            if TUNNEL_T[0] <= t <= TUNNEL_T[1]:
                assert float(w[ACC_WORD]) >= 30.0, (t, x)
                if acc is not None:
                    w[ACC_WORD] = fmt_acc(acc)
                    x = " ".join(w)
            else:
                assert w[ACC_WORD] == ACC_FIX, (t, x)
        out.append(x)
    assert t == 150, t
    return out


def generate(out_dir, plan_path=None):
    src_path = os.path.join(EXP11, "commands", SOURCE_TAG + ".txt")
    if sha256(src_path) != SOURCE_SHA256:
        raise SystemExit(f"{src_path}: sha256 differs from the experiment 11 V4-r1 body that was run")
    if sha256(EXP4_INI) != EXP4_INI_SHA256:
        raise SystemExit(f"{EXP4_INI}: sha256 differs from the experiment 11 V4-r1 ini")
    src = read_lines(src_path)
    os.makedirs(os.path.join(out_dir, "prep"), exist_ok=True)

    def write(name, lines):
        with open(os.path.join(out_dir, name + ".txt"), "w") as f:
            f.write("\n".join(lines) + "\n")

    for name in PREP_FILES:
        shutil.copyfile(os.path.join(EXP11, "commands", "prep", name + ".txt"), os.path.join(out_dir, "prep", name + ".txt"))
    write(T0_TAG, retag(read_lines(os.path.join(EXP11, "commands", T0_SOURCE_TAG + ".txt")), T0_SOURCE_TAG, T0_TAG))

    with open(os.path.join(EXP11, "run-plan.json")) as f:
        p11 = json.load(f)
    base = p11["runs"][SOURCE_TAG]
    runs = {}
    for tag, acc, question in RUNS:
        write(tag, build_run(src, tag, acc))
        frames = []
        for fr in base["frames"]:
            fr = dict(fr)
            fr["screenshot"] = fr["screenshot"].replace(f"{SOURCE_TAG}-", f"{tag}-", 1)
            if acc is not None and fr["phase"] == "tunnel":
                fr["accuracy_m"] = acc
            frames.append(fr)
        runs[tag] = dict(tag=tag, condition=tag.rsplit("-r", 1)[0], question=question, source_file=f"exp11 commands/{SOURCE_TAG}.txt",
                         source_sha256=SOURCE_SHA256, ini="exp4", ini_sha256=EXP4_INI_SHA256,
                         accuracy_tunnel=("ramp ceil(30 + 0.72 tau) capped at 40 (exp 11 V4)" if acc is None else acc),
                         changed_lines="accuracy field of location lines t=20..130 only" if acc is not None else "screenshot tag only",
                         episode_s=base["episode_s"], duration_s=base["duration_s"], tunnel_t=base["tunnel_t"],
                         recovery_t=base["recovery_t"], speed_checks=base["speed_checks"], episode_end=base["episode_end"],
                         heading_error_rate_deg_s=base["heading_error_rate_deg_s"], frames=frames)
    order = [t for t, _, _ in RUNS]
    est = [ENV_MIN + TIER0_MIN[0] + RUN_MIN * n for n in range(1, len(order) + 1)]
    doc = dict(question="H-ACC30: Naver's head-unit accuracy validity boundary is exactly 30 m (valid < 30)",
               source=dict(commands=f"validation/DHU_NAVER_EXP11_PLAN_2026-10-11/commands/{SOURCE_TAG}.txt", sha256=SOURCE_SHA256,
                           tier0=f"validation/DHU_NAVER_EXP11_PLAN_2026-10-11/commands/{T0_SOURCE_TAG}.txt (tag only)",
                           prep="validation/DHU_NAVER_EXP11_PLAN_2026-10-11/commands/prep (byte copies of exp 10 v5)"),
               ini=dict(path="validation/DHU_NAVER_EXP4_2026-10-03/dhu.ini", sha256=EXP4_INI_SHA256,
                        note="the ini of exp 11 V4-r1; exp 11 v4 rawgps only stated the DHU default"),
               site=p11["site"], timeline=p11["timeline"], order=order, tier1_required=TIER1_REQUIRED,
               budget_min=BUDGET_MIN, env_min=ENV_MIN, tier0_min=list(TIER0_MIN), run_min=RUN_MIN,
               cumulative_estimate_min=dict(zip(order, est)),
               v4_reference=dict(grey_onset_tau=GREY_ONSET_V4, runs=["exp11 V4-r1 (exp4 ini) 85", "exp11v4 V4R-r1 65", "exp11v4 V4R-r2 85"],
                                 speed="followed 12/12 in all three"),
               labels=dict(grey_onset_like_v4_tau=GREY_ONSET_LIKE_V4, late_from_tau=LATE_FROM_TAU),
               runs=runs)
    if plan_path:
        with open(plan_path, "w") as f:
            json.dump(doc, f, ensure_ascii=False, indent=1)
            f.write("\n")
    return doc


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(HERE, "commands")
    plan = argv[2] if len(argv) > 2 else os.path.join(HERE, "run-plan.json")
    doc = generate(out, plan)
    for tag in doc["order"]:
        r = doc["runs"][tag]
        print(f"{tag}: {r['duration_s']} s, tunnel accuracy {r['accuracy_tunnel']}, photos {len(r['frames'])}, "
              f"cumulative estimate {doc['cumulative_estimate_min'][tag]} min")


if __name__ == "__main__":
    main(sys.argv)
