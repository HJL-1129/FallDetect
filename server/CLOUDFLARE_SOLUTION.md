# Cloudflare 零成本部署方案

您的域名 `lele1129.top` 已托管在 **Cloudflare**，不需要买任何服务器！

| 组件 | 使用 | 费用 |
|------|------|------|
| 域名DNS | Cloudflare（已配置） | 免费 |
| SSL证书 | Cloudflare自动管理 | 免费 |
| 后端API | Cloudflare Workers | 免费（10万请求/天） |
| GIS页面 | Cloudflare Pages | 免费（无限带宽） |

---

## 第一步：修复 HTTPS 访问（5分钟）

当前 `https://lele1129.top` 报 SSL 错误是因为 Cloudflare SSL/TLS 设置不对。

登录 [Cloudflare 控制台](https://dash.cloudflare.com/)：
1. 点击域名 `lele1129.top`
2. 左侧菜单 → **SSL/TLS**
3. 选择 **完全（严格）**（Full Strict）
4. 等待1分钟自动生效

---

## 第二步：部署后端 API（Worker）

### 2.1 创建 Worker

1. 登录 https://dash.cloudflare.com
2. 左侧 → **Workers 和 Pages**
3. 点击 **创建应用程序** → **Worker**
4. 名称填写：`fall-api`
5. 点击 **部署**

### 2.2 粘贴代码

部署后自动进入编辑页面，**全部删除**默认代码，粘贴以下代码：

> 📁 代码文件位置：项目 `server/worker.js`

```javascript
/**
 * Cloudflare Worker - 跌倒检测数据API
 */
const corsHeaders = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400',
};

let fallRecords = [];

async function handleRequest(request) {
    const url = new URL(request.url);
    const path = url.pathname;
    const method = request.method;

    if (method === 'OPTIONS') {
        return new Response(null, { headers: corsHeaders, status: 204 });
    }

    // POST /api/fall - 接收跌倒数据
    if (method === 'POST' && path === '/api/fall') {
        try {
            const data = await request.json();
            if (!data.event || data.event !== 'fall') {
                return new Response(JSON.stringify({ error: 'Invalid event' }), {
                    status: 400, headers: { 'Content-Type': 'application/json', ...corsHeaders },
                });
            }
            data.received_at = new Date().toISOString();
            fallRecords.unshift(data);
            if (fallRecords.length > 200) fallRecords = fallRecords.slice(0, 200);

            return new Response(JSON.stringify({
                status: 'ok', message: 'Received',
                device_id: data.device_id || 'unknown'
            }), {
                status: 200, headers: { 'Content-Type': 'application/json', ...corsHeaders },
            });
        } catch (e) {
            return new Response(JSON.stringify({ error: e.message }), {
                status: 400, headers: { 'Content-Type': 'application/json', ...corsHeaders },
            });
        }
    }

    // GET /api/fall - 查询全部记录
    if (method === 'GET' && path === '/api/fall') {
        return new Response(JSON.stringify(fallRecords), {
            status: 200, headers: { 'Content-Type': 'application/json', ...corsHeaders },
        });
    }

    // GET /api/health - 健康检查
    if (path === '/api/health') {
        return new Response(JSON.stringify({
            status: 'ok', count: fallRecords.length, timestamp: new Date().toISOString()
        }), {
            status: 200, headers: { 'Content-Type': 'application/json', ...corsHeaders },
        });
    }

    return new Response(JSON.stringify({ error: 'Not Found' }), { status: 404, headers: corsHeaders });
}

addEventListener('fetch', event => {
    event.respondWith(handleRequest(event.request));
});
```

点击 **保存并部署**。

### 2.3 记录 Worker 地址

部署完成后，你会看到地址如：
```
https://fall-api.你的子域.workers.dev
```

→ 复制这个地址，下一步需要用到。

---

## 第三步：部署 GIS 地图页面（Pages）

### 3.1 准备文件

创建一个临时文件夹（比如 `gis-page`），里面只放一个文件：

**`index.html`** — 直接从项目复制 `server/gis.html`，但**修改第1行API地址**：

找到这行：
```javascript
const API_BASE = 'https://fall-api.你的用户名.workers.dev';
```

改为刚才记录的 Worker 地址，例如：
```javascript
const API_BASE = 'https://fall-api.xxx.workers.dev';
```

### 3.2 上传到 Pages

1. Cloudflare 控制台 → **Workers 和 Pages** → **Pages**
2. 点击 **创建应用程序** → **直接上传**
3. 项目名称：`fall-detection`
4. 上传刚刚的 `index.html` 文件
5. 点击 **保存并部署**

部署完成后得到地址：
```
https://fall-detection.pages.dev
```

### 3.3 绑定自定义域名（可选）

想让 `https://gis.lele1129.top` 直接打开地图：

1. 在 Pages 项目页面 → **自定义域**
2. 添加 `gis.lele1129.top`
3. Cloudflare 自动添加 DNS 记录
4. 等待 DNS 生效（几分钟）
5. 浏览器打开 `https://gis.lele1129.top` 即可看到地图

---

## 第四步：测试 API

打开浏览器或使用 curl 测试：

### 测试健康检查
```
https://fall-api.你的子域.workers.dev/api/health
```
返回：`{"status":"ok","count":0,"timestamp":"..."}`

### 测试 POST 上报
```bash
curl -X POST https://fall-api.你的子域.workers.dev/api/fall \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "test_001",
    "event": "fall",
    "time": "2026-07-10 15:30:00",
    "lat": 39.9042,
    "lng": 116.4074,
    "angle": 75,
    "acceleration": 3.5
  }'
```
返回：`{"status":"ok","message":"Received","device_id":"test_001"}`

### 测试 GET 查询
```
https://fall-api.你的子域.workers.dev/api/fall
```
返回：上面POST的数据列表

---

## 第五步：修改 ESP32 上传地址

因为您用 `lele1129.top` 域名，而且域名托管在 Cloudflare，所以要改 `wifi_upload.h` 中的上传地址为 Worker 地址：

```
// 原来: https://lele1129.top/api/fall
// 改为:
#define UPLOAD_SERVER_URL       "https://fall-api.你的子域.workers.dev"
#define UPLOAD_SERVER_PATH      "/api/fall"
```

这是因为：
- `https://lele1129.top` 指向的是 Cloudflare Pages（静态网站）
- Pages 只托管 HTML/CSS/JS 文件，不能处理 POST 请求
- Worker 才能处理 POST 数据请求

> ⚠️ **重要**: `lele1129.top` 目前已经被重定向到 Cloudflare Pages（`le1129.pages.dev`），所以 ESP32 向 `lele1129.top` 发 POST 会是 404。必须改为 Worker 地址。

---

## 整体架构图

```
ESP32 跌倒检测设备
    │
    │  HTTP POST JSON
    ▼
Cloudflare Worker  ←→ 内存/可选KV存储
(fall-api.xxx.workers.dev/api/fall)
    │
    │  GET /api/fall
    ▼
Cloudflare Pages   ←→ Leaflet 地图
(gis.lele1129.top)      显示跌倒标记
```

---

## 如果以后想用 KV 持久化

1. Cloudflare 控制台 → **Workers 和 Pages** → **KV**
2. 创建命名空间 `FALL_KV`
3. 进入 Worker `fall-api` → **设置** → **变量** → **KV 命名空间绑定**
4. 点击 **添加绑定**
   - 变量名称：`FALL_KV`
   - KV 命名空间：选择刚才创建的 `FALL_KV`
5. 保存后数据就不会丢失了