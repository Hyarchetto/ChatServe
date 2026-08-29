<script setup>
import { ref } from 'vue'

const emit = defineEmits(['join'])
const room = ref('')
const nick = ref('')

function join() {
  const r = room.value.trim() || 'lobby'
  const n = nick.value.trim() || '用户' + Math.floor(Math.random() * 1000)
  emit('join', r, n)
}
</script>

<template>
  <div class="login-wrap">
    <div class="login-card">
      <h1>ChatServe</h1>
      <p class="subtitle">加入聊天室</p>
      <input v-model="room" placeholder="请输入房间号" @keyup.enter="join">
      <input v-model="nick" placeholder="昵称（默认随机）" @keyup.enter="join">
      <button class="join-btn" @click="join">加入</button>
    </div>
  </div>
</template>
