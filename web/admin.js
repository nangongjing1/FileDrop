(() => {
  'use strict';

  const $ = (selector) => document.querySelector(selector);

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

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (_) {
      /* fall through */
    }
    const helper = document.createElement('textarea');
    helper.value = text;
    helper.style.position = 'fixed';
    helper.style.opacity = '0';
    document.body.appendChild(helper);
    helper.select();
    let ok = false;
    try {
      ok = document.execCommand('copy');
    } catch (_) {
      ok = false;
    }
    helper.remove();
    return ok;
  }

  function triggerDownload(url, name) {
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = name || '';
    anchor.rel = 'noopener';
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
  }

  function randomId(bytes) {
    const buffer = new Uint8Array(bytes);
    window.crypto.getRandomValues(buffer);
    return Array.from(buffer, (value) => value.toString(16).padStart(2, '0')).join('');
  }

  function statusTag(status) {
    const tag = el('span', 'tag', status);
    if (status === '已完成') tag.classList.add('ok');
    else if (status === '待接收' || status === '发送中' || status === '上传中' || status === '待发送') tag.classList.add('wait');
    else if (status === '已拒绝' || status === '已中断' || status === '写入失败') tag.classList.add('fail');
    return tag;
  }

  let snapshot = null;
  let lastSignature = '';
  let events = null;
  let reloadTimer = null;
  let lastOnline = new Set();
  let lastStatuses = new Map();

  const selected = new Set();

  async function api(path, options) {
    const response = await fetch(path, options);
    let data = null;
    try {
      data = await response.json();
    } catch (_) {
      data = null;
    }
    if (!response.ok || !data || data.ok === false) {
      const error = new Error((data && data.error) || 'HTTP ' + response.status);
      error.status = response.status;
      throw error;
    }
    return data;
  }

  function setAdminOnline(online) {
    $('#adminDot').classList.toggle('on', online);
    $('#adminDot').classList.toggle('warn', !online);
    $('#adminState').textContent = online ? '实时连接正常' : '连接断开';
    $('#connBanner').classList.toggle('show', !online);
  }

  function scheduleReload() {
    clearTimeout(reloadTimer);
    reloadTimer = setTimeout(loadState, 120);
  }

  async function loadState() {
    try {
      const data = await api('/api/admin/state');
      snapshot = data;
      setAdminOnline(true);
      const signature = JSON.stringify([data.server, data.devices, data.staging, data.received, data.transfers, data.requests]);
      if (signature !== lastSignature) {
        lastSignature = signature;
        render();
      }
    } catch (err) {
      setAdminOnline(false);
      if (err.status === 403) {
        $('#adminState').textContent = '需要管理权限';
        $('#connBanner').textContent = '管理权限已失效，请通过启动服务端时控制台打印的地址重新打开。';
        $('#connBanner').classList.add('show');
      }
    }
  }

  function connectEvents() {
    if (events) events.close();
    events = new EventSource('/api/admin/events');
    events.addEventListener('state', scheduleReload);
    events.onopen = () => {
      setAdminOnline(true);
      scheduleReload();
    };
    events.onerror = () => setAdminOnline(false);
  }

  function render() {
    if (!snapshot) return;
    renderServer();
    renderDevices(snapshot.devices || []);
    renderRequests(snapshot.requests || []);
    renderStaging(snapshot.staging || []);
    renderReceived(snapshot.received || []);
    renderHistory(snapshot.transfers || []);
    notifyChanges(snapshot.devices || [], snapshot.transfers || []);
  }

  function renderServer() {
    const server = snapshot.server || {};
    $('#clientUrl').textContent = server.clientUrl || '';
    $('#clientUrl').title = (server.clientUrlIp || '') + '  ' + (server.dataDir || '');
    $('#serverIp').textContent = server.ip || '未知地址';
    $('#dataDir').textContent = server.dataDir || '-';

    const hint = $('#networkHint');
    hint.textContent = '';
    const line1 = el('div');
    line1.appendChild(document.createTextNode('客户端访问：'));
    line1.appendChild(el('code', null, server.clientUrl || ''));
    const line2 = el('div');
    if (server.dns) {
      line2.appendChild(document.createTextNode('把其它设备的 DNS 指向 '));
      line2.appendChild(el('code', null, server.ip || ''));
      line2.appendChild(document.createTextNode('，即可通过 ' + (server.hostname || 'filedrop.ng') + ' 访问本服务。'));
    } else {
      line2.appendChild(document.createTextNode('内置 DNS 未启用，可直接使用 '));
      line2.appendChild(el('code', null, server.clientUrlIp || ''));
      line2.appendChild(document.createTextNode('，或为客户端修改 hosts。'));
    }
    hint.appendChild(line1);
    hint.appendChild(line2);
  }

  function updateSendButton() {
    const staging = (snapshot && snapshot.staging) || [];
    const button = $('#sendSelected');
    button.disabled = selected.size === 0 || staging.length === 0;
    button.textContent = selected.size ? '发送到已选设备 (' + selected.size + ')' : '发送到已选设备';
  }

  function renderDevices(devices) {
    const host = $('#devices');
    host.textContent = '';
    const known = new Set(devices.map((device) => device.id));
    for (const id of Array.from(selected)) {
      if (!known.has(id)) selected.delete(id);
    }

    const online = devices.filter((device) => device.online).length;
    $('#onlineCount').textContent = String(online);

    if (!devices.length) {
      const empty = el('div', 'empty', '暂无设备，等待局域网设备访问客户端页面…');
      empty.style.gridColumn = '1/-1';
      host.appendChild(empty);
      updateSendButton();
      return;
    }

    devices.forEach((device) => {
      const card = el('div', 'device' + (device.online ? '' : ' offline'));

      const checkbox = document.createElement('input');
      checkbox.type = 'checkbox';
      checkbox.checked = selected.has(device.id);
      checkbox.addEventListener('change', () => {
        if (checkbox.checked) selected.add(device.id);
        else selected.delete(device.id);
        updateSendButton();
      });

      const main = el('div', 'device-main');
      const nameRow = el('div', 'device-name');
      nameRow.appendChild(el('span', 'dot ' + (device.online ? 'on' : 'off')));
      const nameText = el('span', null, device.name || device.id);
      nameText.title = device.name || '';
      nameRow.appendChild(nameText);
      main.appendChild(nameRow);
      const metaText = device.ip + ' · ' + (device.platform || '未知设备') + ' · ' +
        (device.online ? '在线' : '最后在线 ' + timeLabel(device.lastSeen));
      main.appendChild(el('div', 'device-meta', metaText));

      const actions = el('div', 'device-actions');
      const rename = el('button', 'ghost small', '重命名');
      rename.type = 'button';
      rename.addEventListener('click', () => renameDevice(device));
      const request = el('button', 'ghost small', '请求文件');
      request.type = 'button';
      request.addEventListener('click', () => requestFile(device));
      actions.appendChild(rename);
      actions.appendChild(request);

      card.appendChild(checkbox);
      card.appendChild(main);
      card.appendChild(actions);
      host.appendChild(card);
    });

    updateSendButton();
  }

  function renderRequests(requests) {
    const host = $('#requests');
    host.textContent = '';
    $('#requestCount').textContent = String(requests.filter((request) => request.status === '待发送').length);
    if (!requests.length) {
      host.appendChild(el('div', 'empty', '暂无请求'));
      return;
    }
    requests.slice(0, 12).forEach((request) => {
      const item = el('div', 'offer');
      const head = el('div', 'offer-head');
      head.appendChild(el('div', 'offer-title', request.deviceName || request.deviceId));
      head.appendChild(el('span', 'muted', timeLabel(request.created)));
      item.appendChild(head);
      item.appendChild(el('div', 'file-meta', request.note || '请求文件'));
      const footer = el('div', 'row');
      footer.style.marginTop = '10px';
      footer.appendChild(statusTag(request.status || '待发送'));
      item.appendChild(footer);
      host.appendChild(item);
    });
  }

  function renderStaging(files) {
    const host = $('#staging');
    host.textContent = '';
    $('#stagingCount').textContent = String(files.length);
    if (!files.length) {
      const item = el('li', 'empty', '暂无待发送文件');
      item.style.border = 'none';
      host.appendChild(item);
      updateSendButton();
      return;
    }
    files.forEach((file) => {
      const item = el('li');
      const main = el('div', 'file-main');
      const name = el('div', 'file-name', file.name);
      name.title = file.relPath || file.name;
      main.appendChild(name);
      const meta = el('div', 'file-meta', fmtBytes(file.size) + ' · SHA-256 ' + shortSha(file.sha256));
      meta.title = 'SHA-256: ' + (file.sha256 || '');
      main.appendChild(meta);
      item.appendChild(main);
      const actions = el('div', 'file-actions');
      const remove = el('button', 'danger small', '移除');
      remove.addEventListener('click', () => removeFile(file.id));
      actions.appendChild(remove);
      item.appendChild(actions);
      host.appendChild(item);
    });
    updateSendButton();
  }

  function renderReceived(files) {
    const host = $('#received');
    host.textContent = '';
    $('#receivedCount').textContent = String(files.length);
    if (!files.length) {
      const item = el('li', 'empty', '尚未收到文件');
      item.style.border = 'none';
      host.appendChild(item);
      return;
    }
    files.forEach((file) => {
      const item = el('li');
      const main = el('div', 'file-main');
      const name = el('div', 'file-name', file.name);
      name.title = file.relPath || file.name;
      main.appendChild(name);
      const meta = el('div', 'file-meta',
        '来自 ' + (file.ownerName || '设备') + ' · ' + fmtBytes(file.size) + ' · ' + timeLabel(file.created));
      meta.title = 'SHA-256: ' + (file.sha256 || '');
      main.appendChild(meta);
      item.appendChild(main);
      const actions = el('div', 'file-actions');
      const download = el('button', 'primary small', '下载');
      download.addEventListener('click', () => {
        triggerDownload('/api/admin/download/' + encodeURIComponent(file.id), file.name);
      });
      const forward = el('button', 'ghost small', '转发');
      forward.title = '转发给已勾选的设备';
      forward.addEventListener('click', () => sendFiles([file.id]));
      const remove = el('button', 'danger small', '移除');
      remove.addEventListener('click', () => removeFile(file.id));
      actions.appendChild(download);
      actions.appendChild(forward);
      actions.appendChild(remove);
      item.appendChild(actions);
      host.appendChild(item);
    });
  }

  function renderHistory(transfers) {
    const body = $('#historyBody');
    body.textContent = '';
    $('#historyHint').textContent = transfers.length ? '最近 ' + transfers.length + ' 条' : '';
    if (!transfers.length) {
      const row = document.createElement('tr');
      const cell = document.createElement('td');
      cell.colSpan = 7;
      cell.className = 'empty';
      cell.style.border = 'none';
      cell.textContent = '暂无传输记录';
      row.appendChild(cell);
      body.appendChild(row);
      return;
    }

    transfers.forEach((transfer) => {
      const row = document.createElement('tr');

      row.appendChild(el('td', 'mono', timeLabel(transfer.created)));
      row.appendChild(el('td', null, transfer.direction === 'to-device' ? '下发' : '接收'));

      const deviceCell = el('td');
      deviceCell.textContent = transfer.deviceName || transfer.deviceId || '';
      row.appendChild(deviceCell);

      const fileCell = el('td');
      const fileName = el('div', 'file-name', transfer.fileName || '');
      fileName.title = transfer.fileName || '';
      fileCell.appendChild(fileName);
      row.appendChild(fileCell);

      row.appendChild(el('td', 'muted', fmtBytes(transfer.size)));

      const statusCell = el('td');
      statusCell.appendChild(statusTag(transfer.status || ''));
      if (transfer.status === '发送中' || transfer.status === '上传中') {
        const ratio = transfer.size ? Math.min(100, Math.round((transfer.bytes / transfer.size) * 100)) : 0;
        const progress = el('div', 'progress');
        const bar = el('span');
        bar.style.width = ratio + '%';
        progress.appendChild(bar);
        statusCell.appendChild(progress);
      }
      row.appendChild(statusCell);

      const shaCell = el('td', 'mono', shortSha(transfer.sha256));
      shaCell.title = 'SHA-256: ' + (transfer.sha256 || '');
      row.appendChild(shaCell);

      body.appendChild(row);
    });
  }

  function notifyChanges(devices, transfers) {
    const online = new Set(devices.filter((device) => device.online).map((device) => device.id));
    if (lastOnline.size) {
      devices.forEach((device) => {
        const wasOnline = lastOnline.has(device.id);
        if (device.online && !wasOnline) toast(device.name + ' 已连接', 'ok');
        else if (!device.online && wasOnline) toast(device.name + ' 已断开');
      });
    }
    lastOnline = online;

    const statuses = new Map();
    transfers.forEach((transfer) => {
      statuses.set(transfer.id, transfer.status);
      const previous = lastStatuses.get(transfer.id);
      if (previous && previous !== transfer.status && transfer.status === '已完成') {
        const action = transfer.direction === 'to-device' ? '发送完成' : '接收完成';
        toast(action + '：' + transfer.fileName, 'ok');
      }
    });
    lastStatuses = statuses;
  }

  async function sendFiles(fileIds) {
    if (!selected.size) {
      toast('请先勾选接收设备', 'error');
      return;
    }
    try {
      const result = await api('/api/admin/send', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ deviceIds: Array.from(selected), fileIds }),
      });
      toast('已发送 ' + result.sent + ' 个文件', 'ok');
      loadState();
    } catch (err) {
      toast('发送失败：' + err.message, 'error');
    }
  }

  async function renameDevice(device) {
    const next = await askModal('重命名设备', '输入新的设备名称', {
      input: true,
      value: device.name || '',
      confirmText: '保存',
    });
    if (next === null) return;
    const name = next.trim();
    if (!name) return;
    try {
      await api('/api/admin/rename', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ deviceId: device.id, name }),
      });
      toast('已重命名', 'ok');
      loadState();
    } catch (err) {
      toast('重命名失败：' + err.message, 'error');
    }
  }

  async function requestFile(device) {
    const note = await askModal('请求文件', '客户端会实时收到提示，可填写说明：', {
      input: true,
      multiline: true,
      value: '请把文件发送给服务器',
      confirmText: '发送请求',
    });
    if (note === null) return;
    try {
      await api('/api/admin/request', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ deviceIds: [device.id], note }),
      });
      toast('请求已发送', 'ok');
      loadState();
    } catch (err) {
      toast('请求失败：' + err.message, 'error');
    }
  }

  async function removeFile(fileId) {
    try {
      await api('/api/admin/remove', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ fileId }),
      });
      toast('已移除', 'ok');
      loadState();
    } catch (err) {
      toast('移除失败：' + err.message, 'error');
    }
  }

  async function clearStaging() {
    if (!snapshot || !snapshot.staging.length) return;
    const confirmed = await askModal('清空待发送文件', '将移除所有待发送文件，确定继续？', {
      confirmText: '清空',
      danger: true,
    });
    if (!confirmed) return;
    for (const file of snapshot.staging) {
      try {
        await api('/api/admin/remove', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ fileId: file.id }),
        });
      } catch (_) {
        /* ignore */
      }
    }
    toast('已清空待发送文件');
    loadState();
  }

  function addQueueRow(path, size) {
    const host = $('#uploadQueue');
    const row = el('li');
    const main = el('div', 'file-main');
    const name = el('div', 'file-name', path);
    name.title = path;
    main.appendChild(name);
    const meta = el('div', 'file-meta', fmtBytes(size) + ' · 上传中');
    main.appendChild(meta);
    const progress = el('div', 'progress');
    const bar = el('span');
    progress.appendChild(bar);
    main.appendChild(progress);
    row.appendChild(main);
    const tag = statusTag('上传中');
    row.appendChild(tag);
    host.insertBefore(row, host.firstChild);
    while (host.children.length > 8) host.removeChild(host.lastChild);
    return { row, bar, meta, tag, size };
  }

  function updateQueueRow(ui, loaded, total, text) {
    const totalSize = total || ui.size || 0;
    ui.bar.style.width = totalSize ? ((loaded / totalSize) * 100).toFixed(1) + '%' : '0%';
    if (text) ui.meta.textContent = text;
  }

  function uploadAdminOne(entry, batchId, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      const query = new URLSearchParams({ batchId, path: entry.path || entry.file.name });
      xhr.open('PUT', '/api/admin/upload?' + query.toString());
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
        if (xhr.status >= 200 && xhr.status < 300 && data && data.ok) resolve(data);
        else reject(new Error((data && data.error) || 'HTTP ' + xhr.status));
      };
      xhr.onerror = () => reject(new Error('网络错误'));
      xhr.send(entry.file);
    });
  }

  async function uploadEntries(entries) {
    const list = (entries || []).filter((entry) => entry && entry.file);
    if (!list.length) return;
    const batchId = randomId(6);
    for (let index = 0; index < list.length; index += 1) {
      const entry = list[index];
      const ui = addQueueRow(entry.path || entry.file.name, entry.file.size);
      try {
        await uploadAdminOne(entry, batchId, (loaded, total) => {
          updateQueueRow(ui, loaded, total, fmtBytes(loaded) + ' / ' + fmtBytes(total));
        });
        updateQueueRow(ui, entry.file.size, entry.file.size, '已加入待发送列表');
        ui.tag.textContent = '完成';
        ui.tag.className = 'tag ok';
      } catch (err) {
        ui.meta.textContent = '上传失败：' + err.message;
        ui.tag.textContent = '失败';
        ui.tag.className = 'tag fail';
      }
    }
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
    uploadEntries(entries);
  });

  $('#pickFiles').addEventListener('click', () => $('#fileInput').click());
  $('#pickDir').addEventListener('click', () => $('#dirInput').click());
  $('#fileInput').addEventListener('change', (event) => {
    uploadEntries(normalizeFileList(event.target.files));
    event.target.value = '';
  });
  $('#dirInput').addEventListener('change', (event) => {
    uploadEntries(normalizeFileList(event.target.files));
    event.target.value = '';
  });

  $('#selectAll').addEventListener('click', () => {
    if (!snapshot) return;
    selected.clear();
    snapshot.devices.forEach((device) => selected.add(device.id));
    renderDevices(snapshot.devices);
  });

  $('#sendSelected').addEventListener('click', () => {
    if (!snapshot) return;
    sendFiles(snapshot.staging.map((file) => file.id));
  });

  $('#clearStaging').addEventListener('click', clearStaging);

  $('#clearHistory').addEventListener('click', async () => {
    const confirmed = await askModal('清空传输记录', '确定清空本次传输记录？', {
      confirmText: '清空',
      danger: true,
    });
    if (!confirmed) return;
    try {
      await api('/api/admin/clear-history', { method: 'POST' });
      toast('记录已清空');
      loadState();
    } catch (err) {
      toast('清空失败：' + err.message, 'error');
    }
  });

  $('#copyUrl').addEventListener('click', async () => {
    if (!snapshot) return;
    const text = snapshot.server.clientUrl;
    if (await copyText(text)) {
      toast('已复制客户端地址', 'ok');
      return;
    }
    await askModal('复制地址', '请手动复制下面的地址：', {
      input: true,
      value: text,
      confirmText: '关闭',
    });
  });

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) loadState();
  });

  setAdminOnline(false);
  connectEvents();
  loadState();
  setInterval(loadState, 8000);
})();
