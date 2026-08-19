// ChatServe 前端入口 — 事件绑定与初始化

// ===================== 移动端 visualViewport 适配 =====================

(function() {
    if (!window.visualViewport || window.innerWidth > 768) return;
    const app = document.getElementById('app');
    if (!app) return;
    function syncHeight() {
        app.style.height = window.visualViewport.height + 'px';
    }
    window.visualViewport.addEventListener('resize', syncHeight);
    syncHeight();
})();

// ===================== 发送消息 =====================

function sendMessage() {
    const text = msgInput.value.trim();
    if (!text || !ws || ws.readyState !== WebSocket.OPEN) return;
    addChatMessage(myNick, text, 'self');
    // 聊天统一走 MSG 命令，内容可含 |
    ws.send('MSG|' + text);
    msgInput.value = '';
    msgInput.focus();
}

// ===================== 按钮事件绑定 =====================

// 摄像头/麦克风
document.getElementById('cam-btn').onclick = async () => {
    const wasOff = !camOn;
    await toggleMedia('video');
    // 开摄像头时顺便把麦也打开，如果麦还关着
    if (wasOff && !micOn) {
        await toggleMedia('audio');
    }
};
document.getElementById('mic-btn').onclick = () => toggleMedia('audio');

// 发送
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

// 文件发送 — 仅注册元数据
fileSendBtn.onclick = () => {
    if (!pendingFile || !ws || ws.readyState !== WebSocket.OPEN) {
        addSystemMessage('❌ 未连接，无法发送文件');
        return;
    }
    document.querySelector('.file-preview-actions').classList.add('hidden');
    ws.send('UPLOAD|' + pendingFile.name + '|' + pendingFile.size);
};

// 取消预览：只是丢弃当前选中文件，预览中的文件尚未注册，不涉及服务器
fileCancelBtn.onclick = () => {
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
    hangupAll();
    uploadSessions = {};
    uploadCards = {};
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
