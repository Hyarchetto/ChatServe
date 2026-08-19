// WebRTC P2P 连接、媒体、信令处理

// ---------- 内部辅助 ----------

function getPeer(peerId) {
    if (!peers[peerId]) {
        peers[peerId] = { pc: null, stream: null };
    }
    return peers[peerId];
}

function closePeer(peerId) {
    const peer = peers[peerId];
    if (!peer) return;
    delete peers[peerId];
    if (peer.pc) {
        // 清掉回调再 close，防止 stale onconnectionstatechange 微任务误杀新 PC
        peer.pc.onconnectionstatechange = null;
        peer.pc.close();
    }
}

function removePeer(peerId) {
    closePeer(peerId);
    memberList = memberList.filter(m => m.id !== peerId);
}

function hasLiveTrack(kind) {
    return myStream?.getTracks().some(t => t.kind === kind && t.readyState === 'live');
}

function setTrackEnabled(kind, enabled) {
    if (!myStream) return;
    for (const t of myStream.getTracks().filter(t => t.kind === kind)) {
        t.enabled = enabled;
    }
    const state = enabled ? 'on' : 'off';
    for (const peerId in peers) {
        if (ws?.readyState === WebSocket.OPEN) {
            ws.send('MEDIA|' + peerId + '|' + kind + '|' + state);
        }
    }
    if (kind === 'video') camOn = enabled;
    if (kind === 'audio') micOn = enabled;
    updateVideoGrid();
}

// ---------- 创建与销毁 P2P 连接 ----------

// 将 myStream 中所有 live track 添加到指定 PC，自动跳过已在 PC 上的
function addMyTracksToPC(pc) {
    if (!myStream) return;
    for (const track of myStream.getTracks()) {
        if (track.readyState === 'live' && !pc.getSenders().some(s => s.track === track)) {
            pc.addTrack(track, myStream);
        }
    }
}

function createPC(peerId) {
    closePeer(peerId);
    const peer = getPeer(peerId);
    const pc = new RTCPeerConnection(rtcConfig);
    peer.pc = pc;
    peer.stream = new MediaStream();

    pc.onnegotiationneeded = async () => {
        if (!ws || ws.readyState !== WebSocket.OPEN) return;
        try {
            await pc.setLocalDescription(await pc.createOffer());
            ws.send('OFFER|' + peerId + '|' + btoa(pc.localDescription.sdp));
        } catch (e) {
            // 频繁触发时可能冲突，忽略即可
        }
    };

    pc.onicecandidate = (e) => {
        if (e.candidate && ws && ws.readyState === WebSocket.OPEN) {
            ws.send('ICE|' + peerId + '|' + btoa(JSON.stringify(e.candidate)));
        }
    };

    pc.ontrack = (e) => {
        if (!peer.stream) peer.stream = new MediaStream();
        peer.stream.addTrack(e.track);
        e.track.onended = () => updateVideoGrid();
        e.track.onmute = () => updateVideoGrid();
        e.track.onunmute = () => updateVideoGrid();
        updateVideoGrid();
    };

    pc.onconnectionstatechange = () => {
        if (pc.connectionState === 'disconnected' ||
            pc.connectionState === 'failed' ||
            pc.connectionState === 'closed') {
            if (!peers[peerId]) return;
            closePeer(peerId);
            updateVideoGrid();
        }
    };

    return pc;
}

// 与房间成员建立 P2P 连接，已连接成员跳过，连接生命周期与成员关系绑定
// 只有 id 较小的成员发起 OFFER，较大的一方等对方发起，避免双方同时协商的 glare
async function sendOffersToAll() {
    for (const m of memberList) {
        if (m.id === myId || peers[m.id]) continue;
        if (Number(myId) >= Number(m.id)) continue;
        const pc = createPC(m.id);
        addMyTracksToPC(pc);  // 先加 track 再 createOffer
        try {
            const offer = await pc.createOffer();
            await pc.setLocalDescription(offer);
            ws.send('OFFER|' + m.id + '|' + btoa(offer.sdp));
        }
        catch (e) {
            addSystemMessage('❌ 与 ' + (m.nick || m.id) + ' 建立连接失败');
        }
    }
}

// ---------- 本地媒体 ----------

async function startMyMedia(opts) {
    const constraints = {};
    if (opts.video) constraints.video = true;
    if (opts.audio) constraints.audio = true;
    if (Object.keys(constraints).length === 0) return true;

    // 非安全上下文下 mediaDevices 不存在，需 HTTPS 或 localhost 访问
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
        addSystemMessage('❌ 浏览器未开放媒体权限：请通过 HTTPS 或 localhost 访问以启用摄像头/麦克风');
        return false;
    }

    try {
        const newStream = await navigator.mediaDevices.getUserMedia(constraints);
        if (!myStream) myStream = new MediaStream();
        for (const track of newStream.getTracks()) {
            myStream.addTrack(track);
        }
        if (opts.video) camOn = true;
        if (opts.audio) micOn = true;
        updateVideoGrid();
        return true;
    } catch (e) {
        addSystemMessage('❌ 无法访问媒体设备: ' + e.message);
        return false;
    }
}

// 开关摄像头或麦克风，每种只负责自己的 kind
async function toggleMedia(kind) {
    const isOn = kind === 'video' ? camOn : micOn;
    if (isOn) {
        // 只关本端发送，P2P 连接保持以继续接收对方音视频
        setTrackEnabled(kind, false);
        updateBtnState();
        return;
    }
    if (!hasLiveTrack(kind)) {
        const constraints = {};
        constraints[kind] = true;
        const ok = await startMyMedia(constraints);
        if (!ok) return;
        // 连接已由成员关系建立，新 track 补进已有 PC 即可
        const track = myStream.getTracks().find(t => t.kind === kind);
        for (const peerId in peers) {
            const pc = peers[peerId].pc;
            if (track && !pc.getSenders().some(s => s.track === track)) {
                pc.addTrack(track, myStream);
            }
        }
    }
    setTrackEnabled(kind, true);
    updateBtnState();
}

function hangupAll() {
    closeAllPCs();
    if (myStream) {
        for (const t of myStream.getTracks()) t.stop();
        myStream = null;
    }
    camOn = false;
    micOn = false;
    updateVideoGrid();
    updateBtnState();
}

function closeAllPCs() {
    for (const pid in peers) closePeer(pid);
}

// ---------- 信令处理 ----------

async function acceptOffer(pc, fromId, sdpBase64) {
    const sdp = atob(sdpBase64);
    await pc.setRemoteDescription({ type: 'offer', sdp });
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);
    ws.send('ANSWER|' + fromId + '|' + btoa(answer.sdp));
}

async function handleOffer(fromId, fromNick, sdpBase64) {
    const peer = getPeer(fromId);
    if (peer.pc) {
        try {
            await acceptOffer(peer.pc, fromId, sdpBase64);
            return;
        }
        catch (e) {
            closePeer(fromId);
        }
    }
    // 先建 PC 不含 track，接受远端 offer，再加本地 track
    // 顺序不能反：addTrack 触发的 onnegotiationneeded 会和 acceptOffer 冲突
    const pc = createPC(fromId);
    try {
        await acceptOffer(pc, fromId, sdpBase64);
        addMyTracksToPC(pc);
    }
    catch (e) {
        addSystemMessage('❗ 与 ' + fromNick + ' 的视频连接异常');
    }
}

async function handleAnswer(fromId, sdpBase64) {
    const peer = peers[fromId];
    if (!peer || !peer.pc) return;
    try {
        const sdp = atob(sdpBase64);
        await peer.pc.setRemoteDescription({ type: 'answer', sdp });
    }
    catch (e) {
        // 偶尔状态冲突可忽略
    }
}

function handleIce(fromId, candidateBase64) {
    const peer = peers[fromId];
    if (!peer || !peer.pc) return;
    try {
        const cand = JSON.parse(atob(candidateBase64));
        peer.pc.addIceCandidate(new RTCIceCandidate(cand));
    }
    catch (e) {
        // 无效 candidate 可忽略
    }
}
