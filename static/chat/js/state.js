// 全局状态变量

let ws = null;
let reconnectTimer = null;
let myNick = '';
let myId = '';
let myRoom = '';
let memberList = [];
let isLeaving = false;
let isReconnect = false;

// ---- WebRTC 视频通话状态 ----
const rtcConfig = {
    iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
};
let peers = {};
let myStream = null;
let camOn = false;
let micOn = false;

// ---- 文件上传状态 ----
let pendingFile = null;
let uploadSessions = {};
let uploadCards = {};
let cancelUpload = false;

// ---- 文件下载状态 ----
let downloadSessions = {};
let pendingChunks = {};
let fileSenders = {};
