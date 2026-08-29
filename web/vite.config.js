import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 构建产物输出到 ../static（服务器 static/ 根目录）
// base './' 用相对路径 服务器把 /chat 指到 index.html 后资源能解析到 /assets/
export default defineConfig({
  plugins: [vue()],
  base: './',
  build: {
    outDir: '../static',
    // outDir 在项目根外 Vite 默认拒绝清空 显式开启 每次构建清掉旧 hashed 产物
    emptyOutDir: true,
  },
})
