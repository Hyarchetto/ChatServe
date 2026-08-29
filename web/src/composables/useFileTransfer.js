// 文件传输 — 从旧 file-transfer.js + protocol.js 文件部分迁移
// 会话状态响应式 驱动 FileCard 渲染
import { ref, reactive } from 'vue'

function currentTime() {
  const d = new Date()
  return String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0')
}

// BINARY 帧结构 [session_id:8][offset:8][data_size:4] + data
function buildBinaryFrame(sessionId, offset, arrayBuffer) {
  const header = new ArrayBuffer(20)
  const view = new DataView(header)
  view.setUint32(0, sessionId >>> 0, true)
  view.setUint32(4, Math.floor(sessionId / 0x100000000), true)
  view.setUint32(8, offset >>> 0, true)
  view.setUint32(12, Math.floor(offset / 0x100000000), true)
  view.setUint32(16, arrayBuffer.byteLength, true)
  const combined = new Uint8Array(20 + arrayBuffer.byteLength)
  combined.set(new Uint8Array(header), 0)
  combined.set(new Uint8Array(arrayBuffer), 20)
  return combined.buffer
}

export function useFileTransfer({ sendCommand, sendBinary, addSystemMessage, addFileMessage }) {
  const uploadSessions = reactive({})
  const downloadSessions = reactive({})
  const pendingChunks = reactive({})
  const fileSenders = reactive({})
  const pendingFile = ref(null)
  const cancelUpload = ref(false)

  // ---------- 上传 ----------

  function selectFile(file) {
    if (!file) return
    pendingFile.value = file
    cancelUpload.value = false
  }

  function resetUpload() {
    pendingFile.value = null
    cancelUpload.value = false
  }

  function sendUpload() {
    const file = pendingFile.value
    if (!file) return
    sendCommand('UPLOAD|' + file.name + '|' + file.size)
  }

  function cancelUploadFile(fileId) {
    cancelUpload.value = true
    sendCommand('UPCANCEL|' + fileId)
    delete uploadSessions[fileId]
  }

  // 收到 DWREQ 读取文件分块并发送 BINARY 帧
  function handleDWREQ(sessionId, fileId, offset, size) {
    const session = uploadSessions[fileId]
    if (!session || cancelUpload.value) return
    const chunk = session.file.slice(offset, offset + size)
    if (chunk.size === 0) return
    const reader = new FileReader()
    reader.onload = (e) => {
      if (cancelUpload.value) return
      sendBinary(buildBinaryFrame(parseInt(sessionId, 10), offset, e.target.result))
    }
    reader.onerror = () => addSystemMessage('❌ 文件 "' + session.name + '" 读取失败')
    reader.readAsArrayBuffer(chunk)
  }

  function handleUPOK(fileId) {
    const file = pendingFile.value
    if (!file) return
    uploadSessions[fileId] = { file, name: file.name, size: file.size }
    addFileMessage({ type: 'file-upload', file: { fileId, filename: file.name, filesize: file.size }, time: currentTime() })
    pendingFile.value = null
  }

  // ---------- 下载 ----------

  function startDownload(fileId, startOffset) {
    let msg = 'DOWNLOAD|' + fileId
    if (startOffset > 0) msg += '|' + startOffset
    sendCommand(msg)
    if (downloadSessions[fileId]) downloadSessions[fileId].paused = false
  }

  function pauseDownload(fileId) {
    sendCommand('DWNPAUSE|' + fileId)
    if (downloadSessions[fileId]) downloadSessions[fileId].paused = true
  }

  function resumeDownload(fileId) {
    const session = downloadSessions[fileId]
    if (!session) return
    let checkpoint = 0
    while (session.chunks[checkpoint]) {
      checkpoint += session.chunks[checkpoint].byteLength
    }
    if (checkpoint >= session.total) {
      finishDownload(fileId)
      return
    }
    for (const off of Object.keys(session.chunks)) {
      if (Number(off) >= checkpoint) delete session.chunks[off]
    }
    session.received = checkpoint
    session.paused = false
    startDownload(fileId, checkpoint)
  }

  function cancelDownloadFile(fileId) {
    sendCommand('DWNCANCEL|' + fileId)
    delete downloadSessions[fileId]
    for (const key in pendingChunks) {
      if (pendingChunks[key].fileId === fileId) delete pendingChunks[key]
    }
  }

  function finishDownload(fileId) {
    const session = downloadSessions[fileId]
    if (!session) return
    const offsets = Object.keys(session.chunks).map(Number).sort((a, b) => a - b)
    const blob = new Blob(offsets.map(o => session.chunks[o]))
    delete downloadSessions[fileId]

    if (blob.size < session.total) {
      const ratio = (blob.size / session.total * 100).toFixed(1)
      addSystemMessage('⚠️ 文件 "' + session.filename + '" 不完整：仅收到 ' + ratio + '%')
    }

    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = session.filename
    document.body.appendChild(a)
    a.click()
    document.body.removeChild(a)
    URL.revokeObjectURL(url)

    addSystemMessage('📥 文件 "' + session.filename + '" 下载完成')
  }

  function expireFileCard(fileId) {
    delete downloadSessions[fileId]
    delete fileSenders[fileId]
  }

  // 成员离开 该成员上传的文件卡全部失效
  function handleLeave(leaverId) {
    for (const fid of Object.keys(fileSenders)) {
      if (fileSenders[fid] === leaverId) expireFileCard(fid)
    }
  }

  // ---------- 协议处理 ----------

  function handleFILE(sender, fileId, filename, filesize, uploaderId) {
    fileSenders[fileId] = uploaderId
    addFileMessage({ type: 'file-notify', sender, file: { fileId, filename, filesize, uploaderId }, time: currentTime() })
  }

  function handleDWSTART(fileId, filename, filesize) {
    if (!downloadSessions[fileId]) {
      downloadSessions[fileId] = { chunks: {}, received: 0, total: filesize, filename, paused: false }
    }
  }

  function handleDWDATA(sessionId, fileId, offset) {
    pendingChunks[sessionId + ':' + offset] = { fileId, sessionId, offset }
  }

  function handleDWNDONE(fileId) {
    finishDownload(fileId)
  }

  function handleDWERR(fileId, reason) {
    const session = downloadSessions[fileId]
    if (session) {
      addSystemMessage('⚠️ 文件 "' + session.filename + '" 传输失败：' + reason)
      delete downloadSessions[fileId]
    }
    expireFileCard(fileId)
  }

  // BINARY 帧处理
  function handleBinary(data) {
    if (data.byteLength < 20) return
    const view = new DataView(data)
    const sid = view.getUint32(0, true) + view.getUint32(4, true) * 0x100000000
    const offset = view.getUint32(8, true) + view.getUint32(12, true) * 0x100000000
    const key = sid + ':' + offset
    const meta = pendingChunks[key]
    if (!meta) return
    delete pendingChunks[key]

    const session = downloadSessions[meta.fileId]
    if (!session) return

    const chunkData = data.slice(20)
    session.chunks[offset] = chunkData
    session.received += chunkData.byteLength

    sendCommand('DWACK|' + meta.sessionId + '|' + offset)
  }

  return {
    uploadSessions, downloadSessions, pendingChunks, fileSenders,
    pendingFile, cancelUpload,
    selectFile, resetUpload, sendUpload, cancelUploadFile,
    startDownload, pauseDownload, resumeDownload, cancelDownloadFile, expireFileCard, handleLeave,
    handleDWREQ, handleUPOK, handleFILE, handleDWSTART, handleDWDATA, handleDWNDONE, handleDWERR, handleBinary,
  }
}
