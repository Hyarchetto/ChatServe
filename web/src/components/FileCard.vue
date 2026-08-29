<script setup>
// 文件卡 — 上传卡/下载卡 按会话状态渲染按钮与进度
import { computed } from 'vue'
import { formatFileSize } from '../utils'

const props = defineProps({ msg: Object, ft: Object })

const file = computed(() => props.msg.file)
const isUpload = computed(() => props.msg.type === 'file-upload')

const uploading = computed(() => !!props.ft.uploadSessions[file.value.fileId])
const session = computed(() => props.ft.downloadSessions[file.value.fileId])
// fileSenders 以 fileId 为键存 uploaderId 上传方离开时该键被删 卡片失效
const invalid = computed(() => !props.ft.fileSenders[file.value.fileId])
const paused = computed(() => !!session.value && session.value.paused)
const downloading = computed(() => !!session.value && !session.value.paused)
const progress = computed(() => {
  const s = session.value
  if (!s || s.total === 0) return ''
  return Math.min(100, Math.round(s.received / s.total * 100)) + '%'
})
</script>

<template>
  <div class="file-card">
    <span class="file-card-icon">📄</span>
    <div class="file-card-info">
      <div class="file-card-name">{{ file.filename }}</div>
      <div class="file-card-size">{{ formatFileSize(file.filesize) }}</div>
    </div>
    <div class="file-card-actions" :class="{ stacked: paused }">
      <template v-if="isUpload">
        <button v-if="uploading" class="file-card-cancel" @click="ft.cancelUploadFile(file.fileId)">✖ 取消上传</button>
        <button v-else class="file-card-dl disabled" disabled>已取消上传</button>
      </template>
      <template v-else>
        <button v-if="invalid" class="file-card-dl disabled" disabled>已失效</button>
        <button v-else-if="downloading" class="file-card-dl downloading" @click="ft.pauseDownload(file.fileId)">⏸ 暂停下载</button>
        <template v-else-if="paused">
          <button class="file-card-dl" @click="ft.resumeDownload(file.fileId)">▶ 继续下载</button>
          <button class="file-card-cancel" @click="ft.cancelDownloadFile(file.fileId)">✖ 取消下载</button>
        </template>
        <button v-else class="file-card-dl" @click="ft.startDownload(file.fileId)">⬇ 下载</button>
      </template>
      <span class="file-card-progress">{{ progress }}</span>
    </div>
  </div>
</template>
