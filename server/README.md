# 🚨 跌倒检测系统 - 从零开始完整部署教程

## 系统架构

```
ESP32 跌倒检测设备
    │
    │ HTTP POST JSON (跌倒数据)
    ▼
┌─────────────────────────────────────┐
│  Cloudflare Worker (后端API)        │
│  POST /api/fall → 保存跌倒事件      │
│  GET  /api/fall  → 查询所有记录     │
│  URL: https://api.lele1129.top      │
└────────────┬────────────────────────┘
             │
             ▼
┌─────────────────────────────────────┐
│  GIS 地图页面 (前端)                │
│  域名: https://gis.lele1129.top     │
│  使用 Leaflet + 天地图(Tianditu)瓦片│
│  显示所有跌倒位置标记               │
└─────────────────────────────────────┘
```

---

## 📦 server/ 目录文件说明

| 文件 | 用途 |
|------|------|
| `worker.js` | Cloudflare Worker 代码（API + 内嵌 GIS 地图页面） |
| `CLOUDFLARE_SOLUTION.md` | Cloudflare 部署精简版指南 |
| `README.md` | 本教程文档 |

---

# 🔧 完整部署步骤

## 第一步：部署 Worker（后端API）

Worker 是负责接收 ESP32 上传的跌倒数据并存储的接口。

### 方法一：通过 Cloudflare 控制台（推荐）

1. **登录 Cloudflare**
   - 打开 https://dash.cloudflare.com
   - 使用你的账号登录

2. **创建 Worker**
   - 左侧菜单 → **Workers 和 Pages**
   - 点击 **创建应用程序**
   - 选择 **创建 Worker**
   - 名称填写：`fall-api`
   - 点击 **部署**

3. **粘贴代码**
   - 部署成功后，点击 **编辑代码**
   - 删除默认代码
   - 打开本仓库的 `server/worker.js`，全选复制
   - 粘贴到 Cloudflare 编辑器中
   - 点击 **保存并部署**

4. **添加自定义域名（可选但推荐）**
   - 在 Worker 页面 → **触发器** 选项卡
   - 点击 **添加自定义域**
   - 输入：`api.lele1129.top`
   - 点击 **添加域**

### 方法二：通过 API 命令行（已执行）

> ✅ 此步骤已经通过 API 自动完成，Worker 代码已部署到 Cloudflare。

---

## 第二步：配置 DNS 路由

让 `api.lele1129.top` 指向 Worker。

1. 登录 https://dash.cloudflare.com
2. 进入 **Workers 和 Pages**
3. 点击 **fall-api**
4. 进入 **触发器** 选项卡
5. 在 **路由** 区域，点击 **添加路由**

填写：
| 字段 | 值 |
|------|-----|
| 路由 | `api.lele1129.top/*` |
| 工作器 | `fall-api` |

点击 **保存**

> ⏱ 等待 1-2 分钟 DNS 生效

---

## 第三步：测试 API 是否正常

### 测试方式一：浏览器访问

打开浏览器，访问：
```
https://api.lele1129.top/api/health
```

正常返回：
```json
{"status":"ok","count":0,"kv_bound":false,"timestamp":"2026-07-13T13:00:00.000Z"}
```

### 测试方式二：命令行测试

```bash
# 健康检查
curl https://api.lele1129.top/api/health

# POST 模拟跌倒事件（测试上传）
curl -X POST https://api.lele1129.top/api/fall \
  -H "Content-Type: application/json" \
  -d '{
    "device_id":"test_001",
    "event":"fall",
    "timestamp":"2026-07-13 21:00:00",
    "latitude":39.9042,
    "longitude":116.4074,
    "coordinate":"WGS84",
    "angle":75,
    "acceleration":3.5
  }'

# 查询所有记录
curl https://api.lele1129.top/api/fall
```

---

## 第四步：GIS 地图页面

GIS 地图页面已内嵌在 `worker.js` 中，无需单独部署。

Worker 自动处理以下路由：

| 路由 | 说明 |
|------|------|
| `GET /` 或 `GET /index.html` | 返回 GIS 地图页面 |
| `POST /api/fall` | 接收跌倒事件 |
| `GET /api/fall` | 查询所有跌倒记录 |

部署好 Worker 后，直接访问 Worker 域名即可查看 GIS 地图。

如果不希望 GIS 页面与 API 共用同一域名，可以创建第二个 Worker（复制 `worker.js` 代码，只保留 GET / 路由部分），参考以下步骤：

1. 在 Cloudflare 控制台再创建一个 Worker（如 `fall-gis`）
2. 粘贴 `worker.js` 代码
3. 添加自定义域名：`gis.lele1129.top`
4. 在第二个 Worker 的 DNS 路由中配置：`gis.lele1129.top/*` → `fall-gis`

---

## 🗺 天地图（Tianditu）地图瓦片

GIS 地图已由 Esri/OpenStreetMap 切换为**天地图**作为主源，支持矢量/卫星一键切换。

### 瓦片源说明

| 图层 | 基础瓦片 | 注记瓦片 |
|------|---------|---------|
| 矢量 | `vec_w` | `cva_w`（中文注记） |
| 卫星 | `img_w` | `cia_w`（中文注记） |

URL 模板：
```
https://t{s}.tianditu.gov.cn/DataServer?T=<图层>&tk=<天地图Key>&x={x}&y={y}&l={z}
```

> ⚠️ 注意：
> - 必须使用 `l={z}`，Leaflet 默认的 `{z}` 会返回 403。
> - 子域必须为 `01234567`，Leaflet 默认 `abc` 会返回 403。
> - `tk` 为本页使用的天地图 Key：`d31a485ae08767b84f5f859987cf74bd`。

### 域名白名单（天地图控制台）

登录 https://console.tianditu.gov.cn → 我的应用 → 修改白名单，添加：
```
lele1129.top,www.lele1129.top,gis.lele1129.top,api.lele1129.top,127.0.0.1
```

> ⚠️ 白名单必须填写**完整域名**（通配符、`https://` 前缀、路径均不支持）。若白名单未添加当前访问域名，天地图将返回 403，页面会自动回退到 Esri → OSM 兜底源。

### 图层切换与标记保留

地图头部有「🗺 矢量 / 🛰 卫星」切换按钮。切换图层仅替换瓦片图层，**不会清空已绘制的跌倒报警标记**。当天地图瓦片加载失败（如白名单未配置）时，会自动切换回 Esri → OSM 兜底源（参考 `worker.js` / `server_local.js` / `gis.html` 中的 `setLayerMode` / `loadFallback` / `loadTiles`）。

---

## 第五步：配置 ESP32 设备

### 修改 WiFi 上传地址

打开 `main/include/wifi_upload.h`，找到：
```c
#define UPLOAD_SERVER_URL "https://api.lele1129.top/api/fall"
```

确认 URL 已正确指向你的 API 地址。

### 编译上传

```bash
cd FallDetect
idf.py build
idf.py -p COM端口 flash monitor
```

---

## 第六步：验证完整链路

### 测试流程

1. **ESP32 启动** → 连接 WiFi → 开始检测
2. **触发跌倒** → ESP32 采集数据 → POST 到 API
3. **打开浏览器** → 访问 `https://gis.lele1129.top`
4. **地图显示** → 跌倒位置标记弹出

### 预期效果

```
┌─────────────────────────────────────────────┐
│  🚨 跌倒检测 GIS 地图                        │
│  ┌─────────────────────────────────────────┐ │
│  │                                         │ │
│  │         🗺️ 地图区域                      │ │
│  │                                         │ │
│  │         🔴 跌倒点标记                     │ │
│  │         ┌──────────────┐                │ │
│  │         │ ⚠ 跌倒报警    │                │ │
│  │         │ 设备: fall_001│                │ │
│  │         │ 时间: 12:30   │                │ │
│  │         │ 坐标: 39.90   │                │ │
│  │         │ 状态: 跌倒报警 │                │ │
│  │         └──────────────┘                │ │
│  │                                         │ │
│  └─────────────────────────────────────────┘ │
│  📊 总计: 5 次跌倒 · 最近1小时: 3 次         │
└─────────────────────────────────────────────┘
```

---

## ❓ 常见问题

### Q1: API 返回 404
- 检查路由是否已配置：`api.lele1129.top/*` → `fall-api`
- 等待 DNS 生效（1-2 分钟）

### Q2: GIS 页面显示"连接失败"
- 打开浏览器开发者工具（F12）→ Console
- 查看具体错误信息
- 检查 `API_BASE` 是否设置为 `https://api.lele1129.top`

### Q3: ESP32 上传失败
- 检查 WiFi 是否连接
- 检查 `UPLOAD_SERVER_URL` 是否正确
- 检查 HTTPS 证书（Cloudflare 自动提供）

### Q4: 地图不显示标记
- 确认 `latitude` 和 `longitude` 字段名称
- 确认坐标值不是 (0, 0)
- 确认 WGS84 坐标系

---

## 📝 ESP32 数据格式

```c
// 跌倒事件数据结构
typedef struct {
    char device_id[32];      // 设备编号（如 "fall_device_001"）
    char event[16];          // 事件类型（"fall"）
    char timestamp[32];      // 发生时间（如 "2026-07-10 12:30:00"）
    float latitude;          // 纬度（WGS84）
    float longitude;         // 经度（WGS84）
    char coordinate[16];     // 坐标系（"WGS84"）
    float angle;             // 倾斜角（度）
    float acceleration;       // 加速度（g）
} fall_event_t;
```

## 🔗 快速链接

- API 健康检查: https://api.lele1129.top/api/health
- GIS 地图: https://gis.lele1129.top
- API 文档: https://api.lele1129.top/api/fall (GET)

---

> 💡 **提示：** 部署完成后，每次修改代码只需要：
> ```bash
> git add .
> git commit -m "修改说明"
> git push
> ```
> Cloudflare 会自动重新部署！