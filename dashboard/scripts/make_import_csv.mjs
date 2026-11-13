// Build a ThingSpeak "Data Import" CSV for channel 1 from the dashboard's
// deterministic demo generator (every row is simulated: status carries s=1).
// Usage: node scripts/make_import_csv.mjs <nowISO> > out.csv
import { demoRawFeeds } from '../demo.js';

const now = Date.parse(process.argv[2] || new Date().toISOString());
const feeds = demoRawFeeds(now);
const cols = ['field1', 'field2', 'field3', 'field4', 'field5', 'field6', 'field7', 'field8'];
const lines = ['created_at,' + cols.join(',') + ',status'];
for (const f of feeds) {
  const t = f.created_at.replace('Z', '+00:00');
  const seq = String(Number(f.field8) + 1); // contract 3.1: seq starts at 1
  const vals = cols.map((c) => (c === 'field8' ? seq : (f[c] ?? '')));
  lines.push([t, ...vals, f.status].join(','));
}
process.stdout.write(lines.join('\n') + '\n');
