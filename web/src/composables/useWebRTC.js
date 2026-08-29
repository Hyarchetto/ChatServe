// WebRTC P2P 视频 — 从旧 webrtc.js 迁移
// peers 响应式 驱动 VideoGrid 渲染
import { reactive, ref, shallowRef, computed } from 'vue'

export function useWebRTC({ sendCommand, myId, myNick, memberList, addSystemMessage }) {
  const peers = reactive({})
  const myStream = shallowRef(null)
  const camOn = ref(false)
  const micOn = ref(false)
  const micGain = ref(0)
  const volume = ref(0.8)
  // 有效出声与有声统一判定 供图标徽标共用
  const micLive = computed(() => micOn.value && micGain.value > 0)
  const volumeLive = computed(() => volume.value > 0)

  let audioCtx = null
  let gainNode = null
  let micRawTrack = null
  let lastMicGain = 1
  let lastVolume = 1
  let openingMic = false

  const rtcConfig = { iceServers: [{ urls: 'stun:stun.l.google.com:19302' }] }

  function getPeer(peerId) {
    if (!peers[peerId]) {
      peers[peerId] = { pc: null, stream: null, mediaState: {} }
    }
    return peers[peerId]
  }

  function closePeer(peerId) {
    const peer = peers[peerId]
    if (!peer) return
    delete peers[peerId]
    if (peer.pc) {
      peer.pc.onconnectionstatechange = null
      peer.pc.close()
    }
  }

  // 流内 track 状态变更是原生事件不触发 Vue 响应式 bump 计数驱动 VideoGrid 重算
  function bumpMediaTick(peer) {
    peer.mediaTick = (peer.mediaTick || 0) + 1
  }

  function removePeer(peerId) {
    closePeer(peerId)
    memberList.value = memberList.value.filter(m => m.id !== peerId)
  }

  function hasLiveTrack(kind) {
    return myStream.value?.getTracks().some(t => t.kind === kind && t.readyState === 'live')
  }

  function setTrackEnabled(kind, enabled) {
    if (!myStream.value) return
    for (const t of myStream.value.getTracks().filter(t => t.kind === kind)) {
      t.enabled = enabled
    }
    const state = enabled ? 'on' : 'off'
    for (const peerId in peers) {
      sendCommand('MEDIA|' + peerId + '|' + kind + '|' + state)
    }
    if (kind === 'video') camOn.value = enabled
    if (kind === 'audio') micOn.value = enabled
  }

  function addMyTracksToPC(pc) {
    if (!myStream.value) return
    getSendAudioTrack()
    for (const track of myStream.value.getTracks()) {
      if (track.readyState === 'live' && !pc.getSenders().some(s => s.track === track)) {
        pc.addTrack(track, myStream.value)
      }
    }
  }

  // 麦克风增益链 首次打开 mic 时把 raw 音频轨换成处理后轨道 发给 peer 的始终是处理后的
  // 已建立的 dest 轨道后续只需改 gainNode 无需重协商
  function getSendAudioTrack() {
    const processed = myStream.value?.getAudioTracks().find(t => t._micGain && t.readyState === 'live')
    if (processed) return processed
    const raw = myStream.value?.getAudioTracks().find(t => !t._micGain && t.readyState === 'live')
    if (!raw) return null
    // 清掉残留的处理后轨道
    for (const t of myStream.value.getAudioTracks()) {
      if (t._micGain) myStream.value.removeTrack(t)
    }
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)()
      gainNode = audioCtx.createGain()
    }
    if (audioCtx.state === 'suspended') audioCtx.resume()
    gainNode.gain.value = micGain.value
    micRawTrack = raw
    const src = audioCtx.createMediaStreamSource(new MediaStream([raw]))
    const dest = audioCtx.createMediaStreamDestination()
    src.connect(gainNode)
    gainNode.connect(dest)
    const sendTrack = dest.stream.getAudioTracks()[0]
    sendTrack._micGain = true
    myStream.value.removeTrack(raw)
    myStream.value.addTrack(sendTrack)
    return sendTrack
  }

  // 滑块改增益 处理后的 dest 轨道输出随之变化 增益过零联动轨道开关
  function setMicGain(v) {
    const g = Number(v) || 0
    micGain.value = g
    if (gainNode) gainNode.gain.value = g
    if (g > 0) ensureMicOn()
    else ensureMicOff()
  }

  // 增益大于 0 时确保麦克风在发送 已开只重新启用
  async function ensureMicOn() {
    if (micOn.value || openingMic) return
    if (!myStream.value?.getAudioTracks().some(t => t.readyState === 'live')) {
      openingMic = true
      try {
        await toggleMedia('audio')
      } finally {
        openingMic = false
      }
    } else {
      setTrackEnabled('audio', true)
    }
  }

  // 增益为 0 时停止发送
  function ensureMicOff() {
    setTrackEnabled('audio', false)
  }

  function tearDownAudio() {
    if (micRawTrack) {
      micRawTrack.stop()
      micRawTrack = null
    }
    if (audioCtx) {
      audioCtx.close()
      audioCtx = null
      gainNode = null
    }
  }

  // 连接建立时向新 peer 补发当前媒体开关 关闭的 track 仍会进 offer 需 off 信号避免黑屏
  function notifyMediaState(peerId) {
    sendCommand('MEDIA|' + peerId + '|video|' + (camOn.value ? 'on' : 'off'))
    sendCommand('MEDIA|' + peerId + '|audio|' + (micOn.value ? 'on' : 'off'))
  }

  function createPC(peerId) {
    // closePeer 会 delete 重建 peer 保留已有媒体状态 否则 MEDIA 先于 OFFER 到达时开关信号被清掉
    const prevMediaState = peers[peerId]?.mediaState
    closePeer(peerId)
    const peer = getPeer(peerId)
    if (prevMediaState) peer.mediaState = prevMediaState
    const pc = new RTCPeerConnection(rtcConfig)
    peer.pc = pc
    peer.stream = new MediaStream()

    pc.onnegotiationneeded = async () => {
      try {
        await pc.setLocalDescription(await pc.createOffer())
        sendCommand('OFFER|' + peerId + '|' + btoa(pc.localDescription.sdp))
      } catch (e) {
        // 频繁触发时可能冲突 忽略
      }
    }

    pc.onicecandidate = (e) => {
      if (e.candidate) {
        sendCommand('ICE|' + peerId + '|' + btoa(JSON.stringify(e.candidate)))
      }
    }

    pc.ontrack = (e) => {
      if (!peer.stream) peer.stream = new MediaStream()
      peer.stream.addTrack(e.track)
      // onmute 对方禁用 track 后浏览器置静音 隐藏视频 恢复时再显示
      e.track.onended = () => bumpMediaTick(peer)
      e.track.onmute = () => bumpMediaTick(peer)
      e.track.onunmute = () => bumpMediaTick(peer)
      bumpMediaTick(peer)
    }

    pc.onconnectionstatechange = () => {
      if (pc.connectionState === 'disconnected' ||
          pc.connectionState === 'failed' ||
          pc.connectionState === 'closed') {
        if (!peers[peerId]) return
        closePeer(peerId)
      }
    }

    return pc
  }

  // 与房间成员建立 P2P 连接 已连接跳过 id 较小的发 offer 避免 glare
  function sendOffersToAll() {
    for (const m of memberList.value) {
      if (m.id === myId.value || peers[m.id]) continue
      if (Number(myId.value) >= Number(m.id)) continue
      const pc = createPC(m.id)
      addMyTracksToPC(pc)
      notifyMediaState(m.id)
      ;(async () => {
        try {
          const offer = await pc.createOffer()
          await pc.setLocalDescription(offer)
          sendCommand('OFFER|' + m.id + '|' + btoa(offer.sdp))
        } catch (e) {
          addSystemMessage('❌ 与 ' + (m.nick || m.id) + ' 建立连接失败')
        }
      })()
    }
  }

  async function startMyMedia(opts) {
    const constraints = {}
    if (opts.video) constraints.video = true
    if (opts.audio) constraints.audio = true
    if (Object.keys(constraints).length === 0) return true

    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      addSystemMessage('❌ 浏览器未开放媒体权限：请通过 HTTPS 或 localhost 访问')
      return false
    }

    try {
      const newStream = await navigator.mediaDevices.getUserMedia(constraints)
      if (!myStream.value) myStream.value = new MediaStream()
      for (const track of newStream.getTracks()) {
        myStream.value.addTrack(track)
      }
      getSendAudioTrack()
      if (opts.video) camOn.value = true
      if (opts.audio) micOn.value = true
      return true
    } catch (e) {
      addSystemMessage('❌ 无法访问媒体设备: ' + e.message)
      return false
    }
  }

  async function toggleMedia(kind) {
    const isOn = kind === 'video' ? camOn.value : micOn.value
    if (isOn) {
      setTrackEnabled(kind, false)
      return
    }
    if (!hasLiveTrack(kind)) {
      const constraints = {}
      constraints[kind] = true
      const ok = await startMyMedia(constraints)
      if (!ok) return
      const track = myStream.value.getTracks().find(t => t.kind === kind)
      for (const peerId in peers) {
        const pc = peers[peerId].pc
        if (track && pc && !pc.getSenders().some(s => s.track === track)) {
          pc.addTrack(track, myStream.value)
        }
      }
    }
    setTrackEnabled(kind, true)
  }

  // 点击图标只改音量 静音归零 取消静音恢复上次音量 轨道开关由 setMicGain 联动
  function toggleMic() {
    if (micLive.value) {
      lastMicGain = micGain.value
      setMicGain(0)
    } else {
      setMicGain(lastMicGain || 1)
    }
  }

  // 网页音量静音图标 静音归零 取消静音恢复上次音量
  function toggleVolume() {
    if (volumeLive.value) {
      lastVolume = volume.value
      volume.value = 0
    } else {
      volume.value = lastVolume
    }
  }

  function hangupAll() {
    for (const pid in peers) closePeer(pid)
    if (myStream.value) {
      for (const t of myStream.value.getTracks()) t.stop()
      myStream.value = null
    }
    tearDownAudio()
    camOn.value = false
    micOn.value = false
  }

  // ---------- 信令处理 ----------

  async function acceptOffer(pc, fromId, sdpBase64) {
    const sdp = atob(sdpBase64)
    await pc.setRemoteDescription({ type: 'offer', sdp })
    const answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)
    sendCommand('ANSWER|' + fromId + '|' + btoa(answer.sdp))
  }

  async function handleOffer(fromId, fromNick, sdpBase64) {
    const peer = getPeer(fromId)
    if (peer.pc) {
      try {
        await acceptOffer(peer.pc, fromId, sdpBase64)
        return
      } catch (e) {
        closePeer(fromId)
      }
    }
    const pc = createPC(fromId)
    try {
      await acceptOffer(pc, fromId, sdpBase64)
      addMyTracksToPC(pc)
      notifyMediaState(fromId)
    } catch (e) {
      addSystemMessage('❗ 与 ' + fromNick + ' 的视频连接异常')
    }
  }

  async function handleAnswer(fromId, sdpBase64) {
    const peer = peers[fromId]
    if (!peer || !peer.pc) return
    try {
      const sdp = atob(sdpBase64)
      await peer.pc.setRemoteDescription({ type: 'answer', sdp })
    } catch (e) {
      // 偶尔状态冲突可忽略
    }
  }

  function handleIce(fromId, candidateBase64) {
    const peer = peers[fromId]
    if (!peer || !peer.pc) return
    try {
      const cand = JSON.parse(atob(candidateBase64))
      peer.pc.addIceCandidate(new RTCIceCandidate(cand))
    } catch (e) {
      // 无效 candidate 忽略
    }
  }

  function handleMedia(fromId, kind, state) {
    const p = getPeer(fromId)
    p.mediaState[kind] = state
  }

  return { peers, myStream, camOn, micOn, micGain, micLive, volume, volumeLive, setMicGain, toggleMic, toggleVolume, sendOffersToAll, toggleMedia, handleOffer, handleAnswer, handleIce, handleMedia, removePeer, hangupAll }
}
