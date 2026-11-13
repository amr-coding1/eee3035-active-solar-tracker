"""Weighted decision matrix for the data host (Week 2b method), with a
weight-sensitivity sweep, a score-uncertainty Monte Carlo and a spider plot.
Run: python3 docs/decisions/data_host_matrix.py  (writes the .md table + figure)
Scores 1-5; every score has a reason in REASONS so the matrix can be audited."""
import itertools, json, random, pathlib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
CRITERIA = [  # (key, label, weight)
    ("brief", "Client brief fit: free, HTTPS, authenticated writes", 0.20),
    ("analysis", "Built-in analysis and charts (MATLAB, report figures)", 0.20),
    ("backfill", "Offline back-fill with device timestamps (F7)", 0.15),
    ("integration", "ESP32 integration effort and team familiarity", 0.15),
    ("capacity", "Free-tier capacity and retention for our load", 0.10),
    ("governance", "Data governance: region, export, deletion (DMP)", 0.10),
    ("access", "Phone dashboard access: CORS, public read, embeds", 0.10),
]
SCORES = {
    "ThingSpeak":        dict(brief=5, analysis=5, backfill=4, integration=5, capacity=4, governance=3, access=5),
    "Supabase":          dict(brief=4, analysis=2, backfill=5, integration=3, capacity=5, governance=5, access=4),
    "Firebase RTDB":     dict(brief=4, analysis=1, backfill=4, integration=4, capacity=4, governance=3, access=4),
    "Adafruit IO":       dict(brief=4, analysis=2, backfill=2, integration=5, capacity=1, governance=3, access=4),
    "Google Sheets + Apps Script": dict(brief=3, analysis=2, backfill=3, integration=2, capacity=2, governance=3, access=3),
}
REASONS = {
    "ThingSpeak": "Free licence, HTTPS, per-channel write key; MATLAB Analysis + Visualizations built in (Lem's design); bulk_update.json takes created_at, 960 entries/request, 15 s spacing; MathWorks ESP32 examples; 3 M msgs/yr vs our ~0.53 M (18 %); US-hosted, CSV export and clear-channel available; public channels with CORS * and iframe charts.",
    "Supabase": "Free tier but projects pause after a week of inactivity; Postgres only, analysis and charts are our own code; any timestamp insertable in batches; REST from ESP32 needs auth headers and row-level-security setup; 500 MB; London region (best governance); anon-key REST reads, no ready-made charts.",
    "Firebase RTDB": "Free Spark plan, HTTPS; no analysis or charting; timestamps are our own keys; community ESP32 library; 1 GB storage; region selectable but Google account and rules to manage; REST reads.",
    "Adafruit IO": "Free tier, HTTPS, easiest Arduino library; simple dashboards only; free plan limited to 30 data points/min and 30 days of storage (fails the >7-day and report-archive needs).",
    "Google Sheets + Apps Script": "Free; Apps Script web-app redirects are awkward from an ESP32 over HTTPS; Sheets charts only; quota limits; university uses Microsoft 365, so accounts and governance sit outside the DMP.",
}

def totals(weights):
    w = np.array(weights) / np.sum(weights)
    return {opt: float(sum(w[i] * s[c[0]] for i, c in enumerate(CRITERIA))) for opt, s in SCORES.items()}

base_w = [c[2] for c in CRITERIA]
base = totals(base_w)
ranking = sorted(base.items(), key=lambda kv: -kv[1])

# 1) weight sensitivity: each weight x0.5, x1, x1.5 (3^7 = 2187 combinations), renormalised
wins = {o: 0 for o in SCORES}; margins = []
for f in itertools.product([0.5, 1.0, 1.5], repeat=len(CRITERIA)):
    t = totals([b * k for b, k in zip(base_w, f)])
    r = sorted(t.items(), key=lambda kv: -kv[1])
    wins[r[0][0]] += 1
    margins.append(t["ThingSpeak"] - max(v for k, v in t.items() if k != "ThingSpeak"))
n_combo = 3 ** len(CRITERIA)

# 2) score uncertainty: every score +/-1 (uniform, clipped 1..5), 20 000 draws
random.seed(3035)
mc_first = {o: 0 for o in SCORES}; N = 20000
w = np.array(base_w) / sum(base_w)
for _ in range(N):
    t = {}
    for opt, s in SCORES.items():
        t[opt] = sum(w[i] * min(5, max(1, s[c[0]] + random.choice((-1, 0, 1)))) for i, c in enumerate(CRITERIA))
    mc_first[max(t, key=t.get)] += 1

# 3) spider plot (Okabe-Ito colours + distinct line styles and markers: colour is not the only cue)
labels = [c[0] for c in CRITERIA]
ang = np.linspace(0, 2 * np.pi, len(labels), endpoint=False).tolist(); ang += ang[:1]
style = [("#0072B2", "-", "o"), ("#D55E00", "--", "^"), ("#009E73", "-.", "s"), ("#CC79A7", ":", "D"), ("#000000", (0, (5, 1, 1, 1)), "v")]
fig = plt.figure(figsize=(7.2, 6.4), dpi=150); ax = plt.subplot(111, polar=True)
for (opt, s), (col, ls, mk) in zip(SCORES.items(), style):
    v = [s[k] for k in labels]; v += v[:1]
    ax.plot(ang, v, color=col, linestyle=ls, marker=mk, linewidth=2, label=f"{opt} ({base[opt]:.2f})")
    if opt == "ThingSpeak": ax.fill(ang, v, color=col, alpha=0.12)
ax.set_xticks(ang[:-1]); ax.set_xticklabels(["Brief fit", "Analysis", "Back-fill", "Integration", "Capacity", "Governance", "Phone access"], fontsize=10)
ax.set_yticks([1, 2, 3, 4, 5]); ax.set_ylim(0, 5); ax.set_rlabel_position(90)
ax.set_title("Data host options: raw scores (1-5)\nlegend shows the weighted total", fontsize=11, pad=24)
ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.06), ncol=2, fontsize=9, frameon=False)
fig.subplots_adjust(top=0.86, bottom=0.2); fig.savefig(HERE.parent / "figures" / "data_host_radar.png"); fig.savefig(HERE.parent / "figures" / "data_host_radar.svg")

# Markdown output
md = ["# Decision matrix: data host (AM, 13 Nov 2026)", "",
      "Method: Week 2b weighted decision matrix. Scores 1 (poor) to 5 (excellent); weights sum to 1. "
      "Weights come from the client brief (free, secure, timed transmission), Lem's design (MATLAB on ThingSpeak) and requirements F7-F9.", "",
      "| Criterion | Weight | " + " | ".join(SCORES) + " |", "|---|---|" + "---|" * len(SCORES)]
for key, label, wt in CRITERIA:
    md.append(f"| {label} | {wt:.2f} | " + " | ".join(str(SCORES[o][key]) for o in SCORES) + " |")
md.append("| **Weighted total** | 1.00 | " + " | ".join(f"**{base[o]:.2f}**" for o in SCORES) + " |")
md += ["", "Ranking: " + ", ".join(f"{i+1}. {o} ({v:.2f})" for i, (o, v) in enumerate(ranking)), "",
       "## Sensitivity", "",
       f"- Weights: every criterion weight scaled by 0.5, 1 or 1.5 independently and renormalised ({n_combo} combinations): "
       + ", ".join(f"{o} first in {wins[o]}" for o in SCORES if wins[o]) + f". Smallest ThingSpeak lead: {min(margins):+.2f} points; median lead {float(np.median(margins)):+.2f}.",
       f"- Scores: every score moved by -1, 0 or +1 at random (20,000 draws): " + ", ".join(f"{o} first in {100*mc_first[o]/N:.1f} %" for o in SCORES if mc_first[o]) + ".",
       "", "![Spider plot](../figures/data_host_radar.png)", "", "## Reasons for each score", ""]
md += [f"- **{o}**: {REASONS[o]}" for o in SCORES]
md += ["", "## Consequence for the design", "",
       "ThingSpeak is chosen; Supabase (the 27 Oct DMP draft) is replaced. The DMP is updated (docs/DMP_v2.md). "
       "Main ThingSpeak weakness: US hosting and no automatic backup -> mitigated by weekly CSV export to the group OneDrive and the device's 8-day buffer."]
(HERE / "data_host_matrix.md").write_text("\n".join(md) + "\n")
print(json.dumps({"base": base, "wins": wins, "min_margin": min(margins), "mc_first_pct": {o: 100 * v / N for o, v in mc_first.items()}}, indent=1))
