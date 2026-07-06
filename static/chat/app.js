// ChatServe WebSocket 聊天室前端
// 协议：JOIN|房间|昵称  →  OK|房间|昵称 / MSG|昵称|内容 / SYS|消息 / MEMBERS|列表

let ws = null;
let reconnectTimer = null;
let myNick = '';
let myRoom = '';
let memberList = [];
let isLeaving = false;
let isReconnect = false;

// ===================== 时间格式化 =====================

function currentTime() {
    const d = new Date();
    return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
}

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

        if (data.startsWith('OK|')) {
            const parts = data.split('|');
            document.getElementById('room-name').textContent = `房间: ${parts[1]}`;
            // 新房间清空旧消息和成员列表
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
            addChatMessage(nick, text);
        }
        else if (data.startsWith('SYS|')) {
            const text = data.substring(4);
            addSystemMessage(text);
            if (text.endsWith(' 加入房间')) {
                const nick = text.slice(0, -5);
                if (nick) {
                    memberList.push(nick);
                    updateMemberList(memberList);
                }
            } else if (text.endsWith(' 离开房间')) {
                const nick = text.slice(0, -5);
                const idx = memberList.lastIndexOf(nick);
                if (idx !== -1) memberList.splice(idx, 1);
                updateMemberList(memberList);
            }
        }
        else if (data.startsWith('MEMBERS|')) {
            const raw = data.substring(8);
            memberList = raw ? raw.split(',') : [];
            updateMemberList(memberList);
        }
    };

    ws.onclose = () => {
        if (isLeaving) return;
        isReconnect = true;
        setConnStatus('reconnecting', '◌ 重连中...');
        addSystemMessage('连接断开，正在重连...');
        memberList = [];
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(() => {
            connect(myRoom, myNick);
        }, 3000);
    };

    ws.onerror = () => {
        // onclose 会随之触发，不需要重复处理
    };
}

// ===================== 消息渲染 =====================

const messagesEl = document.getElementById('messages');
const msgInput = document.getElementById('msg-input');
const sendBtn = document.getElementById('send-btn');

function addChatMessage(nick, text) {
    const time = currentTime();
    const div = document.createElement('div');
    div.className = nick === myNick ? 'msg self' : 'msg other';

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

    addChatMessage(myNick, text);
    ws.send(text);
    msgInput.value = '';
    msgInput.focus();
}

sendBtn.onclick = sendMessage;

msgInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') sendMessage();
});

// ===================== 退出房间 =====================

document.getElementById('leave-btn').onclick = () => {
    isLeaving = true;
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    if (ws) {
        ws.onclose = null;  // 阻止自动重连
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
