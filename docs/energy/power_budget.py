#!/usr/bin/env python3
"""Bottom-up power budget and break-even analysis: EEE3035 Group 8, Active Solar Tracker.

Question (marker, design brief): can the panel pay for its own movement?
    net gain >= 0   <=>   E_use <= E_tracked - E_fixed          (per day, per month)
P_use is everything the tracker draws (MCU with Wi-Fi, sensors, driver, motor),
measured by INA226 #3 (0x44) on the tracker supply line (group decision 17 Nov 2026).

Run:  python3 power_budget.py [path/to/repo/docs/energy]
Writes next to this file: power_budget.md, power_budget_numbers.json,
break_even_by_month.png. Needs only the standard library, numpy and matplotlib.

Every current below is tagged DS (datasheet, cited in SOURCES), MEAS (third-party
measurement, cited) or ASSUMED (no datasheet value found: range given, to be
replaced by the bench measurement on 26 Nov 2026).
"""
import json
import pathlib
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = pathlib.Path(__file__).resolve().parent
REPO_ENERGY = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE

# --------------------------------------------------------------------------- sources
SOURCES = {
    "S3": ("Espressif, ESP32-S3 Series Datasheet v2.2, Tables 5-7, 5-9, 5-10",
           "https://documentation.espressif.com/esp32-s3_datasheet_en.pdf"),
    "NANO": ("Arduino, Nano ESP32 datasheet ABX00083, sections 3, 11.3, features page",
             "https://docs.arduino.cc/resources/datasheets/ABX00083-datasheet.pdf"),
    "FORUM": ("Arduino Forum, 'Power hungry in deep sleep' (Nano ESP32, user measurements, 2023-2025)",
              "https://forum.arduino.cc/t/power-hungry-in-deep-sleep/1204775"),
    "INA": ("Texas Instruments, INA226 datasheet SBOS547, section 5.5 Electrical Characteristics",
            "https://www.ti.com/lit/ds/symlink/ina226.pdf"),
    "DRV": ("Texas Instruments, DRV8833 datasheet SLVSAR1E, sections 6.3 and 6.5",
            "https://www.ti.com/lit/ds/symlink/drv8833.pdf"),
    "RTC": ("Analog Devices (Maxim), DS3231 datasheet 19-5170 Rev 10, Electrical Characteristics",
            "https://www.analog.com/media/en/technical-documentation/data-sheets/DS3231.pdf"),
    "BPX": ("OSRAM Opto Semiconductors, BPX 65 datasheet v1.2 (2015-08-26), Characteristics; ams OSRAM product page",
            "https://www.changpuak.ch/electronics/PhotoDiodes/BPX65.pdf ; "
            "https://ams-osram.com/products/photodetectors/photodiodes/osram-metal-can-to18-bpx-65"),
    "WS": ("Waveshare wiki, Solar Power Manager, Specifications (base model)",
           "https://www.waveshare.com/wiki/Solar_Power_Manager"),
    "M6": ("JGY-370-CE 6 V worm gearmotor listing (18 rpm, no-load 0.08 A, self-locking)",
           "https://www.crcibernetica.com/high-torque-worm-gear-motor-6v"),
    "M12": ("Aslong JGY-370 12 V worm gearmotor listing (no-load 35 mA, load 180 mA, stall 1 A)",
            "https://core-electronics.com.au/aslong-jgy-370-12v-dc-worm-gear-motor-seeed-studio.html"),
    "REPO": ("Group repository: firmware/README.md 'Energy budget', docs/CONTRACT.md 7.1-7.3, "
             "firmware/core/ast_sim.h, docs/energy/", "(local)"),
}

# ------------------------------------------------------------------- component values
# (low, central, high). Central is what the scenarios use.
V33, V5, VBAT = 3.3, 5.0, 3.7

# ESP32-S3, radio off ("modem-sleep" in the datasheet), mA at 3.3 V   [DS S3 Table 5-9]
I_MS80 = (22.0, 29.0, 36.1)     # 80 MHz, both cores idle (WAITI): 22.0 clocks off, 36.1 all peripheral clocks on;
                                # central = midpoint (ASSUMED position inside the DS range)
I_MS80_RUN = 42.6               # 80 MHz, one core running, clocks on (DS) - upper bound while code runs
I_MS240 = (32.9, 47.6, 47.6)    # 240 MHz WAITI: 32.9 clocks off, 47.6 clocks on (DS)
I_RX = 88.0                     # Wi-Fi RX, 802.11b/g/n HT20                      [DS S3 Table 5-7]
I_TX_PEAK = (283.0, 340.0)      # Wi-Fi TX peak, 11n MCS7 .. 11b 1 Mbps           [DS S3 Table 5-7]
I_LS_CHIP = 0.240               # light sleep, chip only                           [DS S3 Table 5-10]
I_DS_CHIP = 0.008               # deep sleep, RTC memory + RTC peripherals         [DS S3 Table 5-10]
# Nano ESP32 BOARD in sleep, mA at 3.3 V / VIN                                    [MEAS FORUM]
I_LS_BOARD = (0.56, 1.27, 1.27)  # light sleep, two users
I_DS_BOARD = (1.75, 1.78, 2.6)   # deep sleep, three users (one saw ~4 mA) - HIGHER than light sleep
# Upload session (radio on, 160 MHz, connect + TLS + POST): mean current, mA at 3.3 V
I_UPLOAD = (100.0, 125.0, 150.0)  # ASSUMED (repo figure); bounded by DS: RX 88 mA, TX peaks 283-340 mA
T_UPLOAD_S = 9.0                  # 8 s radio lead (kRadioLeadS) + ~1 s request    [REPO]
# Always-connected Wi-Fi at 240 MHz: 47.6 mA floor (radio clock-gated) .. 88 mA (receiver always on).
RX_DUTY_CONNECTED = 0.10          # ASSUMED share of time the receiver is on with modem power-save (beacons)
I_WIFI_ON = (I_MS240[1], I_MS240[1] + RX_DUTY_CONNECTED * (I_RX - I_MS240[1]), I_RX)

I_INA = (0.330, 0.330, 0.420)     # per INA226, continuous conversion (typ, typ, max) [DS INA 5.5]
I_INA_PD = 0.0005                 # INA226 power-down, typ (max 0.002)               [DS INA 5.5]
N_INA = 3
I_RTC = (0.110, 0.200, 0.200)     # DS3231 at 3.63 V: standby 110 uA, I2C active 200 uA [DS RTC]
I_LED = (0.5, 1.0, 2.0)           # green power LED, hard-wired. NOT in the datasheet: ASSUMED, measure
# BPX65 arc. Count and circuit are NOT in the repository: ASSUMED 5 diodes, reverse-biased
# from 3V3 through a load resistor. Photocurrent 10 uA typ at 1000 lx (DS BPX), i.e. it only
# costs supply current when biased and lit. Allowance for the daytime mean per diode:
N_PD = 5
I_PD_DAY = (0.01, 0.2, 1.0)       # mA per diode in daylight (0.01 = 1 klx overcast; 1.0 = ~100 klx if not resistor-limited)
DAYLIGHT_H = 10.0                 # hours per day the arc is lit (annual mean, ASSUMED)

I_DRV_ON = (1.7, 1.7, 3.0)        # DRV8833 awake, outputs off, mA at VM = 5 V     [DS DRV 6.5]
I_DRV_SLEEP = (0.0016, 0.0016, 0.0025)  # DRV8833 nSLEEP low, mA at 5 V            [DS DRV 6.5]
R_DRV = 0.36                      # HS + LS on-resistance at 5 V, ohm (200 + 160 mohm) [DS DRV 6.5]

# Converters
ETA_BUCK = (0.85, 0.90, 0.92)       # MP2322 on the Nano, 5 V in, "normal" load: ">90 % at 4.5 V"  [DS NANO 11.3]
ETA_BUCK_LIGHT = (0.60, 0.80, 0.90)  # same converter at a few mA: NOT published in the Arduino datasheet, ASSUMED
ETA_BOOST = 0.86                    # Solar Power Manager, battery -> 5 V "approx. 86 %"     [DS WS]
I_Q_MANAGER = 2.0                   # Solar Power Manager quiescent "< 2 mA" (a maximum)     [DS WS]

# Motor: worm-gear DC motor, exact part not in the repository/BOM. Range from JGY-370 listings.
V_MOTOR = 5.0
I_MOTOR = (0.08, 0.20, 1.0)       # A while moving: no-load 6 V type [M6]; load current 12 V type 180 mA [M12]; stall 1 A [M12]
OMEGA = (0.5, 2.0, 6.0)           # deg/s at the panel axis: 2.0 is the repo simulator default [REPO]; ASSUMED range
I_START, T_START = 1.0, 0.1       # start surge: stall current [M12] for an ASSUMED 0.1 s per start
LIMIT_DEG = 60.0                  # mechanical travel either side of level [REPO ast_sim.h]
SUN_RATE = 15.0                   # deg/h about a polar-ish axis
TRAVEL_DEG = 4 * LIMIT_DEG        # 120 deg sweep + 120 deg return per day = 240 deg
SECONDS = 86400.0

# --------------------------------------------------------------------------- PVGIS
MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
DAYS = np.array([31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31])
PANEL_WP = 5.4
# Fallback copy of docs/energy/pvgis_Ed_per_kWp_guildford.json (kWh/kWp/day, PVGIS 5.3, SARAH-3,
# Guildford 51.243 N, 14 % system loss), used when the repository is not present.
PVGIS_FALLBACK = {
    "fixed_flat|fixed": [0.59, 1.13, 2.12, 3.4, 3.92, 4.31, 4.14, 3.39, 2.58, 1.45, 0.76, 0.46],
    "fixed_opt|fixed": [1.32, 1.94, 2.85, 3.88, 3.92, 4.12, 4.03, 3.59, 3.23, 2.24, 1.6, 1.11],
    "track_incl_opt|inclined_axis": [1.51, 2.32, 3.59, 5.14, 5.28, 5.53, 5.46, 4.8, 4.19, 2.75, 1.87, 1.24],
}


def load_pvgis():
    f = REPO_ENERGY / "pvgis_Ed_per_kWp_guildford.json"
    if f.exists():
        d = json.load(open(f))
        got = {k: [d[k][str(m)] for m in range(1, 13)] for k in PVGIS_FALLBACK}
        assert got == PVGIS_FALLBACK, "repository PVGIS table differs from the copy in this script"
        return got, "docs/energy/" + f.name
    return PVGIS_FALLBACK, "embedded copy"


# --------------------------------------------------------------------------- model
def mode_power_5v(i_mcu_ma, i_33_other_ma, eta):
    """mW at the 5 V rail for loads on the Nano's 3V3 rail."""
    return (i_mcu_ma + i_33_other_ma) * V33 / eta


def motor_energy_wh(interval_min, i_a=I_MOTOR[1], omega=OMEGA[1], i_mcu=I_MS80[1]):
    """Daily motor energy at the 5 V rail (Wh/day) for step tracking every `interval_min`.

    Worm gear is self-locking: no holding current. Driver awake only while moving.
    """
    t_run = TRAVEL_DEG / omega                               # s/day, independent of the interval
    sweep_h = 2 * LIMIT_DEG / SUN_RATE                       # 8 h to sweep 120 deg
    n_moves = int(round(sweep_h * 60 / interval_min)) + 1    # + one return move
    e_run = V_MOTOR * i_a * t_run                            # J (includes the DRV8833 I^2R, it is in series)
    e_start = n_moves * V_MOTOR * max(I_START - i_a, 0.0) * T_START
    e_drv = V5 * I_DRV_ON[1] / 1000 * (t_run + n_moves * 0.05)
    e_mcu = (i_mcu * V33 / ETA_BUCK[1] / 1000) * t_run       # MCU must be awake while the motor runs
    step_deg = SUN_RATE * interval_min / 60
    half = np.radians(step_deg / 2)
    cos_loss = 1 - np.sin(half) / half                       # mean beam loss, error uniform in +/- step/2
    return {"interval_min": interval_min, "moves_per_day": n_moves, "step_deg": step_deg,
            "run_s_per_day": t_run, "E_Wh": (e_run + e_start + e_drv + e_mcu) / 3600,
            "E_run_J": e_run, "E_start_J": e_start, "E_drv_J": e_drv, "E_mcu_J": e_mcu,
            "cos_loss_pct": 100 * cos_loss, "drv_loss_mW_while_running": 1000 * i_a ** 2 * R_DRV}


def electronics(name, parts, note):
    """parts: list of (label, mW at 5 V rail, duty 0..1). Returns dict with mean mW and Wh/day."""
    rows = [{"mode": l, "P_mW": p, "duty": d, "mean_mW": p * d, "Wh_day": p * d * 24 / 1000} for l, p, d in parts]
    mean = sum(r["mean_mW"] for r in rows)
    return {"name": name, "note": note, "rows": rows, "mean_mW": mean, "Wh_day": mean * 24 / 1000}


def build_scenarios():
    sens = N_INA * I_INA[1] + I_RTC[1]                  # 1.19 mA: 3x INA226 continuous + DS3231 (repo uses 1.2)
    sens_sleep = N_INA * I_INA[1] + I_RTC[0]            # DS3231 standby while the bus is idle
    led = I_LED[1]
    pd_ungated = N_PD * I_PD_DAY[1] * DAYLIGHT_H / 24   # 24 h mean, arc biased all the time
    drv_on = V5 * I_DRV_ON[1]                           # mW
    drv_sleep = V5 * I_DRV_SLEEP[1]
    n_up15, n_up60 = 96, 24
    d_up15, d_up60 = n_up15 * T_UPLOAD_S / SECONDS, n_up60 * T_UPLOAD_S / SECONDS
    eb, el = ETA_BUCK[1], ETA_BUCK_LIGHT[1]
    sc = {}

    # (a) always awake, 240 MHz, Wi-Fi connected all the time, nothing gated
    sc["a"] = electronics(
        "(a) always awake, Wi-Fi connected",
        [("MCU 240 MHz, Wi-Fi associated (modem power-save)", mode_power_5v(I_WIFI_ON[1], 0, eb), 1.0),
         ("3x INA226 + DS3231", mode_power_5v(0, sens, eb), 1.0),
         ("power LED", mode_power_5v(0, led, eb), 1.0),
         ("BPX65 arc, biased continuously", mode_power_5v(0, pd_ungated, eb), 1.0),
         ("DRV8833 left awake", drv_on, 1.0)],
        "240 MHz, Wi-Fi never switched off, driver never put to sleep")

    # (b) data link as built: 80 MHz, radio off between 15-min uploads, no sleep
    sc["b"] = electronics(
        "(b) data link as built (radio off between uploads)",
        [("MCU 80 MHz, radio off", mode_power_5v(I_MS80[1], 0, eb), 1 - d_up15),
         ("upload session 96/day x 9 s, 160 MHz + radio", mode_power_5v(I_UPLOAD[1], 0, eb), d_up15),
         ("3x INA226 + DS3231", mode_power_5v(0, sens, eb), 1.0),
         ("power LED", mode_power_5v(0, led, eb), 1.0),
         ("BPX65 arc, biased continuously", mode_power_5v(0, pd_ungated, eb), 1.0),
         ("DRV8833 left awake", drv_on, 1.0)],
        "firmware of 13 Nov 2026 plus a control loop that never sleeps the driver")

    # (c) light sleep; wake every 15 s for 0.25 s (4 reads/min keep a true minute mean with
    #     INA226 1024x averaging); uploads still every 15 min (requirement F8 unchanged)
    d_wake = (0.25 / 15.0)
    sc["c"] = electronics(
        "(c) light sleep, wake 4x/min, upload every 15 min",
        [("board in light sleep", mode_power_5v(I_LS_BOARD[1], 0, el), 1 - d_wake - d_up15),
         ("wake 0.25 s every 15 s, 80 MHz", mode_power_5v(I_MS80[1], 0, eb), d_wake),
         ("upload session 96/day x 9 s", mode_power_5v(I_UPLOAD[1], 0, eb), d_up15),
         ("3x INA226 (continuous) + DS3231 standby", mode_power_5v(0, sens_sleep, el), 1.0),
         ("power LED", mode_power_5v(0, led, el), 1.0),
         ("BPX65 arc, bias gated to the wake window", mode_power_5v(0, pd_ungated * d_wake, el), 1.0),
         ("DRV8833 asleep (nSLEEP low)", drv_sleep, 1.0)],
        "explicit esp_light_sleep_start() in the sampler; driver and arc gated")

    # (d) aggressive: as (c) but hourly uploads, power LED removed, 1 wake/min at night
    night = 1 - DAYLIGHT_H / 24
    d_wake_d = (DAYLIGHT_H / 24) * (0.25 / 15.0) + night * (0.25 / 60.0)
    sc["d"] = electronics(
        "(d) aggressive: light sleep, hourly upload, LED cut, slow at night",
        [("board in light sleep", mode_power_5v(I_LS_BOARD[1], 0, el), 1 - d_wake_d - d_up60),
         ("wake 0.25 s: every 15 s by day, every 60 s at night", mode_power_5v(I_MS80[1], 0, eb), d_wake_d),
         ("upload session 24/day x 9 s", mode_power_5v(I_UPLOAD[1], 0, eb), d_up60),
         ("3x INA226 (continuous) + DS3231 standby", mode_power_5v(0, sens_sleep, el), 1.0),
         ("BPX65 arc, bias gated to the wake window", mode_power_5v(0, pd_ungated * d_wake_d, el), 1.0),
         ("DRV8833 asleep (nSLEEP low)", drv_sleep, 1.0)],
        "needs F8 relaxed from 15 to 60 min and the LED track cut")

    # (d-deep) deep sleep variant, shown to explain why it is NOT recommended on this board
    t_boot, i_boot = 0.5, I_MS80_RUN        # ASSUMED 0.5 s from reset to sample taken (boot at 42.6 mA, DS upper)
    d_boot = t_boot / 60.0
    sc["d_deep"] = electronics(
        "(d-deep) deep sleep between 1-min samples, hourly upload, LED cut",
        [("board in deep sleep", mode_power_5v(I_DS_BOARD[1], 0, el), 1 - d_boot - d_up60),
         ("cold boot + sample, 0.5 s per minute", mode_power_5v(i_boot, 0, eb), d_boot),
         ("upload session 24/day x 9 s", mode_power_5v(I_UPLOAD[1], 0, eb), d_up60),
         ("3x INA226 (continuous) + DS3231 standby", mode_power_5v(0, sens_sleep, el), 1.0),
         ("DRV8833 asleep (nSLEEP low)", drv_sleep, 1.0)],
        "board deep-sleep current measured by users (1.75-2.6 mA) exceeds light sleep (0.56-1.27 mA)")
    return sc


def repo_as_built_reproduction():
    """Reproduce firmware/README.md 'Energy budget' (2.3 Wh/day) with its own assumptions."""
    rows = [("CPU 80 MHz, radio off", 27.0, 85500.0),
            ("Upload session: radio on, 160 MHz (96/day x 9 s)", 125.0, 864.0),
            ("3x INA226 + DS3231", 1.2, 86400.0)]
    out = [{"mode": l, "mA_at_3V3": i, "s_per_day": s, "Wh_day": i / 1000 * V33 * s / 3600} for l, i, s in rows]
    return out, sum(r["Wh_day"] for r in out)


# --------------------------------------------------------------------------- main
def main():
    pv, pv_src = load_pvgis()
    trk = np.array(pv["track_incl_opt|inclined_axis"]) * PANEL_WP
    fix = np.array(pv["fixed_opt|fixed"]) * PANEL_WP
    flat = np.array(pv["fixed_flat|fixed"]) * PANEL_WP
    budget = trk - fix                       # Wh/day the tracker may spend (reference at same tilt)
    budget_flat = trk - flat                 # sensitivity: reference panel flat
    budget_mw = budget / 24 * 1000
    year = lambda arr: float((arr * DAYS).sum() / 365)

    # --- repo E_use assumptions (cross-check against docs/energy/net_gain_by_month.md)
    repo_rows, repo_as_built = repo_as_built_reproduction()
    repo_use = {"always awake": 0.040 * 5 * 24, "data link as built": 2.3, "light sleep": 0.003 * 3.3 * 24}
    pct = lambda use, ref=fix, b=None: 100 * ((trk - ref) - use) / ref
    fmt = lambda a: [f"{x:+.0f} %" for x in a]
    mine = {"Gross gain": fmt(pct(0.0))}
    for k, u in repo_use.items():
        mine[k] = fmt(pct(u))
    check = {"file": "docs/energy/net_gain_by_month.md", "found": False, "mismatches": []}
    md_file = REPO_ENERGY / "net_gain_by_month.md"
    if md_file.exists():
        check["found"] = True
        for line in md_file.read_text().splitlines():
            c = [x.strip() for x in line.strip().strip("|").split("|")]
            if len(c) == 7 and c[0] in MONTHS:
                i = MONTHS.index(c[0])
                want = [f"{trk[i]:.1f}", f"{fix[i]:.1f}", mine["Gross gain"][i], mine["always awake"][i],
                        mine["data link as built"][i], mine["light sleep"][i]]
                if c[1:] != want:
                    check["mismatches"].append({"month": c[0], "repo": c[1:], "this_script": want})
        check["cells_compared"] = 12 * 6
    check["as_built_reproduced_Wh_day"] = repo_as_built

    # --- bottom-up scenarios and motor
    sc = build_scenarios()
    motor = {T: motor_energy_wh(T) for T in (5, 10, 15)}
    motor_worst = motor_energy_wh(10, i_a=I_MOTOR[2], omega=OMEGA[0])
    motor_best = motor_energy_wh(10, i_a=I_MOTOR[0], omega=OMEGA[2])
    MOTOR_T = {"a": 10, "b": 10, "c": 10, "d": 15, "d_deep": 15}
    tot = {}
    for k, s in sc.items():
        e_m = motor[MOTOR_T[k]]["E_Wh"]
        e5 = s["Wh_day"] + e_m
        tot[k] = {"name": s["name"], "electronics_Wh_day": s["Wh_day"], "motor_Wh_day": e_m,
                  "motor_interval_min": MOTOR_T[k], "total_Wh_day_5V": e5, "mean_mW_5V": e5 / 24 * 1000,
                  "total_Wh_day_battery": e5 / ETA_BOOST, "mean_mA_at_5V": e5 / 24 / V5 * 1000}

    # --- net gain per month, sensitivity
    keys = ["a", "b", "c", "d"]
    net = {k: pct(tot[k]["total_Wh_day_5V"]) for k in keys}
    net_wh = {k: budget - tot[k]["total_Wh_day_5V"] for k in keys}
    neg = lambda b, e: [MONTHS[i] for i in range(12) if b[i] - e < 0]
    sens = {}
    for k in keys + ["d_deep"]:
        e = tot[k]["total_Wh_day_5V"]
        sens[k] = {
            "same_tilt": {"-30%": neg(budget, 0.7 * e), "nominal": neg(budget, e), "+30%": neg(budget, 1.3 * e)},
            "flat_ref": {"-30%": neg(budget_flat, 0.7 * e), "nominal": neg(budget_flat, e), "+30%": neg(budget_flat, 1.3 * e)},
            "battery_side": {"nominal": neg(budget, e / ETA_BOOST), "+30%": neg(budget, 1.3 * e / ETA_BOOST)},
            "worst_month_margin_Wh": float((budget - e).min()),
            "worst_month_headroom_pct": float(100 * (budget.min() / e - 1)),
            "year_net_pct": float(100 * ((trk - fix - e) * DAYS).sum() / (fix * DAYS).sum()),
        }
    manager_q_wh = I_Q_MANAGER / 1000 * VBAT * 24

    # design target: positive in the worst month with +30 % on consumption
    target_wh = float(budget.min() / 1.3)
    target_mw = target_wh / 24 * 1000

    # --- bench expectations (at the 5 V rail unless stated)
    sensors = N_INA * I_INA[1] + I_RTC[1]
    p_idle = mode_power_5v(I_MS80[1], sensors + I_LED[1], ETA_BUCK[1])
    p_idle_hi = mode_power_5v(I_MS80_RUN, N_INA * I_INA[2] + I_RTC[2] + I_LED[2], ETA_BUCK[0])
    p_up = mode_power_5v(I_UPLOAD[1], sensors + I_LED[1], ETA_BUCK[1])
    p_ls = mode_power_5v(I_LS_BOARD[1], N_INA * I_INA[1] + I_RTC[0] + I_LED[1], ETA_BUCK_LIGHT[1])
    p_ds = mode_power_5v(I_DS_BOARD[1], N_INA * I_INA[1] + I_RTC[0] + I_LED[1], ETA_BUCK_LIGHT[1])
    bench = [
        ("M0", "Zero check: INA226 #3 reading with the tracker supply line open", "INA226 #3", "0.0 mA",
         "|reading| <= 0.1 mA (4 LSB of 25 uA with the 0.1 ohm shunt); otherwise subtract it"),
        ("M1", "INA226 #3 against the DMM in series (DMM on its 10 A/200 mA range, note burden voltage) at two steady loads, about 20 mA and 200 mA",
         "both", "same reading", "within +/-2 % (repo procedure B3); repeat `cal use` if not"),
        ("M2", "Firmware as built, radio OFF, 80 MHz, driver disconnected: mean over 60 s",
         "both", f"{p_idle:.0f} mW = {p_idle / V5:.1f} mA at 5 V (model range {mode_power_5v(I_MS80[0], sensors + I_LED[0], ETA_BUCK[2]):.0f}-{p_idle_hi:.0f} mW)",
         f"<= {p_idle_hi:.0f} mW. Above it: something unexpected is on (USB, LEDs, 240 MHz)"),
        ("M3", "One upload session (CLI `upload`): mean power while `radio_on_s` counts, and the session length",
         "INA226 #3 (1 Hz) + serial log", f"{p_up:.0f} mW = {p_up / V5:.0f} mA at 5 V for {T_UPLOAD_S:.0f} s = {p_up * T_UPLOAD_S / 1000:.1f} J per upload",
         f"<= {mode_power_5v(I_UPLOAD[2], sensors + I_LED[1], ETA_BUCK[0]) * 12 / 1000:.1f} J per upload (150 mA, 12 s); radio_on_s <= 12 s"),
        ("M4", "Test sketch: esp_light_sleep_start() with timer wake, sensors connected, USB unplugged, 60 s mean",
         "DMM (uA/mA range) and INA226 #3", f"{p_ls:.1f} mW = {p_ls / V5:.2f} mA at 5 V",
         f"<= {target_mw * 0.75:.0f} mW ({target_mw * 0.75 / V5:.1f} mA at 5 V): leaves 25 % of the {target_mw:.1f} mW target for wakes, uploads and motor"),
        ("M5", "Same sketch with esp_deep_sleep_start()", "DMM", f"{p_ds:.1f} mW = {p_ds / V5:.2f} mA at 5 V (expected HIGHER than M4)",
         "information only: use deep sleep only if it reads lower than M4"),
        ("M6", "DRV8833 on the 5 V rail, motor stopped: nSLEEP high minus nSLEEP low", "DMM",
         f"{I_DRV_ON[1]:.1f} mA difference (max {I_DRV_ON[2]:.0f} mA); asleep {I_DRV_SLEEP[1] * 1000:.1f} uA",
         f"awake <= {I_DRV_ON[2]:.0f} mA; asleep: no visible change on the mA range (< 0.01 mA)"),
        ("M7", "Motor moving the panel, both directions, at the supply voltage used: running current and start peak",
         "DMM 10 A range (running), INA226 #3 for the mean", f"{I_MOTOR[1] * 1000:.0f} mA running (range {I_MOTOR[0] * 1000:.0f}-{I_MOTOR[2] * 1000:.0f} mA); start peak about {I_START:.0f} A",
         "running <= 400 mA; below the INA226 range (819 mA with 0.1 ohm) or fit a smaller shunt"),
        ("M8", "Panel speed: stopwatch over 90 deg of travel, both directions", "stopwatch + protractor/angle sensor",
         f"{OMEGA[1]:.1f} deg/s (45 s for 90 deg)", "energy per degree = V x I / speed <= 1.5 J/deg (0.1 Wh/day for 240 deg)"),
        ("M9", "Worm gear holds the panel with the driver asleep (push the panel by hand, 10 N at the edge)", "angle sensor",
         "no movement, 0 mA", "angle change < 0.5 deg: confirms no holding current is needed"),
        ("M10", "Sensors alone on 3V3 (3x INA226 + DS3231, MCU held in reset or measured in the 3V3 feed to the modules)", "DMM",
         f"{sensors:.2f} mA at 3.3 V", f"<= {N_INA * I_INA[2] + I_RTC[2]:.2f} mA (datasheet maxima)"),
        ("M11", "Power manager: battery-side input power against 5 V output power at about 20 mA and 100 mA load; and battery current with no load",
         "DMM x2 (or DMM + INA226 #3)", f"efficiency about {100 * ETA_BOOST:.0f} %; no-load battery current < {I_Q_MANAGER:.0f} mA",
         "efficiency >= 80 %; no-load current <= 2 mA"),
        ("M12", "24 h run of the final firmware (starts 26 Nov): E_use from channel-1 field3", "INA226 #3 via ThingSpeak",
         f"(b) {tot['b']['total_Wh_day_5V']:.2f} Wh/day as built; (c) {tot['c']['total_Wh_day_5V']:.2f} Wh/day with light sleep",
         f"within +/-20 % of the model for the firmware flashed; final design <= {target_wh:.2f} Wh/day ({target_mw:.1f} mW)"),
    ]

    # ----------------------------------------------------------------------- figure
    INK, MUTED, SURF = "#0b0b0b", "#52514e", "#fcfcfb"
    COL = {"a": "#2a78d6", "b": "#eb6834", "c": "#1baf7a", "d": "#eda100"}   # validated categorical order
    LS = {"a": (0, (6, 2)), "b": (0, (4, 1.5, 1, 1.5)), "c": (0, (2, 1.5)), "d": (0, (1, 1))}
    SHORT = {"a": "(a) always awake, Wi-Fi on", "b": "(b) data link as built",
             "c": "(c) light sleep, 15 min uploads", "d": "(d) light sleep, hourly uploads, LED cut"}
    x = np.arange(1, 13)
    fig, ax = plt.subplots(figsize=(9.2, 6.0), dpi=200)
    fig.patch.set_facecolor(SURF); ax.set_facecolor(SURF)
    ax.fill_between(x, budget, 0.1, color="#000000", alpha=0.05, linewidth=0, zorder=1)
    ax.plot(x, budget, color=INK, linewidth=2.4, marker="o", markersize=6.5, markeredgecolor=SURF, markeredgewidth=1.2,
            zorder=5, label="Break-even budget: tracked minus fixed panel at the same tilt (PVGIS)")
    ax.plot(x, budget_flat, color=MUTED, linewidth=1.2, linestyle=(0, (1, 2)), zorder=3,
            label="Sensitivity: budget if the reference panel lies flat")
    for k in keys:
        e = tot[k]["total_Wh_day_5V"]
        ax.fill_between([0.5, 12.5], 0.7 * e, 1.3 * e, color=COL[k], alpha=0.13, linewidth=0, zorder=2)
        ax.plot([0.5, 12.5], [e, e], color=COL[k], linewidth=2, linestyle=LS[k], zorder=4,
                label=f"{SHORT[k]}: {e:.2f} Wh/day ({tot[k]['mean_mW_5V']:.0f} mW)")
        ax.text(12.62, e, f"({k}) {e:.2f}", va="center", ha="left", fontsize=8.5, color=INK)
    for i, off, ha, va in ((0, (12, -5), "left", "center"), (5, (0, 9), "center", "bottom"),
                           (10, (10, 6), "left", "bottom"), (11, (10, 5), "left", "bottom")):
        ax.annotate(f"{budget[i]:.2f} Wh\n{budget_mw[i]:.0f} mW", (x[i], budget[i]), textcoords="offset points",
                    xytext=off, ha=ha, va=va, fontsize=8, color=INK, annotation_clip=False)
    ax.text(6.5, 2.05, "tracker pays for itself where a scenario line\nlies below the black curve",
            ha="center", va="center", fontsize=8.5, color=MUTED)
    ax.set_yscale("log"); ax.set_ylim(0.18, 40); ax.set_xlim(0.5, 12.5)
    ax.set_yticks([0.2, 0.5, 1, 2, 5, 10, 20]); ax.set_yticklabels(["0.2", "0.5", "1", "2", "5", "10", "20"])
    ax.set_xticks(x); ax.set_xticklabels(MONTHS)
    ax.set_ylabel("Energy per day (Wh/day, log scale); 1 Wh/day = 41.7 mW average", color=INK)
    ax.set_xlabel("Month (PVGIS 5.3 long-term mean, Guildford 51.24° N, 5.4 W panel)", color=INK)
    ax.set_title("Can the panel pay for its own movement?\nMonthly break-even budget against what the tracker draws "
                 "(bands: consumption ±30 %)", fontsize=10.5, loc="left", color=INK)
    ax.grid(True, which="major", color="#e4e3df", linewidth=0.8); ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color("#b9b8b2")
    ax.tick_params(colors=MUTED)
    ax.legend(fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.13), ncol=2, frameon=False, handlelength=4)
    fig.subplots_adjust(bottom=0.27, right=0.9, top=0.9, left=0.09)
    fig.savefig(HERE / "break_even_by_month.png", facecolor=SURF)

    # ----------------------------------------------------------------------- JSON
    numbers = {
        "generated_by": "power_budget.py", "date": "2026-11-19", "pvgis_source": pv_src, "panel_Wp": PANEL_WP,
        "months": MONTHS,
        "tracked_Wh_day": trk.round(3).tolist(), "fixed_same_tilt_Wh_day": fix.round(3).tolist(),
        "fixed_flat_Wh_day": flat.round(3).tolist(),
        "break_even_Wh_day": budget.round(3).tolist(), "break_even_mW": budget_mw.round(1).tolist(),
        "break_even_flat_ref_Wh_day": budget_flat.round(3).tolist(),
        "break_even_year_mean_Wh_day": round(year(budget), 3),
        "design_target": {"Wh_day": round(target_wh, 3), "mW": round(target_mw, 1),
                          "rule": "worst month (Dec) break-even divided by 1.3"},
        "repo_cross_check": check, "repo_as_built_rows": repo_rows, "repo_E_use_Wh_day": repo_use,
        "scenarios": {k: {**tot[k], "modes": sc[k]["rows"], "note": sc[k]["note"],
                          "net_gain_pct_by_month": (pct(tot[k]["total_Wh_day_5V"]).round(1).tolist()),
                          "net_margin_Wh_day_by_month": (budget - tot[k]["total_Wh_day_5V"]).round(3).tolist(),
                          "sensitivity": sens[k]} for k in sc},
        "motor": {"strategies": {str(T): {a: (float(b) if not isinstance(b, int) else b) for a, b in m.items()} for T, m in motor.items()},
                  "best_case_Wh_day": motor_best["E_Wh"], "worst_case_Wh_day": motor_worst["E_Wh"],
                  "assumptions": {"V": V_MOTOR, "I_A_low_central_high": I_MOTOR, "deg_per_s_low_central_high": OMEGA,
                                  "travel_deg_per_day": TRAVEL_DEG, "start_A": I_START, "start_s": T_START}},
        "power_manager": {"boost_efficiency": ETA_BOOST, "quiescent_mA_max": I_Q_MANAGER,
                          "quiescent_Wh_day_max": manager_q_wh},
        "bench": [dict(zip(("id", "what", "instrument", "expected", "pass"), b)) for b in bench],
        "sources": {k: {"what": v[0], "url": v[1]} for k, v in SOURCES.items()},
    }
    json.dump(numbers, open(HERE / "power_budget_numbers.json", "w"), indent=1)

    # ----------------------------------------------------------------------- markdown
    L = []
    A = L.append
    A("# Power budget and break-even: can the panel pay for its own movement?")
    A("")
    A("EEE3035 Group 8, Active Solar Tracker. Generated by `power_budget.py` on 19 Nov 2026; every number in this file "
      "comes from the script (`power_budget_numbers.json` holds them unrounded). **Nothing here has been measured on our "
      "hardware yet**: the values are datasheet figures, cited third-party measurements, or assumptions marked as such. "
      "Section 8 lists the bench measurements for 26 Nov that replace them.")
    A("")
    A("## 1. Answer in brief")
    A("")
    dec, jan, nov, jun = 11, 0, 10, 5
    A(f"- **Break-even budget** (tracked minus fixed panel at the same tilt, PVGIS): the tracker may draw at most "
      f"**{budget[dec]:.2f} Wh/day = {budget_mw[dec]:.0f} mW average in December**, {budget[jan]:.2f} Wh/day ({budget_mw[jan]:.0f} mW) in January, "
      f"{budget[nov]:.2f} Wh/day ({budget_mw[nov]:.0f} mW) in November and {budget[jun]:.2f} Wh/day ({budget_mw[jun]:.0f} mW) in June. "
      f"Year mean {year(budget):.2f} Wh/day ({year(budget) / 24 * 1000:.0f} mW).")
    for k in keys:
        n = sens[k]["same_tilt"]["nominal"]
        A(f"- **{SHORT[k]}**: {tot[k]['total_Wh_day_5V']:.2f} Wh/day ({tot[k]['mean_mW_5V']:.0f} mW). "
          + (f"Net loss in {len(n)} months ({', '.join(n)})" if n else "Net gain in every month")
          + f"; {sens[k]['year_net_pct']:+.0f} % over the year.")
    A(f"- **Design target: P_use <= {target_mw:.1f} mW average ({target_wh:.2f} Wh/day)** at INA226 #3. That is the December "
      f"budget divided by 1.3, so the net gain stays positive in every month even if consumption is 30 % above the model.")
    A(f"- The motor is the smallest term if it is set up properly: {motor[10]['E_Wh']:.3f} Wh/day for 240° of travel at "
      f"{I_MOTOR[1] * 1000:.0f} mA and {OMEGA[1]:.0f}°/s. The electronics that are on for 24 h decide the result, not the movement.")
    A("")
    A("![Break-even budget by month](break_even_by_month.png)")
    A("")
    A("*Figure: monthly break-even budget (black) against the daily consumption of the four scenarios (bands: ±30 %). "
      "Log scale. A scenario pays for itself in the months where its line lies below the black curve.*")
    A("")
    A("## 2. Method")
    A("")
    A("1. `net gain = (E_tracked - E_fixed - E_use) / E_fixed` (contract 6.2). Net gain >= 0 exactly when "
      "`E_use <= E_tracked - E_fixed`. The right-hand side is the **break-even budget**; divided by 24 h it is the maximum "
      "average tracker power.")
    A(f"2. `E_tracked` and `E_fixed` are the PVGIS 5.3 monthly means already in the repository ({pv_src}): inclined-axis "
      "tracker and fixed panel at the same tilt (39°, south), 14 % system loss, scaled to one 5.4 W FIT0330 panel.")
    A("3. `E_use` is built bottom-up: for each operating mode, current of each part x rail voltage / converter efficiency x "
      "time in that mode. All scenario figures are **at the 5 V output of the Solar Power Manager**, which is where INA226 #3 "
      "has to sit to see the whole tracker and nothing else (see flag F1). 3.3 V loads go through the Nano's MP2322 buck "
      f"converter ({ETA_BUCK[1]:.2f} at normal load, {ETA_BUCK_LIGHT[1]:.2f} assumed at sleep currents).")
    A("4. The motor is added as a separate term (section 4) so the control-code author can see what each strategy costs.")
    A("5. Cross-check: with the repository's own `E_use` assumptions the script reproduces "
      "`docs/energy/net_gain_by_month.md` (section 5).")
    A("")
    A("## 3. Component currents (cited)")
    A("")
    A("DS = datasheet, MEAS = published third-party measurement, ASSUMED = no datasheet value found (range given, to be measured).")
    A("")
    A("| Part | Mode | Typical | Max / range | Rail | Kind | Source |")
    A("|---|---|---|---|---|---|---|")
    T = [
        ("ESP32-S3 (Nano ESP32)", "CPU 80 MHz, radio off, both cores idle", "22.0 mA (peripheral clocks off)", "36.1 mA (all clocks on); 42.6 mA one core running", "3.3 V", "DS", "S3 Table 5-9"),
        ("", "CPU 240 MHz, radio off, idle", "32.9 mA", "47.6 mA (clocks on)", "3.3 V", "DS", "S3 Table 5-9"),
        ("", "Wi-Fi receive", "88 mA", "91 mA (HT40)", "3.3 V", "DS", "S3 Table 5-7"),
        ("", "Wi-Fi transmit, peak", "283 mA (11n MCS7)", "340 mA (11b, 21 dBm)", "3.3 V", "DS", "S3 Table 5-7"),
        ("", "Light sleep, chip only", "240 µA", "-", "3.3 V", "DS", "S3 Table 5-10; NANO features"),
        ("", "Deep sleep, chip only", "7-8 µA", "-", "3.3 V", "DS", "S3 Table 5-10; NANO features"),
        ("Nano ESP32 board", "Light sleep, whole board", "0.56-1.27 mA", "used: 1.27 mA", "3.3 V / VIN", "MEAS", "FORUM"),
        ("", "Deep sleep, whole board", "1.75-2.6 mA", "one report about 4 mA", "3.3 V / VIN", "MEAS", "FORUM"),
        ("", "Upload session mean (connect, TLS, POST)", "125 mA", "100-150 mA", "3.3 V", "ASSUMED", "REPO; bounded by S3 Table 5-7"),
        ("", "Wi-Fi associated all the time, 240 MHz", f"{I_WIFI_ON[1]:.1f} mA", "47.6-88 mA", "3.3 V", "ASSUMED", "10 % receive duty between the two DS bounds"),
        ("", "Green power LED (hard-wired)", "1 mA", "0.5-2 mA", "3.3 V", "ASSUMED", "not in NANO datasheet"),
        ("", "MP2322 buck, 5 V to 3.3 V, normal load", "> 90 % at 4.5 V in", "85-90 % at 12 V in", "-", "DS", "NANO 11.3"),
        ("", "MP2322 buck at a few mA", "80 %", "60-90 %", "-", "ASSUMED", "not published by Arduino"),
        ("INA226 (x3)", "Continuous conversion", "330 µA each", "420 µA each", "3.3 V", "DS", "INA 5.5"),
        ("", "Power-down", "0.5 µA", "2 µA", "3.3 V", "DS", "INA 5.5"),
        ("DS3231", "I2C active (400 kHz)", "-", "200 µA at 3.63 V", "3.3 V", "DS", "RTC"),
        ("", "Standby (bus idle)", "-", "110 µA at 3.63 V", "3.3 V", "DS", "RTC"),
        ("", "Temperature conversion (brief, every 64 s)", "-", "575 µA at 3.63 V", "3.3 V", "DS", "RTC"),
        ("", "On backup cell, timekeeping", "0.84 µA", "3.0 µA", "coin cell", "DS", "RTC"),
        ("BPX65 photodiode", "Photocurrent at 1000 lx, V_R = 5 V", "10 µA", "min 5.5 µA", "bias", "DS", "BPX"),
        ("", "Dark current, V_R = 20 V", "1 nA", "5 nA", "bias", "DS", "BPX"),
        ("", f"Arc of {N_PD} diodes in daylight, biased", "0.2 mA each", "0.01-1 mA each", "3.3 V", "ASSUMED", "count and circuit not in the repository"),
        ("DRV8833", "Awake, outputs off (VM = 5 V)", "1.7 mA", "3 mA", "5 V", "DS", "DRV 6.5"),
        ("", "Sleep (nSLEEP low)", "1.6 µA", "2.5 µA", "5 V", "DS", "DRV 6.5"),
        ("", "On-resistance, high side + low side at 5 V", "200 + 160 mΩ", "-", "-", "DS", "DRV 6.5"),
        ("Worm-gear DC motor", "Running (part not identified in the repository)", "200 mA", "80 mA (no load, 6 V type) to 1 A (stall, 12 V type)", "5 V", "ASSUMED from listings", "M6, M12"),
        ("", "Holding", "0 mA (self-locking)", "-", "-", "listing", "M6"),
        ("Waveshare Solar Power Manager", "Battery to 5 V boost", "about 86 %", "-", "-", "DS", "WS"),
        ("", "Solar charging", "about 78 %", "-", "-", "DS", "WS"),
        ("", "Quiescent current", "-", "< 2 mA", "battery", "DS", "WS"),
    ]
    for r in T:
        A("| " + " | ".join(r) + " |")
    A("")
    A("Sources:")
    A("")
    for k, (what, url) in SOURCES.items():
        A(f"- **{k}**: {what}. {url}")
    A("")
    A("Flags (things the model had to assume):")
    A("")
    A("- **F1, where INA226 #3 sits.** The firmware comments call 0x44 the \"battery line\" and report its bus voltage as V_bat. "
      "With the Solar Power Manager the battery line also carries the charging current, so a sensor there cannot separate "
      "what the tracker draws. For P_use the sensor has to be in the 5 V (or 3.3 V) output that feeds the tracker. "
      f"Measured at the battery instead, every figure here is divided by the boost efficiency ({ETA_BOOST:.2f}).")
    A("- **F2, Nano ESP32 supply.** Arduino recommends 6-21 V on VIN (the cheat sheet also says keep VIN within 5-18 V), so 5 V on VIN "
      "is below the recommended range, and VBUS is not an input (it only outputs 5 V when the board is powered from USB). Feed the power "
      "manager's 5 V USB output into the Nano's USB-C port, or 3.3 V into the 3V3 pin only as Arduino describes (solder jumper), which also removes the MP2322 loss.")
    A("- **F3, BPX65 arc.** The repository does not say how many photodiodes there are or how they are read; "
      f"{N_PD} diodes reverse-biased from 3V3 are assumed. Used in photovoltaic mode (no bias) they draw nothing.")
    A("- **F4, motor.** The part number, gear ratio, supply voltage, running current and panel speed are not in the repository. "
      "The 2°/s is the simulator default (`ast_sim.h`).")
    A("- **F5, panel-angle sensor and limit switches** are not in the hardware list and are not in the model. A 10 kΩ potentiometer "
      "across 3.3 V would add 0.33 mA (about 0.03 Wh/day): power it from a GPIO during the wake window.")
    A("- **F6, board sleep currents** come from forum users, not from Arduino. They vary by a factor of two.")
    A("")
    A("## 4. Scenarios")
    A("")
    for k in ["a", "b", "c", "d", "d_deep"]:
        s = sc[k]
        A(f"### {s['name']}")
        A("")
        A(f"{s['note'].capitalize()}.")
        A("")
        A("| Mode | Power at the 5 V rail (mW) | Share of the day | Mean (mW) | Wh/day |")
        A("|---|---|---|---|---|")
        for r in s["rows"]:
            A(f"| {r['mode']} | {r['P_mW']:.2f} | {100 * r['duty']:.2f} % | {r['mean_mW']:.2f} | {r['Wh_day']:.3f} |")
        A(f"| Motor, one move every {MOTOR_T[k]} min (section 4.1) | | | {tot[k]['motor_Wh_day'] / 24 * 1000:.2f} | {tot[k]['motor_Wh_day']:.3f} |")
        A(f"| **Total** | | | **{tot[k]['mean_mW_5V']:.1f}** | **{tot[k]['total_Wh_day_5V']:.2f}** |")
        A("")
    A(f"Deep sleep is not the aggressive option on this board: users measure {I_DS_BOARD[0]}-{I_DS_BOARD[2]} mA for the whole board in deep sleep "
      f"against {I_LS_BOARD[0]}-{I_LS_BOARD[2]} mA in light sleep (FORUM), although the chip alone is specified at 7-8 µA against 240 µA. "
      "Deep sleep also restarts the sketch at every wake, which loses the 1 Hz averaging and the FreeRTOS tasks. "
      f"Like for like (hourly uploads, LED cut) deep sleep comes to {tot['d_deep']['total_Wh_day_5V']:.2f} Wh/day against {tot['d']['total_Wh_day_5V']:.2f} for light sleep. "
      "Light sleep is the recommendation; M4 and M5 in section 8 check it.")
    A("")
    A("### 4.0 The repository's 2.3 Wh/day, reproduced")
    A("")
    A("`firmware/README.md` (Energy budget) uses three terms, all at 3.3 V with no converter loss:")
    A("")
    A("| Mode | Current at 3.3 V (mA) | Seconds per day | Wh/day |")
    A("|---|---|---|---|")
    for r in repo_rows:
        A(f"| {r['mode']} | {r['mA_at_3V3']:.1f} | {r['s_per_day']:.0f} | {r['Wh_day']:.3f} |")
    A(f"| **Total** | | | **{repo_as_built:.2f}** ({repo_as_built / 24 * 1000:.0f} mW) |")
    A("")
    A("Its assumptions: 27 mA for the CPU at 80 MHz with the radio off (inside the datasheet range of 22.0-36.1 mA); 96 uploads "
      "a day of 9 s at 125 mA; 1.2 mA for 3x INA226 + DS3231 (3 x 0.33 + 0.2 = 1.19 mA, confirmed); no buck-converter loss, no power "
      "LED, no photodiode bias, no motor driver, no motor. Adding those gives scenario (b): "
      f"**{tot['b']['total_Wh_day_5V']:.2f} Wh/day**, {100 * (tot['b']['total_Wh_day_5V'] / repo_as_built - 1):.0f} % more than 2.3. "
      f"The largest additions are the converter loss ({100 * (1 / ETA_BUCK[1] - 1):.0f} %) and the DRV8833 left awake "
      f"({V5 * I_DRV_ON[1] * 24 / 1000:.2f} Wh/day).")
    A("")
    A("### 4.1 Motor energy")
    A("")
    A(f"Assumptions: travel {TRAVEL_DEG:.0f}°/day (±{LIMIT_DEG:.0f}° limits: 120° sweep at 15°/h over 8 h, then 120° back), "
      f"{V_MOTOR:.0f} V, {I_MOTOR[1] * 1000:.0f} mA running, {OMEGA[1]:.0f}°/s at the panel, start surge {I_START:.0f} A for {T_START} s, "
      "MCU awake and DRV8833 awake only while the motor runs. No holding current: the worm gear is self-locking.")
    A("")
    A("| Move every | Moves/day | Step | Motor on (s/day) | Running (J) | Starts (J) | MCU + driver while moving (J) | **Total (Wh/day)** | Beam lost to pointing error |")
    A("|---|---|---|---|---|---|---|---|---|")
    for Tm, m in motor.items():
        A(f"| {Tm} min | {m['moves_per_day']} | {m['step_deg']:.2f}° | {m['run_s_per_day']:.0f} | {m['E_run_J']:.0f} | {m['E_start_J']:.0f} | "
          f"{m['E_mcu_J'] + m['E_drv_J']:.0f} | **{m['E_Wh']:.3f}** | {m['cos_loss_pct']:.3f} % |")
    A("")
    A(f"The running time is fixed by the travel and the speed, not by the interval; only the number of starts changes. "
      f"Range: {motor_best['E_Wh']:.3f} Wh/day ({I_MOTOR[0] * 1000:.0f} mA, {OMEGA[2]:.0f}°/s) to **{motor_worst['E_Wh']:.2f} Wh/day** "
      f"({I_MOTOR[2]:.0f} A, {OMEGA[0]}°/s), and the top of that range equals the whole December budget. Motor current and panel speed "
      "are therefore the two numbers to measure first (M7, M8). "
      f"DRV8833 conduction loss at {I_MOTOR[1] * 1000:.0f} mA is {motor[10]['drv_loss_mW_while_running']:.0f} mW while moving (already inside V x I). "
      "A 15 min step leaves at most 1.9° of error and costs under 0.02 % of the beam, so stepping more often buys nothing.")
    A("")
    A(f"Left awake all day the DRV8833 alone costs {V5 * I_DRV_ON[1] * 24 / 1000:.2f} Wh/day (1.7 mA at 5 V), "
      f"{100 * V5 * I_DRV_ON[1] * 24 / 1000 / budget[dec]:.0f} % of the December budget and several times the motor itself.")
    A("")
    A("## 5. Break-even budget by month")
    A("")
    A("Net gain is against the fixed panel at the same tilt; the scenario columns include the motor.")
    A("")
    A("| Month | Tracked (Wh/day) | Fixed, same tilt (Wh/day) | **Break-even (Wh/day)** | **Break-even (mW avg)** | Gross gain | Net (a) | Net (b) | Net (c) | Net (d) |")
    A("|---|---|---|---|---|---|---|---|---|---|")
    for i in range(12):
        A(f"| {MONTHS[i]} | {trk[i]:.2f} | {fix[i]:.2f} | **{budget[i]:.2f}** | **{budget_mw[i]:.0f}** | {100 * budget[i] / fix[i]:+.0f} % | "
          + " | ".join(f"{net[k][i]:+.0f} %" for k in keys) + " |")
    A(f"| **Year** | {year(trk):.2f} | {year(fix):.2f} | **{year(budget):.2f}** | **{year(budget) / 24 * 1000:.0f}** | "
      f"{100 * (budget * DAYS).sum() / (fix * DAYS).sum():+.0f} % | " + " | ".join(f"**{sens[k]['year_net_pct']:+.0f} %**" for k in keys) + " |")
    A("")
    A("Net margin in Wh/day (break-even minus consumption; negative = the tracker costs more than it gains):")
    A("")
    A("| Month | (a) " + f"{tot['a']['total_Wh_day_5V']:.2f} Wh/day | (b) {tot['b']['total_Wh_day_5V']:.2f} | (c) {tot['c']['total_Wh_day_5V']:.2f} | (d) {tot['d']['total_Wh_day_5V']:.2f} |")
    A("|---|---|---|---|---|")
    for i in range(12):
        A(f"| {MONTHS[i]} | " + " | ".join(f"{net_wh[k][i]:+.2f}" for k in keys) + " |")
    A("")
    A("### Cross-check against the repository")
    A("")
    if check["found"]:
        A(f"With the repository's E_use values (always awake {repo_use['always awake']:.1f}, data link as built {repo_use['data link as built']:.1f}, "
          f"light sleep {repo_use['light sleep']:.2f} Wh/day) the script recomputed all {check['cells_compared']} cells of "
          f"`docs/energy/net_gain_by_month.md` (tracked, fixed, gross and the three net columns, 12 months): "
          + ("**all identical**." if not check["mismatches"] else f"**{len(check['mismatches'])} rows differ** (see the JSON)."))
    else:
        A("Repository table not found on this machine: cross-check skipped.")
    A("")
    A("| Month | Gross | Net, always awake (4.8) | Net, data link as built (2.3) | Net, light sleep (0.24) |")
    A("|---|---|---|---|---|")
    for i in range(12):
        A(f"| {MONTHS[i]} | {mine['Gross gain'][i]} | {mine['always awake'][i]} | {mine['data link as built'][i]} | {mine['light sleep'][i]} |")
    A("")
    A(f"The bottom-up figures are higher than the repository's for the same three cases: (a) {tot['a']['total_Wh_day_5V']:.2f} against 4.8, "
      f"(b) {tot['b']['total_Wh_day_5V']:.2f} against 2.3, (c) {tot['c']['total_Wh_day_5V']:.2f} against 0.24 Wh/day. The 0.24 figure "
      "(3 mA at 3.3 V) left out the uploads, the converter loss and the LED; light sleep still pays in every month, but with a much smaller December margin.")
    A("")
    A("## 6. Sensitivity")
    A("")
    A("Months with a net loss (blank = gain in every month):")
    A("")
    A("| Scenario | Wh/day | Same tilt, -30 % | Same tilt, nominal | Same tilt, +30 % | Flat reference, nominal | Flat reference, +30 % | Measured at the battery (/0.86) | Battery, +30 % | Dec headroom |")
    A("|---|---|---|---|---|---|---|---|---|---|")
    j = lambda m: ", ".join(m)
    for k in keys + ["d_deep"]:
        s = sens[k]
        A(f"| ({k.replace('_', '-')}) | {tot[k]['total_Wh_day_5V']:.2f} | {j(s['same_tilt']['-30%'])} | {j(s['same_tilt']['nominal'])} | {j(s['same_tilt']['+30%'])} | "
          f"{j(s['flat_ref']['nominal'])} | {j(s['flat_ref']['+30%'])} | {j(s['battery_side']['nominal'])} | {j(s['battery_side']['+30%'])} | {s['worst_month_headroom_pct']:+.0f} % |")
    A("")
    A(f"- **Reference panel flat instead of tilted.** The budget rises to {budget_flat[dec]:.2f} Wh/day in December ({budget_flat[dec] / 24 * 1000:.0f} mW) and "
      f"{budget_flat[jun]:.2f} in June, so even scenario (b) shows a gain in "
      f"{12 - len(sens['b']['flat_ref']['nominal'])} of 12 months. Most of that is the tilt, not the tracking (Nov: flat {flat[nov]:.1f}, tilted {fix[nov]:.1f}, "
      f"tracked {trk[nov]:.1f} Wh/day), so it must not be reported as tracking gain.")
    A(f"- **Consumption ±30 %.** (d) stays positive in every month at +30 % (December headroom {sens['d']['worst_month_headroom_pct']:+.0f} %). "
      f"(c) has {sens['c']['worst_month_headroom_pct']:+.0f} % December headroom"
      + (", so +30 % turns December negative." if sens['c']['same_tilt']['+30%'] else ", which survives +30 %."))
    A(f"- **Where the sensor sits.** At the battery every figure is 1/{ETA_BOOST} = {1 / ETA_BOOST:.2f} times larger. The power manager's own quiescent current "
      f"(< {I_Q_MANAGER:.0f} mA, at most {manager_q_wh:.2f} Wh/day) is not charged to the tracker here because a fixed panel with a battery needs the same module; "
      f"if the group decides to count it, add up to {manager_q_wh:.2f} Wh/day to every scenario: December is then "
      + "; ".join(f"({k}) {budget[dec] - tot[k]['total_Wh_day_5V'] / ETA_BOOST - manager_q_wh:+.2f} Wh/day" for k in ("c", "d"))
      + " at the battery, so measure the real quiescent current (M11).")
    A("- **PVGIS is a long-term monthly mean.** Single overcast days have almost no beam and no tracking gain; the lab weeks (late Nov, Dec) sit at the bottom of the curve.")
    A("")
    A("## 7. Design rules for the control code")
    A("")
    A(f"Target: **P_use <= {target_mw:.1f} mW averaged over 24 h ({target_wh:.2f} Wh/day, {target_mw / V5:.1f} mA at 5 V)**. Hard limit: "
      f"{budget_mw[dec]:.0f} mW ({budget[dec]:.2f} Wh/day), the December break-even.")
    A("")
    A("1. **Sleep mode: light sleep, not deep sleep, and never a `delay()` loop.** Call `esp_light_sleep_start()` with a timer wake from the sampler "
      f"(automatic light sleep is not available in this core build, see `firmware/README.md`). Awake at 80 MHz costs about {mode_power_5v(I_MS80[1], 0, ETA_BUCK[1]):.0f} mW, "
      f"which is {mode_power_5v(I_MS80[1], 0, ETA_BUCK[1]) / budget_mw[dec]:.1f} times the December budget on its own, so the MCU may be awake for at most "
      f"about 5 % of the time. Budget used in (c)/(d): awake <= 0.25 s every 15 s (1.7 %).")
    A("2. **Sample interval: wake every 15 s, log every 60 s.** Set the INA226 to 1024 averages x 8.244 ms for both conversions (one result per 16.9 s, "
      "config 0x4FFF; the firmware now uses 64 averages, 0x47FF, one result per 1.06 s) so four reads per minute still cover the whole minute; the contract's "
      "one-minute mean (7.1) is kept. The INA226s stay in continuous mode: they are the measurement, and three of them cost only 0.08 Wh/day.")
    A("3. **Control loop only when it can matter.** Read the BPX65 arc and decide once per wake by day; at night (arc dark for 10 min) wake once a minute, "
      "skip the control step and leave the panel where the return move put it. The 5 Hz loop in the INTERFACE.md example must not be used as it is.")
    A("4. **Wi-Fi batching.** Keep the radio off between uploads (as built). At 15 min intervals the uploads cost "
      f"{sc['c']['rows'][2]['Wh_day']:.2f} Wh/day, {100 * sc['c']['rows'][2]['Wh_day'] / budget[dec]:.0f} % of the December budget; hourly (`upload every 60`) costs "
      f"{sc['d']['rows'][2]['Wh_day']:.2f} Wh/day. Use 15 min while F8 stands; ask for F8 to be relaxed to 60 min for the winter months. "
      f"Each upload must stay under {mode_power_5v(I_UPLOAD[1], 0, ETA_BUCK[1]) * T_UPLOAD_S / 1000:.1f} J: cut `kRadioLeadS` from 8 s once the real hotspot join time is known, "
      "and never leave the radio on waiting for NTP.")
    A("5. **Motor duty.** Move at most once every 10-15 min (dead-band 2.5-3.75°), one continuous move per step, no hunting: a second move within "
      "5 min of the last is forbidden unless the error exceeds 5°. One return move after dusk. Do not move when the arc reports no clear sun (state HLD). "
      f"Budget: motor-on time x motor power <= 360 J/day (0.1 Wh), i.e. <= 1.5 J per degree.")
    A("6. **DRV8833 nSLEEP on a GPIO, low except during a move.** Raise it, wait 1 ms, move, lower it. Left high it costs "
      f"{V5 * I_DRV_ON[1] * 24 / 1000:.2f} Wh/day. Hold the GPIO state through light sleep (`gpio_hold_en`) or fit a pull-down.")
    A("7. **Gate everything else.** BPX65 bias (and any angle potentiometer) from a GPIO that is high only in the wake window, or run the photodiodes "
      "unbiased. LEDs off (done); cut the green power LED for the outdoor unit. USB unplugged in every energy test.")
    A("8. **Stay awake only while the motor runs,** and call `telemetry_set_motor_running()` around the move. With light sleep the INTERFACE.md rule "
      "\"call at least once per second or the data link reports IDL after 5 s\" has to be changed to match the 15 s wake: agree this with the data-link owner before coding.")
    A("9. **Check with the device itself.** `P_use` (field 3) over 24 h must come out at or below "
      f"{target_wh:.2f} Wh/day; above {budget[dec]:.2f} Wh/day the December demonstration shows a net loss whatever the pointing accuracy.")
    A("")
    A("## 8. Bench measurements for 26 Nov (INA226 #3 and a DMM)")
    A("")
    A("Power the tracker from the 5 V output with USB unplugged. INA226 #3 with the 0.1 Ω shunt resolves 25 µA (2.5 µV per bit); use the DMM for "
      "the sleep currents, `cal` for an instantaneous INA226 reading, and ThingSpeak field 3 (one-minute means) for bursts and 24 h energy "
      "(the `power` command prints CPU MHz, Wi-Fi state and radio-on seconds; it does not stream INA226 readings).")
    A("")
    A("| # | Measurement | Instrument | Expected (model) | Pass threshold |")
    A("|---|---|---|---|---|")
    for b in bench:
        A("| " + " | ".join(b) + " |")
    A("")
    A("After the session, replace the constants at the top of `power_budget.py` with the measured values (M2 to `I_MS80`, M3 to `I_UPLOAD`/`T_UPLOAD_S`, "
      "M4/M5 to `I_LS_BOARD`/`I_DS_BOARD`, M6 to `I_DRV_ON`, M7/M8 to `I_MOTOR`/`OMEGA`, M11 to `ETA_BOOST`) and run it again.")
    A("")
    (HERE / "power_budget.md").write_text("\n".join(L) + "\n")

    # ----------------------------------------------------------------------- console
    print("Break-even (Wh/day):", " ".join(f"{m} {b:.2f}" for m, b in zip(MONTHS, budget)))
    print("Break-even (mW):    ", " ".join(f"{m} {b:.0f}" for m, b in zip(MONTHS, budget_mw)))
    print(f"Repo as-built reproduction: {repo_as_built:.3f} Wh/day; cross-check found={check['found']} mismatches={len(check['mismatches'])}")
    for k in sc:
        print(f"{k:7s} elec {tot[k]['electronics_Wh_day']:.3f} + motor {tot[k]['motor_Wh_day']:.3f} = {tot[k]['total_Wh_day_5V']:.3f} Wh/day "
              f"({tot[k]['mean_mW_5V']:.1f} mW); loss months: {sens[k]['same_tilt']['nominal']} ; +30 %: {sens[k]['same_tilt']['+30%']}; "
              f"battery: {sens[k]['battery_side']['nominal']}; Dec headroom {sens[k]['worst_month_headroom_pct']:+.0f} %")
    print("Motor Wh/day:", {T: round(m['E_Wh'], 4) for T, m in motor.items()}, "best", round(motor_best['E_Wh'], 4), "worst", round(motor_worst['E_Wh'], 3))
    print(f"Design target: {target_mw:.1f} mW = {target_wh:.3f} Wh/day")


if __name__ == "__main__":
    main()
