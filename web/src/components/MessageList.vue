<script setup>
// 消息列表 — 贴底时新消息自动跟随 上翻看历史时新消息弹出回底按钮
import { ref, watch, nextTick } from 'vue'
import FileCard from './FileCard.vue'

const props = defineProps({ messages: Array, ft: Object })

const el = ref(null)
const showJump = ref(false)

// 距离底部 40px 内视为贴底
function isNearBottom() {
  const e = el.value
  if (!e) return true
  return e.scrollHeight - e.scrollTop - e.clientHeight < 40
}

function onScroll() {
  if (isNearBottom()) showJump.value = false
}

function scrollToBottom(smooth) {
  const e = el.value
  if (!e) return
  e.scrollTo({ top: e.scrollHeight, behavior: smooth ? 'smooth' : 'auto' })
}

// 回到最新并隐藏回底按钮 smooth 区分按钮平滑与外部触发即时
function goToLatest(smooth) {
  scrollToBottom(smooth)
  showJump.value = false
}

function jumpToBottom() {
  goToLatest(true)
}

// 新消息 flush pre 在 DOM 更新前量贴底 此时量的是添加前的真实位置
// 浏览器追加内容不调 scrollTop 若在 post 后量会把贴底误判成离底
// 贴底则 nextTick 等渲染完滚到底 上翻则弹回底按钮
watch(
  () => props.messages.length,
  (len, prevLen) => {
    if (len < prevLen) {
      // 消息被清空 重连或重置 回到贴底不弹按钮
      showJump.value = false
      return
    }
    if (isNearBottom()) nextTick(() => scrollToBottom(false))
    else showJump.value = true
  }
)

// 外部触发 输入消息时强制回到最新
function scrollToLatest() {
  goToLatest(false)
}

defineExpose({ scrollToLatest })

function msgClass(m) {
  if (m.type === 'sys') return 'sys'
  if (m.type === 'file-upload' || m.type === 'self') return 'self'
  if (m.type === 'file-notify' || m.type === 'other') return 'other'
  return ''
}

function msgName(m) {
  if (m.type === 'self') return '我'
  if (m.type === 'file-upload') return '你上传了文件'
  if (m.type === 'file-notify') return m.sender + ' 发送了文件'
  return m.name
}
</script>

<template>
  <div class="messages-wrap">
    <div ref="el" class="messages" @scroll="onScroll">
      <div v-for="(m, i) in messages" :key="i" class="msg" :class="msgClass(m)">
        <div v-if="m.type !== 'sys'" class="msg-header">
          <span class="msg-name">{{ msgName(m) }}</span>
          <span class="msg-time">{{ m.time }}</span>
        </div>
        <FileCard v-if="m.type === 'file-notify' || m.type === 'file-upload'" :msg="m" :ft="ft" />
        <div v-else class="msg-body">{{ m.text }}</div>
      </div>
    </div>
    <button v-show="showJump" class="jump-btn" title="回到底部" @click="jumpToBottom">↓ 点击查看最新消息</button>
  </div>
</template>
