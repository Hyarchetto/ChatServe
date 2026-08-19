// WebSocket 连接/重连

function connect(room, nick) {
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    // 重置传输状态，isLeaving 路径 onclose 跳过清理时保证下次连接干净
    uploadSessions = {};
    uploadCards = {};
    downloadSessions = {};
    fileSenders = {};
    pendingChunks = {};
    myNick = nick;
    myRoom = room;

    const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = protocol + '//' + location.host + '/ws';
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
        isLeaving = false;
        setConnStatus('connected', '● 已连接');
        document.getElementById('login').style.display = 'none';
        document.getElementById('chat').style.display = 'flex';
        document.getElementById('msg-input').focus();
        ws.send('JOIN|' + room + '|' + nick);
    };

    ws.onmessage = (evt) => {
        if (typeof evt.data === 'string') {
            handleTextMessage(evt.data);
        } else {
            handleBinaryMessage(evt.data);
        }
    };

    ws.onclose = () => {
        cancelUpload = true;
        if (isLeaving) return;
        isReconnect = true;
        uploadSessions = {};
        downloadSessions = {};
        fileSenders = {};
        pendingChunks = {};
        hangupAll();
        setConnStatus('reconnecting', '◌ 重连中...');
        addSystemMessage('连接断开，正在重连...');
        memberList = [];
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(() => {
            if (myRoom && myNick) connect(myRoom, myNick);
        }, 3000);
    };
}
