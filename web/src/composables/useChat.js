// WebSocket 连接与协议路由 — 聊天/成员/WebRTC/文件传输
import { ref, reactive, onUnmounted } from 'vue'
import { useWebRTC } from './useWebRTC'
import { useFileTransfer } from './useFileTransfer'

// 应用层心跳 起搏间隔与判死阈值 与服务端 Heartbeat 的三个常数同源
const kHeartbeatInterval = 30 * 1000
const kSilenceLimit = 90 * 1000
const kPingFrame = 'PING|'   // 服务端回 PONG| 客户端不必识别 任何帧都算活着

export function useChat() {
  const joined = ref(false)
  const connStatus = ref('disconnected') // connected / reconnecting / disconnected
  const roomName = ref('')
  const myNick = ref('')
  const myId = ref('')
  const memberList = ref([])
  const messages = ref([])

  let ws = null
  let reconnectTimer = null
  let heartbeatTimer = null
  let lastRecvAt = 0
  let isLeaving = false
  let isReconnect = false
  let myRoom = ''

  function addSystemMessage(text) {
    messages.value.push({ type: 'sys', text })
  }

  function addChatMessage(nick, text, kind) {
    const time = new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' })
    messages.value.push({ type: kind, name: nick, text, time })
  }

  function addFileMessage(msg) {
    messages.value.push(msg)
  }

  // 按 fd 从本地成员表解析昵称 找不到回退 fd
  function nickOf(id) {
    const m = memberList.value.find(x => x.id === id)
    return m ? m.nick : id
  }

  function sendCommand(text) {
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(text)
  }

  function sendBinary(buf) {
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(buf)
  }

  // reactive 包裹后模板取 rtc.camOn / ft.pendingFile 等自动解包 ref 传值而非 ref 对象
  const rtc = reactive(useWebRTC({ sendCommand, myId, myNick, memberList, addSystemMessage }))
  const ft = reactive(useFileTransfer({ sendCommand, sendBinary, addSystemMessage, addFileMessage }))

  function connect(room, nick) {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer)
      heartbeatTimer = null
    }
    myRoom = room
    myNick.value = nick
    myId.value = ''

    const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'
    ws = new WebSocket(protocol + '//' + location.host + '/ws')
    ws.binaryType = 'arraybuffer'

    ws.onopen = () => {
      isLeaving = false
      connStatus.value = 'connected'
      ws.send('JOIN|' + room + '|' + nick)
      // 起搏 重连后 onopen 会再进来一次 先清再起 不叠加
      // 时间戳必须重置 否则长断线后第一拍就拿上一轮留下的过期时间戳把新连接判死
      if (heartbeatTimer) clearInterval(heartbeatTimer)
      lastRecvAt = performance.now()
      heartbeatTimer = setInterval(heartbeatTick, kHeartbeatInterval)
    }

    ws.onmessage = (evt) => {
      // 收到任何一帧都算对端还活着 心跳回包只是其中最规律的一种
      // 故这里不区分文本与二进制 时钟刷新必须早于下面各分支的提前 return
      lastRecvAt = performance.now()
      if (typeof evt.data === 'string') handleTextMessage(evt.data)
      else handleBinaryMessage(evt.data)
    }

    ws.onclose = () => handleDisconnect()
  }

  // 断线收尾与重连排定 正常 onclose 与假死判死共用这一条路径 只此一处
  // notice 只换提示文案 收尾动作一次都不重复
  function handleDisconnect(notice = '连接断开，正在重连...') {
    if (isLeaving) return
    isReconnect = true
    connStatus.value = 'reconnecting'
    addSystemMessage(notice)
    memberList.value = []
    rtc.hangupAll()
    ft.resetUpload()
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer)
      heartbeatTimer = null
    }
    if (reconnectTimer) clearTimeout(reconnectTimer)
    reconnectTimer = setTimeout(() => {
      if (myRoom && myNick.value) connect(myRoom, myNick.value)
    }, 3000)
  }

  // 心跳一拍 先判死再起搏
  // 判死在前 免得在一条马上要丢掉的 socket 上再排一帧
  function heartbeatTick() {
    // 只在连接态起搏 重连间隙与 CONNECTING 期都由此兜掉 不会二次判死
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    // 页面不可见时定时器可能被浏览器冻结 解冻后这个时间戳必然过期 不拿它判死
    if (!document.hidden && performance.now() - lastRecvAt >= kSilenceLimit) {
      // 假死 socket 不会触发 onclose 收尾得自己叫
      const dead = ws
      ws = null             // 先摘引用 本拍与后续各拍都以它为界
      dead.onclose = null   // 摘干净 免得 close 之后又回来走一遍收尾
      dead.onmessage = null // 同理 免得半路回来往死连接上灌一帧
      dead.close()
      handleDisconnect('连接无响应，正在重连...')
      return
    }
    sendCommand(kPingFrame)
  }

  function handleTextMessage(data) {
    if (data.startsWith('OK|')) {
      const parts = data.split('|')
      joined.value = true
      roomName.value = parts[1]
      myId.value = parts[2] || ''       // 服务端只发 fd 昵称沿用 JOIN 时填的
      messages.value = []
      if (isReconnect) {
        isReconnect = false
        addSystemMessage('重新连接成功')
      } else {
        addSystemMessage('您已加入房间 ' + parts[1])
      }
    } else if (data.startsWith('MSG|')) {
      const rest = data.substring(4)
      const idx1 = rest.indexOf('|')
      if (idx1 === -1) return
      const senderId = rest.substring(0, idx1)
      const text = rest.substring(idx1 + 1)
      addChatMessage(senderId === myId.value ? myNick.value : nickOf(senderId), text, senderId === myId.value ? 'self' : 'other')
    } else if (data.startsWith('SYS|')) {
      const text = data.substring(4)
      if (text.startsWith('ERR|')) addSystemMessage('❌ ' + text.substring(4))
      else addSystemMessage(text)
    } else if (data.startsWith('LEAVE|')) {
      const leaverId = data.split('|')[1]
      const leaver = memberList.value.find(m => m.id === leaverId)
      if (leaver) addSystemMessage(leaver.nick + ' 离开房间')
      ft.handleLeave(leaverId)
      rtc.removePeer(leaverId)
    } else if (data.startsWith('JOIN|')) {
      const joinerId = data.split('|')[1]
      const joiner = memberList.value.find(m => m.id === joinerId)
      if (joiner) addSystemMessage(joiner.nick + ' 加入房间')
      // 看见 JOIN 就主动去连 后进房间的人只收 MEMBERS 不建连
      rtc.connectTo(joinerId)
    } else if (data.startsWith('MEMBERS|')) {
      const raw = data.substring(8)
      memberList.value = raw
        ? raw.split(',').filter(s => s).map(p => {
            const sep = p.indexOf(':')
            return sep > 0 ? { id: p.substring(0, sep), nick: p.substring(sep + 1) } : { id: p, nick: p }
          })
        : []
    }

    // ---- WebRTC 信令 ----
    else if (data.startsWith('OFFER|')) {
      const idx1 = data.indexOf('|', 6)
      if (idx1 > 0) {
        const fromId = data.substring(6, idx1)
        rtc.handleOffer(fromId, nickOf(fromId), data.substring(idx1 + 1))
      }
    } else if (data.startsWith('ANSWER|')) {
      const idx1 = data.indexOf('|', 7)
      if (idx1 > 0) {
        rtc.handleAnswer(data.substring(7, idx1), data.substring(idx1 + 1))
      }
    } else if (data.startsWith('ICE|')) {
      const idx1 = data.indexOf('|', 4)
      if (idx1 > 0) {
        rtc.handleIce(data.substring(4, idx1), data.substring(idx1 + 1))
      }
    } else if (data.startsWith('MEDIA|')) {
      const parts = data.split('|')
      if (parts.length >= 4) rtc.handleMedia(parts[1], parts[2], parts[3])
    }

    // ---- 文件传输 ----
    else if (data.startsWith('UPOK|')) {
      ft.handleUPOK(data.substring(5))
    } else if (data.startsWith('FILE|')) {
      const parts = data.split('|')
      if (parts.length >= 5) {
        const uploaderId = parts[4]
        ft.handleFILE(nickOf(uploaderId), parts[1], parts[2], parseInt(parts[3], 10), uploaderId)
      }
    } else if (data.startsWith('DWREQ|')) {
      const parts = data.split('|')
      if (parts.length >= 5) {
        ft.handleDWREQ(parts[1], parts[2], parseInt(parts[3], 10), parseInt(parts[4], 10))
      }
    } else if (data.startsWith('DWSTART|')) {
      const parts = data.split('|')
      if (parts.length >= 4) {
        ft.handleDWSTART(parts[1], parts[2], parseInt(parts[3], 10))
      }
    } else if (data.startsWith('DWDATA|')) {
      const parts = data.split('|')
      if (parts.length >= 5) {
        ft.handleDWDATA(parts[1], parts[2], parseInt(parts[3], 10))
      }
    } else if (data.startsWith('DWNDONE|')) {
      ft.handleDWNDONE(data.substring(8))
    } else if (data.startsWith('DWERR|')) {
      const parts = data.split('|')
      if (parts.length >= 3) {
        ft.handleDWERR(parts[1], parts.slice(2).join('|'))
      }
    }
  }

  function handleBinaryMessage(data) {
    ft.handleBinary(data)
  }

  function sendMessage(text) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    addChatMessage(myNick.value, text, 'self')
    ws.send('MSG|' + text)
  }

  function leave() {
    isLeaving = true
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer)
      heartbeatTimer = null
    }
    if (ws) {
      ws.onclose = null
      ws.close()
      ws = null
    }
    rtc.hangupAll()
    ft.resetUpload()
    joined.value = false
    memberList.value = []
    messages.value = []
    connStatus.value = 'disconnected'
  }

  onUnmounted(() => {
    if (reconnectTimer) clearTimeout(reconnectTimer)
    if (heartbeatTimer) clearInterval(heartbeatTimer)
    if (ws) ws.close()
  })

  return { joined, connStatus, roomName, myNick, myId, memberList, messages, rtc, ft, connect, sendMessage, leave }
}
