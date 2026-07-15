/**
 * Cloudflare Worker - 跌倒检测系统（API + GIS 前端）
 * 
 * 部署到 Cloudflare Workers
 * 使用 Worksers KV 存储数据（可选）
 * 
 * API:  /api/fall (GET/POST)
 * GIS:  /
 */

const corsHeaders = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400',
};

let fallRecords = [];

function getGisHtml() {
return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>跌倒检测GIS地图</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"><\/script>
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
<h1>&#128680; 跌倒检测GIS地图</h1>
<div class="sub">WGS84 &middot; 实时监控</div>
</div>
<div class="ctrl">
<span id="si"><span class="dot off" id="sd"></span><span id="st">检查中...</span></span>
<span class="badge" id="rc">0条</span>
<button onclick="rd()">&#128260; 刷新</button>
</div>
</div>
<div id="map"></div>
<script>
var map=L.map('map',{center:[39.9042,116.4074],zoom:13,zoomControl:true});
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{attribution:'&copy; OSM',maxZoom:19}).addTo(map);
var fi=L.divIcon({className:'fi pulse',iconSize:[14,14],iconAnchor:[7,7]});
var mks=[];
function rd(){
var sd=document.getElementById('sd'),st=document.getElementById('st'),rc=document.getElementById('rc');
sd.className='dot off';st.textContent='加载中...';
fetch('/api/fall').then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json()}).then(function(ds){
sd.className='dot on';st.textContent='已连接';rc.textContent=ds.length+'条';
mks.forEach(function(m){map.removeLayer(m)});mks=[];
if(!ds.length){return}
var bds=[];
ds.forEach(function(d,i){
var lat=parseFloat(d.lat),lng=parseFloat(d.lng);
if(isNaN(lat)||isNaN(lng)||(lat===0&&lng===0))return;
var pop='<div class="fall-popup"><h3>&#128680; 跌倒报警</h3><table>'+
'<tr><td>设备:</td><td>'+(d.device_id||'未知')+'</td></tr>'+
'<tr><td>时间:</td><td>'+(d.time||'未知')+'</td></tr>'+
'<tr><td>坐标:</td><td>'+lat.toFixed(6)+', '+lng.toFixed(6)+'</td></tr>'+
'<tr><td>倾斜角:</td><td>'+(d.angle?d.angle.toFixed(1)+'&deg;':'-')+'</td></tr>'+
'<tr><td>加速度:</td><td>'+(d.acceleration?d.acceleration.toFixed(2)+'g':'-')+'</td></tr>'+
'<tr><td>状态:</td><td><span class="tag">跌倒报警</span></td></tr></table></div>';
var mk=L.marker([lat,lng],{icon:fi}).addTo(map).bindPopup(pop);
mks.push(mk);bds.push([lat,lng]);
if(i===ds.length-1)mk.openPopup();
});
if(bds.length===1)map.setView(bds[0],15);
else if(bds.length>1)map.fitBounds(bds,{padding:[50,50]});
}).catch(function(e){sd.className='dot off';st.textContent='连接失败';console.error(e)});
}
setInterval(rd,30000);
window.addEventListener('load',function(){setTimeout(rd,500)});
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

    // GET / → GIS 页面
    if (method === 'GET' && (path === '/' || path === '/index.html')) {
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
        let records = [];
        if (typeof FALL_KV !== 'undefined') {
            const list = await FALL_KV.list({ prefix: 'fall_', limit: 100 });
            for (const key of list.keys) {
                const val = await FALL_KV.get(key);
                if (val) records.push(JSON.parse(val));
            }
            records.sort((a, b) => {
                const ta = a.received_at || a.timestamp || '';
                const tb = b.received_at || b.timestamp || '';
                return tb.localeCompare(ta);
            });
        } else {
            records = fallRecords;
        }
        return new Response(JSON.stringify(records), {
            status: 200, headers: { 'Content-Type': 'application/json', ...corsHeaders },
        });
    }

    return new Response(JSON.stringify({ error: 'Not Found' }), {
        status: 404, headers: { 'Content-Type': 'application/json', ...corsHeaders },
    });
}

addEventListener('fetch', event => {
    event.respondWith(handleRequest(event.request));
});