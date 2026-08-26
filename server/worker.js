/**
 * Cloudflare Worker - 跌倒检测系统（API + GIS 前端）
 * 
 * 部署到 Cloudflare Workers
 * 使用 Workers KV 存储数据（可选）
 * 
 * API:  /api/fall (GET/POST)  /api/health (GET)
 * GIS:  /gis 或 /
 * 
 * 2026-08-24 更新：
 *  - GIS 页面增加「跌倒自动弹出报警弹窗 + 蜂鸣音 + 地图自动定位 + 记录去重」
 *  - POST 增加 timestamp→time 字段兼容（ESP32 上传 timestamp，页面读取 time）
 *  - 增加 /api/health 便于排查
 *  - 地图数据自动刷新：30 秒轮询，只增量画标记，不重置视野
 *  - 自动报警保留：后台每 5 秒轮询，仅对新跌倒记录弹窗+蜂鸣，不重绘地图/不移动视野
 */

const corsHeaders = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400',
};

let fallRecords = [];

/**
 * 获取持久化数据
 * 优先级: D1 → KV → 内存
 */
async function getRecords() {
    // 1. 尝试 D1 数据库（如果有绑定的 D1）
    if (typeof FALL_DB !== 'undefined') {
        try {
            const result = await FALL_DB.prepare(
                'SELECT * FROM falls ORDER BY id DESC LIMIT 200'
            ).all();
            if (result.results && result.results.length > 0) {
                return result.results.map(r => ({
                    device_id: r.device_id,
                    event: r.event,
                    time: r.time,
                    lat: r.lat,
                    lng: r.lng,
                    latitude: r.lat,
                    longitude: r.lng,
                    angle: r.angle,
                    acceleration: r.acceleration,
                    received_at: r.received_at
                }));
            }
        } catch (e) {
            // D1 表可能未创建，忽略
            console.error('D1 error:', e.message);
        }
    }

    // 2. 尝试 KV 存储（如果有绑定的 KV）
    if (typeof FALL_KV !== 'undefined') {
        try {
            const list = await FALL_KV.list({ prefix: 'fall_', limit: 200 });
            const records = [];
            for (const key of list.keys) {
                const val = await FALL_KV.get(key.name);
                if (val) records.push(JSON.parse(val));
            }
            records.sort((a, b) => {
                const ta = a.received_at || a.time || a.timestamp || '';
                const tb = b.received_at || b.time || b.timestamp || '';
                return tb.localeCompare(ta);
            });
            return records;
        } catch (e) {
            console.error('KV error:', e.message);
        }
    }

    // 3. 内存数据（单实例工作，多实例/热启动时丢失）
    if (fallRecords.length > 0) {
        return fallRecords;
    }

    // 4. 无数据时返回空数组
    return [];
}

/**
 * 保存一条跌倒记录
 * 优先级: D1 → KV → 内存
 */
async function saveRecord(data) {
    // 先保存到内存（快速访问）
    fallRecords.unshift(data);
    if (fallRecords.length > 500) fallRecords = fallRecords.slice(0, 500);

    // 尝试保存到 D1
    if (typeof FALL_DB !== 'undefined') {
        try {
            // 自动创建表（幂等）
            await FALL_DB.prepare(
                `CREATE TABLE IF NOT EXISTS falls (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    device_id TEXT,
                    event TEXT,
                    time TEXT,
                    lat REAL,
                    lng REAL,
                    angle REAL,
                    acceleration REAL,
                    received_at TEXT
                )`
            ).run();
            // 插入数据
            await FALL_DB.prepare(
                `INSERT INTO falls (device_id, event, time, lat, lng, angle, acceleration, received_at)
                 VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
            ).bind(
                data.device_id || '',
                data.event || '',
                data.time || data.timestamp || '',
                data.lat || 0,
                data.lng || 0,
                data.angle || 0,
                data.acceleration || 0,
                data.received_at || ''
            ).run();
            return;
        } catch (e) {
            console.error('D1 save error:', e.message);
        }
    }

    // 尝试保存到 KV
    if (typeof FALL_KV !== 'undefined') {
        try {
            const key = 'fall_' + Date.now() + '_' + Math.random().toString(36).substr(2, 6);
            await FALL_KV.put(key, JSON.stringify(data));
            return;
        } catch (e) {
            console.error('KV save error:', e.message);
        }
    }
}

function getGisHtml() {
return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>跌倒检测GIS地图</title>
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
/* ===== 全屏报警弹窗 ===== */
#alert{position:fixed;inset:0;background:rgba(179,9,9,.94);z-index:99999;display:none;align-items:center;justify-content:center;color:#fff}
#alert.show{display:flex}
#alert .box{background:#fff;border-radius:14px;padding:26px 30px;max-width:380px;width:92%;color:#333;text-align:center;box-shadow:0 10px 50px rgba(0,0,0,.5);animation:alertin .3s}
@keyframes alertin{from{transform:scale(.7);opacity:0}to{transform:scale(1);opacity:1}}
#alert h2{color:#d32f2f;font-size:24px;margin-bottom:4px}
#alert .sub{color:#999;font-size:12px;margin-bottom:10px}
#alert table{margin:8px auto 6px;font-size:13px;border-collapse:collapse}
#alert td{padding:5px 8px;text-align:left;border-bottom:1px solid #f0f0f0}
#alert td:first-child{color:#888;padding-right:14px;white-space:nowrap}
#alert .warn{font-size:40px;display:block;margin-bottom:2px}
#alert .btn{margin-top:16px;background:#d32f2f;color:#fff;border:none;padding:11px 32px;border-radius:8px;font-size:16px;cursor:pointer;transition:background .2s}
#alert .btn:hover{background:#b71c1c}
</style>
</head>
<body>
<div id="header">
<div>
<h1>&#128680; 跌倒检测GIS地图</h1>
<div class="sub">WGS84 &middot; 30秒自动刷新 &middot; 自动报警</div>
</div>
<div class="ctrl">
<span id="si"><span class="dot off" id="sd"></span><span id="st">检查中...</span></span>
<span class="badge" id="rc">0条</span>
<button onclick="askNotify()">&#128276; 通知</button>
</div>
</div>
<div id="map"></div>
<div id="lib-error" style="display:none;background:#b71c1c;color:#fff;text-align:center;padding:8px 12px;font-size:13px;z-index:1001;position:relative;">⚠️ 地图库（Leaflet）加载失败：所有 CDN 均不可达，请检查网络后刷新重试。</div>

<!-- 跌倒报警弹窗 -->
<div id="alert" role="alertdialog" aria-modal="true">
<div class="box">
<span class="warn">&#128680;</span>
<h2>跌倒报警！</h2>
<div class="sub">检测到跌倒事件，请尽快确认</div>
<table id="altb">
<tr><td>设备号</td><td id="al_dev">-</td></tr>
<tr><td>跌倒时间</td><td id="al_time">-</td></tr>
<tr><td>坐标(WGS84)</td><td id="al_latlng">-</td></tr>
<tr><td>倾斜角</td><td id="al_angle">-</td></tr>
<tr><td>加速度</td><td id="al_acc">-</td></tr>
<tr><td>状态</td><td><span style="color:#d32f2f;font-weight:600">&#128680; 跌倒报警</span></td></tr>
</table>
<button class="btn" onclick="closeAlert()">&#10004; 确认已处理</button>
</div>
</div>

<script>
var map=L.map('map',{center:[39.9042,116.4074],zoom:13,zoomControl:true});
var TPS=[{url:'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',a:'&copy; OSM'},{url:'https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png',a:'&copy; OSM &copy; CARTO'},{url:'https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}',a:'Esri'}];
var TL=null,TFC=0;
function lt(i){if(i>=TPS.length)return;if(TL)map.removeLayer(TL);TL=L.tileLayer(TPS[i].url,{attribution:TPS[i].a,maxZoom:19}).addTo(map);TFC=0;TL.on('tileerror',function(){TFC++;if(TFC>8)lt(i+1);});}
lt(0);
var fi=L.divIcon({className:'fi pulse',iconSize:[14,14],iconAnchor:[7,7]});
var mks=[];
var seen={};        // 已处理记录去重集合
var inited=false;   // 首次加载不报警
var AC=null;        // Web Audio 上下文
var beepTimer=null;

/* ===== 报警蜂鸣音（Web Audio 合成，无需音频文件） ===== */
function playAlarm(){
  if(beepTimer) return;
  if(!AC){try{AC=new (window.AudioContext||window.webkitAudioContext)();}catch(e){AC=null;}}
  if(!AC) return;
  function beep(f1,f2){
    if(!AC) return;
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
    if(n<6&&document.getElementById('alert').classList.contains('show')){
      beepTimer=setTimeout(loop,700);
    }else{beepTimer=null;}
  })();
}
function stopAlarm(){if(beepTimer){clearTimeout(beepTimer);beepTimer=null;}}

/* ===== 全屏报警弹窗 ===== */
function showAlert(d){
  var lat=parseFloat(d.lat),lng=parseFloat(d.lng);
  document.getElementById('al_dev').textContent=d.device_id||'未知';
  document.getElementById('al_time').textContent=d.time||d.timestamp||'未知';
  document.getElementById('al_latlng').textContent=(isNaN(lat)||isNaN(lng))?'-':lat.toFixed(6)+', '+lng.toFixed(6);
  document.getElementById('al_angle').textContent=(d.angle||d.angle===0)?angleStr(d.angle):'-';
  document.getElementById('al_acc').textContent=(d.acceleration||d.acceleration===0)?accStr(d.acceleration):'-';
  document.getElementById('alert').classList.add('show');
  playAlarm();
  // 浏览器通知（若已授权）
  if('Notification'in window&&Notification.permission==='granted'){
    try{new Notification('&#128680; 跌倒报警',{body:'设备:'+(d.device_id||'未知')+' 时间:'+(d.time||d.timestamp||'未知')});}catch(e){}
  }
}
function angleStr(a){a=parseFloat(a);return isNaN(a)?'-':(Math.round(a*10)/10)+'&deg;';}
function accStr(a){a=parseFloat(a);return isNaN(a)?'-':(Math.round(a*100)/100)+'g';}
function closeAlert(){
  document.getElementById('alert').classList.remove('show');
  stopAlarm();
}
function askNotify(){
  if('Notification'in window&&Notification.permission==='default'){
    Notification.requestPermission().then(function(p){
      alert(p==='granted'?'通知已开启':'未开启通知权限');
    });
  }else if('Notification'in window&&Notification.permission==='granted'){
    alert('通知已开启');
  }else{
    alert('当前浏览器不支持通知，请使用HTTPS访问');
  }
}
function keyFor(d){return (d.device_id||'')+'|'+(d.time||d.timestamp||'')+'|'+(d.received_at||'');}

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
    '<tr><td>时间:</td><td>'+(d.time||d.timestamp||'未知')+'</td></tr>'+
    '<tr><td>坐标:</td><td>'+lat.toFixed(6)+', '+lng.toFixed(6)+'</td></tr>'+
    '<tr><td>倾斜角:</td><td>'+angleStr(d.angle)+'</td></tr>'+
    '<tr><td>加速度:</td><td>'+accStr(d.acceleration)+'</td></tr>'+
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
window.addEventListener('load',function(){setTimeout(rd,300)});
setInterval(pal,5000);   // 5 秒报警轮询（报警 + 增量标记）
setInterval(rd,30000);   // 30 秒自动刷新（只增量刷新标记）
<\/script>
</body>
</html>`;
}

async function handleRequest(request) {
    const url = new URL(request.url);
    const path = url.pathname;
    const method = request.method;

    if (method === 'OPTIONS') {
        return new Response(null, { headers: corsHeaders, status: 204 });
    }

    // GET / 、/index.html 、/gis 、/gis.html → GIS 页面
    if (method === 'GET' && (path === '/' || path === '/index.html' || path === '/gis' || path === '/gis.html')) {
        return new Response(getGisHtml(), {
            status: 200,
            headers: { 'Content-Type': 'text/html; charset=utf-8' },
        });
    }

    // POST /api/fall → 接收跌倒事件
    if (method === 'POST' && path === '/api/fall') {
        try {
            const data = await request.json();
            if (!data.event || data.event !== 'fall') {
                return new Response(JSON.stringify({ error: 'Invalid event type' }), {
                    status: 400, headers: { 'Content-Type': 'application/json', ...corsHeaders },
                });
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

            if (typeof FALL_KV !== 'undefined') {
                const key = 'fall_' + Date.now() + '_' + Math.random().toString(36).substr(2, 6);
                await FALL_KV.put(key, JSON.stringify(data));
            }
            fallRecords.unshift(data);
            if (fallRecords.length > 200) fallRecords = fallRecords.slice(0, 200);

            return new Response(JSON.stringify({
                status: 'ok', message: 'Fall event received',
                device_id: data.device_id || 'unknown',
            }), { status: 200, headers: { 'Content-Type': 'application/json', ...corsHeaders } });
        } catch (e) {
            return new Response(JSON.stringify({ error: 'Invalid JSON: ' + e.message }), {
                status: 400, headers: { 'Content-Type': 'application/json', ...corsHeaders },
            });
        }
    }

    // GET /api/fall → 查询所有记录
    if (method === 'GET' && path === '/api/fall') {
        const records = await getRecords();
        return new Response(JSON.stringify(records), {
            status: 200, headers: { 'Content-Type': 'application/json', ...corsHeaders },
        });
    }

    // GET /api/health → 健康检查（排查用）
    if (method === 'GET' && path === '/api/health') {
        const records = await getRecords();
        return new Response(JSON.stringify({
            status: 'ok',
            count: records.length,
            kv: (typeof FALL_KV !== 'undefined') ? 'bound' : 'not-bound',
            timestamp: new Date().toISOString(),
        }), { status: 200, headers: { 'Content-Type': 'application/json', ...corsHeaders } });
    }

    return new Response(JSON.stringify({ error: 'Not Found' }), {
        status: 404, headers: { 'Content-Type': 'application/json', ...corsHeaders },
    });
}

addEventListener('fetch', event => {
    event.respondWith(handleRequest(event.request));
});