// 文件上传/下载协议逻辑

// ---------- 上传 ----------

function selectFile(file) {
    if (!file) return;
    pendingFile = file;
    filePreviewName.textContent = file.name;
    filePreviewSize.textContent = formatFileSize(file.size);
    document.querySelector('.file-preview-actions').classList.remove('hidden');
    filePreview.style.display = 'flex';
    cancelUpload = false;
}

function resetUpload() {
    pendingFile = null;
    cancelUpload = false;
    filePreview.style.display = 'none';
    fileInput.value = '';
}

// 取消上传：通知服务器，卡片变为已取消状态
function cancelUploadFile(fileId) {
    cancelUpload = true;
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send('UPCANCEL|' + fileId);
    }
    const card = uploadCards[fileId];
    if (card) {
        const btn = card.querySelector('.file-card-cancel');
        if (btn) {
            btn.textContent = '已取消上传';
            btn.disabled = true;
        }
    }
    delete uploadSessions[fileId];
}

function handleDWREQ(sessionId, fileId, offset, size) {
    const session = uploadSessions[fileId];
    if (!session || !ws || ws.readyState !== WebSocket.OPEN) return;
    if (cancelUpload) return;

    const chunk = session.file.slice(offset, offset + size);
    if (chunk.size === 0) return;

    const reader = new FileReader();
    reader.onload = (e) => {
        if (cancelUpload || !ws || ws.readyState !== WebSocket.OPEN) return;
        ws.send(buildBinaryFrame(parseInt(sessionId, 10), offset, e.target.result));
    };
    reader.onerror = () => {
        addSystemMessage('❌ 文件 "' + session.name + '" 读取失败');
    };
    reader.readAsArrayBuffer(chunk);
}

// ---------- 下载 ----------

function finishDownload(fileId) {
    const session = downloadSessions[fileId];
    if (!session) return;

    const offsets = Object.keys(session.chunks).map(Number).sort((a, b) => a - b);
    const blob = new Blob(offsets.map(o => session.chunks[o]));
    delete downloadSessions[fileId];

    if (blob.size < session.total) {
        const ratio = (blob.size / session.total * 100).toFixed(1);
        const missing = formatFileSize(session.total - blob.size);
        addSystemMessage('⚠️ 文件 "' + session.filename + '" 不完整：仅收到 ' + ratio + '%（缺少 ' + missing + '）');
    }

    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = session.filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);

    setFileCardDone(fileId);
    addSystemMessage('📥 文件 "' + session.filename + '" 下载完成');
}

function startDownload(fileId, startOffset) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        addSystemMessage('❌ 未连接，无法下载');
        return;
    }
    let msg = 'DOWNLOAD|' + fileId;
    if (startOffset > 0) msg += '|' + startOffset;
    ws.send(msg);
    renderFileActions(fileId, 'downloading');
}

// 暂停下载：通知服务端取消当前会话 已收到的部分保留作断点
function pauseDownload(fileId) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send('DWNPAUSE|' + fileId);
    }
    renderFileActions(fileId, 'paused');
}

// 继续下载：从已收到的连续断点重新发起下载
function resumeDownload(fileId) {
    const session = downloadSessions[fileId];
    if (!session) return;

    // 连续前缀长度即断点
    let checkpoint = 0;
    while (session.chunks[checkpoint]) {
        checkpoint += session.chunks[checkpoint].byteLength;
    }
    if (checkpoint >= session.total) {
        finishDownload(fileId);
        return;
    }
    // 清除断点之后残留的乱序块 由新会话重新请求
    for (const off of Object.keys(session.chunks)) {
        if (Number(off) >= checkpoint) delete session.chunks[off];
    }
    session.received = checkpoint;
    startDownload(fileId, checkpoint);
}

// 取消下载：通知服务端清理会话，卡片回到待下载
function cancelDownloadFile(fileId) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send('DWNCANCEL|' + fileId);
    }
    delete downloadSessions[fileId];
    for (const key in pendingChunks) {
        if (pendingChunks[key].fileId === fileId) {
            delete pendingChunks[key];
        }
    }
    renderFileActions(fileId, 'idle');
}

function expireFileCard(fileId) {
    delete downloadSessions[fileId];
    delete fileSenders[fileId];
    setFileCardInvalid(fileId);
}
