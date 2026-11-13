/** CSV building and download (RFC 4180 quoting, CRLF line ends, UTF-8). */

function cell(v) {
  const s = v === null || v === undefined ? '' : String(v);
  return /[",\r\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
}

/** header: string[]; rows: any[][] -> CSV text with a header line. */
export function toCsv(header, rows) {
  return [header, ...rows].map((r) => r.map(cell).join(',')).join('\r\n') + '\r\n';
}

/** For hosts that block downloads (e.g. a preview frame): copy the CSV text. Call from a click. */
export function copyCsv(text) {
  return navigator.clipboard.writeText(text);
}

export function downloadCsv(filename, text) {
  const url = URL.createObjectURL(new Blob([text], { type: 'text/csv;charset=utf-8' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
