<script setup>
// 视频网格 — 自己排第一 有视频显示 video 否则头像占位
// video 元素按成员 key 保持 不重建 避免重挂载闪黑
// srcObject 在元素创建时立即赋值 之后流内增删 track 浏览器自动播放 不依赖重渲染
// active 记录每格是否有视频 驱动 video/占位 v-show 深watch 重算 useWebRTC 的 mediaTick bump 兜底
import { ref, reactive, computed, watch } from 'vue'

const props = defineProps({
  memberList: Array,
  myId: String,
  peers: Object,
  myStream: Object,
  camOn: Boolean,
  micLive: Boolean,
  volume: Number,
})

const videoEls = ref({})
const active = reactive({})

// 麦克风状态 自己用 micLive 统一判定 他人看媒体状态
function micOpen(m) {
  if (m.id === props.myId) {
    return !!props.micLive
  }
  return props.peers?.[m.id]?.mediaState?.audio === 'on'
}

// 成员是否有视频 自己看 camOn+myStream 他人看 peer 流和媒体开关
function hasLiveVideo(m) {
  if (m.id === props.myId) {
    return !!(props.myStream && props.camOn)
  }
  const peer = props.peers?.[m.id]
  if (!peer) return false
  if (peer.mediaState?.video === 'off') return false
  // muted 为对方禁用 track 后浏览器置的静音 有视频才算可见
  return !!peer.stream?.getVideoTracks().some(t => t.readyState === 'live' && !t.muted)
}

function srcObjectOf(m) {
  if (m.id === props.myId) return props.myStream || null
  return props.peers?.[m.id]?.stream || null
}

function updateVideos() {
  for (const m of props.memberList) {
    const el = videoEls.value[m.id]
    if (!el) continue
    const src = srcObjectOf(m)
    if (el.srcObject !== src) el.srcObject = src
    active[m.id] = hasLiveVideo(m)
  }
  // 清理已离开成员的记录
  for (const id in active) {
    if (!props.memberList.some(m => m.id === id)) delete active[id]
  }
}

// 元素创建/更新时立即挂流 不等 watch
// 远端 track 到达由 useWebRTC 的 mediaTick 计数驱动深 watch 重算占位
function bindVideo(el, m) {
  videoEls.value[m.id] = el
  if (!el) return
  const src = srcObjectOf(m)
  if (el.srcObject !== src) el.srcObject = src
}

watch(
  () => [props.memberList, props.peers, props.myStream, props.camOn],
  () => updateVideos(),
  { deep: true, immediate: true, flush: 'post' }
)

const cols = computed(() => {
  const n = props.memberList.length
  if (n <= 1) return 1
  if (n <= 4) return 2
  if (n <= 9) return 3
  return 4
})

const rows = computed(() => Math.ceil(props.memberList.length / cols.value))

const nameOf = (m) => m.nick + (m.id === props.myId ? '（我）' : '')
</script>

<template>
  <div class="video-grid-wrapper">
    <div
      class="video-grid"
      :style="{
        gridTemplateColumns: `repeat(${cols}, 1fr)`,
        gridTemplateRows: `repeat(${rows}, 1fr)`,
      }"
    >
      <div v-for="m in memberList" :key="m.id" class="video-cell">
        <video
          :ref="el => bindVideo(el, m)"
          autoplay
          playsinline
          :muted="m.id === myId"
          :volume="volume"
          v-show="active[m.id]"
        />
        <div class="video-placeholder" v-show="!active[m.id]">
          <div class="avatar">{{ (m.nick || m.id).charAt(0) }}</div>
          <div class="video-placeholder-name">{{ nameOf(m) }}</div>
        </div>
        <div class="video-label">{{ nameOf(m) }}</div>
        <div class="mic-status" :class="{ off: !micOpen(m) }" :title="micOpen(m) ? '麦克风已开' : '麦克风已关'">
          {{ micOpen(m) ? '🎤' : '🔇' }}
        </div>
      </div>
    </div>
  </div>
</template>
