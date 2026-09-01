/**
 * 本地测试服务器 - 跌倒检测系统
 * 
 * 在开发电脑上运行，ESP32 上传数据到该服务器进行链路验证。
 * 与 Cloudflare Worker 保持 API 兼容。
 * 
 * 使用:
 *   node server_local.js
 * 
 * ESP32 配置:
 *   wifi_upload.h 中 UPLOAD_SERVER_URL = "http://192.168.3.31:3000"
 * 
 * 浏览器访问:
 *   http://192.168.3.31:3000   → GIS 地图页面
 *   http://192.168.3.31:3000/api/fall  → GET/POST API
 */

const http = require('http');
const url = require('url');
const fs = require('fs');
const path = require('path');

// ============================================================
// 配置
// ============================================================
const PORT = 3000;
const HOST = '0.0.0.0'; // 监听所有网卡，局域网可访问

// 获取本机局域网 IP（用于控制台提示）
function getLocalIP() {
    const os = require('os');
    const nets = os.networkInterfaces();
    for (const name of Object.keys(nets)) {
        for (const net of nets[name]) {
            if (net.family === 'IPv4' && !net.internal && net.address.startsWith('192.168.')) {
                return net.address;
            }
        }
    }
    return 'localhost';
}

// ============================================================
// 数据存储（内存）
// ============================================================
let fallRecords = [];

// ============================================================
// CORS 头
// ============================================================
const corsHeaders = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400',
};

// ============================================================
// GIS 页面 HTML（与 worker.js 中一致）
// ============================================================
function getGisHtml() {
return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>跌倒检测GIS地图 - 本地测试</title>
<!-- Leaflet 多 CDN 兜底加载（unpkg → cdnjs → jsdelivr → bootcdn → staticfile） -->
<script>
/* Leaflet JS/CSS 多 CDN 兜底加载器：当前 CDN 失败时自动尝试下一个 */
(function () {
var CSS_URLS = [
'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css',
'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css',
'https://cdn.jsdelivr.net/npm/leaflet@1.9.4/dist/leaflet.min.css',
'https://cdn.bootcdn.net/ajax/libs/leaflet/1.9.4/leaflet.min.css',
'https://cdn.staticfile.org/leaflet/1.9.4/leaflet.min.css'
];
var JS_URLS = [
'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js',
'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.js',
'https://cdn.jsdelivr.net/npm/leaflet@1.9.4/dist/leaflet.min.js',
'https://cdn.bootcdn.net/ajax/libs/leaflet/1.9.4/leaflet.min.js',
'https://cdn.staticfile.org/leaflet/1.9.4/leaflet.min.js'
];
function showLibError() {
var b = document.getElementById('lib-error');
if (b) b.style.display = 'block';
}
function loadCss(i) {
if (i >= CSS_URLS.length) return;
var l = document.createElement('link');
l.rel = 'stylesheet';
l.href = CSS_URLS[i];
l.onerror = function () { loadCss(i + 1); };
document.head.appendChild(l);
}
function loadJs(i) {
if (window.L) return;
if (i >= JS_URLS.length) { showLibError(); return; }
var s = document.createElement('script');
s.src = JS_URLS[i];
s.onload = function () { if (!window.L) loadJs(i + 1); };
s.onerror = function () { loadJs(i + 1); };
document.head.appendChild(s);
}
loadCss(0);
loadJs(0);
setTimeout(function () { if (!window.L) showLibError(); }, 8000);
})();
<\/script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif}
#header{background:linear-gradient(135deg,#1a237e,#283593);color:#fff;padding:14px 20px;display:flex;align-items:center;justify-content:space-between;box-shadow:0 2px 8px rgba(0,0,0,.2);position:relative;z-index:1000}
#header h1{font-size:18px;font-weight:600}
#header .sub{font-size:12px;opacity:.8;margin-top:2px}
#header .badge{background:#ff5252;padding:3px 10px;border-radius:20px;font-size:12px;font-weight:500}
#header .ctrl{display:flex;align-items:center;gap:10px}
#header button{background:rgba(255,255,255,.15);color:#fff;border:1px solid rgba(255,255,255,.3);padding:5px 14px;border-radius:4px;cursor:pointer;font-size:12px}
#header button:hover{background:rgba(255,255,255,.25)}
#header .dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}
.dot.on{background:#4caf50}.dot.off{background:#ff5252}
#map{height:calc(100vh - 52px);width:100%}
.fall-popup .leaflet-popup-content-wrapper{border-radius:8px}
.fall-popup .leaflet-popup-content{margin:10px 14px;min-width:210px}
.fall-popup h3{color:#d32f2f;font-size:15px;margin-bottom:6px}
.fall-popup td{padding:3px 0;font-size:12px;color:#333}
.fall-popup td:first-child{color:#666;width:65px;padding-right:6px}
.fall-popup .tag{display:inline-block;background:#ffebee;color:#d32f2f;padding:1px 6px;border-radius:3px;font-size:11px;font-weight:500}
.fi{background:#ff5252;border:3px solid #fff;border-radius:50%;box-shadow:0 2px 8px rgba(255,82,82,.4);width:14px!important;height:14px!important;margin-left:-7px!important;margin-top:-7px!important}
.fi.pulse{animation:pulse 1.5s infinite}
@keyframes pulse{0%{box-shadow:0 0 0 0 rgba(255,82,82,.7)}70%{box-shadow:0 0 0 10px rgba(255,82,82,0)}100%{box-shadow:0 0 0 0 rgba(255,82,82,0)}}
</style>
</head>
<body>
<div id="header">
<div>
<h1>&#128680; 跌倒检测GIS地图 <span style="font-size:11px;opacity:0.6">(本地测试)</span></h1>
<div class="sub">WGS84 &middot; 30秒自动刷新 &middot; 自动报警</div>
</div>
<div class="ctrl">
<span id="si"><span class="dot off" id="sd"></span><span id="st">检查中...</span></span>
<span class="badge" id="rc">0条</span>
<button id="layerBtn" onclick="toggleLayer()">🗺 矢量</button>
</div>
</div>
<div id="map"></div>
<div id="lib-error" style="display:none;background:#b71c1c;color:#fff;text-align:center;padding:8px 12px;font-size:13px;z-index:1001;position:relative;">⚠️ 地图库（Leaflet）加载失败：所有 CDN 均不可达，请检查网络后刷新重试。</div>
<!-- 跌倒报警弹窗 -->
<div id="alert" role="alertdialog" aria-modal="true" style="position:fixed;inset:0;background:rgba(179,9,9,.94);z-index:99999;display:none;align-items:center;justify-content:center;color:#fff">
<div style="background:#fff;border-radius:14px;padding:26px 30px;max-width:380px;width:92%;color:#333;text-align:center;box-shadow:0 10px 50px rgba(0,0,0,.5)">
<span style="font-size:40px">&#128680;</span>
<h2 style="color:#d32f2f;font-size:24px;margin:6px 0">跌倒报警！</h2>
<div style="color:#999;font-size:12px;margin-bottom:10px">检测到跌倒事件，请尽快确认</div>
<table style="margin:8px auto 6px;font-size:13px;border-collapse:collapse">
<tr><td style="padding:5px 8px;text-align:left;color:#888">设备号</td><td style="padding:5px 8px;text-align:left" id="al_dev">-</td></tr>
<tr><td style="padding:5px 8px;text-align:left;color:#888">跌倒时间</td><td style="padding:5px 8px;text-align:left" id="al_time">-</td></tr>
<tr><td style="padding:5px 8px;text-align:left;color:#888">坐标</td><td style="padding:5px 8px;text-align:left" id="al_latlng">-</td></tr>
<tr><td style="padding:5px 8px;text-align:left;color:#888">倾斜角</td><td style="padding:5px 8px;text-align:left" id="al_angle">-</td></tr>
<tr><td style="padding:5px 8px;text-align:left;color:#888">加速度</td><td style="padding:5px 8px;text-align:left" id="al_acc">-</td></tr>
</table>
<button onclick="ca()" style="margin-top:14px;background:#d32f2f;color:#fff;border:none;padding:11px 32px;border-radius:8px;font-size:16px;cursor:pointer">&#10004; 确认已处理</button>
</div>
</div>
<script>
var map=L.map('map',{center:[39.9042,116.4074],zoom:13,zoomControl:true});
var TPS=[{url:'https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}',a:'&copy; Esri'},{url:'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',a:'&copy; OSM'}];
/* 天地图（Tianditu）瓦片：矢量(vec_w+cva_w) / 卫星(img_w+cia_w)；域名需在天地图控制台白名单添加 lele1129.top 等 */
var TDT_KEY='d31a485ae08767b84f5f859987cf74bd';var TD_SUB='01234567';
var TL=null,TFC=0,tdGroup=null,tdMode='vec',tdFail=0;
function updLayerBtn(){var b=document.getElementById('layerBtn');if(!b)return;b.textContent=(tdMode==='vec')?'🗺 矢量':'🛰 卫星';}
function loadFallback(i){if(i>=TPS.length)return;if(tdGroup){map.removeLayer(tdGroup);tdGroup=null;}if(TL)map.removeLayer(TL);TL=L.tileLayer(TPS[i].url,{attribution:TPS[i].a,maxZoom:19}).addTo(map);TFC=0;TL.on('tileerror',function(){TFC++;if(TFC>8)loadFallback(i+1);});updLayerBtn();}
/* 切换天地图图层：只替换瓦片图层，绝不清空跌倒报警标记 */
function setLayerMode(mode){tdMode=mode;if(TL){map.removeLayer(TL);TL=null;}if(tdGroup){map.removeLayer(tdGroup);tdGroup=null;}var types=(mode==='sat')?['img_w','cia_w']:['vec_w','cva_w'];var ls=types.map(function(t){return L.tileLayer('https://t{s}.tianditu.gov.cn/DataServer?T='+t+'&tk='+TDT_KEY+'&x={x}&y={y}&l={z}',{subdomains:TD_SUB,attribution:'&copy; 天地图',maxZoom:18});});tdGroup=L.layerGroup(ls).addTo(map);tdFail=0;ls.forEach(function(tl){tl.on('tileerror',function(){tdFail++;if(tdFail>8)loadFallback(0);});});updLayerBtn();}
function toggleLayer(){setLayerMode(tdMode==='vec'?'sat':'vec');}
setLayerMode('vec');
var fi=L.divIcon({className:'fi pulse',iconSize:[14,14],iconAnchor:[7,7]});
var mks=[];
var seen={},inited=false;   // 已处理记录去重 + 首次加载不报警
var AC=null,beepTimer=null; // Web Audio 报警音
/* ===== 增量标记：只补画新标记，不删旧标记、不重置视野 ===== */
var mkd={},vset=false;
function amk(ds){
  ds.forEach(function(d){
    var k=keyFor(d);
    if(mkd[k])return;
    mkd[k]=1;
    var lat=parseFloat(d.lat),lng=parseFloat(d.lng);
    if(isNaN(lat)||isNaN(lng)||(lat===0&&lng===0))return;
    var pop='<div class="fall-popup"><h3>&#128680; 跌倒报警</h3><table>'+
    '<tr><td>设备:</td><td>'+(d.device_id||'未知')+'</td></tr>'+
    '<tr><td>时间:</td><td>'+(d.time||'未知')+'</td></tr>'+
    '<tr><td>坐标:</td><td>'+lat.toFixed(6)+', '+lng.toFixed(6)+'</td></tr>'+
    '<tr><td>倾斜角:</td><td>'+(d.angle?d.angle.toFixed(1)+'&deg;':'-')+'</td></tr>'+
    '<tr><td>加速度:</td><td>'+(d.acceleration?d.acceleration.toFixed(2)+'g':'-')+'</td></tr>'+
    '<tr><td>状态:</td><td><span class="tag">跌倒报警</span></td></tr></table></div>';
    var mk=L.marker([lat,lng],{icon:fi}).addTo(map).bindPopup(pop,{autoPan:false});
    mks.push(mk);
  });
  if(!vset&&mks.length>0){
    if(mks.length===1)map.setView(mks[0].getLatLng(),15);
    else map.fitBounds(L.latLngBounds(mks.map(function(m){return m.getLatLng();})),{padding:[50,50]});
    vset=true;
  }
}

/* ===== 自动刷新数据（页面加载 + 每 30 秒，只增量画标记） ===== */
function rd(){
var sd=document.getElementById('sd'),st=document.getElementById('st'),rc=document.getElementById('rc');
sd.className='dot off';st.textContent='加载中...';
fetch('/api/fall').then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json()}).then(function(ds){
sd.className='dot on';st.textContent='已连接';rc.textContent=ds.length+'条';
var fresh=[];
ds.forEach(function(d){var k=keyFor(d);if(!seen[k]){seen[k]=1;fresh.push(d);}});
amk(ds);
if(!inited){inited=true;}
else if(fresh.length>0){showAlert(fresh[fresh.length-1]);}
}).catch(function(e){sd.className='dot off';st.textContent='连接失败';console.error(e)});
}
function keyFor(d){return (d.device_id||'')+'|'+(d.time||d.timestamp||'')+'|'+(d.received_at||'');}
function angleStr(a){a=parseFloat(a);return isNaN(a)?'-':(Math.round(a*10)/10)+'&deg;';}
function accStr(a){a=parseFloat(a);return isNaN(a)?'-':(Math.round(a*100)/100)+'g';}
/* ===== 全屏报警弹窗 ===== */
function showAlert(d){
  var lat=parseFloat(d.lat),lng=parseFloat(d.lng);
  document.getElementById('al_dev').textContent=d.device_id||'未知';
  document.getElementById('al_time').textContent=d.time||d.timestamp||'未知';
  document.getElementById('al_latlng').textContent=(isNaN(lat)||isNaN(lng))?'-':lat.toFixed(6)+', '+lng.toFixed(6);
  document.getElementById('al_angle').textContent=(d.angle||d.angle===0)?angleStr(d.angle):'-';
  document.getElementById('al_acc').textContent=(d.acceleration||d.acceleration===0)?accStr(d.acceleration):'-';
  document.getElementById('alert').style.display='flex';
  playAlarm();
}
function ca(){document.getElementById('alert').style.display='none';stopAlarm();}
/* ===== 报警蜂鸣音（Web Audio 合成，无需音频文件） ===== */
function playAlarm(){
  if(beepTimer)return;
  if(!AC){try{AC=new (window.AudioContext||window.webkitAudioContext)();}catch(e){AC=null;}}
  if(!AC)return;
  function beep(f1,f2){
    if(!AC)return;
    var o1=AC.createOscillator(),g1=AC.createGain();
    o1.type='square';o1.frequency.value=f1;
    g1.gain.setValueAtTime(0.18,AC.currentTime);
    g1.gain.exponentialRampToValueAtTime(0.001,AC.currentTime+0.22);
    o1.connect(g1);g1.connect(AC.destination);o1.start();o1.stop(AC.currentTime+0.22);
    var o2=AC.createOscillator(),g2=AC.createGain();
    o2.type='square';o2.frequency.value=f2;
    g2.gain.setValueAtTime(0.18,AC.currentTime+0.28);
    g2.gain.exponentialRampToValueAtTime(0.001,AC.currentTime+0.5);
    o2.connect(g2);g2.connect(AC.destination);o2.start(AC.currentTime+0.28);o2.stop(AC.currentTime+0.5);
  }
  var n=0;
  (function loop(){
    beep(880,660);n++;
    if(n<6&&document.getElementById('alert').style.display==='flex'){beepTimer=setTimeout(loop,700);}
    else{beepTimer=null;}
  })();
}
function stopAlarm(){if(beepTimer){clearTimeout(beepTimer);beepTimer=null;}}
/* ===== 自动报警（5 秒轮询：检测新跌倒 → 报警 + 增量画标记，不重置视野） ===== */
function pal(){
  fetch('/api/fall').then(function(r){if(!r.ok)throw 0;return r.json()}).then(function(ds){
    var fresh=[];
    ds.forEach(function(d){var k=keyFor(d);if(!seen[k]){seen[k]=1;fresh.push(d);}});
    amk(ds);
    if(!inited){inited=true;return;}
    if(fresh.length>0){showAlert(fresh[fresh.length-1]);}
  }).catch(function(){});
}
window.addEventListener('load',function(){setTimeout(rd,500)});
setInterval(pal,5000);   // 5 秒报警轮询（报警 + 增量标记）
setInterval(rd,30000);   // 30 秒自动刷新（只增量刷新标记）
<\/script>
</body>
</html>`;
}

// ============================================================
// HTTP 请求处理
// ============================================================
function handleRequest(req, res) {
    const parsedUrl = url.parse(req.url, true);
    const pathname = parsedUrl.pathname;
    const method = req.method.toUpperCase();

    // 打印请求日志
    console.log(`[${new Date().toLocaleTimeString()}] ${method} ${pathname}`);

    // CORS 预检
    if (method === 'OPTIONS') {
        res.writeHead(204, corsHeaders);
        res.end();
        return;
    }

    // GET / → GIS 页面
    if (method === 'GET' && pathname === '/') {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(getGisHtml());
        return;
    }

    // GET /api/fall → 查询所有记录
    if (method === 'GET' && pathname === '/api/fall') {
        res.writeHead(200, { 'Content-Type': 'application/json', ...corsHeaders });
        res.end(JSON.stringify(fallRecords));
        console.log(`  返回 ${fallRecords.length} 条记录`);
        return;
    }

    // POST /api/fall → 接收跌倒事件
    if (method === 'POST' && pathname === '/api/fall') {
        let body = '';
        req.on('data', chunk => { body += chunk; });
        req.on('end', () => {
            try {
                const data = JSON.parse(body);
                if (!data.event || data.event !== 'fall') {
                    res.writeHead(400, { 'Content-Type': 'application/json', ...corsHeaders });
                    res.end(JSON.stringify({ error: 'Invalid event type' }));
                    return;
                }
                // 规范化字段：兼容 ESP32 上传的 latitude/longitude 与 lat/lng 两种命名
                if (data.lat === undefined && data.latitude !== undefined) data.lat = data.latitude;
                if (data.lng === undefined && data.longitude !== undefined) data.lng = data.longitude;
                if (data.latitude === undefined && data.lat !== undefined) data.latitude = data.lat;
                if (data.longitude === undefined && data.lng !== undefined) data.longitude = data.lng;
                // 兼容 time/timestamp 两种字段（ESP32 上传 timestamp，GIS 页面读取 time）
                if (data.time === undefined && data.timestamp !== undefined) data.time = data.timestamp;
                if (data.timestamp === undefined && data.time !== undefined) data.timestamp = data.time;
                data.received_at = new Date().toISOString();
                fallRecords.unshift(data);
                if (fallRecords.length > 200) fallRecords = fallRecords.slice(0, 200);

                console.log(`  ✓ 收到跌倒事件 | 设备: ${data.device_id || 'unknown'} | 位置: ${data.latitude || '?'}, ${data.longitude || '?'}`);

                res.writeHead(200, { 'Content-Type': 'application/json', ...corsHeaders });
                res.end(JSON.stringify({
                    status: 'ok',
                    message: 'Fall event received',
                    device_id: data.device_id || 'unknown',
                }));
            } catch (e) {
                res.writeHead(400, { 'Content-Type': 'application/json', ...corsHeaders });
                res.end(JSON.stringify({ error: 'Invalid JSON: ' + e.message }));
            }
        });
        return;
    }

    // 404
    res.writeHead(404, { 'Content-Type': 'application/json', ...corsHeaders });
    res.end(JSON.stringify({ error: 'Not Found' }));
}

// ============================================================
// 启动服务器
// ============================================================
const localIP = getLocalIP();
const server = http.createServer(handleRequest);
server.listen(PORT, HOST, () => {
    console.log('');
    console.log('╔══════════════════════════════════════════════════════╗');
    console.log('║       跌倒检测系统 - 本地测试服务器                  ║');
    console.log('╠══════════════════════════════════════════════════════╣');
    console.log(`║  本机地址:  http://${localIP}:${PORT}                   ║`);
    console.log(`║  本机地址:  http://127.0.0.1:${PORT}                   ║`);
    console.log('╠══════════════════════════════════════════════════════╣');
    console.log('║  GIS地图:   http://' + localIP + `:${PORT}/              ║`);
    console.log('║  POST API:  http://' + localIP + `:${PORT}/api/fall      ║`);
    console.log('║  GET  API:  http://' + localIP + `:${PORT}/api/fall      ║`);
    console.log('╠══════════════════════════════════════════════════════╣');
    console.log('║  ESP32 配置:                                        ║');
    console.log(`║  UPLOAD_SERVER_URL = "http://${localIP}:${PORT}"               ║`);
    console.log('╚══════════════════════════════════════════════════════╝');
    console.log('');
    console.log('按 Ctrl+C 停止服务器');
    console.log('');
});