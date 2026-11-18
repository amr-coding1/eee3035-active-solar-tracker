"""Energy budget for the Active Solar Tracker from PVGIS 5.3 (JRC) for Guildford.

Inputs: docs/energy/pvgis_Ed_per_kWp_guildford.json, fetched 13 Nov 2026 from
https://re.jrc.ec.europa.eu/api/v5_3/PVcalc (lat 51.243, lon -0.589, 1 kWp,
14 % system loss, SARAH-3 radiation) for five mountings. Scaled to the 5.4 Wp
DFRobot FIT0330 panel. Electronics bands are estimates to be replaced by the
bench measurement (INA226 channel 3).
Run: python3 docs/energy/energy_budget.py
"""
import json, pathlib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = pathlib.Path(__file__).resolve().parent
PANEL_WP = 5.4
d = json.load(open(HERE / "pvgis_Ed_per_kWp_guildford.json"))
series = {  # label: (json key, colour, linestyle, marker)
    "Fixed, flat (0°)": ("fixed_flat|fixed", "#D55E00", "--", "^"),
    "Fixed, optimal tilt (39°, south)": ("fixed_opt|fixed", "#E69F00", "-.", "s"),
    "Tracker, horizontal N-S axis": ("track_horiz_NS|inclined_axis", "#56B4E9", ":", "D"),
    "Tracker, inclined N-S axis (41°)": ("track_incl_opt|inclined_axis", "#0072B2", "-", "o"),
    "Two-axis tracker (upper bound)": ("track_2axis|two_axis", "#000000", (0, (1, 1)), "v"),
}
months = list(range(1, 13))
mname = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
wh = {lab: [d[key][str(m)] * PANEL_WP for m in months] for lab, (key, *_) in series.items()}

# Electronics consumption bands (Wh/day): 24 h operation
bands = [
    ("MCU always awake ~40 mA at 5 V", 0.040 * 5 * 24),
    ("MCU awake at 80 MHz, radio off ~20 mA at 3.3 V", 0.020 * 3.3 * 24),
    ("Target: light sleep between samples ~3 mA at 3.3 V", 0.003 * 3.3 * 24),
]

fig, ax = plt.subplots(figsize=(8.6, 6.2), dpi=150)
for lab, (key, col, ls, mk) in series.items():
    ax.plot(months, wh[lab], color=col, linestyle=ls, marker=mk, linewidth=2, label=lab)
for i, (lab, v) in enumerate(bands):
    ax.axhline(v, color="0.35", linewidth=1.2, linestyle=[(0, (6, 3)), (0, (3, 3)), (0, (1, 3))][i])
    ax.text(6.5, v + 0.25, f"{lab}: {v:.1f} Wh/day", ha="center", va="bottom", fontsize=7.5, color="0.2",
            bbox=dict(boxstyle="round,pad=0.15", fc="white", ec="none", alpha=0.85))
ax.axvspan(10.5, 12.5, color="0.9", zorder=0); ax.text(11.5, max(wh['Two-axis tracker (upper bound)']) * 0.97, "test\nperiod", ha="center", fontsize=8)
ax.axvspan(0.5, 1.5, color="0.9", zorder=0)
ax.set_xticks(months); ax.set_xticklabels(mname); ax.set_xlim(0.5, 12.5)
ax.set_ylabel("Energy per day for a 5.4 W panel (Wh/day)"); ax.set_xlabel("Month (PVGIS 5.3 long-term mean, Guildford 51.24° N)")
ax.set_title("Expected daily energy: tracking gain vs electronics consumption", fontsize=11)
ax.grid(True, color="0.88"); ax.legend(fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.13), ncol=2, frameon=False)
fig.subplots_adjust(bottom=0.3)
fig.savefig(HERE.parent / "figures" / "energy_budget_pvgis.png"); fig.savefig(HERE.parent / "figures" / "energy_budget_pvgis.svg")

def row(m):
    i = m - 1
    flat, opt, hz, inc = (wh[k][i] for k in list(series)[:4])
    return flat, opt, hz, inc
lines = ["| Month | Fixed flat | Fixed 39° | Horizontal-axis tracker | Inclined-axis tracker | Gain vs flat (incl.) | Gain vs 39° (incl.) |", "|---|---|---|---|---|---|---|"]
for m in (11, 12, 1, 6):
    flat, opt, hz, inc = row(m)
    lines.append(f"| {mname[m-1]} | {flat:.2f} | {opt:.2f} | {hz:.2f} | {inc:.2f} | +{inc-flat:.2f} Wh (+{100*(inc/flat-1):.0f} %) | +{inc-opt:.2f} Wh (+{100*(inc/opt-1):.0f} %) |")
print("\n".join(lines))
json.dump({"wh_per_day": wh, "bands": bands}, open(HERE / "energy_budget_numbers.json", "w"), indent=1)

# Net gain month by month (P_use is everything the tracker draws: Lem, 17 Nov 2026).
# Reference: a fixed panel at the tracker's tilt. Motor energy is not included yet.
days = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
trk = wh["Tracker, inclined N-S axis (41°)"]
fix = wh["Fixed, optimal tilt (39°, south)"]
uses = [  # label, Wh/day, colour, linestyle, marker
    ("board always awake, Wi-Fi on, ~4.8 Wh/day", 0.040 * 5 * 24, "#D55E00", "--", "^"),
    ("data link as built (radio off between uploads), ~2.3 Wh/day", 2.3, "#0072B2", "-", "s"),
    ("light sleep between samples (target), ~0.24 Wh/day", 0.003 * 3.3 * 24, "#009E73", "-.", "D"),
]
pct = lambda use: [100 * (t - f - use) / f for t, f in zip(trk, fix)]
year = lambda use: 100 * sum((t - f - use) * n for t, f, n in zip(trk, fix, days)) / sum(f * n for f, n in zip(fix, days))
fig2, ax2 = plt.subplots(figsize=(8.6, 5.8), dpi=150)
ax2.axvspan(10.5, 12.5, color="0.92", zorder=0); ax2.axvspan(0.5, 1.5, color="0.92", zorder=0)
ax2.axhline(0, color="0.15", linewidth=1.3, zorder=1)
ax2.plot(months, pct(0.0), color="black", linewidth=2.2, marker="o", label=f"Gross gain, tracking only ({year(0.0):+.0f} % over a year)")
for lab, use, col, ls, mk in uses:
    ax2.plot(months, pct(use), color=col, linestyle=ls, marker=mk, linewidth=2, label=f"Net gain, {lab} ({year(use):+.0f} % over a year)")
ax2.text(11.5, ax2.get_ylim()[1] - 2, "lab tests", ha="center", va="top", fontsize=8)
ax2.text(1, ax2.get_ylim()[1] - 2, "demo", ha="center", va="top", fontsize=8)
ax2.set_xticks(months); ax2.set_xticklabels(mname); ax2.set_xlim(0.5, 12.5)
ax2.set_ylabel("Energy gain over a fixed panel at the same tilt (%)")
ax2.set_xlabel("Month (PVGIS 5.3 long-term mean, Guildford 51.24° N; 5.4 W panel; motor not included)")
ax2.set_title("Tracker gain over a fixed panel at the same tilt, month by month\n"
              "net gain = (E_tracked \u2212 E_fixed \u2212 E_use) / E_fixed, E_use = everything the tracker draws", fontsize=10)
ax2.grid(True, color="0.88"); ax2.legend(fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.14), ncol=1, frameon=False)
fig2.subplots_adjust(bottom=0.33)
fig2.savefig(HERE.parent / "figures" / "net_gain_by_month.png", bbox_inches="tight")
fig2.savefig(HERE.parent / "figures" / "net_gain_by_month.svg", bbox_inches="tight", metadata={"Date": None})

hdr = "| Month | Tracked (Wh/day) | Fixed at same tilt (Wh/day) | Gross gain | Net, always awake | Net, data link as built | Net, light sleep |"
md = ["# Net gain by month (generated by energy_budget.py)", "",
      "PVGIS 5.3, Guildford, 5.4 W panel; inclined-axis tracker vs a fixed panel at the same tilt.",
      "Net gain counts everything the tracker draws (P_use, agreed 17 Nov 2026); the motor is not included yet,",
      "so replace the E_use estimates with the bench measurement (INA226 0x44) when the hardware runs.", "",
      hdr, "|---|---|---|---|---|---|---|"]
for i in range(12):
    cells = [f"{100 * (trk[i] - fix[i] - u) / fix[i]:+.0f} %" for u in (0.0, uses[0][1], uses[1][1], uses[2][1])]
    md.append(f"| {mname[i]} | {trk[i]:.1f} | {fix[i]:.1f} | " + " | ".join(cells) + " |")
md.append("| **Year** | " + f"{sum(t * n for t, n in zip(trk, days)) / 365:.1f} | {sum(f * n for f, n in zip(fix, days)) / 365:.1f} | "
          + " | ".join(f"**{year(u):+.0f} %**" for u in (0.0, uses[0][1], uses[1][1], uses[2][1])) + " |")
(HERE / "net_gain_by_month.md").write_text("\n".join(md) + "\n")
print("\n".join(md[6:]))
