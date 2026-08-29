<script setup>
import { useChat } from './composables/useChat'
import LoginPanel from './components/LoginPanel.vue'
import ChatView from './components/ChatView.vue'

const { joined, connStatus, roomName, myId, memberList, messages, rtc, ft, connect, sendMessage, leave } = useChat()
</script>

<template>
  <div class="app-root">
    <LoginPanel v-if="!joined" @join="connect" />
    <ChatView
      v-else
      :conn-status="connStatus"
      :room-name="roomName"
      :my-id="myId"
      :member-list="memberList"
      :messages="messages"
      :rtc="rtc"
      :ft="ft"
      @send="sendMessage"
      @leave="leave"
    />
  </div>
</template>
