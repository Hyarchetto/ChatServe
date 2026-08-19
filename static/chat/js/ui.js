// DOM 引用 + 所有 UI 渲染函数

const messagesEl = document.getElementById('messages');
const msgInput = document.getElementById('msg-input');
const sendBtn = document.getElementById('send-btn');
const fileInput = document.getElementById('file-input');
const filePreview = document.getElementById('file-preview');
const filePreviewName = document.getElementById('file-preview-name');
const filePreviewSize = document.getElementById('file-preview-size');
const fileCancelBtn = document.getElementById('file-cancel-btn');
const fileSendBtn = document.getElementById('file-send-btn');

// ---------- 连接状态 ----------

function setConnStatus(type, text) {
    const el = document.getElementById('conn-status');
    el.className = 'status-' + type;
    el.textContent = text;
}

// ---------- 消息渲染 ----------

function addChatMessage(nick, text, side) {
    const time = currentTime();
    const div = document.createElement('div');
    div.className = 'msg ' + side;

    const header = document.createElement('div');
    header.className = 'msg-header';
    const nameSpan = document.createElement('span');
    nameSpan.className = 'msg-name';
    nameSpan.textContent = nick;
    const timeSpan = document.createElement('span');
    timeSpan.className = 'msg-time';
    timeSpan.textContent = time;
    header.appendChild(nameSpan);
    header.appendChild(timeSpan);

    const body = document.createElement('div');
    body.className = 'msg-body';
    body.textContent = text;

    div.appendChild(header);
    div.appendChild(body);
    messagesEl.appendChild(div);
    messagesEl.scrollTop = messagesEl.scrollHeight;
}

function addSystemMessage(text) {
    const div = document.createElement('div');
    div.className = 'msg sys';
    div.textContent = text;
    messagesEl.appendChild(div);
    messagesEl.scrollTop = messagesEl.scrollHeight;
}

function addFileNotification(sender, fileId, filename, filesize) {
    const time = currentTime();
    const div = document.createElement('div');
    div.className = 'msg other';

    const header = document.createElement('div');
    header.className = 'msg-header';
    const nameSpan = document.createElement('span');
    nameSpan.className = 'msg-name';
    nameSpan.textContent = sender + ' 发送了文件';
    const timeSpan = document.createElement('span');
    timeSpan.className = 'msg-time';
    timeSpan.textContent = time;
    header.appendChild(nameSpan);
    header.appendChild(timeSpan);

    const body = document.createElement('div');
    body.className = 'msg-body';

    const card = document.createElement('div');
    card.className = 'file-card';
    card.dataset.fileId = fileId;

    const icon = document.createElement('span');
    icon.className = 'file-card-icon';
    icon.textContent = '📄';

    const info = document.createElement('div');
    info.className = 'file-card-info';
    const nameEl = document.createElement('div');
    nameEl.className = 'file-card-name';
    nameEl.textContent = filename;
    const sizeEl = document.createElement('div');
    sizeEl.className = 'file-card-size';
    sizeEl.textContent = formatFileSize(filesize);
    info.appendChild(nameEl);
    info.appendChild(sizeEl);

    const actions = document.createElement('div');
    actions.className = 'file-card-actions';

    const progress = document.createElement('span');
    progress.className = 'file-card-progress';

    actions.appendChild(progress);

    card.appendChild(icon);
    card.appendChild(info);
    card.appendChild(actions);

    body.appendChild(card);
    div.appendChild(header);
    div.appendChild(body);
    messagesEl.appendChild(div);
    messagesEl.scrollTop = messagesEl.scrollHeight;

    renderFileActions(fileId, 'idle');
}

// 上传卡片，仿下载卡片，带取消按钮
function addUploadCard(fileId, filename, filesize) {
    const time = currentTime();
    const div = document.createElement('div');
    div.className = 'msg self';

    const header = document.createElement('div');
    header.className = 'msg-header';
    const nameSpan = document.createElement('span');
    nameSpan.className = 'msg-name';
    nameSpan.textContent = '你上传了文件';
    const timeSpan = document.createElement('span');
    timeSpan.className = 'msg-time';
    timeSpan.textContent = time;
    header.appendChild(nameSpan);
    header.appendChild(timeSpan);

    const body = document.createElement('div');
    body.className = 'msg-body';

    const card = document.createElement('div');
    card.className = 'file-card';
    card.dataset.fileId = fileId;

    const icon = document.createElement('span');
    icon.className = 'file-card-icon';
    icon.textContent = '📄';

    const info = document.createElement('div');
    info.className = 'file-card-info';
    const nameEl = document.createElement('div');
    nameEl.className = 'file-card-name';
    nameEl.textContent = filename;
    const sizeEl = document.createElement('div');
    sizeEl.className = 'file-card-size';
    sizeEl.textContent = formatFileSize(filesize);
    info.appendChild(nameEl);
    info.appendChild(sizeEl);

    const actions = document.createElement('div');
    actions.className = 'file-card-actions';

    const cancelBtn = document.createElement('button');
    cancelBtn.className = 'file-card-cancel';
    cancelBtn.textContent = '✖ 取消上传';
    cancelBtn.onclick = () => cancelUploadFile(fileId);

    actions.appendChild(cancelBtn);

    card.appendChild(icon);
    card.appendChild(info);
    card.appendChild(actions);

    body.appendChild(card);
    div.appendChild(header);
    div.appendChild(body);
    messagesEl.appendChild(div);
    messagesEl.scrollTop = messagesEl.scrollHeight;

    uploadCards[fileId] = card;
}

// ---------- 按钮状态 ----------

function updateBtnState() {
    const camBtn = document.getElementById('cam-btn');
    const micBtn = document.getElementById('mic-btn');
    if (camBtn) camBtn.classList.toggle('active', camOn);
    if (micBtn) micBtn.classList.toggle('active', micOn);
}

// ---------- 视频网格 ----------

function updateVideoGrid() {
    const grid = document.getElementById('video-grid');
    if (!grid) return;

    // 已有 cell 按 peerId 记录，只增删不重建
    // 播放中的 video 一旦重挂载会闪黑，故不整体清空 innerHTML
    const cells = new Map();
    for (const child of grid.children) {
        cells.set(child.dataset.peerId, child);
    }

    // 自己排第一，其他人按原顺序
    const sorted = [...memberList];
    const selfIdx = sorted.findIndex(m => m.id === myId);
    if (selfIdx > 0) {
        const self = sorted.splice(selfIdx, 1)[0];
        sorted.unshift(self);
    }
    if (sorted.length === 0 && myId) {
        sorted.push({ id: myId, nick: myNick || '我' });
    }

    // 网格列数：1人 1列，2~4人 2列，5~9人 3列
    const count = sorted.length;
    let cols = 1;
    if (count >= 5) cols = 3;
    else if (count >= 2) cols = 2;
    grid.style.gridTemplateColumns = 'repeat(' + cols + ', 1fr)';
    grid.style.gridTemplateRows = 'repeat(' + Math.ceil(count / cols) + ', 1fr)';

    for (const member of sorted) {
        let cell = cells.get(member.id);
        if (!cell) {
            cell = document.createElement('div');
            cell.className = 'video-cell';
            cell.dataset.peerId = member.id;
            const video = document.createElement('video');
            video.autoplay = true;
            video.playsInline = true;
            video.className = '';
            cell.appendChild(video);
            const label = document.createElement('div');
            label.className = 'video-label';
            cell.appendChild(label);
            grid.appendChild(cell);
        }

        const video = cell.querySelector('video');
        const label = cell.querySelector('.video-label');
        let ph = cell.querySelector('.video-placeholder');
        let hasVideo = false;

        if (member.id === myId) {
            video.muted = true;
            if (myStream && camOn) {
                if (video.srcObject !== myStream) video.srcObject = myStream;
                video.classList.add('active');
                hasVideo = true;
            } 
            else {
                video.classList.remove('active');
            }
        } 
        else {
            const peer = peers[member.id];
            const peerMedia = peer?.mediaState;
            const mediaOff = peerMedia && peerMedia.video === 'off';
            const hasLive = peer?.stream && peer.stream.getVideoTracks().some(t => t.readyState === 'live' && !t.muted);
            if (!mediaOff && hasLive) {
                if (video.srcObject !== peer.stream) video.srcObject = peer.stream;
                video.classList.add('active');
                hasVideo = true;
            } else {
                video.classList.remove('active');
            }
        }

        if (hasVideo) {
            if (ph) ph.remove();
        } 
        else {
            if (!ph) {
                ph = document.createElement('div');
                ph.className = 'video-placeholder';
                ph.innerHTML = '<span class="video-placeholder-icon">👤</span>'
                             + '<span class="video-placeholder-name"></span>';
                cell.appendChild(ph);
            }
            ph.querySelector('.video-placeholder-name').textContent = member.nick || member.id;
        }

        label.textContent = member.nick || member.id;
    }

    // 移除已不在成员列表的 cell
    for (const [id, cell] of cells) {
        if (!sorted.some(m => m.id === id)) {
            cell.remove();
        }
    }

    document.getElementById('member-count').textContent = '在线: ' + memberList.length + ' 人';
}

// ---------- 房间初始化OK| 协议响应----------

function initRoom(room, nick, id) {
    document.getElementById('room-name').textContent = '房间: ' + room;
    myNick = nick;
    myId = id || '';
    messagesEl.innerHTML = '';
    memberList = [];
    updateVideoGrid();
    updateBtnState();
}

// ---------- 下载进度更新 ----------

function updateDownloadProgress(fileId, received, total) {
    const card = document.querySelector('[data-file-id="' + fileId + '"]');
    if (!card) return;
    const pct = Math.min(100, Math.round((received / total) * 100));
    const progressEl = card.querySelector('.file-card-progress');
    if (progressEl) progressEl.textContent = pct + '%';
}

// ---------- 下载卡状态渲染 ----------

// 下载卡动作区按状态重建 末尾始终保留进度 span
// 状态 idle 待下载 downloading 下载中 paused 已暂停 done 完成 invalid 失效
function renderFileActions(fileId, state) {
    const card = document.querySelector('[data-file-id="' + fileId + '"]');
    if (!card) return;
    const actions = card.querySelector('.file-card-actions');
    if (!actions) return;

    // 暂停态两按钮整行换到卡片底部
    actions.classList.toggle('stacked', state === 'paused');
    actions.innerHTML = '';

    function addBtn(cls, text, onClick, disabled) {
        const btn = document.createElement('button');
        btn.className = cls;
        btn.textContent = text;
        btn.disabled = !!disabled;
        if (onClick) btn.onclick = onClick;
        actions.appendChild(btn);
    }

    if (state === 'downloading') {
        addBtn('file-card-dl downloading', '⏸ 暂停下载', () => pauseDownload(fileId));
    } 
    else if (state === 'paused') {
        addBtn('file-card-dl', '▶ 继续下载', () => resumeDownload(fileId));
        addBtn('file-card-cancel', '✖ 取消下载', () => cancelDownloadFile(fileId));
    } 
    else if (state === 'done') {
        addBtn('file-card-dl', '重新下载', () => startDownload(fileId));
    } 
    else if (state === 'invalid') {
        addBtn('file-card-dl disabled', '已失效', null, true);
    } 
    else {
        addBtn('file-card-dl', '⬇ 下载', () => startDownload(fileId));
    }

    const progress = document.createElement('span');
    progress.className = 'file-card-progress';
    actions.appendChild(progress);
}

function setFileCardDone(fileId) {
    renderFileActions(fileId, 'done');
}

function setFileCardInvalid(fileId) {
    renderFileActions(fileId, 'invalid');
}
