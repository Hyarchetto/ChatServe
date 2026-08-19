// 协议消息路由 + BINARY 帧构造

// BINARY 帧结构：
// [session_id:8bytes LE][offset:8bytes LE][data_size:4bytes LE] 后接原始数据
function buildBinaryFrame(sessionId, offset, arrayBuffer) {
    const header = new ArrayBuffer(20);  // 8 + 8 + 4
    const view = new DataView(header);
    view.setUint32(0, sessionId >>> 0, true);
    view.setUint32(4, Math.floor(sessionId / 0x100000000), true);
    view.setUint32(8, offset >>> 0, true);
    view.setUint32(12, Math.floor(offset / 0x100000000), true);
    view.setUint32(16, arrayBuffer.byteLength, true);
    const combined = new Uint8Array(20 + arrayBuffer.byteLength);
    combined.set(new Uint8Array(header), 0);
    combined.set(new Uint8Array(arrayBuffer), 20);
    return combined.buffer;
}

// ===================== 文本消息路由 =====================

function handleTextMessage(data) {
    if (data.startsWith('OK|')) {
        const parts = data.split('|');
        initRoom(parts[1], parts[2], parts[3] || '');
        if (isReconnect) {
            isReconnect = false;
            addSystemMessage('重新连接成功');
        } else {
            addSystemMessage('您已加入房间 ' + parts[1]);
        }
    }

    else if (data.startsWith('MSG|')) {
        const rest = data.substring(4);
        const idx1 = rest.indexOf('|');
        if (idx1 === -1) return;
        const senderId = rest.substring(0, idx1);
        const rest2 = rest.substring(idx1 + 1);
        const idx2 = rest2.indexOf('|');
        if (idx2 === -1) return;
        const nick = rest2.substring(0, idx2);
        const text = rest2.substring(idx2 + 1);
        addChatMessage(nick, text, senderId === myId ? 'self' : 'other');
    }

    else if (data.startsWith('SYS|')) {
        const text = data.substring(4);
        if (text.startsWith('ERR|')) {
            addSystemMessage('❌ ' + text.substring(4));
        } else {
            addSystemMessage(text);
        }
    }

    // ---- 用户离开 ----
    else if (data.startsWith('LEAVE|')) {
        const parts = data.split('|');
        if (parts.length >= 2) {
            const leaverId = parts[1];
            for (const [fid, id] of Object.entries(fileSenders)) {
                if (id === leaverId) expireFileCard(fid);
            }
            removePeer(leaverId);
            updateVideoGrid();
        }
    }

    // ---- 成员列表 ----
    else if (data.startsWith('MEMBERS|')) {
        const raw = data.substring(8);
        memberList = raw ? raw.split(',').filter(s => s).map(p => {
            const sep = p.indexOf(':');
            return sep > 0 ? { id: p.substring(0, sep), nick: p.substring(sep + 1) } : { id: p, nick: p };
        }) : [];
        // 连接与成员关系绑定，与自己的媒体开关无关
        sendOffersToAll();
        updateVideoGrid();
        updateBtnState();
    }

    // ---- 文件上传协议 ----
    else if (data.startsWith('UPOK|')) {
        const fileId = data.substring(5);
        if (pendingFile) {
            uploadSessions[fileId] = { file: pendingFile, name: pendingFile.name, size: pendingFile.size };
            addUploadCard(fileId, pendingFile.name, pendingFile.size);
            pendingFile = null;
            filePreview.style.display = 'none';
        }
    }

    else if (data.startsWith('DONE|')) {
        // 上传卡片在取消时已变为已取消状态，这里只清理预览态
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

    // ---- 滑动窗口传输 ----
    else if (data.startsWith('DWREQ|')) {
        const parts = data.split('|');
        if (parts.length >= 5) {
            handleDWREQ(parts[1], parts[2], parseInt(parts[3], 10), parseInt(parts[4], 10));
        }
    }

    else if (data.startsWith('DWSTART|')) {
        const parts = data.split('|');
        if (parts.length >= 4) {
            const fileId = parts[1];
            // 断点续传时保留已收到的部分数据 首次下载才新建会话
            if (!downloadSessions[fileId]) {
                downloadSessions[fileId] = {
                    chunks: {},
                    received: 0,
                    total: parseInt(parts[3], 10),
                    filename: parts[2]
                };
            }
        }
    }

    else if (data.startsWith('DWDATA|')) {
        const parts = data.split('|');
        if (parts.length >= 5) {
            const sessionId = parts[1];
            const fileId = parts[2];
            const offset = parseInt(parts[3], 10);
            pendingChunks[sessionId + ':' + offset] = { fileId, sessionId, offset };
        }
    }

    else if (data.startsWith('DWNDONE|')) {
        finishDownload(data.substring(8));
    }

    else if (data.startsWith('DWERR|')) {
        const parts = data.split('|');
        if (parts.length >= 3) {
            const fileId = parts[1];
            const reason = parts.slice(2).join('|');
            const session = downloadSessions[fileId];
            if (session) {
                addSystemMessage('⚠️ 文件 "' + session.filename + '" 传输失败：' + reason);
                delete downloadSessions[fileId];
            }
            expireFileCard(fileId);
        }
    }

    // ---- WebRTC 信令 ----
    else if (data.startsWith('OFFER|')) {
        const idx1 = data.indexOf('|', 6);
        const idx2 = data.indexOf('|', idx1 + 1);
        if (idx1 > 0 && idx2 > 0) {
            handleOffer(data.substring(6, idx1), data.substring(idx1 + 1, idx2), data.substring(idx2 + 1));
        }
    }

    else if (data.startsWith('ANSWER|')) {
        const idx1 = data.indexOf('|', 7);
        if (idx1 > 0) {
            handleAnswer(data.substring(7, idx1), data.substring(idx1 + 1));
        }
    }

    else if (data.startsWith('ICE|')) {
        const idx1 = data.indexOf('|', 4);
        if (idx1 > 0) {
            handleIce(data.substring(4, idx1), data.substring(idx1 + 1));
        }
    }

    else if (data.startsWith('MEDIA|')) {
        const parts = data.split('|');
        if (parts.length >= 4) {
            const p = getPeer(parts[1]);
            if (!p.mediaState) p.mediaState = {};
            p.mediaState[parts[2]] = parts[3];
            updateVideoGrid();
        }
    }
}

// ===================== 二进制消息处理 =====================

function handleBinaryMessage(data) {
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
        ws.send('DWACK|' + meta.sessionId + '|' + offset);
    }

    updateDownloadProgress(meta.fileId, session.received, session.total);
}
