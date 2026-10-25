(() => {
  'use strict';

  const $ = (selector) => document.querySelector(selector);
  const params = new URLSearchParams(location.search);

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = text;
    return node;
  }

  function fmtBytes(bytes) {
    const value = Number(bytes) || 0;
    if (value < 1024) return value + ' B';
    const units = ['KB', 'MB', 'GB', 'TB'];
    let size = value;
    let index = -1;
    do {
      size /= 1024;
      index += 1;
    } while (size >= 1024 && index < units.length - 1);
    return size.toFixed(1) + ' ' + units[index];
  }

  function timeLabel(ms) {
    if (!ms) return '';
    return new Date(ms).toLocaleTimeString('zh-CN', { hour12: false });
  }

  function shortSha(value) {
    if (!value) return '';
    return value.slice(0, 12) + '…';
  }

  function toast(text, kind) {
    const host = $('#toasts');
    if (!host) return;
    const item = el('div', 'toast' + (kind ? ' ' + kind : ''), text);
    host.appendChild(item);
    setTimeout(() => {
      item.style.opacity = '0';
      item.style.transform = 'translateY(6px)';
      setTimeout(() => item.remove(), 220);
    }, 3600);
  }

  function askModal(title, message, options) {
    const opts = options || {};
    return new Promise((resolve) => {
      const mask = el('div', 'modal-mask');
      const box = el('div', 'modal');
      box.appendChild(el('h3', null, title));
      if (message) box.appendChild(el('p', null, message));

      let field = null;
      if (opts.input) {
        field = document.createElement(opts.multiline ? 'textarea' : 'input');
        if (!opts.multiline) field.type = 'text';
        field.value = opts.value || '';
        if (opts.placeholder) field.placeholder = opts.placeholder;
        box.appendChild(field);
      }

      const actions = el('div', 'modal-actions');
      const cancel = el('button', 'ghost', '取消');
      const ok = el('button', opts.danger ? 'danger' : 'primary', opts.confirmText || '确定');
      actions.appendChild(cancel);
      actions.appendChild(ok);
      box.appendChild(actions);
      mask.appendChild(box);
      document.body.appendChild(mask);

      const finish = (result) => {
        document.removeEventListener('keydown', onKey);
        mask.remove();
        resolve(result);
      };
      const onKey = (event) => {
        if (event.key === 'Escape') finish(opts.input ? null : false);
        if (event.key === 'Enter' && opts.input && !opts.multiline) {
          event.preventDefault();
          finish(field.value.trim());
        }
      };
      cancel.addEventListener('click', () => finish(opts.input ? null : false));
      ok.addEventListener('click', () => finish(opts.input ? field.value.trim() : true));
      mask.addEventListener('click', (event) => {
        if (event.target === mask) finish(opts.input ? null : false);
      });
      document.addEventListener('keydown', onKey);
      if (field) {
        field.focus();
        field.select();
      } else {
        ok.focus();
      }
    });
  }

  const memoryStore = {};
  function storeGet(key) {
    try {
      return localStorage.getItem(key);
    } catch (_) {
      return Object.prototype.hasOwnProperty.call(memoryStore, key) ? memoryStore[key] : null;
    }
  }
  function storeSet(key, value) {
    try {
      localStorage.setItem(key, value);
    } catch (_) {
      memoryStore[key] = value;
    }
  }

  function randomId(bytes) {
    const buffer = new Uint8Array(bytes);
    if (window.crypto && window.crypto.getRandomValues) {
      window.crypto.getRandomValues(buffer);
    } else {
      for (let i = 0; i < buffer.length; i += 1) buffer[i] = Math.floor(Math.random() * 256);
    }
    return Array.from(buffer, (value) => value.toString(16).padStart(2, '0')).join('');
  }

  const deviceId = (params.get('device') || storeGet('fd.device') || randomId(8)).replace(/[^a-zA-Z0-9_-]/g, '');
  const deviceKey = params.get('key') || storeGet('fd.key') || randomId(16);
  storeSet('fd.device', deviceId);
  storeSet('fd.key', deviceKey);

  const nameInput = $('#deviceName');
  const initialName = params.get('name') || storeGet('fd.name') || '设备-' + deviceId.slice(0, 4);
  storeSet('fd.name', initialName);
  nameInput.value = initialName;

  if (params.has('device') || params.has('key') || params.has('name')) {
    history.replaceState(null, '', location.pathname);
  }

  let snapshot = null;
  let lastSignature = '';
  let events = null;
  let reloadTimer = null;
  let requestTarget = null;

  function currentName() {
    const value = (nameInput.value || '').trim();
    return value || initialName;
  }

  function describePlatform() {
    const ua = navigator.userAgent || '';
    const os = /Windows/.test(ua) ? 'Windows'
      : /Android/.test(ua) ? 'Android'
      : /iPhone|iPad|iPod/.test(ua) ? 'iOS'
      : /Mac OS X/.test(ua) ? 'macOS'
      : /Linux/.test(ua) ? 'Linux'
      : '未知系统';
    const browser = /Edg\//.test(ua) ? 'Edge'
      : /Chrome\//.test(ua) ? 'Chrome'
      : /Firefox\//.test(ua) ? 'Firefox'
      : /Safari\//.test(ua) ? 'Safari'
      : '浏览器';
    return os + ' · ' + browser;
  }

  async function api(path, options) {
    const config = Object.assign({}, options || {});
    config.headers = Object.assign(
      { 'X-Device-Id': deviceId, 'X-Device-Key': deviceKey },
      config.headers || {}
    );
    const response = await fetch(path, config);
    let data = null;
    try {
      data = await response.json();
    } catch (_) {
      data = null;
    }
    if (!response.ok || !data || data.ok === false) {
      throw new Error((data && data.error) || 'HTTP ' + response.status);
    }
    return data;
  }

  function setOnline(online) {
    const dot = $('#onlineDot');
    dot.classList.toggle('on', online);
    dot.classList.toggle('warn', !online);
    $('#connBanner').classList.toggle('show', !online);
  }

  function scheduleReload() {
    clearTimeout(reloadTimer);
    reloadTimer = setTimeout(loadState, 120);
  }

  async function loadState() {
    try {
      const data = await api('/api/client/state');
      snapshot = data;
      setOnline(true);
      const signature = JSON.stringify([
        data.device && data.device.name,
        data.server,
        data.offers,
        data.requests,
        data.history,
      ]);
      if (signature !== lastSignature) {
        lastSignature = signature;
        render();
      }
    } catch (err) {
      setOnline(false);
    }
  }

  function connectEvents() {
    if (events) events.close();
    const query = new URLSearchParams({
      deviceId,
      key: deviceKey,
      name: currentName(),
      platform: describePlatform(),
    });
    events = new EventSource('/api/client/events?' + query.toString());
    events.addEventListener('state', scheduleReload);
    events.addEventListener('rename', scheduleReload);
    events.onopen = () => {
      setOnline(true);
      scheduleReload();
    };
    events.onerror = () => setOnline(false);
  }

  function emptyBox(text) {
    return el('div', 'empty', text);
  }

  function statusTag(status) {
    const tag = el('span', 'tag', status);
    if (status === '已完成') tag.classList.add('ok');
    else if (status === '待接收' || status === '发送中' || status === '上传中') tag.classList.add('wait');
    else if (status === '已拒绝' || status === '已中断' || status === '写入失败') tag.classList.add('fail');
    return tag;
  }

  function render() {
    if (!snapshot) return;

    const server = snapshot.server || {};
    $('#serverLabel').textContent = (server.hostname || 'filedrop.ng') + (server.port ? ':' + server.port : '');

    if (document.activeElement !== nameInput && snapshot.device && snapshot.device.name) {
      nameInput.value = snapshot.device.name;
      storeSet('fd.name', snapshot.device.name);
    }

    renderOffers(snapshot.offers || []);
    renderRequests(snapshot.requests || []);
    renderHistory(snapshot.history || []);
  }

  function renderOffers(offers) {
    const host = $('#offers');
    host.textContent = '';
    $('#offerCount').textContent = String(offers.reduce((sum, offer) => sum + offer.files.length, 0));
    if (!offers.length) {
      host.appendChild(emptyBox('暂无待接收文件'));
      return;
    }
    offers.forEach((offer) => {
      const card = el('div', 'offer');
      const head = el('div', 'offer-head');
      head.appendChild(el('div', 'offer-title', '服务器发来 ' + offer.files.length + ' 个文件'));
      head.appendChild(el('span', 'muted', timeLabel(offer.created)));
      card.appendChild(head);

      offer.files.forEach((file) => {
        const row = el('div', 'offer-file');
        const main = el('div', 'file-main');
        const name = el('div', 'file-name', file.name);
        name.title = file.relPath || file.name;
        main.appendChild(name);
        const meta = el('div', 'file-meta', fmtBytes(file.size) + ' · SHA-256 ' + shortSha(file.sha256));
        meta.title = 'SHA-256: ' + (file.sha256 || '');
        main.appendChild(meta);
        const actions = el('div', 'file-actions');
        const button = el('button', 'primary small', '接收');
        button.addEventListener('click', () => receiveFile(file, button));
        actions.appendChild(button);
        row.appendChild(main);
        row.appendChild(actions);
        card.appendChild(row);
      });

      const footer = el('div', 'row');
      const acceptAll = el('button', 'ghost small', '全部接收');
      acceptAll.addEventListener('click', () => receiveAll(offer, acceptAll));
      const reject = el('button', 'danger small', '全部拒绝');
      reject.addEventListener('click', () => rejectOffer(offer));
      footer.appendChild(acceptAll);
      footer.appendChild(reject);
      card.appendChild(footer);
      host.appendChild(card);
    });
  }

  function renderRequests(requests) {
    const host = $('#requests');
    host.textContent = '';
    $('#requestCount').textContent = String(requests.length);
    if (!requests.length) {
      host.appendChild(emptyBox('暂无请求'));
      return;
    }
    requests.forEach((request) => {
      const card = el('div', 'request-card');
      const head = el('div', 'offer-head');
      head.appendChild(el('div', 'offer-title', '服务器请求你发送文件'));
      head.appendChild(el('span', 'muted', timeLabel(request.created)));
      card.appendChild(head);
      card.appendChild(el('div', 'note', request.note || '服务器请求你发送文件'));
      const row = el('div', 'row');
      const pick = el('button', 'primary small', '选择文件并发送');
      pick.addEventListener('click', () => {
        requestTarget = request.id;
        const input = $('#requestInput');
        input.value = '';
        input.click();
      });
      const dismiss = el('button', 'ghost small', '忽略');
      dismiss.addEventListener('click', async () => {
        dismiss.disabled = true;
        try {
          await api('/api/client/ack', {
            method: 'POST',
            body: JSON.stringify({ requestId: request.id, status: '已拒绝' }),
          });
          toast('已忽略该请求');
          loadState();
        } catch (err) {
          dismiss.disabled = false;
          toast('操作失败：' + err.message, 'error');
        }
      });
      row.appendChild(pick);
      row.appendChild(dismiss);
      card.appendChild(row);
      host.appendChild(card);
    });
  }

  function renderHistory(history) {
    const host = $('#history');
    host.textContent = '';
    if (!history.length) {
      const item = el('li', 'empty', '暂无记录');
      item.style.border = 'none';
      host.appendChild(item);
      return;
    }
    history.forEach((record) => {
      const item = el('li');
      const main = el('div', 'file-main');
      const title = el('div', 'file-name', record.fileName || '文件');
      title.title = record.fileName || '';
      main.appendChild(title);
      const direction = record.direction === 'from-device' ? '上传给服务器' : '接收自服务器';
      const meta = el('div', 'file-meta',
        direction + ' · ' + fmtBytes(record.size) + ' · SHA-256 ' + shortSha(record.sha256) + ' · ' + timeLabel(record.created));
      meta.title = 'SHA-256: ' + (record.sha256 || '');
      main.appendChild(meta);
      if (record.status === '上传中' || record.status === '发送中') {
        const ratio = record.size ? Math.min(100, Math.round((record.bytes / record.size) * 100)) : 0;
        const progress = el('div', 'progress');
        const bar = el('span');
        bar.style.width = ratio + '%';
        progress.appendChild(bar);
        main.appendChild(progress);
      }
      item.appendChild(main);
      item.appendChild(statusTag(record.status || ''));
      host.appendChild(item);
    });
  }

  function addUploadRow(path, size) {
    const host = $('#uploadList');
    const row = el('li');
    const main = el('div', 'file-main');
    const name = el('div', 'file-name', path);
    name.title = path;
    main.appendChild(name);
    const meta = el('div', 'file-meta', fmtBytes(size) + ' · 等待上传');
    main.appendChild(meta);
    const progress = el('div', 'progress');
    const bar = el('span');
    progress.appendChild(bar);
    main.appendChild(progress);
    row.appendChild(main);
    const tag = statusTag('上传中');
    row.appendChild(tag);
    host.insertBefore(row, host.firstChild);
    while (host.children.length > 12) host.removeChild(host.lastChild);
    return { row, bar, meta, tag, size };
  }

  function updateUploadRow(ui, loaded, total, statusText) {
    const totalSize = total || ui.size || 0;
    const ratio = totalSize ? Math.min(100, (loaded / totalSize) * 100) : 0;
    ui.bar.style.width = ratio.toFixed(1) + '%';
    if (statusText) ui.meta.textContent = statusText;
  }

  function uploadOne(entry, batchId, requestId, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      const query = new URLSearchParams({ batchId, path: entry.path || entry.file.name });
      if (requestId) query.set('requestId', requestId);
      xhr.open('PUT', '/api/client/upload?' + query.toString());
      xhr.setRequestHeader('X-Device-Id', deviceId);
      xhr.setRequestHeader('X-Device-Key', deviceKey);
      xhr.setRequestHeader('X-File-Name', encodeURIComponent(entry.file.name));
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = (event) => {
        if (event.lengthComputable) onProgress(event.loaded, event.total);
      };
      xhr.onload = () => {
        let data = null;
        try {
          data = JSON.parse(xhr.responseText);
        } catch (_) {
          data = null;
        }
        if (xhr.status >= 200 && xhr.status < 300 && data && data.ok) {
          resolve(data);
        } else {
          reject(new Error((data && data.error) || 'HTTP ' + xhr.status));
        }
      };
      xhr.onerror = () => reject(new Error('网络错误'));
      xhr.onabort = () => reject(new Error('上传已取消'));
      xhr.send(entry.file);
    });
  }

  async function uploadFiles(entries, options) {
    const list = (entries || []).filter((entry) => entry && entry.file);
    if (!list.length) return;
    const opts = options || {};
    const batchId = randomId(6);
    const summary = $('#uploadSummary');
    let done = 0;
    for (let index = 0; index < list.length; index += 1) {
      const entry = list[index];
      const ui = addUploadRow(entry.path || entry.file.name, entry.file.size);
      summary.textContent = '上传中 ' + (index + 1) + ' / ' + list.length;
      try {
        const result = await uploadOne(entry, batchId, opts.requestId, (loaded, total) => {
          updateUploadRow(ui, loaded, total, fmtBytes(loaded) + ' / ' + fmtBytes(total));
        });
        const file = result.file || {};
        updateUploadRow(ui, file.size || entry.file.size, file.size || entry.file.size, '已完成 · SHA-256 ' + shortSha(file.sha256));
        ui.bar.style.width = '100%';
        ui.tag.textContent = '已完成';
        ui.tag.className = 'tag ok';
        done += 1;
      } catch (err) {
        ui.meta.textContent = '失败：' + err.message;
        ui.tag.textContent = '失败';
        ui.tag.className = 'tag fail';
        toast(entry.file.name + ' 上传失败：' + err.message, 'error');
      }
    }
    summary.textContent = done ? '已发送 ' + done + ' 个文件' : '支持多文件与文件夹';
    if (done) toast('已发送 ' + done + ' 个文件给服务器', 'ok');
    loadState();
  }

  function normalizeFileList(fileList) {
    return Array.from(fileList || []).map((file) => ({
      file,
      path: file.webkitRelativePath || file.name,
    }));
  }

  async function entriesFromDataTransfer(dataTransfer) {
    const items = dataTransfer.items;
    if (!items || !items.length || !items[0].webkitGetAsEntry) {
      return normalizeFileList(dataTransfer.files);
    }
    const out = [];
    const walk = async (entry, prefix) => {
      if (!entry) return;
      if (entry.isFile) {
        const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
        out.push({ file, path: prefix + file.name });
        return;
      }
      if (entry.isDirectory) {
        const reader = entry.createReader();
        let batch = [];
        do {
          batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
          for (const child of batch) {
            await walk(child, prefix + entry.name + '/');
          }
        } while (batch.length);
      }
    };
    for (const item of items) {
      const entry = item.webkitGetAsEntry && item.webkitGetAsEntry();
      if (entry) await walk(entry, '');
    }
    return out;
  }

  function triggerBrowserDownload(file) {
    const anchor = document.createElement('a');
    anchor.href = file.url;
    anchor.download = file.name;
    anchor.rel = 'noopener';
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
  }

  async function streamToDisk(file, onProgress) {
    const handle = await window.showSaveFilePicker({ suggestedName: file.name });
    const writable = await handle.createWritable();
    const response = await fetch(file.url, {
      headers: { 'X-Device-Id': deviceId, 'X-Device-Key': deviceKey },
    });
    if (!response.ok || !response.body) {
      await writable.abort().catch(() => {});
      throw new Error('服务器拒绝下载 (' + response.status + ')');
    }
    const total = Number(response.headers.get('Content-Length') || file.size || 0);
    const reader = response.body.getReader();
    let loaded = 0;
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        await writable.write(value);
        loaded += value.length;
        onProgress(loaded, total);
      }
      await writable.close();
    } catch (err) {
      await writable.abort().catch(() => {});
      throw err;
    }
  }

  async function downloadFile(file, onProgress) {
    if (window.showSaveFilePicker) {
      await streamToDisk(file, onProgress);
      await api('/api/client/ack', {
        method: 'POST',
        body: JSON.stringify({ transferId: file.transferId, status: '已完成' }),
      });
      return true;
    }
    triggerBrowserDownload(file);
    return false;
  }

  async function receiveFile(file, button) {
    const original = button.textContent;
    button.disabled = true;
    button.textContent = '准备中…';
    try {
      const saved = await downloadFile(file, (loaded, total) => {
        button.textContent = total ? Math.round((loaded / total) * 100) + '%' : fmtBytes(loaded);
      });
      toast(saved ? '已保存 ' + file.name : '已交给浏览器下载 ' + file.name, 'ok');
    } catch (err) {
      if (err && err.name === 'AbortError') {
        button.disabled = false;
        button.textContent = original;
        return;
      }
      toast('接收失败：' + err.message, 'error');
      button.disabled = false;
      button.textContent = '重试';
      return;
    } finally {
      setTimeout(loadState, 1200);
    }
  }

  async function receiveAll(offer, button) {
    button.disabled = true;
    for (const file of offer.files) {
      try {
        await downloadFile(file);
      } catch (err) {
        if (!err || err.name !== 'AbortError') toast(file.name + ' 接收失败：' + err.message, 'error');
      }
    }
    toast('已处理 ' + offer.files.length + ' 个文件');
    setTimeout(loadState, 1200);
    button.disabled = false;
  }

  async function rejectOffer(offer) {
    const confirmed = await askModal('拒绝文件', '确定拒绝这批文件吗？', { confirmText: '拒绝', danger: true });
    if (!confirmed) return;
    for (const file of offer.files) {
      try {
        await api('/api/client/ack', {
          method: 'POST',
          body: JSON.stringify({ transferId: file.transferId, status: '已拒绝' }),
        });
      } catch (_) {
        /* ignore */
      }
    }
    toast('已拒绝这批文件');
    loadState();
  }

  const dropZone = $('#dropZone');
  ['dragenter', 'dragover'].forEach((type) => {
    dropZone.addEventListener(type, (event) => {
      event.preventDefault();
      dropZone.classList.add('drag');
    });
  });
  ['dragleave', 'drop'].forEach((type) => {
    dropZone.addEventListener(type, (event) => {
      event.preventDefault();
      dropZone.classList.remove('drag');
    });
  });
  dropZone.addEventListener('drop', async (event) => {
    const entries = await entriesFromDataTransfer(event.dataTransfer);
    uploadFiles(entries);
  });

  $('#pickFiles').addEventListener('click', () => $('#fileInput').click());
  $('#pickDir').addEventListener('click', () => $('#dirInput').click());
  $('#fileInput').addEventListener('change', (event) => {
    uploadFiles(normalizeFileList(event.target.files));
    event.target.value = '';
  });
  $('#dirInput').addEventListener('change', (event) => {
    uploadFiles(normalizeFileList(event.target.files));
    event.target.value = '';
  });

  $('#requestInput').addEventListener('change', async (event) => {
    const entries = normalizeFileList(event.target.files);
    const requestId = requestTarget;
    requestTarget = null;
    event.target.value = '';
    if (!entries.length) return;
    await uploadFiles(entries, { requestId });
  });

  $('#saveName').addEventListener('click', () => {
    const name = (nameInput.value || '').trim().slice(0, 24);
    if (!name) {
      toast('设备名称不能为空', 'error');
      return;
    }
    storeSet('fd.name', name);
    nameInput.blur();
    toast('设备名称已更新', 'ok');
    connectEvents();
    loadState();
  });

  nameInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') $('#saveName').click();
  });

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) loadState();
  });

  setOnline(false);
  connectEvents();
  loadState();
  setInterval(loadState, 12000);
})();
