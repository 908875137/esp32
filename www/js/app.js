/* ThinkINK admin SPA — no frameworks, no CDN. Talks to /api/*. */

const state = {
  page: 'home',
  status: null,
  images: [],
  imagesTotal: 0,
  imagesPage: 0,
  imagesPageSize: 24,
  quotes: [],
  quotesTotal: 0,
  quotesPage: 0,
  quotesPageSize: 10,
  current: null,
  stats: null,
  daily: [],
  aiQuotes: [],
  logs: '',
  files: [],
  filesPath: '',
  mode: 'sd',
};

function $(sel, root) { return (root || document).querySelector(sel); }
function $$(sel, root) { return Array.from((root || document).querySelectorAll(sel)); }

function fmtBytes(n) {
  if (n == null) return '—';
  const units = ['B', 'KB', 'MB', 'GB'];
  let i = 0, v = n;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return v.toFixed(v >= 100 || i === 0 ? 0 : 1) + ' ' + units[i];
}

function toast(msg) {
  let el = $('.toast');
  if (!el) {
    el = document.createElement('div');
    el.className = 'toast';
    document.body.appendChild(el);
  }
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(el._t);
  el._t = setTimeout(() => el.classList.remove('show'), 2200);
}

async function api(path, opts) {
  const res = await fetch(path, opts);
  if (!res.ok) throw new Error('HTTP ' + res.status);
  const ct = res.headers.get('content-type') || '';
  return ct.includes('json') ? res.json() : res.text();
}

/* ---- Render helpers ---- */

function renderHome() {
  const s = state.status || {};
  const cur = state.current || {};
  const st = state.stats || {};
  const img = st.images || {};
  const qo = st.quotes || {};
  const page = $('#page');

  let curHtml = '<div style="color:var(--text-dim)">暂无记录</div>';
  if (cur.has_content) {
    curHtml = `
      <div style="display:flex;gap:12px;align-items:flex-start">
        ${cur.image_name ? `<img src="/images/${encodeURIComponent(cur.image_name)}" style="width:120px;height:72px;object-fit:cover;border-radius:6px;flex-shrink:0" />` : ''}
        <div style="min-width:0">
          <div style="font-size:15px;line-height:1.5">${esc(cur.quote_text || '')}</div>
          <div style="color:var(--text-dim);font-size:13px;margin-top:4px">${cur.quote_author ? '—— ' + esc(cur.quote_author) : ''}</div>
          <div style="color:var(--text-dim);font-size:12px;margin-top:6px">${esc(cur.date || '')} · 图用 ${cur.image_used ?? 0} 次 · 句用 ${cur.quote_used ?? 0} 次</div>
        </div>
      </div>
    `;
  }

  const dailyHtml = (state.daily || []).slice(-5).reverse().map(d =>
    `<div style="color:var(--text-dim);font-size:13px;padding:2px 0">${esc(d.date)} · 图 ${d.image_id} · 句 ${d.quote_id}</div>`
  ).join('') || '<div style="color:var(--text-dim);font-size:13px">暂无</div>';

  page.innerHTML = `
    <h2>首页</h2>

    <div class="card">
      <div style="font-size:14px;font-weight:600;margin-bottom:8px">当前显示</div>
      ${curHtml}
    </div>

    <div class="card">
      <div style="font-size:14px;font-weight:600;margin-bottom:8px">使用统计</div>
      <div class="card-row"><span class="label">图片</span><span class="value">${img.count ?? 0} 张 · 共 ${img.total_used ?? 0} 次 · 均 ${(img.avg_used ?? 0).toFixed(1)}</span></div>
      <div class="card-row"><span class="label">文本</span><span class="value">${qo.count ?? 0} 条 · 共 ${qo.total_used ?? 0} 次 · 均 ${(qo.avg_used ?? 0).toFixed(1)}</span></div>
    </div>

    <div class="card">
      <div class="card-row"><span class="label">设备</span><span class="value">${s.device || 'ThinkINK'}</span></div>
      <div class="card-row"><span class="label">WiFi</span><span class="value ${s.wifi ? 'ok' : ''}">${s.wifi ? '已连接' : '未连接'}</span></div>
      <div class="card-row"><span class="label">IP</span><span class="value">${s.ip || '—'}</span></div>
      <div class="card-row"><span class="label">mDNS</span><span class="value">${s.mdns || 'thinkink.local'}</span></div>
    </div>

    <div class="card">
      <div class="card-row"><span class="label">当前模式</span><span class="value mode-pill">${state.mode.toUpperCase()}</span></div>
      <div class="card-row"><span class="label">图片数量</span><span class="value">${s.image_count ?? '—'}</span></div>
      <div class="card-row"><span class="label">SD 总量</span><span class="value">${fmtBytes(s.sd_total)}</span></div>
      <div class="card-row"><span class="label">SD 剩余</span><span class="value">${fmtBytes(s.sd_free)}</span></div>
    </div>

    <div class="card">
      <div style="font-size:14px;font-weight:600;margin-bottom:8px">每日记录（最近）</div>
      ${dailyHtml}
    </div>

    <div class="btn-row">
      <button class="btn" id="btn-prev">上一张</button>
      <button class="btn" id="btn-random">随机</button>
      <button class="btn primary" id="btn-refresh">刷新</button>
      <button class="btn" id="btn-next">下一张</button>
    </div>
  `;
  $('#btn-prev').onclick = () => postDisplay('previous');
  $('#btn-next').onclick = () => postDisplay('next');
  $('#btn-refresh').onclick = () => postDisplay('refresh');
  $('#btn-random').onclick = () => postDisplay('random');
}

function renderImages() {
  const page = $('#page');
  const totalPages = Math.max(1, Math.ceil(state.imagesTotal / state.imagesPageSize));
  page.innerHTML = `
    <h2>图片</h2>
    <div class="btn-row" style="margin-bottom:12px">
      <button class="btn primary" id="btn-upload">上传图片</button>
      <button class="btn" id="btn-reload">刷新列表</button>
      <button class="btn" id="img-export" style="margin-left:auto">导出 CSV</button>
    </div>
    <div class="card" style="font-size:13px;color:var(--text-dim);line-height:1.6">
      <b>上传限制：</b>仅 PNG 格式；单张文件 ≤ 500KB；像素 ≤ 50 万（约 800×625，解码器硬限制）；相同图片（按内容 Hash）自动去重；文件名自动改为 8 位 ID（每 1000 张进下一个子目录）。
    </div>
    <div class="btn-row" style="margin:12px 0">
      <button class="btn" id="img-prev">上一页</button>
      <span style="align-self:center;color:var(--text-dim)">${state.imagesPage + 1} / ${totalPages}</span>
      <button class="btn" id="img-next">下一页</button>
      <span class="label" style="margin-left:auto">共 ${state.imagesTotal} 张</span>
    </div>
    <div class="img-grid" id="img-grid"></div>
    <input type="file" id="file-input" accept=".png" style="display:none" multiple />
  `;

  $('#btn-upload').onclick = () => $('#file-input').click();
  $('#btn-reload').onclick = loadImages;
  $('#img-export').onclick = () => window.open('/api/export/images.csv', '_blank');
  $('#file-input').onchange = (e) => uploadFiles(e.target.files);
  $('#img-prev').onclick = () => { if (state.imagesPage > 0) { state.imagesPage--; loadImages(); } };
  $('#img-next').onclick = () => { if (state.imagesPage < totalPages - 1) { state.imagesPage++; loadImages(); } };

  const grid = $('#img-grid');
  if (state.images.length === 0) {
    grid.innerHTML = '<div style="color:var(--text-dim);padding:20px;text-align:center">暂无图片</div>';
    return;
  }
  state.images.forEach((img) => {
    const cell = document.createElement('div');
    cell.className = 'img-cell';
    cell.innerHTML = `
      <div class="thumb">
        <img src="/images/${encodeURIComponent(img.name)}" alt="${img.name}" loading="lazy"
             onerror="this.parentElement.textContent='📷'" />
      </div>
      <div class="meta">
        <span class="name">${img.name}</span>
        <span>${fmtBytes(img.size)} · 已用 ${img.used_count ?? 0} 次</span>
      </div>
    `;
    cell.onclick = () => displayImage(img.name);
    cell.oncontextmenu = (e) => { e.preventDefault(); deleteImage(img.name); };
    grid.appendChild(cell);
  });
}

function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function renderQuotes() {
  const page = $('#page');
  const totalPages = Math.max(1, Math.ceil(state.quotesTotal / state.quotesPageSize));
  const qs = state.quotes || [];
  const list = qs.length === 0
    ? '<div style="color:var(--text-dim);padding:20px;text-align:center">暂无文本</div>'
    : qs.map(q => `
        <div class="card">
          <div style="font-size:17px;line-height:1.55">${esc(q.text)}</div>
          <div style="display:flex;justify-content:space-between;align-items:center;margin-top:10px;color:var(--text-dim);font-size:13px">
            <span>${q.author ? '—— ' + esc(q.author) : ''}</span>
            <span>已用 ${q.used_count} 次</span>
          </div>
          <div class="btn-row" style="margin-top:8px">
            <button class="btn" data-qedit="${q.id}">编辑</button>
            <button class="btn" data-qdel="${q.id}">删除</button>
          </div>
        </div>
      `).join('');

  const aiQs = state.aiQuotes || [];
  const aiReversed = aiQs.slice().reverse();
  const aiHtml = aiQs.length === 0
    ? '<div style="color:var(--text-dim);font-size:13px">暂无 AI 生成记录</div>'
    : aiReversed.map((q, i) =>
        `<label style="display:flex;gap:8px;align-items:flex-start;padding:6px 0;border-bottom:1px solid #262626">
          <input type="checkbox" class="ai-qcb" data-idx="${i}" style="margin-top:4px;flex-shrink:0" />
          <span style="font-size:15px;line-height:1.5;min-width:0">${esc(q.text)}${q.author ? `<div style="color:var(--text-dim);font-size:13px;margin-top:2px">—— ${esc(q.author)}${q.source ? '（' + esc(q.source) + '）' : ''}</div>` : ''}</span>
        </label>`
      ).join('');

  page.innerHTML = `
    <h2>语录</h2>

    <div class="card">
      <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:8px">
        <div style="font-size:14px;font-weight:600">AI 生成历史（最近）</div>
        <label style="font-size:13px;color:var(--text-dim)"><input type="checkbox" id="ai-sel-all" /> 全选</label>
      </div>
      ${aiHtml}
      <div class="btn-row" style="margin-top:10px">
        <button class="btn primary" id="btn-ai-import">添加到语录</button>
      </div>
    </div>

    <div class="card">
      <div style="font-size:14px;font-weight:600;margin-bottom:10px">添加一句</div>
      <textarea id="q-text" rows="2" placeholder="正文（10～100 字）" style="width:100%;box-sizing:border-box;padding:8px;font-size:15px;border:1px solid #444;border-radius:6px;background:transparent;color:inherit;resize:vertical"></textarea>
      <div style="display:flex;gap:8px;margin-top:8px">
        <input id="q-author" placeholder="作者（可选）" style="flex:1;min-width:0;padding:8px;font-size:14px;border:1px solid #444;border-radius:6px;background:transparent;color:inherit" />
        <input id="q-source" placeholder="来源（可选）" style="flex:1;min-width:0;padding:8px;font-size:14px;border:1px solid #444;border-radius:6px;background:transparent;color:inherit" />
      </div>
      <div class="btn-row" style="margin-top:10px">
        <button class="btn primary" id="q-add">添加</button>
      </div>
    </div>

    <div class="card" style="font-size:13px;color:var(--text-dim);line-height:1.6">
      <b>批量导入：</b>上传 <code>quotes.csv</code> 直接覆盖当前语录库（无需刷固件）。格式：
      <pre style="margin:8px 0;overflow:auto">id,text,author,source,used_count,last_used_at
00000001,"慢一点，也没有关系。","","原创",0,</pre>
      正文可含逗号/引号（用双引号包裹，<code>""</code> 表示字面引号）。
      <div class="btn-row" style="margin-top:8px">
        <button class="btn" id="q-import">导入 CSV</button>
      </div>
      <input type="file" id="q-import-input" accept=".csv" style="display:none" />
    </div>

    <div class="btn-row" style="margin:12px 0">
      <button class="btn" id="q-prev">上一页</button>
      <span style="align-self:center;color:var(--text-dim)">${state.quotesPage + 1} / ${totalPages}</span>
      <button class="btn" id="q-next">下一页</button>
      <button class="btn" id="q-export" style="margin-left:auto">导出 CSV</button>
      <span class="label">共 ${state.quotesTotal} 条</span>
    </div>
    ${list}
  `;

  $('#q-add').onclick = addQuote;
  $('#q-import').onclick = () => $('#q-import-input').click();
  $('#q-import-input').onchange = (e) => importQuotes(e.target.files[0]);
  $('#q-prev').onclick = () => { if (state.quotesPage > 0) { state.quotesPage--; loadQuotes(); } };
  $('#q-next').onclick = () => { if (state.quotesPage < totalPages - 1) { state.quotesPage++; loadQuotes(); } };
  $('#q-export').onclick = () => window.open('/api/export/quotes.csv', '_blank');
  $$('#page [data-qedit]').forEach(b => b.onclick = () => editQuote(b.dataset.qedit));
  $$('#page [data-qdel]').forEach(b => b.onclick = () => deleteQuote(b.dataset.qdel));
  $('#ai-sel-all').onchange = (e) => $$('#page .ai-qcb').forEach(cb => cb.checked = e.target.checked);
  $('#btn-ai-import').onclick = importAiQuotes;
}

function renderSettings() {
  const page = $('#page');
  const isAi = state.mode === 'ai';
  page.innerHTML = `
    <h2>设置</h2>
    <div class="card">
      <div class="card-row"><span class="label">当前模式</span><span class="value mode-pill">${state.mode.toUpperCase()}</span></div>
      <div class="card-row"><span class="label">切换模式</span>
        <div class="btn-row" style="flex:0">
          <button class="btn ${isAi ? 'primary' : ''}" id="btn-mode-ai">AI</button>
          <button class="btn ${!isAi ? 'primary' : ''}" id="btn-mode-sd">SD</button>
        </div>
      </div>
    </div>
    <div class="card">
      <div class="card-row"><span class="label">设备</span><span class="value">${state.status?.device || 'ThinkINK'}</span></div>
      <div class="card-row"><span class="label">mDNS</span><span class="value">${state.status?.mdns || 'thinkink.local'}</span></div>
      <div class="card-row"><span class="label">IP</span><span class="value">${state.status?.ip || '—'}</span></div>
    </div>

    <div class="card">
      <div style="font-size:14px;font-weight:600;margin-bottom:8px">固件升级</div>
      <div class="card-row"><span class="label">当前版本</span><span class="value">${state.status?.version || '—'}</span></div>
      <div style="font-size:13px;color:var(--text-dim);line-height:1.6;margin:8px 0">
        上传 <code>.bin</code> 固件文件，设备校验后写入备用分区并自动重启。升级期间请保持供电和网络连接，不要断电。
      </div>
      <div class="btn-row">
        <button class="btn primary" id="btn-ota-upload">选择固件文件</button>
      </div>
      <div id="ota-progress" style="font-size:13px;color:var(--text-dim);margin-top:8px"></div>
      <input type="file" id="ota-file-input" accept=".bin" style="display:none" />
    </div>

    <div class="card">
      <div style="font-size:14px;font-weight:600;margin-bottom:8px">上传前端文件</div>
      <div style="font-size:13px;color:var(--text-dim);line-height:1.6;margin-bottom:10px">
        上传 <code>app.js</code> / <code>index.html</code> / <code>style.css</code> 等文件，直接覆盖 <code>/sdcard/www/</code> 下同名文件，无需拔 SD 卡。
      </div>
      <div class="btn-row">
        <button class="btn primary" id="btn-www-upload">选择文件上传</button>
      </div>
      <input type="file" id="www-file-input" style="display:none" />
    </div>
  `;
  $('#btn-mode-ai').onclick = () => setMode('ai');
  $('#btn-mode-sd').onclick = () => setMode('sd');
  $('#btn-ota-upload').onclick = () => $('#ota-file-input').click();
  $('#ota-file-input').onchange = (e) => uploadFirmware(e.target.files[0]);
  $('#btn-www-upload').onclick = () => $('#www-file-input').click();
  $('#www-file-input').onchange = (e) => uploadWwwFile(e.target.files[0]);
}

function renderLogs() {
  const page = $('#page');
  const text = state.logs || '';
  page.innerHTML = `
    <h2>日志</h2>
    <div class="btn-row" style="margin-bottom:12px">
      <button class="btn primary" id="btn-logs-reload">刷新</button>
      <button class="btn danger" id="btn-logs-clear">清空</button>
      <span class="label" style="margin-left:auto">最近 ${text.length} 字节</span>
    </div>
    <div class="card">
      <pre style="margin:0;font-size:12px;line-height:1.5;overflow:auto;white-space:pre-wrap;word-break:break-all;max-height:60vh">${text ? esc(text) : '暂无日志'}</pre>
    </div>
  `;
  $('#btn-logs-reload').onclick = loadLogs;
  $('#btn-logs-clear').onclick = clearLogs;
}

async function loadLogs() {
  try {
    const r = await api('/api/logs');
    state.logs = (typeof r === 'string') ? r : '';
  } catch (e) { state.logs = ''; }
  if (state.page === 'logs') renderLogs();
}

async function clearLogs() {
  if (!confirm('确认清空日志文件？')) return;
  try {
    await api('/api/logs/clear', { method: 'POST' });
    state.logs = '';
    toast('日志已清空');
    if (state.page === 'logs') renderLogs();
  } catch (e) { toast('清空失败: ' + e.message); }
}

function renderFiles() {
  const page = $('#page');
  const files = state.files || [];
  const cur = state.filesPath || '';
  const parent = cur.includes('/') ? cur.slice(0, cur.lastIndexOf('/')) : '';
  const full = (name) => cur ? cur + '/' + name : name;

  const list = files.length === 0
    ? '<div style="color:var(--text-dim);padding:20px;text-align:center">空目录</div>'
    : files.map(f => `
        <div class="card" style="${f.is_dir ? 'cursor:pointer' : ''}" data-fname="${esc(f.name)}" data-fdir="${f.is_dir}">
          <div style="display:flex;align-items:center;gap:8px">
            <span style="font-size:16px">${f.is_dir ? '📁' : '📄'}</span>
            <span style="flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${esc(f.name)}">${esc(f.name)}</span>
            <span style="color:var(--text-dim);font-size:13px">${f.is_dir ? '—' : fmtBytes(f.size)}</span>
            <span style="color:var(--text-dim);font-size:12px">${new Date(f.mtime * 1000).toLocaleString()}</span>
            ${f.is_dir ? '' : `
              <button class="btn" data-fdl="${esc(full(f.name))}">下载</button>
              <button class="btn" data-fdel="${esc(full(f.name))}">删除</button>
            `}
          </div>
        </div>
      `).join('');

  page.innerHTML = `
    <h2>文件</h2>
    <div class="btn-row" style="margin-bottom:8px">
      <button class="btn" id="btn-file-up" ${cur ? '' : 'disabled'}>⬆ 上级</button>
      <span style="align-self:center;color:var(--text-dim);margin-left:8px">/sdcard/${esc(cur)}</span>
    </div>
    <div class="btn-row" style="margin-bottom:12px">
      <button class="btn primary" id="btn-file-upload">上传到当前目录</button>
      <button class="btn" id="btn-file-reload">刷新</button>
      <span class="label" style="margin-left:auto">${files.length} 项</span>
    </div>
    ${list}
    <input type="file" id="file-upload-input" style="display:none" />
  `;

  $('#btn-file-up').onclick = () => { state.filesPath = parent; loadFiles(); };
  $('#btn-file-upload').onclick = () => $('#file-upload-input').click();
  $('#btn-file-reload').onclick = loadFiles;
  $('#file-upload-input').onchange = (e) => uploadFile(e.target.files[0]);
  $$('#page [data-fdl]').forEach(b => b.onclick = () => window.open('/api/files/download?name=' + encodeURIComponent(b.dataset.fdl), '_blank'));
  $$('#page [data-fdel]').forEach(b => b.onclick = () => deleteFile(b.dataset.fdel));
  $$('#page [data-fdir="true"]').forEach(b => b.onclick = () => {
    state.filesPath = cur ? cur + '/' + b.dataset.fname : b.dataset.fname;
    loadFiles();
  });
}

async function loadFiles() {
  const p = state.filesPath ? '?path=' + encodeURIComponent(state.filesPath) : '';
  try {
    const r = await api('/api/files' + p);
    state.files = r.files || [];
  } catch (e) { state.files = []; }
  if (state.page === 'files') renderFiles();
}

async function uploadFile(file) {
  if (!file) return;
  const dir = state.filesPath || '';
  try {
    const url = '/api/files/upload?path=' + encodeURIComponent(dir) + '&filename=' + encodeURIComponent(file.name);
    await fetch(url, { method: 'POST', body: file });
    toast('已上传 ' + file.name);
    await loadFiles();
  } catch (e) { toast('上传失败: ' + e.message); }
}

async function deleteFile(name) {
  if (!confirm('删除 ' + name + ' ?')) return;
  try {
    await api('/api/files/' + encodeURIComponent(name), { method: 'DELETE' });
    toast('已删除');
    await loadFiles();
  } catch (e) { toast('删除失败: ' + e.message); }
}

/* ---- Router ---- */

const RENDERERS = { home: renderHome, images: renderImages, quotes: renderQuotes, settings: renderSettings, logs: renderLogs, files: renderFiles };

function navigate(page) {
  state.page = page;
  $$('.tab').forEach(t => t.classList.toggle('active', t.dataset.page === page));
  RENDERERS[page]();
  if (page === 'home') { loadCurrent(); loadStats(); loadDaily(); }
  if (page === 'images') loadImages();
  if (page === 'quotes') { loadQuotes(); loadAiQuotes(); }
  if (page === 'logs') loadLogs();
  if (page === 'files') loadFiles();
}

/* ---- API actions ---- */

async function loadStatus() {
  try { state.status = await api('/api/status'); state.mode = state.status.mode || 'sd'; } catch (e) {}
  $('#wifi-badge').textContent = state.status?.wifi ? (state.status.ip || 'WiFi') : '未连接';
}

async function loadImages() {
  const offset = state.imagesPage * state.imagesPageSize;
  try {
    const r = await api(`/api/images?offset=${offset}&limit=${state.imagesPageSize}`);
    state.images = r.images || [];
    state.imagesTotal = r.total || 0;
  } catch (e) { state.images = []; state.imagesTotal = 0; }
  if (state.page === 'images') renderImages();
}

async function loadQuotes() {
  const offset = state.quotesPage * state.quotesPageSize;
  try {
    const r = await api(`/api/quotes?offset=${offset}&limit=${state.quotesPageSize}`);
    state.quotes = r.quotes || [];
    state.quotesTotal = r.total || 0;
  } catch (e) { state.quotes = []; state.quotesTotal = 0; }
  if (state.page === 'quotes') renderQuotes();
}

async function loadAiQuotes() {
  try {
    const r = await api('/api/ai/quotes');
    state.aiQuotes = r.quotes || [];
  } catch (e) { state.aiQuotes = []; }
  if (state.page === 'quotes') renderQuotes();
}

async function importAiQuotes() {
  const aiReversed = (state.aiQuotes || []).slice().reverse();
  const checked = $$('#page .ai-qcb:checked').map(cb => aiReversed[Number(cb.dataset.idx)]);
  if (checked.length === 0) { toast('请先勾选要添加的句子'); return; }
  try {
    const r = await api('/api/ai/quotes/import', { method: 'POST', body: JSON.stringify({ quotes: checked }) });
    const skip = r.skipped ? '，跳过重复 ' + r.skipped + ' 句' : '';
    toast(`已添加 ${r.added} 句${skip}`);
    await loadQuotes();
  } catch (e) { toast('添加失败: ' + e.message); }
}

async function addQuote() {
  const text = $('#q-text').value.trim();
  const author = $('#q-author').value.trim();
  const source = $('#q-source').value.trim();
  if (!text) { toast('正文不能为空'); return; }
  try {
    const r = await api('/api/quotes', { method: 'POST', body: JSON.stringify({ text, author, source }) });
    toast('已添加，ID ' + r.id);
    $('#q-text').value = ''; $('#q-author').value = ''; $('#q-source').value = '';
    await loadQuotes();
  } catch (e) { toast('添加失败: ' + e.message); }
}

async function importQuotes(file) {
  if (!file) return;
  if (!confirm('导入将覆盖当前所有语录，确定？')) return;
  try {
    await fetch('/api/quotes/import', { method: 'POST', body: file });
    toast('导入成功');
    await loadQuotes();
  } catch (e) { toast('导入失败: ' + e.message); }
}

async function deleteQuote(id) {
  if (!confirm('删除这句话？')) return;
  try {
    await api('/api/quotes/' + id, { method: 'DELETE' });
    toast('已删除');
    await loadQuotes();
  } catch (e) { toast('删除失败: ' + e.message); }
}

async function editQuote(id) {
  const q = state.quotes.find(x => x.id == id);
  if (!q) return;
  const text = prompt('正文', q.text);
  if (text == null) return;
  const author = prompt('作者（留空清除）', q.author || '') || '';
  const source = prompt('来源（留空清除）', q.source || '') || '';
  try {
    await api('/api/quotes/edit', { method: 'POST', body: JSON.stringify({ id: Number(id), text, author, source }) });
    toast('已保存');
    await loadQuotes();
  } catch (e) { toast('保存失败: ' + e.message); }
}

async function loadCurrent() {
  try { state.current = await api('/api/current'); } catch (e) { state.current = null; }
  if (state.page === 'home') renderHome();
}

async function loadStats() {
  try { state.stats = await api('/api/stats'); } catch (e) { state.stats = null; }
  if (state.page === 'home') renderHome();
}

async function loadDaily() {
  try { const r = await api('/api/daily'); state.daily = r.entries || []; } catch (e) { state.daily = []; }
  if (state.page === 'home') renderHome();
}

async function postDisplay(action) {
  try { await api('/api/display/' + action, { method: 'POST' }); toast('已执行 ' + action); }
  catch (e) { toast('失败: ' + e.message); }
}

async function displayImage(name) {
  try {
    await api('/api/display/image', {
      method: 'POST', body: JSON.stringify({ filename: name }),
    });
    toast('正在显示 ' + name);
  } catch (e) { toast('失败: ' + e.message); }
}

async function deleteImage(name) {
  if (!confirm('删除 ' + name + ' ?')) return;
  try {
    await api('/api/images/' + encodeURIComponent(name), { method: 'DELETE' });
    toast('已删除');
    await loadImages();
  } catch (e) { toast('删除失败: ' + e.message); }
}

async function uploadFiles(files) {
  for (const f of files) {
    try {
      const url = '/api/images/upload?filename=' + encodeURIComponent(f.name);
      await fetch(url, { method: 'POST', body: f });
      toast('已上传 ' + f.name);
    } catch (e) { toast('上传失败: ' + f.name); }
  }
  await loadImages();
}

async function setMode(mode) {
  try {
    await api('/api/mode', { method: 'POST', body: JSON.stringify({ mode }) });
    state.mode = mode;
    toast('已切换到 ' + mode.toUpperCase());
    await loadStatus();
    if (state.page === 'settings') renderSettings();
  } catch (e) { toast('切换失败: ' + e.message); }
}

async function uploadWwwFile(file) {
  if (!file) return;
  let path = prompt('目标相对路径（如 js/app.js、css/style.css）', file.name);
  if (path == null) return;
  path = path.trim();
  if (!path) path = file.name;
  try {
    const url = '/api/www/upload?filename=' + encodeURIComponent(path);
    await fetch(url, { method: 'POST', body: file });
    toast('已上传 ' + path + '（刷新页面生效）');
  } catch (e) { toast('上传失败: ' + e.message); }
}

async function uploadFirmware(file) {
  if (!file) return;
  if (!confirm('确认升级固件？升级完成后设备会自动重启。')) return;
  const progressEl = $('#ota-progress');
  if (!progressEl) return;
  progressEl.textContent = '上传中 0%...';
  try {
    await new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.setRequestHeader('X-OTA-Token', 'thinkink');
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const pct = Math.round((e.loaded / e.total) * 100);
          progressEl.textContent = '上传中 ' + pct + '%...';
        }
      };
      xhr.onload = () => {
        if (xhr.status === 200) resolve();
        else reject(new Error('HTTP ' + xhr.status));
      };
      xhr.onerror = () => reject(new Error('网络错误'));
      xhr.send(file);
    });
    progressEl.textContent = '写入完成，设备即将重启...';
    toast('升级成功，设备即将重启');
  } catch (e) {
    progressEl.textContent = '升级失败: ' + e.message;
    toast('升级失败: ' + e.message);
  }
}

/* ---- Boot ---- */

$$('.tab').forEach(t => t.onclick = () => navigate(t.dataset.page));

(async function init() {
  await loadStatus();
  await loadImages();
  await loadQuotes();
  await loadCurrent();
  await loadStats();
  await loadDaily();
  navigate('home');
})();
