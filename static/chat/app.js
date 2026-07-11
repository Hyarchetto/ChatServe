// ChatServe WebSocket 聊天室前端 — 支持文件传输
// 协议：
//   文本: JOIN/MSG/SYS/MEMBERS/UPLOAD/UPOK/UPCK/UPDONE/DONE/FILE/DOWNLOAD/DWINFO/DWCHUNK/DWNDONE
//   二进制: 文件数据块

let ws = null;
let reconnectTimer = null;
let myNick = '';
let myRoom = '';
let memberList = [];
let isLeaving = false;
let isReconnect = false;

// ---------- 文件上传状态 ----------
let pendingFile = null;          // 用户选择的待发送文件
let uploadFileId = '';
let uploadChunkSize = 64 * 1024; // 64KB 每块
let cancelUpload = false;

// ---------- 文件下载状态 ----------
let downloadSessions = {};       // file_id -> { chunks:[], received, total, filename }

// ===================== 时间格式化 =====================

function currentTime() {
    const d = new Date();
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
}

function formatFileSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

// ===================== DOM 引用 =====================

const messagesEl = document.getElementById('messages');
const msgInput = document.getElementById('msg-input');
const sendBtn = document.getElementById('send-btn');
const fileInput = document.getElementById('file-input');
const filePreview = document.getElementById('file-preview');
const filePreviewName = document.getElementById('file-preview-name');
const filePreviewSize = document.getElementById('file-preview-size');
const fileCancelBtn = document.getElementById('file-cancel-btn');
const fileSendBtn = document.getElementById('file-send-btn');
const uploadProgressWrap = document.getElementById('upload-progress-wrap');
const uploadProgressFill = document.getElementById('upload-progress-fill');
const uploadProgressText = document.getElementById('upload-progress-text');

// ===================== 连接状态指示 =====================

function setConnStatus(type, text) {
    const el = document.getElementById('conn-status');
    el.className = 'status-' + type;
    el.textContent = text;
}

// ===================== WebSocket 连接 =====================

function connect(room, nick) {
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    myNick = nick;
    myRoom = room;

    const url = `ws://${location.host}/ws`;
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        isLeaving = false;
        setConnStatus('connected', '● 已连接');
        document.getElementById('login').style.display = 'none';
        document.getElementById('chat').style.display = 'flex';
        document.getElementById('msg-input').focus();
        ws.send(`JOIN|${room}|${nick}`);
    };

    ws.onmessage = (evt) => {
        const data = evt.data;

        if (typeof data === 'string') {
            handleTextMessage(data);
        } else {
            handleBinaryMessage(data);
        }
    };

    ws.onclose = () => {
        cancelUpload = true;
        if (isLeaving) return;
        isReconnect = true;
        setConnStatus('reconnecting', '◌ 重连中...');
        addSystemMessage('连接断开，正在重连...');
        memberList = [];
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(() => {
            if (myRoom && myNick) connect(myRoom, myNick);
        }, 3000);
    };

    ws.onerror = () => {
        // onclose 会随之触发
    };
}

// ===================== 文本消息处理 =====================

function handleTextMessage(data) {
    if (data.startsWith('OK|')) {
        const parts = data.split('|');
        document.getElementById('room-name').textContent = `房间: ${parts[1]}`;
        messagesEl.innerHTML = '';
        memberList = [];
        if (isReconnect) {
            isReconnect = false;
            addSystemMessage('重新连接成功');
        } else {
            addSystemMessage(`您已加入房间 ${parts[1]}`);
        }
    }
    else if (data.startsWith('MSG|')) {
        const rest = data.substring(4);
        const idx = rest.indexOf('|');
        if (idx === -1) return;
        const nick = rest.substring(0, idx);
        const text = rest.substring(idx + 1);
        addChatMessage(nick, text, 'other');
        }
        else if (data.startsWith('SYS|')) {
            const text = data.substring(4);
        if (text.startsWith('ERR|')) {
            addSystemMessage('❌ ' + text.substring(4));
        } else {
            addSystemMessage(text);
        }
        // SYS 只用来显示消息，不参与成员列表维护
        // 成员列表的唯一来源是 MEMBERS，由服务端全量推送
        // 同名用户进出时靠 SYS 文本推断成员列表会出错
    }
    else if (data.startsWith('MEMBERS|')) {
        const raw = data.substring(8);
        memberList = raw ? raw.split(',') : [];
        updateMemberList(memberList);
    }
    // ---- 文件上传协议 ----
    else if (data.startsWith('UPOK|')) {
        uploadFileId = data.substring(5);
        // 开始上传文件数据
        startUpload();
    }
    else if (data.startsWith('UPCK|')) {
        const received = parseInt(data.substring(5), 10);
        if (pendingFile && uploadFileId) {
            const pct = Math.min(100, Math.round((received / pendingFile.size) * 100));
            uploadProgressFill.style.width = pct + '%';
            uploadProgressText.textContent = pct + '%';
        }
    }
    else if (data.startsWith('DONE|')) {
        // 上传完成或取消确认
        if (uploadFileId) {
            addSystemMessage(`📄 文件 "${pendingFile ? pendingFile.name : ''}" 上传完成`);
        }
        resetUpload();
    }
    // ---- 文件通知 ----
    else if (data.startsWith('FILE|')) {
        const parts = data.split('|');
        if (parts.length >= 5) {
            const fileId = parts[1];
            const filename = parts[2];
            const filesize = parseInt(parts[3], 10);
            const sender = parts[4];
            addFileNotification(sender, fileId, filename, filesize);
        }
    }
    // ---- 文件下载协议 ----
    else if (data.startsWith('DWINFO|')) {
        const parts = data.split('|');
        if (parts.length >= 4) {
            const fileId = parts[1];
            const filename = parts[2];
            const filesize = parseInt(parts[3], 10);
            // 初始化下载会话
            downloadSessions[fileId] = {
                chunks: [],
                received: 0,
                total: filesize,
                filename: filename
            };
        }
    }
    else if (data.startsWith('DWCHUNK|')) {
        const parts = data.split('|');
        if (parts.length >= 3) {
            const fileId = parts[1];
            const totalChunks = parseInt(parts[2], 10);
            if (downloadSessions[fileId]) {
                downloadSessions[fileId].totalChunks = totalChunks;
            }
        }
    }
    else if (data.startsWith('DWNDONE|')) {
        const fileId = data.substring(8);
        // 下载完成，组装并保存文件
        finishDownload(fileId);
    }
}

// ===================== 二进制消息处理 =====================

function handleBinaryMessage(data) {
    // 将二进制数据路由到最新的下载会话
    const ids = Object.keys(downloadSessions);
    if (ids.length === 0) return;

    // 找到最近的活跃下载
    const latestId = ids[ids.length - 1];
    const session = downloadSessions[latestId];
    if (!session) return;

    session.chunks.push(data);
    session.received += data.byteLength;

    // 更新下载进度
    const card = document.querySelector(`[data-file-id="${latestId}"]`);
    if (card) {
        const pct = Math.min(100, Math.round((session.received / session.total) * 100));
        const progressEl = card.querySelector('.file-card-progress');
        if (progressEl) {
            progressEl.textContent = pct + '%';
        }
    }
}

// ===================== 文件上传 =====================

function selectFile(file) {
    if (!file) return;
    pendingFile = file;
    filePreviewName.textContent = file.name;
    filePreviewSize.textContent = formatFileSize(file.size);
    uploadProgressWrap.style.display = 'none';
    // 恢复操作按钮显示
    document.querySelector('.file-preview-actions').style.display = 'flex';
    filePreview.style.display = 'flex';
    cancelUpload = false;
}

function resetUpload() {
    pendingFile = null;
    uploadFileId = '';
    cancelUpload = false;
    filePreview.style.display = 'none';
    uploadProgressWrap.style.display = 'none';
    uploadProgressFill.style.width = '0%';
    uploadProgressText.textContent = '0%';
    fileInput.value = '';
}

function startUpload() {
    if (!pendingFile || !uploadFileId || !ws || ws.readyState !== WebSocket.OPEN) return;

    // 显示进度条
    uploadProgressWrap.style.display = 'flex';
    uploadProgressFill.style.width = '0%';
    uploadProgressText.textContent = '0%';

    const file = pendingFile;
    const fileId = uploadFileId;
    const totalSize = file.size;
    let offset = 0;

    // 分块读取并发送
    function sendNextChunk() {
        if (cancelUpload || !ws || ws.readyState !== WebSocket.OPEN) {
            if (cancelUpload && ws && ws.readyState === WebSocket.OPEN) {
                ws.send('UPCANCEL|' + fileId);
            }
            return;
        }

        const chunk = file.slice(offset, offset + uploadChunkSize);
        if (chunk.size === 0) {
            // 发送完成
            ws.send('UPDONE|' + fileId);
            return;
        }

        const reader = new FileReader();
        reader.onload = (e) => {
            if (cancelUpload) return;
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(e.target.result);
            }
            offset += chunk.size;
            // 继续下一块
            setTimeout(sendNextChunk, 0);
        };
        reader.onerror = () => {
            addSystemMessage('❌ 文件读取失败');
            resetUpload();
        };
        reader.readAsArrayBuffer(chunk);
    }

    sendNextChunk();
}

// ===================== 文件下载 =====================

function finishDownload(fileId) {
    const session = downloadSessions[fileId];
    if (!session) return;

    // 合并所有 chunks
    const totalLen = session.received;
    const blob = new Blob(session.chunks);
    delete downloadSessions[fileId];

    // 触发浏览器下载
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = session.filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);

    // 更新卡片按钮状态
    const card = document.querySelector(`[data-file-id="${fileId}"]`);
    if (card) {
        const btn = card.querySelector('.file-card-dl');
        if (btn) {
            btn.textContent = '✅ 已下载';
            btn.disabled = true;
            btn.className = 'file-card-dl';
        }
        const progressEl = card.querySelector('.file-card-progress');
        if (progressEl) progressEl.textContent = '';
    }

    addSystemMessage(`📥 文件 "${session.filename}" 下载完成`);
}

// ===================== 消息渲染 =====================

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
    // 网络来的文件通知都在左侧
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

    // 文件卡片
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
    actions.style.display = 'flex';
    actions.style.alignItems = 'center';
    actions.style.gap = '6px';

    const dlBtn = document.createElement('button');
    dlBtn.className = 'file-card-dl';
    dlBtn.textContent = '⬇ 下载';
    dlBtn.onclick = () => startDownload(fileId, dlBtn);

    const progress = document.createElement('span');
    progress.className = 'file-card-progress';

    actions.appendChild(dlBtn);
    actions.appendChild(progress);

    card.appendChild(icon);
    card.appendChild(info);
    card.appendChild(actions);

    body.appendChild(card);
    div.appendChild(header);
    div.appendChild(body);
    messagesEl.appendChild(div);
    messagesEl.scrollTop = messagesEl.scrollHeight;
}

function startDownload(fileId, btn) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        addSystemMessage('❌ 未连接，无法下载');
        return;
    }
    btn.textContent = '⏳ 下载中...';
    btn.className = 'file-card-dl downloading';
    btn.disabled = true;
    ws.send('DOWNLOAD|' + fileId);
}

// ===================== 成员列表 =====================

function updateMemberList(members) {
    const container = document.getElementById('members');
    container.innerHTML = '';
    for (const m of members) {
        if (!m) continue;
        const item = document.createElement('div');
        item.className = 'member-item';
        item.textContent = m;
        container.appendChild(item);
    }
    document.getElementById('member-count').textContent = `在线: ${members.length} 人`;
}

// ===================== 发送消息 =====================

function sendMessage() {
    const text = msgInput.value.trim();
    if (!text || !ws || ws.readyState !== WebSocket.OPEN) return;

    addChatMessage(myNick, text, 'self');
    ws.send(text);
    msgInput.value = '';
    msgInput.focus();
}

// ===================== 事件绑定 =====================

sendBtn.onclick = sendMessage;

msgInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendMessage();
    }
});

// 文件选择
fileInput.addEventListener('change', (e) => {
    if (e.target.files.length > 0) {
        selectFile(e.target.files[0]);
    }
});

// 文件发送
fileSendBtn.onclick = () => {
    if (!pendingFile || !ws || ws.readyState !== WebSocket.OPEN) {
        addSystemMessage('❌ 未连接，无法发送文件');
        return;
    }
    // 隐藏操作按钮，显示进度
    document.querySelector('.file-preview-actions').style.display = 'none';
    // 发送 UPLOAD 命令
    ws.send(`UPLOAD|${pendingFile.name}|${pendingFile.size}|${myRoom}`);
};

// 取消文件
fileCancelBtn.onclick = () => {
    cancelUpload = true;
    if (uploadFileId && ws && ws.readyState === WebSocket.OPEN) {
        ws.send('UPCANCEL|' + uploadFileId);
    }
    resetUpload();
};

// 拖放支持
document.getElementById('chat').addEventListener('dragover', (e) => {
    e.preventDefault();
    e.stopPropagation();
});
document.getElementById('chat').addEventListener('drop', (e) => {
    e.preventDefault();
    e.stopPropagation();
    if (e.dataTransfer.files.length > 0) {
        selectFile(e.dataTransfer.files[0]);
    }
});

// ===================== 退出房间 =====================

document.getElementById('leave-btn').onclick = () => {
    cancelUpload = true;
    isLeaving = true;
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    if (ws) {
        ws.onclose = null;
        ws.close();
    }
    memberList = [];
    document.getElementById('chat').style.display = 'none';
    document.getElementById('login').style.display = 'block';
};

// ===================== 加入房间 =====================

document.getElementById('join-btn').onclick = () => {
    const room = document.getElementById('room-input').value.trim() || 'lobby';
    const nick = document.getElementById('nick-input').value.trim() || '用户' + Math.floor(Math.random() * 1000);
    document.getElementById('nick-input').value = nick;
    connect(room, nick);
};

document.getElementById('nick-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') document.getElementById('join-btn').click();
});
document.getElementById('room-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') document.getElementById('nick-input').focus();
});
