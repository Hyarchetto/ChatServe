const { app, BrowserWindow } = require('electron')

// 服务器地址 优先命令行 --server= 其次 CHATSERVE_SERVER 环境变量 最后内置默认
// 末尾斜杠统一去掉 免得和下面拼出双斜杠路径
function resolve_server() {
    const flag = process.argv.find(a => a.startsWith('--server='))
    const raw = (flag && flag.slice('--server='.length)) ||
                process.env.CHATSERVE_SERVER ||
                'https://192.168.1.24:8443'
    return raw.replace(/\/+$/, '')
}

const SERVER = resolve_server()

// 自签证书会被 Chromium 拒绝 只放行配置的那个 host 其余照常拒
app.on('certificate-error', (event, contents, url, error, cert, callback) => {
    let same_host = false
    try {
        same_host = new URL(url).host === new URL(SERVER).host
    }
    catch {
        same_host = false
    }
    if (!same_host) {
        callback(false)
        return
    }
    event.preventDefault()
    callback(true)
})

function create_window() {
    const win = new BrowserWindow({
        width: 1280,
        height: 800,
        // 壳不设菜单 要 DevTools 按 Alt 唤出
        autoHideMenuBar: true,
    })
    win.loadURL(SERVER + '/chat')
}

app.whenReady().then(create_window)

app.on('window-all-closed', () => app.quit())
