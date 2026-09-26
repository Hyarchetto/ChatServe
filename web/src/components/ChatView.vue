<script setup>
import { ref, nextTick } from 'vue'
import VideoGrid from './VideoGrid.vue'
import MessageList from './MessageList.vue'
import { formatFileSize } from '../utils'

const props = defineProps({
  connStatus: String,
  roomName: String,
  myId: String,
  memberList: Array,
  messages: Array,
  rtc: Object,
  ft: Object,
})
const emit = defineEmits(['send', 'leave'])
const msg = ref('')
const msgListRef = ref(null)
const msgInputRef = ref(null)

// 输入框长高的上限 与 style.css 里 .input-area textarea 的 max-height 保持一致
const kMaxInputHeight = 120

// 输入框随内容长高 到上限后转成内部滚动
// 先把高度压回 auto 再读 scrollHeight 否则量到的是上一轮的高度 只会越量越大
function growInput() {
  const e = msgInputRef.value
  if (!e) return
  e.style.height = 'auto'
  e.style.height = Math.min(e.scrollHeight, kMaxInputHeight) + 'px'
}

function send() {
  const t = msg.value.trim()
  if (!t) return
  emit('send', t)
  msg.value = ''
  // 发送后收回一行并回到底部 都等渲染完再做
  nextTick(() => {
    growInput()
    msgListRef.value?.scrollToLatest()
  })
}

// 回车发送 Shift+回车换行 其余修饰键组合一律放行
// 输入法组合期间必须直接返回 中文选词的确认键同样是回车
// 不判这一下 打一个词按回车确认候选 半截拼音就被当消息发了出去
// keyCode 229 是输入法自己的信号 Safari 的 keydown 不设 isComposing 只认这个 两个都要
function onKeydown(e) {
  if (e.isComposing || e.keyCode === 229) return
  if (e.key !== 'Enter' || e.shiftKey || e.ctrlKey || e.altKey || e.metaKey) return
  e.preventDefault()
  send()
}

function onFile(e) {
  if (e.target.files.length > 0) props.ft.selectFile(e.target.files[0])
  e.target.value = ''
}

const connText = { connected: '已连接', reconnecting: '重连中', disconnected: '未连接' }
</script>

<template>
  <div class="chat-wrap">
    <header class="chat-header">
      <div class="header-left">
        <span class="conn-dot" :class="connStatus"></span>
        <span class="conn-text">{{ connText[connStatus] || '未连接' }}</span>
        <span class="room-name">{{ roomName }}</span>
      </div>
      <div class="header-right">
        <span class="member-count">在线: {{ memberList.length }} 人</span>
        <button class="leave-btn" @click="$emit('leave')">退出房间</button>
      </div>
    </header>
    <div class="chat-body">
      <div class="video-area">
        <VideoGrid
          :member-list="memberList"
          :my-id="myId"
          :peers="rtc.peers"
          :my-stream="rtc.myStream"
          :cam-on="rtc.camOn"
          :mic-live="rtc.micLive"
          :volume="rtc.volume"
        />
        <div class="video-controls">
          <button :class="{ active: rtc.camOn }" title="开关摄像头" @click="rtc.toggleMedia('video')">📷</button>
          <div class="volume-control" title="麦克风">
            <button
              class="volume-btn mic-btn"
              :class="{ off: !rtc.micLive }"
              :title="rtc.micLive ? '静音麦克风' : '取消静音'"
              @click="rtc.toggleMic"
            ><span class="mic-icon">🎤</span></button>
            <input :value="rtc.micGain" @input="rtc.setMicGain($event.target.value)" type="range" min="0" max="1" step="0.05" class="volume-slider">
          </div>
          <div class="volume-control" title="网页音量">
            <button
              class="volume-btn"
              :class="{ off: !rtc.volumeLive }"
              :title="rtc.volumeLive ? '静音网页' : '取消静音'"
              @click="rtc.toggleVolume"
            >{{ rtc.volumeLive ? '🔉' : '🔇' }}</button>
            <input v-model.number="rtc.volume" type="range" min="0" max="1" step="0.05" class="volume-slider">
          </div>
        </div>
      </div>
      <div class="chat-panel">
        <MessageList ref="msgListRef" :messages="messages" :ft="ft" />
        <div class="file-preview" v-if="ft.pendingFile">
          <span class="file-icon">📎</span>
          <span class="file-preview-name">{{ ft.pendingFile.name }}</span>
          <span class="file-preview-size">{{ formatFileSize(ft.pendingFile.size) }}</span>
          <div class="file-preview-actions">
            <button class="file-cancel-btn" @click="ft.resetUpload">取消</button>
            <button class="file-send-btn" @click="ft.sendUpload">发送文件</button>
          </div>
        </div>
        <div class="input-area">
          <label class="file-btn" title="发送文件">
            <svg viewBox="0 0 24 24" width="22" height="22">
              <path fill="currentColor" d="M16.5 6v11.5c0 2.21-1.79 4-4 4s-4-1.79-4-4V5c0-1.38 1.12-2.5 2.5-2.5s2.5 1.12 2.5 2.5v10.5c0 .55-.45 1-1 1s-1-.45-1-1V6H10v9.5c0 1.38 1.12 2.5 2.5 2.5s2.5-1.12 2.5-2.5V5c0-2.21-1.79-4-4-4S7 2.79 7 5v12.5c0 3.04 2.46 5.5 5.5 5.5s5.5-2.46 5.5-5.5V6h-1.5z"/>
            </svg>
            <input type="file" hidden @change="onFile">
          </label>
          <textarea
            ref="msgInputRef"
            v-model="msg"
            id="msg"
            name="msg"
            rows="1"
            autocomplete="off"
            placeholder="输入消息..."
            title="回车发送，Shift+回车换行"
            @input="growInput"
            @keydown="onKeydown"
          ></textarea>
          <button class="send-btn" @click="send">发送</button>
        </div>
      </div>
    </div>
  </div>
</template>
