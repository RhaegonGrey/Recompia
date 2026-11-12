// The Testing tab's log tools: send (export a redacted zip, open the folder and the bug form) and the viewer. The Rust side is testlogs.rs.
import { api } from '../api.js';
import { confirm, errText, esc, icon, toast } from '../ui.js';
import { extractReport, formatReport } from '../handheld-report-model.js';
import { REPORT_URL, fileLabel, filterLines, sizeText, zipName } from '../testing-model.js';

const open = (url) => api.openLink({ url });

/** Exports the logs, then shows the folder and the bug form and says what to drag where. Nothing is uploaded from the launcher. */
export async function sendLogs(version) {
  let ex;
  try {
    ex = await api.testingLogsExport({ version });
  } catch (e) {
    toast(errText(e), 'error');
    return;
  }
  await api.testingLogsOpenDir().catch((e) => console.warn(e));
  await confirm({
    title: 'Your logs are ready',
    html: `<p><code>${esc(zipName(ex.path))}</code> (${sizeText(ex.bytes)}) is in the folder that just opened. Names in paths, account and server settings and IP addresses were removed.</p>
      <p><strong>1.</strong> Press <em>Open the report form</em>. <strong>2.</strong> Say what happened. <strong>3.</strong> Drag the zip into the <em>pkp.log</em> box of the form. GitHub keeps what you attach, so look through it first with <em>View logs</em> if you wish.</p>`,
    ok: 'Open the report form',
    cancel: 'Not now',
  }) && await open(REPORT_URL);
}

/** A window with the end of each log, a filter and a copy button. Reads through testing_log_read, which redacts like the export. */
export async function viewLogs() {
  const files = await api.testingLogsList();
  if (!files.length) {
    toast('There are no logs yet. Start the game once, then look again.', 'error');
    return;
  }
  await confirm({
    title: 'Logs',
    ok: 'Close',
    alert: true,
    html: `<div class="logv">
      <div class="logv-bar">
        <select class="select" id="logv-file" aria-label="Log file">${files.map((f) => `<option value="${esc(f.name)}">${esc(fileLabel(f))}</option>`).join('')}</select>
        <input class="input" id="logv-q" type="search" placeholder="Filter lines (for example: error)" aria-label="Filter lines" />
        <button class="btn quiet" id="logv-copy" type="button">${icon('copy')}Copy</button>
      </div>
      <pre class="logv-text mono" id="logv-text" tabindex="0"></pre>
      <p class="logv-note" id="logv-note"></p></div>`,
    setup(dlg) {
      const $ = (id) => dlg.querySelector(id);
      let text = '';
      const show = () => {
        const out = filterLines(text, $('#logv-q').value);
        $('#logv-text').textContent = out || (text ? 'No line matches.' : 'This file is empty.');
        $('#logv-text').scrollTop = $('#logv-text').scrollHeight;   // the newest lines are at the end
      };
      const load = async () => {
        try {
          const t = await api.testingLogRead({ name: $('#logv-file').value });
          text = t.text;
          $('#logv-note').textContent = t.truncated ? 'Showing the end of the file. The export keeps the end too.' : '';
        } catch (e) {
          text = '';
          $('#logv-note').textContent = errText(e);
        }
        show();
      };
      $('#logv-file').addEventListener('change', load);
      $('#logv-q').addEventListener('input', show);
      $('#logv-copy').addEventListener('click', () => navigator.clipboard.writeText($('#logv-text').textContent).then(() => toast('Copied', 'ok'), () => toast('Cannot copy here', 'error')));
      load();
    },
  });
}

/** Copies one block with the lines of pkp.log a handheld bug report needs (handheld-report-model.js), read through the redacting viewer command. */
export async function copyHandheldReport(version, channel) {
  let tail;
  try {
    tail = await api.testingLogRead({ name: 'pkp.log' });
  } catch (e) {
    toast(errText(e), 'error');
    return;
  }
  const block = formatReport(extractReport(tail.text, tail.truncated), { version, channel });
  try {
    await navigator.clipboard.writeText(block);
    toast('Report copied. Paste it into your bug report.', 'ok');
  } catch { toast('Cannot copy here', 'error'); }
}
