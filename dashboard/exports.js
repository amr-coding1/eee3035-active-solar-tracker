/**
 * CSV export tables (pure functions: data in, { filename, header, rows } out).
 * These are the four exports the project lead asked for: raw samples, per-run
 * summary (gain per run / calibration runs), histogram bins and energy budget.
 * Numbers are written unrounded (6 dp) so they can be checked against the
 * device log and the Python/MATLAB reference.
 * Simulated samples (status s=1, contract v1.4) are flagged in every file:
 * `sim` per raw sample, `n_sim` per run and per day.
 */
import { HIST_LO, histLabels } from './metrics.js';
import { csvNum, fmtLondonIso } from './format.js';

const iso = (ms) => (ms === null ? '' : new Date(ms).toISOString().replace('.000Z', 'Z'));

/** File name rule of the group's Data Management Plan: G8_<type>_<YYYYMMDD>-<YYYYMMDD>.csv */
export const dmpName = (type, fromDay, toDay) => `G8_${type}_${fromDay.replace(/-/g, '')}-${toDay.replace(/-/g, '')}.csv`;

export function rawCsv(entries, fromDay, toDay) {
  return {
    filename: dmpName('raw', fromDay, toDay),
    header: ['created_at_utc', 'time_uk', 'entry_id', 'seq', 'p_trk_mw', 'p_fix_mw', 'p_use_mw', 'ang_panel_deg',
      'ang_sun_deg', 'temp_c', 'vbat_v', 'state', 'run', 'panel', 'fault', 'sim', 'status'],
    rows: entries.map((e) => [iso(e.t), fmtLondonIso(e.t), e.entryId ?? '', e.seq ?? '', csvNum(e.pTrk, 3), csvNum(e.pFix, 3),
      csvNum(e.pUse, 3), csvNum(e.angPanel, 3), csvNum(e.angSun, 3), csvNum(e.temp, 3), csvNum(e.vbat, 3),
      e.state ?? '', e.run, e.panel ?? '', e.fault, e.sim ? 1 : 0, e.status ?? '']),
  };
}

export function runsCsv(runs, fromDay, toDay) {
  return {
    filename: dmpName('runs', fromDay, toDay),
    header: ['run', 'kind', 'panel', 'start_utc', 'end_utc', 'n_samples', 'n_unique', 'expected', 'e_trk_wh', 'e_fix_wh', 'e_use_wh',
      'gain_gross_pct', 'gain_net_pct', 'mae_point_deg', 'complete_pct', 'moves', 'gaps', 'longest_gap_min', 'n_sim'],
    rows: runs.map((r) => [r.run, r.kind, r.panel ?? '', iso(r.first), iso(r.last), r.n, r.nUnique, r.expected, csvNum(r.eTrk),
      csvNum(r.eFix), csvNum(r.eUse), csvNum(r.gGross, 4), csvNum(r.gNet, 4), csvNum(r.mae, 4), csvNum(r.complete, 4),
      r.moves, r.gaps, r.longestGapMin, r.nSim]),
  };
}

/**
 * Pointing-error histogram, one file: the selected day, the whole loaded range
 * and every run (tracking tests and calibration runs) side by side.
 * Columns: bin, lower_deg, upper_deg, day_<YYYY-MM-DD>, range_<from>_<to>,
 * then run_<id>_<panel> per run.
 * @param {{day:string, dayHist:number[], fromDay:string, toDay:string, rangeHist:number[], runs:Array}} h
 */
export function histCsv({ day, dayHist, fromDay, toDay, rangeHist, runs = [] }) {
  const labels = histLabels();
  const last = labels.length - 1;
  return {
    filename: dmpName('pointing-histogram', fromDay, toDay),
    header: ['bin', 'lower_deg', 'upper_deg', `day_${day}`, `range_${fromDay}_${toDay}`,
      ...runs.map((r) => `run_${r.run}_${r.panel ?? 'unknown'}`)],
    // underflow has no lower edge, overflow has no upper edge
    rows: labels.map((label, i) => [label, i === 0 ? '' : HIST_LO + i - 1, i === last ? '' : HIST_LO + i,
      dayHist[i], rangeHist[i], ...runs.map((r) => r.hist[i])]),
  };
}

export function energyCsv(days, fromDay, toDay) {
  return {
    filename: dmpName('energy-budget', fromDay, toDay),
    header: ['date', 'e_trk_wh', 'e_fix_wh', 'e_use_wh', 'e_net_gain_wh', 'gain_gross_pct', 'gain_net_pct', 'n_samples', 'complete_pct', 'n_sim'],
    rows: days.map((d) => [d.day, csvNum(d.eTrk), csvNum(d.eFix), csvNum(d.eUse), csvNum(d.eTrk - d.eFix - d.eUse),
      csvNum(d.gGross, 4), csvNum(d.gNet, 4), d.n, csvNum(d.complete, 4), d.nSim]),
  };
}
