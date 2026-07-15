// ChatServe WebSocket 聊天室前端 — 滑动窗口文件传输
// 上传方仅注册元数据，下载方点击后触发滑动窗口传输
// 协议：
//   注册   UPLOAD/UPOK/FILE
//   下载   DOWNLOAD/DWSTART
//   传输   DWREQ → BINARY(嵌入offset头部) → DWDATA+BINARY → DWACK
//   完成   DWNDONE/DONE

let ws = null;
let reconnectTimer = null;
let myNick = '';
let myId = '';
let myRoom = '';
let memberList = [];
let isLeaving = false;
let isReconnect = false;

// 移动端 visualViewport 适配
(function() {
    if (!window.visualViewport || window.innerWidth > 768) {
        return;
    }
    const app = document.getElementById('app');
    if (!app) {
        return;
    }
    function syncHeight() {
        app.style.height = window.visualViewport.height + 'px';
    }
    window.visualViewport.addEventListener('resize', syncHeight);
    syncHeight();
})();

// ---------- 文件上传状态 ----------
let pendingFile = null;          // 用户选择的待发送文件
let uploadSessions = {};         // file_id -> { file: File, name: string, size: number }
let cancelUpload = false;

// ---------- 文件下载状态 ----------
let downloadSessions = {};       // file_id -> { chunks:{offset: ArrayBuffer}, received, total, filename }
let pendingChunks = {};          // sessionId:offset -> { fileId, sessionId, offset } 来自 DWDATA
let fileSenders = {};            // file_id -> sender_nickname，用于上传方离开时失效链接

// ===================== 时间格式化 =====================

function currentTime() {
    const d = new Date();
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
}

function formatFileSize(bytes) {
    if (bytes < 1024) {
        return bytes + ' B';
    }
    else if (bytes < 1024 * 1024) {
        return (bytes / 1024).toFixed(1) + ' KB';
    }
    else{
        return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    }
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
    // 重置传输状态，isLeaving 路径 onclose 跳过清理时保证下次连接干净
    uploadSessions = {};
    downloadSessions = {};
    fileSenders = {};
    pendingChunks = {};
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
        } 
        else {
            handleBinaryMessage(data);
        }
    };

    ws.onclose = () => {
        cancelUpload = true;
        if (isLeaving) return;
        isReconnect = true;
        // 清理传输状态——重连后服务端已清理旧连接的传输会话
        uploadSessions = {};
        downloadSessions = {};
        fileSenders = {};
        pendingChunks = {};
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
        myNick = parts[2];
        myId = parts[3] || '';
        messagesEl.innerHTML = '';
        memberList = [];
        if (isReconnect) {
            isReconnect = false;
            addSystemMessage('重新连接成功');
        } 
        else {
            addSystemMessage(`您已加入房间 ${parts[1]}`);
        }
    }
    else if (data.startsWith('MSG|')) {
        const rest = data.substring(4);
        const idx1 = rest.indexOf('|');
        if (idx1 === -1) {
            return;
        }
        const senderId = rest.substring(0, idx1);
        const rest2 = rest.substring(idx1 + 1);
        const idx2 = rest2.indexOf('|');
        if (idx2 === -1) {
            return;
        }
        const nick = rest2.substring(0, idx2);
        const text = rest2.substring(idx2 + 1);
        addChatMessage(nick, text, senderId === myId ? 'self' : 'other');
    }
    else if (data.startsWith('SYS|')) {
        const text = data.substring(4);
        if (text.startsWith('ERR|')) {
            addSystemMessage('❌ ' + text.substring(4));
        } 
        else {
            addSystemMessage(text);
        }
    }
    // ---- 用户离开，按 fd 精确匹配失效文件 ----
    else if (data.startsWith('LEAVE|')) {
        const parts = data.split('|');
        if (parts.length >= 3) {
            const uploaderId = parts[1];
            for (const [fid, id] of Object.entries(fileSenders)) {
                if (id === uploaderId) {
                    expireFileCard(fid);
                }
            }
        }
    }
    else if (data.startsWith('MEMBERS|')) {
        const raw = data.substring(8);
        memberList = raw ? raw.split(',').filter(s => s).map(p => {
            const sep = p.indexOf(':');
            return sep > 0 ? { id: p.substring(0, sep), nick: p.substring(sep + 1) } : { id: '', nick: p };
        }) : [];
        updateMemberList(memberList);
    }
    // ---- 文件上传协议 ----
    else if (data.startsWith('UPOK|')) {
        const fileId = data.substring(5);
        if (pendingFile) {
            uploadSessions[fileId] = { file: pendingFile, name: pendingFile.name, size: pendingFile.size };
            addSystemMessage(`📄 文件 "${pendingFile.name}" 已注册`);
            pendingFile = null;
            filePreview.style.display = 'none';
            uploadProgressWrap.style.display = 'flex';
            uploadProgressFill.style.width = '100%';
            uploadProgressText.textContent = '已注册';
        }
    }
    else if (data.startsWith('DONE|')) {
        const part = data.substring(5);
        if (part === 'cancelled') {
            addSystemMessage('❌ 上传已取消');
        } 
        else if (part && uploadSessions[part]) {
            addSystemMessage(`📄 文件 "${uploadSessions[part].name}" 传输完成`);
            delete uploadSessions[part];
        }
        resetUpload();
    }
    // ---- 文件通知 ----
    else if (data.startsWith('FILE|')) {
        const parts = data.split('|');
        if (parts.length >= 6) {
            const fileId = parts[1];
            const filename = parts[2];
            const filesize = parseInt(parts[3], 10);
            const sender = parts[4];
            const uploaderId = parts[5];
            fileSenders[fileId] = uploaderId;
            addFileNotification(sender, fileId, filename, filesize);
        }
    }
    // ---- 滑动窗口传输：上传方接收 DWREQ ----
    // DWREQ|<session_id>|<file_id>|<offset>|<size>
    else if (data.startsWith('DWREQ|')) {
        const parts = data.split('|');
        if (parts.length >= 5) {
            const sessionId = parts[1];
            const fileId = parts[2];
            const offset = parseInt(parts[3], 10);
            const size = parseInt(parts[4], 10);
            handleDWREQ(sessionId, fileId, offset, size);
        }
    }
    // ---- 滑动窗口传输：下载方接收 DWSTART ----
    else if (data.startsWith('DWSTART|')) {
        const parts = data.split('|');
        if (parts.length >= 4) {
            const fileId = parts[1];
            const filename = parts[2];
            const filesize = parseInt(parts[3], 10);
            downloadSessions[fileId] = {
                chunks: {},     // offset -> ArrayBuffer
                received: 0,
                total: filesize,
                filename: filename
            };
        }
    }
    // ---- 滑动窗口传输：下载方接收 DWDATA ----
    else if (data.startsWith('DWDATA|')) {
        const parts = data.split('|');
        if (parts.length >= 5) {
            const sessionId = parts[1];
            const fileId = parts[2];
            const offset = parseInt(parts[3], 10);
            const size = parseInt(parts[4], 10);
            pendingChunks[sessionId + ':' + offset] = { fileId, sessionId, offset };
        }
    }
    // ---- 滑动窗口传输：下载方接收 DWNDONE ----
    else if (data.startsWith('DWNDONE|')) {
        const fileId = data.substring(8);
        finishDownload(fileId);
    }
    // ---- 滑动窗口传输：下载方接收 DWERR（上传方离线或传输异常）----
    else if (data.startsWith('DWERR|')) {
        const parts = data.split('|');
        if (parts.length >= 3) {
            const fileId = parts[1];
            const reason = parts.slice(2).join('|');
            const session = downloadSessions[fileId];
            if (session) {
                addSystemMessage(`⚠️ 文件 "${session.filename}" 传输失败：${reason}`);
                delete downloadSessions[fileId];
            }
            expireFileCard(fileId);
        }
    }
}

// ===================== 二进制消息处理 =====================

function handleBinaryMessage(data) {
    // BINARY 帧头部 20 字节，格式与上传方一致，含 session_id + offset
    // 按 key 查找 DWDATA 元数据，不依赖收发顺序
    if (data.byteLength < 20) return;
    const view = new DataView(data);
    const sid = view.getUint32(0, true) + view.getUint32(4, true) * 0x100000000;
    const offset = view.getUint32(8, true) + view.getUint32(12, true) * 0x100000000;
    const key = sid + ':' + offset;
    const meta = pendingChunks[key];
    if (!meta) return;
    delete pendingChunks[key];

    const session = downloadSessions[meta.fileId];
    if (!session) return;

    const chunkData = data.slice(20);
    session.chunks[offset] = chunkData;
    session.received += chunkData.byteLength;

    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(`DWACK|${meta.sessionId}|${offset}`);
    }

    const card = document.querySelector(`[data-file-id="${meta.fileId}"]`);
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
    if (!file) {
        return;
    }
    pendingFile = file;
    filePreviewName.textContent = file.name;
    filePreviewSize.textContent = formatFileSize(file.size);
    uploadProgressWrap.style.display = 'none';
    document.querySelector('.file-preview-actions').classList.remove('hidden');
    filePreview.style.display = 'flex';
    cancelUpload = false;
}

function resetUpload() {
    pendingFile = null;
    cancelUpload = false;
    filePreview.style.display = 'none';
    uploadProgressWrap.style.display = 'none';
    uploadProgressFill.style.width = '0%';
    uploadProgressText.textContent = '0%';
    fileInput.value = '';
}

// BINARY 帧结构: [session_id:8bytes LE][offset:8bytes LE][data_size:4bytes LE] 后接原始数据
// session_id 让服务器 O(1) 定位传输会话，无需猜文件
function buildBinaryFrame(sessionId, offset, arrayBuffer) {
    const header = new ArrayBuffer(20);  // 8 + 8 + 4
    const view = new DataView(header);
    // session_id 拆两个 32 位 LE
    view.setUint32(0, sessionId >>> 0, true);
    view.setUint32(4, Math.floor(sessionId / 0x100000000), true);
    // offset 拆两个 32 位 LE
    view.setUint32(8, offset >>> 0, true);
    view.setUint32(12, Math.floor(offset / 0x100000000), true);
    // data_size
    view.setUint32(16, arrayBuffer.byteLength, true);
    const combined = new Uint8Array(20 + arrayBuffer.byteLength);
    combined.set(new Uint8Array(header), 0);
    combined.set(new Uint8Array(arrayBuffer), 20);
    return combined.buffer;
}

// 收到 DWREQ 后读取并发送一个分块
// DWREQ|<session_id>|<file_id>|<offset>|<size>
function handleDWREQ(sessionId, fileId, offset, size) {
    const session = uploadSessions[fileId];
    if (!session || !ws || ws.readyState !== WebSocket.OPEN) {
        return;
    }
    if (cancelUpload) {
        return;
    }
    const chunk = session.file.slice(offset, offset + size);
    if (chunk.size === 0) return;

    const reader = new FileReader();
    reader.onload = (e) => {
        if (cancelUpload || !ws || ws.readyState !== WebSocket.OPEN) {
            return;
        }
        ws.send(buildBinaryFrame(parseInt(sessionId, 10), offset, e.target.result));
    };
    reader.onerror = () => {
        addSystemMessage(`❌ 文件 "${session.name}" 读取失败`);
    };
    reader.readAsArrayBuffer(chunk);
}

// ===================== 文件下载 =====================

function finishDownload(fileId) {
    const session = downloadSessions[fileId];
    if (!session) {
        return;
    }
    // 按偏移排序并组装
    const offsets = Object.keys(session.chunks).map(Number).sort((a, b) => a - b);
    const blob = new Blob(offsets.map(o => session.chunks[o]));
    delete downloadSessions[fileId];

    // 完整性检查：防止传输提前终止导致静默截断
    if (blob.size < session.total) {
        const ratio = (blob.size / session.total * 100).toFixed(1);
        const missing = formatFileSize(session.total - blob.size);
        addSystemMessage(`⚠️ 文件 "${session.filename}" 不完整：仅收到 ${ratio}%（缺少 ${missing}）`);
        // 虽然不完整但让用户决定是否保留
    }

    // 触发浏览器下载
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = session.filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);

    // 更新卡片按钮，支持重新下载
    const card = document.querySelector(`[data-file-id="${fileId}"]`);
    if (card) {
        const btn = card.querySelector('.file-card-dl');
        if (btn) {
            btn.textContent = '重新下载';
            btn.disabled = false;
            btn.className = 'file-card-dl';
            btn.onclick = () => startDownload(fileId, btn);
        }
        const progressEl = card.querySelector('.file-card-progress');
        if (progressEl) {
            progressEl.textContent = '';
        }
    }

    addSystemMessage(`📥 文件 "${session.filename}" 下载完成`);
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

function expireFileCard(fileId) {
    delete downloadSessions[fileId];
    delete fileSenders[fileId];
    const card = document.querySelector(`[data-file-id="${fileId}"]`);
    if (card) {
        const btn = card.querySelector('.file-card-dl');
        if (btn) {
            btn.textContent = '已失效';
            btn.className = 'file-card-dl disabled';
            btn.disabled = true;
            btn.onclick = null;
        }
        const progressEl = card.querySelector('.file-card-progress');
        if (progressEl) {
            progressEl.textContent = '已失效';
        }
    }
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
    actions.className = 'file-card-actions';

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

// ===================== 成员列表 =====================

function updateMemberList(members) {
    const container = document.getElementById('members');
    container.innerHTML = '';
    for (const m of members) {
        if (!m) {
            continue;
        }
        const item = document.createElement('div');
        item.className = 'member-item';
        item.textContent = m.nick || m;
        container.appendChild(item);
    }
    document.getElementById('member-count').textContent = `在线: ${members.length} 人`;
}

// ===================== 发送消息 =====================

function sendMessage() {
    const text = msgInput.value.trim();
    if (!text || !ws || ws.readyState !== WebSocket.OPEN) {
        return;
    }
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

// 文件发送 — 仅注册元数据，不上传文件内容
fileSendBtn.onclick = () => {
    if (!pendingFile || !ws || ws.readyState !== WebSocket.OPEN) {
        addSystemMessage('❌ 未连接，无法发送文件');
        return;
    }
    document.querySelector('.file-preview-actions').classList.add('hidden');
    ws.send(`UPLOAD|${pendingFile.name}|${pendingFile.size}|${myRoom}`);
};

// 取消文件，取消当前选择的文件或移除已注册的文件
fileCancelBtn.onclick = () => {
    cancelUpload = true;
    // 如还有已注册文件则通过 UPCANCEL 通知服务器
    const ids = Object.keys(uploadSessions);
    if (ids.length > 0 && ws && ws.readyState === WebSocket.OPEN) {
        ws.send('UPCANCEL|' + ids[ids.length - 1]);
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
    // 清传输状态，退出房间后所有文件注册和服务端会话均已失效
    uploadSessions = {};
    downloadSessions = {};
    fileSenders = {};
    pendingChunks = {};
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
    document.getElementById('login').style.display = '';
};

// ===================== 加入房间 =====================

document.getElementById('join-btn').onclick = () => {
    const room = document.getElementById('room-input').value.trim() || 'lobby';
    const nick = document.getElementById('nick-input').value.trim() || '用户' + Math.floor(Math.random() * 1000);
    document.getElementById('nick-input').value = nick;
    connect(room, nick);
};

document.getElementById('nick-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
        document.getElementById('join-btn').click();
    }
});
document.getElementById('room-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
        document.getElementById('nick-input').focus();
    }
});
