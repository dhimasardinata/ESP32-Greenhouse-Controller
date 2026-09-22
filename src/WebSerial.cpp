#include "WebSerial.h"
#include "ConfigManager.h"
#include "CryptoUtils.h"
#include "EmbeddedCryptoJs.h"

AsyncWebSocket *WebSerialClass::_ws = nullptr;
WebSerialCallback WebSerialClass::_messageCallback = nullptr;

namespace {
bool sendEncryptedWebSerialFrame(AsyncWebSocket* ws, const String& plain)
{
    if (!ws || ws->count() == 0)
        return false;
    String encrypted;
    if (!CryptoUtils::encryptNodeMediniPayload(plain.c_str(), plain.length(), encrypted))
        return false;
    ws->textAll(encrypted);
    return true;
}
}

const char UNIFIED_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>ESP32 Manager</title>
    <style>
        :root{--bg:#121212;--panel:#1e1e1e;--panel-alt:#252a33;--text:#e0e0e0;--muted:#aaa;--soft:#888;--accent:#03dac6;--accent-strong:#00bcd4;--border:#333;--line:#444;--row-border:#2a2a2a;--ok:#00e676;--warn:#ffab40;--err:#ff5252;--card-title:#bb86fc;--shadow:0 4px 10px rgba(0,0,0,.5);--shadow-soft:0 4px 6px rgba(0,0,0,.3);--nav-active:#333;--input-bg:#333;--input-border:#555;--input-text:#fff;--input-disabled-bg:#222;--input-disabled-text:#666;--input-disabled-border:#333;--term-bg:#000;--term-text:#e0e0e0;--cmd-text:#0f0;--wifi-log-bg:#111;--wifi-log-border:#3a3a3a;--btn-primary-text:#000;--btn-secondary:#2196f3;--btn-secondary-hover:#1976d2;--btn-secondary-text:#fff;--btn-danger:#f44336;--btn-danger-text:#fff;--toggle-off:#444;--toggle-thumb:#fff}
        body[data-theme="light"]{color-scheme:light;--bg:#edf2f7;--panel:#fff;--panel-alt:#f7fafc;--text:#13202b;--muted:#5f6b76;--soft:#73808c;--accent:#0f8b8d;--accent-strong:#16697a;--border:#d5dee6;--line:#c7d2dc;--row-border:#e2e8f0;--card-title:#1d6fd6;--shadow:0 8px 24px rgba(15,23,42,.08);--shadow-soft:0 6px 18px rgba(15,23,42,.06);--nav-active:#dfe8ef;--input-bg:#fff;--input-border:#b8c4cf;--input-text:#13202b;--input-disabled-bg:#e9eef3;--input-disabled-text:#7b8794;--input-disabled-border:#ccd6df;--term-bg:#f3f7fb;--term-text:#13202b;--cmd-text:#16697a;--wifi-log-bg:#f7fafc;--wifi-log-border:#d5dee6;--btn-primary-text:#fff;--btn-secondary:#1d6fd6;--btn-secondary-hover:#175eb5;--btn-secondary-text:#fff;--btn-danger:#d64545;--btn-danger-text:#fff;--toggle-off:#b8c4cf;--toggle-thumb:#fff}
        body{font-family:sans-serif;background:var(--bg);color:var(--text);margin:0;display:flex;height:100vh;height:100dvh;overflow:hidden;color-scheme:dark;transition:background-color .2s,color .2s;}
        .sidebar,.card,.node-card,.alarm-card,.file-item,#term-box,input,select,.wifi-log,.slider{transition:background-color .2s,color .2s,border-color .2s,box-shadow .2s}
        .sidebar{width:60px;background:var(--panel);display:flex;flex-direction:column;align-items:center;padding-top:20px;padding-bottom:14px;border-right:1px solid var(--border);transition:width .3s;z-index:100;box-sizing:border-box}
        .sidebar:hover{width:180px}.nav-btn,.theme-btn{width:100%;padding:15px 0;background:0 0;border:none;color:var(--text);cursor:pointer;display:flex;align-items:center;overflow:hidden;white-space:nowrap;transition:.2s;font:inherit}
        .nav-btn:hover,.nav-btn.active,.theme-btn:hover{background:var(--nav-active);border-left:3px solid var(--accent)}.nav-icon,.theme-icon{min-width:60px;text-align:center;font-size:1.2em}
        .nav-label,.theme-label{margin-left:10px;font-weight:500;opacity:0;transition:opacity .2s}.sidebar:hover .nav-label,.sidebar:hover .theme-label{opacity:1}
        .theme-btn{margin-top:auto;border-top:1px solid var(--border);padding-top:18px}
        .main{flex:1;padding:20px;height:100%;overflow-y:hidden;box-sizing:border-box;display:flex;flex-direction:column;}
        .tab{display:none;flex-direction:column;height:100%}.tab.active{display:flex}
        h2{border-bottom:1px solid var(--border);padding-bottom:10px;color:var(--accent);margin:0 0 15px 0;flex-shrink:0;}
        .grid{display:grid;grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr;gap:20px;flex:1;min-height:0;padding-bottom:5px;}
        .card{background:var(--panel);border-radius:12px;padding:20px;border:1px solid var(--border);box-shadow:var(--shadow);display:flex;flex-direction:column;justify-content:space-between}
        .card h3{margin:0;color:var(--card-title);font-size:1.3em;border-bottom:1px solid var(--line);padding-bottom:10px;margin-bottom:10px}
        .row{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--row-border);padding:8px 0;font-size:1.1em;flex-grow:1}.row:last-child{border-bottom:none}
        .ok{color:var(--ok);font-weight:700}.warn{color:var(--warn);font-weight:700}.err{color:var(--err);font-weight:700}
        .cfg-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
        .cfg-item label{font-size:0.8em;color:var(--muted);display:block;margin-bottom:2px;}
        .cfg-item input{width:100%;box-sizing:border-box;background:var(--input-bg);border:1px solid var(--input-border);color:var(--input-text);padding:5px;border-radius:4px;text-align:center;}
        .cfg-item input:disabled{background:var(--input-disabled-bg);color:var(--input-disabled-text);border-color:var(--input-disabled-border);cursor:not-allowed;}
        
        /* Node Card Updated */
        .node-grid{display:grid;grid-template-columns:repeat(auto-fill, minmax(240px, 1fr));gap:15px;overflow-y:auto;padding-bottom:20px;}
        .node-card{background:var(--panel-alt);border:1px solid var(--border);border-radius:10px;padding:15px;position:relative;}
        .node-id{font-size:1.2em;font-weight:bold;color:var(--accent);margin-bottom:10px;display:flex;justify-content:space-between;}
        .node-stat{font-size:0.9em;color:var(--muted);margin:4px 0;display:flex;justify-content:space-between;}
        .node-stat b{color:var(--text);}
        .node-detail{font-size:0.75em;color:var(--soft);border-top:1px solid var(--line);margin-top:8px;padding-top:8px;}
        .node-detail div {display:flex; justify-content:space-between; margin-bottom:2px;}
        .dot{height:8px;width:8px;border-radius:50%;display:inline-block;background-color:var(--ok);}

        .alarm-list{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:15px;max-width:800px;margin:0 auto;width:100%;padding-right:5px}
        .alarm-card{background:var(--panel-alt);border-radius:12px;padding:15px;border:1px solid var(--border);box-shadow:var(--shadow-soft);flex-shrink:0}
        .alarm-head{display:flex;justify-content:space-between;align-items:center;cursor:pointer}.time-txt{font-size:1.8em;color:var(--text)}.meta-txt{font-size:.9em;color:var(--muted)}
        .alarm-body{display:none;margin-top:15px;padding-top:15px;border-top:1px solid var(--line)}
        .alarm-body.show{display:block;animation:slideDown .3s}
        @keyframes slideDown{from{opacity:0;transform:translateY(-10px)}to{opacity:1;transform:translateY(0)}}
        .toggle{position:relative;width:45px;height:26px}.toggle input{opacity:0;width:0;height:0}.slider{position:absolute;top:0;left:0;right:0;bottom:0;background:var(--toggle-off);border-radius:26px;transition:.4s}
        .slider:before{position:absolute;content:"";height:20px;width:20px;left:3px;bottom:3px;background:var(--toggle-thumb);border-radius:50%;transition:.4s}
        input:checked+.slider{background:var(--accent)}input:checked+.slider:before{transform:translateX(19px)}
        .form-row{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}input,select{background:var(--input-bg);border:1px solid var(--input-border);color:var(--input-text);padding:8px;border-radius:5px}
        .btn{width:100%;background:var(--accent);color:var(--btn-primary-text);border:none;padding:12px;border-radius:5px;font-weight:700;cursor:pointer;margin-top:10px}
        .btn:disabled{background:var(--nav-active);color:var(--soft);cursor:not-allowed;}
        .wifi-log{margin-top:10px;background:var(--wifi-log-bg);border:1px solid var(--wifi-log-border);border-radius:6px;padding:8px;max-height:160px;overflow-y:auto;font-family:'Courier New',Consolas,monospace;font-size:12px;line-height:1.4}
        .wifi-line{margin:0 0 4px 0;color:var(--muted)}
        .wifi-line.ok{color:#00e676}
        .wifi-line.warn{color:#ffb74d}
        .wifi-line.err{color:#ff6e6e}
        
        /* New File List Styles */
        .file-list { display: flex; flex-direction: column; gap: 10px; overflow-y: auto; }
        .file-item { background: var(--panel-alt); padding: 15px; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; border: 1px solid var(--line); }
        .file-name { font-weight: bold; color: var(--text); }
        .file-actions a { text-decoration: none; }
        .btn-dl { background: var(--btn-secondary); color: var(--btn-secondary-text); padding: 8px 15px; border-radius: 4px; border: none; font-size: 0.9em; cursor: pointer; }
        .btn-dl:hover { background: var(--btn-secondary-hover); }

        #term-box{flex:1;background:var(--term-bg);border:1px solid var(--line);padding:10px;font-family:'Courier New',Consolas,monospace;font-size:13px;line-height:1.4;overflow-y:auto;margin-bottom:10px;border-radius:5px;white-space:pre-wrap;min-height:0;color:var(--term-text);}
        .msg-tx{color:var(--accent-strong)}.msg-rx{color:var(--term-text)}
        .input-bar{display:flex;gap:10px;flex-shrink:0;padding-bottom:5px;}
        #cmd-in{flex:1;background:var(--input-bg);border:1px solid var(--line);color:var(--cmd-text);padding:10px;font-family:monospace}.btn-send{width:auto;margin:0;background:var(--btn-secondary);color:var(--btn-secondary-text)}.btn-cls{width:auto;margin:0;background:var(--btn-danger);color:var(--btn-danger-text)}
        .disabled-overlay{opacity:0.5;pointer-events:none;}
        @media(max-width:800px){.main{padding:10px;}.grid{display:block;overflow-y:auto}.card{margin-bottom:15px;min-height:250px}.sidebar{width:50px}.nav-label,.theme-label{display:none}.input-bar{gap:5px;}.btn-send,.btn-cls{padding:10px;font-size:0.9em;}}
    </style>
</head>
<body>
    <nav class="sidebar">
        <button class="nav-btn active" onclick="tab('dash')"><span class="nav-icon">&#128202;</span><span class="nav-label">Dash</span></button>
        <button class="nav-btn" onclick="tab('nodes')"><span class="nav-icon">&#128246;</span><span class="nav-label">Nodes</span></button>
        <button class="nav-btn" onclick="tab('sched')"><span class="nav-icon">&#128337;</span><span class="nav-label">Sched</span></button>
        <button class="nav-btn" onclick="tab('wifi')"><span class="nav-icon">&#128246;</span><span class="nav-label">WiFi</span></button>
        <button class="nav-btn" onclick="tab('files')"><span class="nav-icon">&#128193;</span><span class="nav-label">Files</span></button>
        <button class="nav-btn" onclick="tab('term')"><span class="nav-icon">&#128187;</span><span class="nav-label">Term</span></button>
        <button class="nav-btn" onclick="tab('ota')"><span class="nav-icon">&#128229;</span><span class="nav-label">OTA</span></button>
        <button class="theme-btn" id="theme-btn" onclick="toggleTheme()"><span class="theme-icon" id="theme-icon">&#9790;</span><span class="theme-label" id="theme-label">Dark</span></button>
    </nav>
    <div class="main">
        <div id="dash" class="tab active">
            <h2>Dashboard <span id="st-dash" class="err" style="font-size:0.6em;float:right;">Conn...</span></h2>
            <div class="grid">
                <div class="card"><h3>Info</h3><div class="row"><span>ID:</span><b id="d_id">-</b></div><div class="row"><span>FW:</span><b id="d_fw">-</b></div><div class="row"><span>Time:</span><span id="d_time">-</span></div></div>
                <div class="card"><h3>Network</h3><div class="row"><span>Signal:</span><span id="d_sig">-</span></div><div class="row"><span>IP:</span><span id="d_ip">-</span></div><div class="row"><span>SSID:</span><span id="d_ssid">-</span></div></div>
                <div class="card"><h3>Sensors</h3><div class="row"><span>Temp:</span><b id="d_t">-</b></div><div class="row"><span>Hum:</span><b id="d_h">-</b></div><div class="row"><span>Light:</span><b id="d_l">-</b></div><div class="row"><span>Mode:</span><b id="d_mode">-</b></div><div class="row" id="row_fog" style="display:none;border-top:1px solid var(--line);margin-top:5px;padding-top:5px;"><span>Fog:</span><b id="d_fog">-</b></div></div>
                <div class="card"><h3>Relays</h3><div class="row"><span>R1 (Exh):</span><b id="d_r1">-</b></div><div class="row"><span>R2 (Deh):</span><b id="d_r2">-</b></div><div class="row"><span>R3 (Blw):</span><b id="d_r3">-</b></div></div>
            </div>
        </div>
        <div id="nodes" class="tab">
            <h2>Node & Config <span id="node-count" style="font-size:0.6em;float:right;color:var(--muted)">0 Found</span></h2>
	            <div class="card" style="margin-bottom:20px;">
	                <h3>Thresholds <span style="font-size:0.6em;float:right;margin-right:12px;color:var(--muted)">View: <select id="cfg-source" onchange="setThresholdView(this.value)" style="padding:2px 6px;font-size:0.95em"><option value="runtime">Runtime</option><option value="edge">Edge</option><option value="cloud">Cloud</option></select></span><span id="cfg-mode" style="font-size:0.6em;float:right;color:var(--soft)">Mode</span></h3>
	                <div class="cfg-grid" id="cfg-grid">
	                    <div class="cfg-item"><label>Temp Min</label><input type="number" id="tm_min" step="0.1"></div>
	                    <div class="cfg-item"><label>Temp Max</label><input type="number" id="tm_max" step="0.1"></div>
	                    <div class="cfg-item"><label>Hum Min</label><input type="number" id="hm_min" step="1"></div>
	                    <div class="cfg-item"><label>Hum Max</label><input type="number" id="hm_max" step="1"></div>
                </div>
                <button class="btn" id="btn-save-cfg" onclick="saveCfg()">Save Thresholds</button>
            </div>
            <div id="node-list" class="node-grid"></div>
        </div>
        <div id="sched" class="tab"><h2>Schedules <span style="font-size:0.6em;float:right;margin-right:12px;color:var(--muted)">View: <select id="sched-source" onchange="setSchedSource(this.value)" style="padding:2px 6px;font-size:0.95em"><option value="runtime">Runtime</option><option value="edge">Edge</option><option value="cloud">Cloud</option></select></span><span id="mode-badge" style="font-size:0.6em;float:right;color:var(--soft)">Loading...</span></h2><div id="alm-list" class="alarm-list"><div style="text-align:center;color:var(--soft);">Waiting data...</div></div></div>

        <div id="wifi" class="tab">
            <h2>WiFi Portal</h2>
            <div class="card" style="max-width:680px;margin:0 auto;height:auto;">
                <h3>Ganti WiFi</h3>
                <button class="btn" onclick="scanWifi()">Scan WiFi</button>
                <div class="form-row" style="margin-top:10px;"><label>Pilih SSID</label><select id="wifi-list" onchange="pickWifi()"><option value="">-- scan dulu --</option></select></div>
                <div class="form-row"><label>SSID</label><input type="text" id="wifi-ssid" placeholder="Nama WiFi"></div>
                <div class="form-row"><label>Password</label><input type="password" id="wifi-pass" placeholder="Kosongkan jika open"></div>
                <button class="btn" onclick="changeWifi()">Simpan & Connect</button>
                <div id="wifi-status" style="margin-top:12px;color:var(--muted);font-size:0.9em;">Belum ada aksi.</div>
                <div style="margin-top:8px;color:var(--soft);font-size:0.8em;">Jika IP berubah setelah ganti WiFi, buka dashboard lewat IP baru.</div>
                <div id="wifi-log" class="wifi-log"><div class="wifi-line">[log] Menunggu aksi WiFi...</div></div>
            </div>
        </div>
        
        <!-- Files Tab -->
        <div id="files" class="tab">
            <h2>SD Card Files</h2>
            <div class="card">
                <h3>Available Logs</h3>
                <div class="file-list">
                    <div class="file-item">
                        <span class="file-name">&#128196; log.csv (Sensor Data)</span>
                        <div class="file-actions">
                            <button class="btn-dl" onclick="downloadFile('/log.csv')">Download</button>
                        </div>
                    </div>
                    <div class="file-item">
                        <span class="file-name">&#128196; qos.csv (Network QoS)</span>
                        <div class="file-actions">
                            <button class="btn-dl" onclick="downloadFile('/qos.csv')">Download</button>
                        </div>
                    </div>
                </div>
                <div style="margin-top:15px; color:var(--muted); font-size:0.8em; text-align:center;">
                    Note: Files are downloaded directly from the SD Card.<br>Large files may take some time.
                </div>
            </div>
        </div>

        <div id="term" class="tab"><h2>Web Serial <span id="st-term" class="err" style="font-size:0.6em;float:right;">Disc</span></h2><div id="term-box"></div><div class="input-bar"><input type="text" id="cmd-in" placeholder="Cmd..."><button class="btn btn-send" onclick="send()">Send</button><button class="btn btn-cls" onclick="cls()">Clear</button></div></div>
        <div id="ota" class="tab"><h2>Update</h2><div class="card" style="max-width:500px;margin:0 auto;text-align:center;height:auto;"><form id="up-form"><input type="file" id="file" required style="width:100%;margin-bottom:15px;"><input type="submit" value="Upload" class="btn"></form><div id="prog" style="background:var(--nav-active);height:10px;margin-top:10px;width:0%"></div></div></div>
    </div>
    <script src="/crypto.js"></script>
    <script>
        // Update tab function mapping to include 'wifi' and 'files'
        function tab(id){
            document.querySelectorAll('.tab,.nav-btn').forEach(e=>e.classList.remove('active'));
            document.getElementById(id).classList.add('active');
            // Mapping index sidebar buttons: dash=0, nodes=1, sched=2, wifi=3, files=4, term=5, ota=6
            const m={dash:0,nodes:1,sched:2,wifi:3,files:4,term:5,ota:6};
            document.querySelectorAll('.nav-btn')[m[id]].classList.add('active');
            if(id==='wifi'&&wifiScanList.length===0)scanWifi();
        }
        
        let wsD, schedsRuntime=[], schedsEdge=[], schedsCloud=[], thresholdsRuntime=null, thresholdsEdge=null, thresholdsCloud=null, currentMode="", currentConfiguredMode="", currentRuntimeSource="CLOUD", thresholdRuntimeSource="CLOUD", thresholdViewSource="runtime", thresholdCloudAvailable=false, scheduleRuntimeSource="CLOUD", scheduleViewSource="runtime", scheduleEdgeAvailable=false, scheduleCloudAvailable=false, canEditThresholds=false, canEditSchedules=false, cloudSyncMode="off", isEditingCfg=false, wifiScanList=[], currentGhId=0, lastClientTimePushMs=0;
        let lastSchedRenderKey="";
        let wifiSwitchInProgress=false, wifiSwitchNoticeShown=false;
        let adminToken=sessionStorage.getItem('gatewayAdminToken')||"";
        let adminTokenExpiresAt=Number(sessionStorage.getItem('gatewayAdminTokenExpiresAt')||"0");
        let pendingAdminAction=null;
        const pendingDashboardActions=new Map();
        const KNOWN_WIFI_PASS="change-me-wifi-password";
        const GATEWAY_AES_KEY="abcdefghijklmnopqrstuvwxyz123456";
        const THEME_STORAGE_KEY="gatewayThemePreference";
        let gatewayCryptoKeyPromise=null;
        let gatewayCryptoCompatKey=null;
        const fmtNum=(v,d)=>{const n=Number(v);return Number.isFinite(n)?n.toFixed(d):'-';};
        ['tm_min','tm_max','hm_min','hm_max'].forEach(id=>{const el=document.getElementById(id);el.addEventListener('focus',()=>isEditingCfg=true);el.addEventListener('blur',()=>isEditingCfg=false)});
        function getStoredThemePreference(){
            try{
                const theme=localStorage.getItem(THEME_STORAGE_KEY)||"";
                return (theme==="light"||theme==="dark")?theme:"";
            }catch(_err){
                return "";
            }
        }
        function setStoredThemePreference(theme){
            try{localStorage.setItem(THEME_STORAGE_KEY,theme)}catch(_err){}
        }
        function updateThemeToggle(theme){
            const isLight=theme==="light";
            const btn=document.getElementById('theme-btn');
            const icon=document.getElementById('theme-icon');
            const label=document.getElementById('theme-label');
            if(icon) icon.innerHTML=isLight?'&#9728;':'&#9790;';
            if(label) label.textContent=isLight?'Light':'Dark';
            if(btn){
                btn.title=`Switch to ${isLight?'dark':'light'} mode`;
                btn.setAttribute('aria-label',btn.title);
            }
        }
        function applyTheme(theme,persist){
            const nextTheme=theme==="light"?"light":"dark";
            document.body.setAttribute('data-theme',nextTheme);
            updateThemeToggle(nextTheme);
            if(persist) setStoredThemePreference(nextTheme);
        }
        function toggleTheme(){
            applyTheme(document.body.getAttribute('data-theme')==="light"?"dark":"light",true);
        }
        function initTheme(){
            const storedTheme=getStoredThemePreference();
            const media=window.matchMedia?window.matchMedia('(prefers-color-scheme: light)'):null;
            applyTheme(storedTheme||(media&&media.matches?"light":"dark"),false);
            if(!media) return;
            const syncTheme=e=>{
                if(getStoredThemePreference()) return;
                applyTheme(e.matches?"light":"dark",false);
            };
            if(media.addEventListener) media.addEventListener('change',syncTheme);
            else if(media.addListener) media.addListener(syncTheme);
        }

        function waitForDashboardAction(key,timeoutMs=6000){
            const existing=pendingDashboardActions.get(key);
            if(existing){
                clearTimeout(existing.timer);
                existing.reject(new Error('Permintaan sebelumnya digantikan.'));
                pendingDashboardActions.delete(key);
            }
            return new Promise((resolve,reject)=>{
                const timer=setTimeout(()=>{
                    pendingDashboardActions.delete(key);
                    reject(new Error('Gateway tidak memberi balasan tepat waktu.'));
                },timeoutMs);
                pendingDashboardActions.set(key,{resolve,reject,timer});
            });
        }
        function settleDashboardAction(key,payload){
            const pending=pendingDashboardActions.get(key);
            if(!pending) return false;
            clearTimeout(pending.timer);
            pendingDashboardActions.delete(key);
            pending.resolve(payload);
            return true;
        }
        function makeRequestId(prefix){
            return `${prefix||'req'}-${Date.now()}-${Math.random().toString(16).slice(2,10)}`;
        }
        function getThresholdSnapshotForView(viewKey){
            if(viewKey==="edge") return thresholdsEdge;
            if(viewKey==="cloud") return thresholdsCloud&&thresholdsCloud.valid!==false?thresholdsCloud:null;
            return thresholdsRuntime||thresholdsEdge||thresholdsCloud;
        }
        function isThresholdViewEditable(viewKey){
            if(viewKey==="edge") return canEditThresholds;
            if(viewKey==="runtime") return canEditThresholds && thresholdRuntimeSource==="EDGE";
            return false;
        }
        function refreshThresholdSourcePicker(){
            const sel=document.getElementById('cfg-source');
            if(!sel) return;
            [...sel.options].forEach(opt=>{
                if(opt.value==="cloud") opt.disabled=!thresholdCloudAvailable;
            });
            if(thresholdViewSource==="cloud"&&!thresholdCloudAvailable){
                thresholdViewSource="runtime";
            }
            sel.value=thresholdViewSource;
        }
        function setThresholdView(value){
            thresholdViewSource=value||"runtime";
            refreshThresholdSourcePicker();
            renderThresholdEditor();
        }
        function renderThresholdEditor(){
            const snapshot=getThresholdSnapshotForView(thresholdViewSource);
            const editable=isThresholdViewEditable(thresholdViewSource);
            const sourceLabel=thresholdViewSource==="edge"?"EDGE":(thresholdViewSource==="cloud"?"CLOUD":"RUNTIME");
            const runtimeLabel=thresholdRuntimeSource||"CLOUD";
            const editableLabel=(thresholdViewSource==="runtime"&&editable&&thresholdRuntimeSource==="EDGE")
                ? "(Editable via EDGE)"
                : (editable ? "(Editable)" : "(Read Only)");
            const badge=document.getElementById('cfg-mode');
            if(badge){
                badge.textContent=`RUN: ${runtimeLabel} | VIEW: ${sourceLabel} ${editableLabel}`;
                badge.style.color=editable?"#00e676":"#ffab40";
            }
            const grid=document.getElementById('cfg-grid');
            if(grid) grid.classList.toggle('disabled-overlay',!editable);
            document.querySelectorAll('.cfg-item input').forEach(i=>i.disabled=!editable);
            const btn=document.getElementById('btn-save-cfg');
            if(btn) btn.disabled=!editable;
            if(isEditingCfg || !snapshot) return;
            document.getElementById('tm_min').value=snapshot.t_min;
            document.getElementById('tm_max').value=snapshot.t_max;
            document.getElementById('hm_min').value=snapshot.h_min;
            document.getElementById('hm_max').value=snapshot.h_max;
        }
        function updateScheduleModeBadge(sourceLabelOverride){
            const sourceLabel=sourceLabelOverride||(scheduleViewSource==="edge"?"EDGE":(scheduleViewSource==="cloud"?"CLOUD":"RUNTIME"));
            const runtimeLabel=scheduleRuntimeSource||"CLOUD";
            const mb=document.getElementById('mode-badge');
            if(mb){
                mb.textContent=`RUN: ${runtimeLabel} | VIEW: ${sourceLabel}`;
                mb.style.color=(runtimeLabel==="EDGE")?"#00e676":"#ffab40";
            }
        }
        function updateDashboardModeSummary(control){
            const dm=document.getElementById('d_mode');
            if(!dm) return;
            const syncLabel=cloudSyncMode==="active"?' | Cloud Active':(cloudSyncMode==="recovery"?' | Cloud Recovery':'');
            const ctrlLabel=(control&&control.schedule_degraded)?' | Threshold Only':'';
            dm.textContent=(currentMode||'-')+syncLabel+ctrlLabel;
        }
        function applyNetworkPayload(network){
            if(!network) return;
            document.getElementById('d_sig').textContent=network.signal||'0';
            document.getElementById('d_ip').textContent=network.ip||'-';
            document.getElementById('d_ssid').textContent=network.ssid||'-';
            cloudSyncMode=network.cloud_sync_mode||((network.cloud_polling&&currentConfiguredMode!=="LOCAL")?'active':'off');
        }
        async function applyControlPayload(control){
            if(!control) return;
            if(control.time_valid===false) await pushClientTime(false);
            scheduleRuntimeSource=control.schedule_runtime_source||scheduleRuntimeSource;
            scheduleEdgeAvailable=(control.schedule_edge_available!==undefined)?!!control.schedule_edge_available:scheduleEdgeAvailable;
            scheduleCloudAvailable=(control.schedule_cloud_available!==undefined)?!!control.schedule_cloud_available:scheduleCloudAvailable;
            refreshScheduleSourcePicker();
            updateScheduleModeBadge();
        }
        function applySensorPayload(sensors, control){
            if(!sensors) return;
            document.getElementById('d_t').textContent=fmtNum(sensors.temperature,1);
            document.getElementById('d_h').textContent=fmtNum(sensors.humidity,0);
            document.getElementById('d_l').textContent=fmtNum(sensors.light,0);
            if(sensors.fog_status!==undefined){
                const r=document.getElementById('row_fog'),v=document.getElementById('d_fog');
                r.style.display='flex';
                v.textContent=sensors.fog_status?"BERKABUT":"CLEAR";
                v.className=sensors.fog_status?"warn":"ok";
            }
            currentMode=sensors.mode;
            currentConfiguredMode=sensors.mode_config||"AUTO";
            currentRuntimeSource=sensors.runtime_source||((currentMode&&currentMode.includes("LOCAL"))?"LOCAL":"CLOUD");
            thresholdRuntimeSource=sensors.threshold_runtime_source||thresholdRuntimeSource;
            canEditThresholds=(sensors.threshold_editable!==undefined)?!!sensors.threshold_editable:(currentConfiguredMode!=="CLOUD");
            canEditSchedules=(sensors.schedule_editable!==undefined)?!!sensors.schedule_editable:(currentConfiguredMode!=="CLOUD");
            refreshScheduleSourcePicker();
            refreshThresholdSourcePicker();
            renderThresholdEditor();
            updateDashboardModeSummary(control);
            updateScheduleModeBadge();
        }
        function applyThresholdPayload(payload){
            if(!(payload.thresholds_runtime||payload.thresholds||payload.thresholds_edge||payload.thresholds_cloud)) return;
            thresholdsRuntime=payload.thresholds_runtime||payload.thresholds||thresholdsRuntime;
            thresholdsEdge=payload.thresholds_edge||thresholdsEdge;
            thresholdsCloud=payload.thresholds_cloud||thresholdsCloud;
            thresholdCloudAvailable=!!(thresholdsCloud&&thresholdsCloud.valid!==false);
            refreshThresholdSourcePicker();
            renderThresholdEditor();
        }
        function applySchedulePayload(payload){
            if(!(payload.schedules_runtime||payload.schedules||payload.schedules_edge||payload.schedules_cloud)) return;
            schedsRuntime=Array.isArray(payload.schedules_runtime)?payload.schedules_runtime:(Array.isArray(payload.schedules)?payload.schedules:schedsRuntime);
            schedsEdge=Array.isArray(payload.schedules_edge)?payload.schedules_edge:schedsEdge;
            schedsCloud=Array.isArray(payload.schedules_cloud)?payload.schedules_cloud:schedsCloud;
            refreshScheduleSourcePicker();
            renderSched();
        }
        function applyRelayPayload(relays){
            if(!relays) return;
            const sr=(id,s)=>{const el=document.getElementById(id);el.textContent=s?'ON':'OFF';el.className=s?'ok':'warn'};
            sr('d_r1',relays.exhaust);
            sr('d_r2',relays.dehumidifier);
            sr('d_r3',relays.blower);
        }
        function getScheduleListForView(viewKey){
            if(viewKey==="edge") return schedsEdge;
            if(viewKey==="cloud") return schedsCloud;
            return schedsRuntime;
        }
        function isScheduleViewEditable(viewKey){
            if(viewKey==="edge") return canEditSchedules;
            if(viewKey==="runtime") return canEditSchedules && scheduleRuntimeSource==="EDGE";
            return false;
        }
        function refreshScheduleSourcePicker(){
            const sel=document.getElementById('sched-source');
            if(!sel) return;
            const edgeEnabled=scheduleEdgeAvailable||canEditSchedules;
            const cloudEnabled=scheduleCloudAvailable;
            [...sel.options].forEach(opt=>{
                if(opt.value==="edge") opt.disabled=!edgeEnabled;
                if(opt.value==="cloud") opt.disabled=!cloudEnabled;
            });
            if((scheduleViewSource==="edge"&&!edgeEnabled)||(scheduleViewSource==="cloud"&&!cloudEnabled)){
                scheduleViewSource="runtime";
            }
            sel.value=scheduleViewSource;
        }
        function setSchedSource(value){
            scheduleViewSource=value||"runtime";
            refreshScheduleSourcePicker();
            lastSchedRenderKey="";
            renderSched();
        }

        function bytesToB64(bytes){
            let binary='';
            const chunk=0x8000;
            for(let i=0;i<bytes.length;i+=chunk){
                binary+=String.fromCharCode.apply(null,bytes.subarray(i,i+chunk));
            }
            return btoa(binary);
        }
        function b64ToBytes(b64){
            const bin=atob(b64);
            const out=new Uint8Array(bin.length);
            for(let i=0;i<bin.length;i++) out[i]=bin.charCodeAt(i);
            return out;
        }
        function looksEncryptedFrame(payload){
            if(typeof payload!=="string"||payload.length<25) return false;
            if((payload.match(/:/g)||[]).length!==1) return false;
            return /^[A-Za-z0-9+/=:]+$/.test(payload);
        }
        function hasGatewayWebCrypto(){
            return !!(window.crypto&&window.crypto.subtle);
        }
        function getGatewayCryptoCompatKey(){
            if(!window.CryptoJS) return null;
            if(!gatewayCryptoCompatKey){
                gatewayCryptoCompatKey=window.CryptoJS.enc.Utf8.parse(GATEWAY_AES_KEY);
            }
            return gatewayCryptoCompatKey;
        }
        function markGatewayCryptoUnavailable(target){
            if(target==="term"){
                setTermStatus('Crypto','err');
                const box=document.getElementById('term-box');
                if(box && !document.getElementById('term-crypto-warn')){
                    const line=document.createElement('div');
                    line.id='term-crypto-warn';
                    line.className='msg-rx';
                    line.textContent='Crypto browser tidak aktif; frame WebSocket tidak bisa diproses.';
                    box.appendChild(line);
                    box.scrollTop=box.scrollHeight;
                }
                return;
            }
            const dash=document.getElementById('st-dash');
            if(dash){
                dash.textContent='Crypto';
                dash.className='err';
            }
        }
        async function getGatewayCryptoKey(){
            if(!window.crypto||!window.crypto.subtle) return null;
            if(!gatewayCryptoKeyPromise){
                gatewayCryptoKeyPromise=window.crypto.subtle.importKey(
                    "raw",
                    new TextEncoder().encode(GATEWAY_AES_KEY),
                    {name:"AES-CBC"},
                    false,
                    ["encrypt","decrypt"]
                ).catch(()=>null);
            }
            return gatewayCryptoKeyPromise;
        }
        function encryptGatewayFrameCompat(plainText){
            const key=getGatewayCryptoCompatKey();
            if(!key) return null;
            const now=Math.floor(Date.now()/1000);
            const tsWA=window.CryptoJS.lib.WordArray.create([
                (((now>>>24)&0xFF)<<24)|(((now>>>16)&0xFF)<<16)|(((now>>>8)&0xFF)<<8)|(now&0xFF)
            ],4);
            const combined=tsWA.clone();
            combined.concat(window.CryptoJS.enc.Utf8.parse(String(plainText||"")));
            const iv=window.CryptoJS.lib.WordArray.random(16);
            const encrypted=window.CryptoJS.AES.encrypt(combined,key,{
                iv:iv,
                mode:window.CryptoJS.mode.CBC,
                padding:window.CryptoJS.pad.Pkcs7
            });
            return `${window.CryptoJS.enc.Base64.stringify(iv)}:${window.CryptoJS.enc.Base64.stringify(encrypted.ciphertext)}`;
        }
        function decryptGatewayFrameCompat(payload){
            const key=getGatewayCryptoCompatKey();
            if(!key||!payload) return null;
            const sep=payload.indexOf(':');
            if(sep<=0) return null;
            try{
                const iv=window.CryptoJS.enc.Base64.parse(payload.slice(0,sep));
                const ciphertext=window.CryptoJS.enc.Base64.parse(payload.slice(sep+1));
                if(iv.sigBytes!==16||ciphertext.sigBytes===0||(ciphertext.sigBytes%16)!==0) return null;
                const cipherParams=window.CryptoJS.lib.CipherParams.create({ciphertext});
                const decrypted=window.CryptoJS.AES.decrypt(cipherParams,key,{
                    iv:iv,
                    mode:window.CryptoJS.mode.CBC,
                    padding:window.CryptoJS.pad.Pkcs7
                });
                if(decrypted.sigBytes<=4) return null;
                const latin1=window.CryptoJS.enc.Latin1.stringify(decrypted);
                try{
                    return decodeURIComponent(escape(latin1.substring(4)));
                }catch(_err){
                    return latin1.substring(4);
                }
            }catch(_err){
                return null;
            }
        }
        async function encryptGatewayFrame(plainText){
            if(!hasGatewayWebCrypto()){
                return encryptGatewayFrameCompat(plainText);
            }
            const key=await getGatewayCryptoKey();
            if(!key) return null;
            const now=Math.floor(Date.now()/1000);
            const textBytes=new TextEncoder().encode(String(plainText||""));
            const payload=new Uint8Array(4+textBytes.length);
            payload[0]=(now>>>24)&0xFF;
            payload[1]=(now>>>16)&0xFF;
            payload[2]=(now>>>8)&0xFF;
            payload[3]=now&0xFF;
            payload.set(textBytes,4);
            const iv=window.crypto.getRandomValues(new Uint8Array(16));
            const cipher=await window.crypto.subtle.encrypt({name:"AES-CBC",iv},key,payload);
            return `${bytesToB64(iv)}:${bytesToB64(new Uint8Array(cipher))}`;
        }
        async function decryptGatewayFrame(payload){
            if(typeof payload!=="string"||payload.length===0) return null;
            if(!looksEncryptedFrame(payload)) return payload;
            if(!hasGatewayWebCrypto()){
                return decryptGatewayFrameCompat(payload);
            }
            const sep=payload.indexOf(':');
            try{
                const key=await getGatewayCryptoKey();
                if(!key) return null;
                const iv=b64ToBytes(payload.slice(0,sep));
                const cipher=b64ToBytes(payload.slice(sep+1));
                if(iv.length!==16||cipher.length===0||(cipher.length%16)!==0) return null;
                const plainBuf=await window.crypto.subtle.decrypt({name:"AES-CBC",iv},key,cipher);
                const plainBytes=new Uint8Array(plainBuf);
                if(plainBytes.length<=4) return null;
                return new TextDecoder().decode(plainBytes.slice(4));
            }catch(_err){
                return null;
            }
        }
        async function sendWsJson(ws,obj){
            if(!ws||ws.readyState!==WebSocket.OPEN) return false;
            const encrypted=await encryptGatewayFrame(JSON.stringify(obj));
            if(!encrypted){
                markGatewayCryptoUnavailable('dash');
                return false;
            }
            ws.send(encrypted);
            return true;
        }
        async function sendWsText(ws,text){
            if(!ws||ws.readyState!==WebSocket.OPEN) return false;
            const encrypted=await encryptGatewayFrame(String(text||""));
            if(!encrypted){
                markGatewayCryptoUnavailable('term');
                return false;
            }
            ws.send(encrypted);
            return true;
        }
        function bytesToHex(bytes){
            return Array.from(bytes).map(b=>b.toString(16).padStart(2,'0')).join('');
        }
        async function sha256HexFromFile(file){
            const buf=await file.arrayBuffer();
            const digest=await window.crypto.subtle.digest("SHA-256",buf);
            return bytesToHex(new Uint8Array(digest));
        }
        async function buildAdminProof(method,path,extra){
            if(!hasAdminSession()) return "";
            return (await encryptGatewayFrame(JSON.stringify(Object.assign({
                token:adminToken,
                method:method,
                path:path
            },extra||{}))))||"";
        }

        function getKnownGatewayPassword(ssid){
            if(!ssid) return "";
            const s=ssid.trim();
            return (s==="Greenhouse-1"||s==="Greenhouse-2")?KNOWN_WIFI_PASS:"";
        }
        function getSsidPriority(ssid){
            const s=(ssid||"").trim();
            if(currentGhId===1){
                if(s==="Greenhouse-1") return 0;
                if(s==="Greenhouse-2") return 1;
            }else if(currentGhId===2){
                if(s==="Greenhouse-2") return 0;
                if(s==="Greenhouse-1") return 1;
            }
            return 10;
        }
        function appendWifiLog(level,msg,epoch){
            const box=document.getElementById('wifi-log');
            if(!box) return;
            const line=document.createElement('div');
            const cls=level==="success"?"ok":(level==="warn"?"warn":(level==="error"?"err":""));
            line.className=`wifi-line ${cls}`.trim();
            const t=epoch?new Date(epoch*1000).toLocaleTimeString():new Date().toLocaleTimeString();
            line.textContent=`[${t}] ${msg}`;
            box.appendChild(line);
            box.scrollTop=box.scrollHeight;
        }
        function clearAdminSession(){
            adminToken="";
            adminTokenExpiresAt=0;
            pendingAdminAction=null;
            sessionStorage.removeItem('gatewayAdminToken');
            sessionStorage.removeItem('gatewayAdminTokenExpiresAt');
        }
        function rememberAdminSession(token,expiresInSec){
            adminToken=token||"";
            const ttlSec=Math.max(60,Number(expiresInSec)||1800);
            adminTokenExpiresAt=Date.now()+(ttlSec*1000);
            sessionStorage.setItem('gatewayAdminToken',adminToken);
            sessionStorage.setItem('gatewayAdminTokenExpiresAt',String(adminTokenExpiresAt));
        }
        function hasAdminSession(){
            if(!adminToken) return false;
            if(adminTokenExpiresAt && Date.now()>=adminTokenExpiresAt){
                clearAdminSession();
                return false;
            }
            return true;
        }
        function buildAdminPayload(type,extra){
            return Object.assign({type:type,admin_token:adminToken},extra||{});
        }
        async function withAdminSession(action){
            if(typeof action!=="function") return;
            if(hasAdminSession()){await action();return;}
            if(!wsD||wsD.readyState!==WebSocket.OPEN){alert('Dashboard belum terhubung');return;}
            const password=prompt('Masukkan admin password');
            if(!password) return;
            pendingAdminAction=action;
            await sendWsJson(wsD,{type:"admin_auth",password:password});
        }
        async function handleAdminAuthResult(d){
            if(d.success&&d.admin_token){
                rememberAdminSession(d.admin_token,d.expires_in_sec);
                appendWifiLog('success',d.message||'Admin auth OK');
                const nextAction=pendingAdminAction;
                pendingAdminAction=null;
                if(nextAction) await nextAction();
                await pushClientTime(true);
                return;
            }
            pendingAdminAction=null;
            clearAdminSession();
            alert(d.message||'Admin auth gagal');
        }
        function downloadFile(path){
            withAdminSession(async ()=>{
                try{
                    const url=`/download?file=${encodeURIComponent(path)}`;
                    const proof=await buildAdminProof('GET','/download',{file:path});
                    const res=await fetch(url,{headers:{'X-Admin-Proof':proof}});
                    if(!res.ok){
                        if(res.status===403) clearAdminSession();
                        alert(res.status===403?'Forbidden':'Download gagal');
                        return;
                    }
                    const blob=await res.blob();
                    const objUrl=URL.createObjectURL(blob);
                    const a=document.createElement('a');
                    a.href=objUrl;
                    a.download=(path.split('/').pop()||'download.bin');
                    document.body.appendChild(a);
                    a.click();
                    a.remove();
                    setTimeout(()=>URL.revokeObjectURL(objUrl),1000);
                }catch(_err){
                    alert('Download gagal');
                }
            });
        }
        async function pushClientTime(force=false){
            const nowMs=Date.now();
            if(!force && (nowMs-lastClientTimePushMs)<15000) return;
            if(!wsD || wsD.readyState!==WebSocket.OPEN) return;
            const payload=hasAdminSession()
                ? buildAdminPayload("client_time",{epoch:Math.floor(nowMs/1000)})
                : {type:"client_time",epoch:Math.floor(nowMs/1000)};
            await sendWsJson(wsD,payload);
            lastClientTimePushMs=nowMs;
        }
        
        function connD(){
            wsD=new WebSocket((location.protocol==='https:'?'wss:':'ws:')+'//'+location.host+'/status_ws');
            wsD.onopen=async ()=>{document.getElementById('st-dash').textContent='Live';document.getElementById('st-dash').className='ok';wifiSwitchNoticeShown=false;if(hasAdminSession()) await pushClientTime(true);};
            wsD.onclose=()=>{
                document.getElementById('st-dash').textContent='Disc';document.getElementById('st-dash').className='err';
                if(wifiSwitchInProgress && !wifiSwitchNoticeShown){
                    wifiSwitchNoticeShown=true;
                    appendWifiLog('warn','WebSocket terputus saat ganti WiFi. Kemungkinan IP berubah; buka dashboard dari IP baru.');
                    const st=document.getElementById('wifi-status');
                    if(st) st.textContent='Koneksi dashboard terputus sementara. Jika sukses pindah SSID, akses ulang via IP baru.';
                }
                setTimeout(connD,2000)
            };
            wsD.onmessage=async (e)=>{
                try{
                    const plain=await decryptGatewayFrame(e.data);
                    if(!plain) return;
                    const d=JSON.parse(plain);
                    if(d.type==="admin_auth_result"){await handleAdminAuthResult(d);return}
                    if(d.type==="set_thresholds_result"){if(d.request_id)settleDashboardAction(d.request_id,d);else settleDashboardAction("set_thresholds_result",d);return}
                    if(d.type==="set_schedule_result"){if(d.request_id)settleDashboardAction(d.request_id,d);else settleDashboardAction(`set_schedule_result:${d.idx??-1}`,d);return}
                    if(d.type==="wifi_scan_result"){renderWifiOptions(d.networks||[]);return}
                    if(d.type==="wifi_change_result"){handleWifiChangeResult(d);return}
                    if(d.type==="wifi_log"){appendWifiLog(d.level||"info",d.message||"",d.epoch);return}
                    if(d.type==="error"&&d.auth_required){
                        clearAdminSession();
                        alert(d.message||'Admin auth required');
                        return;
                    }
                    if(d.gh_id){currentGhId=d.gh_id;document.getElementById('d_id').textContent=d.gh_id;}
                    if(d.firmware) document.getElementById('d_fw').textContent=d.firmware;
                    if(d.epoch) document.getElementById('d_time').textContent=new Date(d.epoch*1000).toLocaleTimeString();
                    applyNetworkPayload(d.network);
                    await applyControlPayload(d.control);
                    applySensorPayload(d.sensors,d.control);
                    applyThresholdPayload(d);
                    if(d.nodes) renderNodes(d.nodes);
                    else {document.getElementById('node-count').textContent="0 Found";document.getElementById('node-list').innerHTML='<div style="grid-column:1/-1;text-align:center;color:var(--soft);padding:20px;">No Active Local Nodes</div>'}
                    applyRelayPayload(d.relays);
                    applySchedulePayload(d);
                }catch(e){console.warn("Parse Err",e)}
            }
        }
        async function saveCfg(){
            if(!wsD||wsD.readyState!==WebSocket.OPEN)return;
            try{
                if(!isThresholdViewEditable(thresholdViewSource)) throw new Error('Threshold source ini read-only pada mode saat ini.');
                const requestId=makeRequestId("threshold");
                const waitAck=waitForDashboardAction(requestId);
                await sendWsJson(wsD,{type:"set_thresholds",request_id:requestId,tm_min:parseFloat(document.getElementById('tm_min').value),tm_max:parseFloat(document.getElementById('tm_max').value),hm_min:parseFloat(document.getElementById('hm_min').value),hm_max:parseFloat(document.getElementById('hm_max').value)});
                const result=await waitAck;
                if(!result.success) throw new Error(result.message||"Threshold update ditolak gateway.");
                isEditingCfg=false;
                alert(result.message||"Threshold update diterima gateway.");
            }catch(err){
                alert(err&&err.message?err.message:"Threshold update gagal.");
            }
        }
        function scanWifi(){
            if(!wsD||wsD.readyState!==WebSocket.OPEN){document.getElementById('wifi-status').textContent='WebSocket belum terhubung';return}
            document.getElementById('wifi-status').textContent='Scanning WiFi...';
            appendWifiLog('info','Meminta scan SSID dari gateway...');
            sendWsJson(wsD,{type:"wifi_scan"});
        }
        function renderWifiOptions(list){
            wifiScanList=Array.isArray(list)?list:[];
            wifiScanList.sort((a,b)=>{
                const pa=getSsidPriority(a.ssid||"");
                const pb=getSsidPriority(b.ssid||"");
                if(pa!==pb) return pa-pb;
                return (b.rssi||-999)-(a.rssi||-999);
            });
            const sel=document.getElementById('wifi-list');
            if(!sel) return;
            sel.innerHTML='<option value="">-- pilih SSID --</option>';
            wifiScanList.forEach(n=>{
                const o=document.createElement('option');
                o.value=n.ssid||'';
                const p=getSsidPriority(n.ssid||"");
                const pr=(p===0)?' [Priority 1]':(p===1?' [Priority 2]':'');
                o.textContent=`${n.ssid||'(hidden)'} (${n.rssi||0} dBm)${pr}`;
                sel.appendChild(o);
            });
            document.getElementById('wifi-status').textContent=`Scan selesai. Ditemukan ${wifiScanList.length} jaringan.`;
            appendWifiLog('info',`Scan selesai, ${wifiScanList.length} SSID terdeteksi.`);
        }
        function pickWifi(){
            const sel=document.getElementById('wifi-list');
            if(!sel||!sel.value) return;
            document.getElementById('wifi-ssid').value=sel.value;
            const autoPass=getKnownGatewayPassword(sel.value);
            if(autoPass){
                document.getElementById('wifi-pass').value=autoPass;
                appendWifiLog('info',`SSID ${sel.value} dikenali, password default diisi otomatis.`);
            }
        }
        function changeWifi(){
            const ssid=document.getElementById('wifi-ssid').value.trim();
            let pass=document.getElementById('wifi-pass').value;
            if(!wsD||wsD.readyState!==WebSocket.OPEN){document.getElementById('wifi-status').textContent='WebSocket belum terhubung';return}
            if(!ssid){document.getElementById('wifi-status').textContent='SSID wajib diisi';return}
            if(!pass){
                const autoPass=getKnownGatewayPassword(ssid);
                if(autoPass){
                    pass=autoPass;
                    document.getElementById('wifi-pass').value=autoPass;
                    appendWifiLog('info',`Password default otomatis dipakai untuk ${ssid}.`);
                }
            }
            withAdminSession(async ()=>{
                document.getElementById('wifi-status').textContent=`Mengganti ke "${ssid}"...`;
                appendWifiLog('info',`Mengirim request ganti ke SSID "${ssid}"...`);
                wifiSwitchInProgress=true;
                wifiSwitchNoticeShown=false;
                await sendWsJson(wsD,buildAdminPayload("wifi_change",{ssid:ssid,pass:pass}));
            });
        }
        function handleWifiChangeResult(d){
            const st=document.getElementById('wifi-status');
            if(!st) return;
            if(d.pending){
                st.textContent=d.message||'Mencoba pindah WiFi...';
                appendWifiLog('info',d.message||'Mencoba pindah WiFi...');
                return;
            }
            if(d.success){
                const ipTxt=d.ip?` IP baru: ${d.ip}.`:'';
                st.textContent=`${d.message||'Berhasil'}${ipTxt}`;
                appendWifiLog('success',`${d.message||'Berhasil'}${ipTxt}`);
                wifiSwitchInProgress=false;
            }else{
                const stat=(d.wifi_status_text||'UNKNOWN');
                const reason=d.disconnect_reason_text?` ${d.disconnect_reason_text}`:'';
                const head=`${d.message||'Gagal ganti WiFi'} [${stat}${reason?`,${reason}`:''}]`;
                st.textContent=d.hint?`${head} ${d.hint}`:head;
                appendWifiLog('error',head);
                if(d.detail) appendWifiLog('error',d.detail);
                if(d.force_hidden) appendWifiLog('warn','Mode hidden aktif untuk SSID ini (scan tidak selalu menampilkan SSID).');
                if(d.ignored_assoc_leave) appendWifiLog('warn','Reason ASSOC_LEAVE diabaikan karena terdeteksi saat lepas dari AP lama.');
                if(d.scan_seen===false&&d.force_hidden!==true) appendWifiLog('warn','SSID tidak terlihat saat scan ulang oleh gateway.');
                if(d.scan_seen===true&&d.scan_rssi!==undefined) appendWifiLog('warn',`RSSI target saat scan ulang: ${d.scan_rssi} dBm.`);
                if(d.scan_code!==undefined&&d.scan_code<0) appendWifiLog('warn',`Scan ulang gagal sementara (code ${d.scan_code}), coba ulang 1x.`);
                if(d.hint) appendWifiLog('warn',`Saran: ${d.hint}`);
                wifiSwitchInProgress=false;
            }
        }
        function renderNodes(nodes){
            const c=document.getElementById('node-list');
            let html='';
            nodes.forEach(n=>{
                let detailHtml = '';
                if(n.name.startsWith("cam")) {
                    detailHtml = `
                        <div class="node-stat"><span>Status:</span><b class="${n.is_f?'warn':'ok'}">${n.is_f?'FOGGY':'CLEAR'}</b></div>
                        <div class="node-stat"><span>Confidence:</span><b>${fmtNum(n.conf,1)}%</b></div>`;
                } else {
                    detailHtml = `
                        <div class="node-stat"><span>Temp:</span><b>${fmtNum(n.t,1)}°C</b></div>
                        <div class="node-stat"><span>Hum:</span><b>${fmtNum(n.h,0)}%</b></div>
                        <div class="node-stat"><span>Lux:</span><b>${fmtNum(n.l,0)}</b></div>`;
                }

                html+=`
                <div class="node-card">
                    <div class="node-id">${n.name.toUpperCase()} <span class="dot"></span></div>
                    ${detailHtml}
                    <div class="node-detail">
                        <div style="text-align:right; font-size:0.9em; color:var(--soft)">${n.age}s ago</div>
                    </div>
                </div>`;
            });
            c.innerHTML=html;
        }
        function renderSched(){
            const c=document.getElementById('alm-list');
            const activeEl=document.activeElement;
            if(activeEl&&(activeEl.tagName==="INPUT"||(activeEl.tagName==="SELECT"&&activeEl.id!=="sched-source")))return;
            const scheds=getScheduleListForView(scheduleViewSource);
            const editable=isScheduleViewEditable(scheduleViewSource);
            const renderKey=JSON.stringify({view:scheduleViewSource,runtime:scheduleRuntimeSource,scheds:scheds,editable:editable});
            if(renderKey===lastSchedRenderKey)return;
            const expanded=new Set();
            c.querySelectorAll('.alarm-body.show').forEach(el=>expanded.add(el.id.replace('det-','')));
            c.innerHTML='';
            const sourceLabel=scheduleViewSource==="edge"?"EDGE":(scheduleViewSource==="cloud"?"CLOUD":"RUNTIME");
            const isLocal=editable;
            if(!Array.isArray(scheds)||scheds.length===0){
                c.innerHTML=`<div style="text-align:center;color:var(--soft);">No ${sourceLabel} schedule data.</div>`;
                lastSchedRenderKey=renderKey;
                updateScheduleModeBadge(sourceLabel);
                return;
            }
            scheds.forEach((s,i)=>{
                const fh=n=>String(n).padStart(2,'0'),tStr=`${fh(s.sh)}:${fh(s.sm)} - ${fh(s.eh)}:${fh(s.em)}`;
                const mkR=(idx,n,l,e,st)=>`<div class="form-row"><label>${l}</label><div style="display:flex;gap:5px"><label style="font-size:0.8em;display:flex;align-items:center;"><input type="checkbox" id="e${n}-${idx}" ${e?'checked':''} ${isLocal?'':'disabled'}> Ovr</label><select id="v${n}-${idx}" ${isLocal?'':'disabled'}><option value="1" ${st?'selected':''}>ON</option><option value="0" ${!st?'selected':''}>OFF</option></select></div></div>`;
                c.insertAdjacentHTML('beforeend',`<div class="alarm-card"><div class="alarm-head" onclick="togDet(${i},event)" style="cursor:pointer"><div><div class="time-txt" id="txt-t-${i}">${tStr}</div><div class="meta-txt" id="txt-m-${i}">Source: ${sourceLabel} | Mode: ${s.relay_code||'222'} ${isLocal?'(Click to Edit)':'(Read Only)'}</div></div><label class="toggle" ${isLocal?'onclick="event.stopPropagation()"':''}><input type="checkbox" ${s.en?'checked':''} ${isLocal?`onchange="togEn(${i},this.checked)"`:'disabled'}><span class="slider"></span></label></div><div id="det-${i}" class="alarm-body"><div class="${isLocal?'':'disabled-overlay'}"><div class="form-row"><label>Start</label><input type="time" id="s-${i}" value="${fh(s.sh)}:${fh(s.sm)}" ${isLocal?'':'disabled'}></div><div class="form-row"><label>End</label><input type="time" id="e-${i}" value="${fh(s.eh)}:${fh(s.em)}" ${isLocal?'':'disabled'}></div><hr style="border-color:var(--line);margin:10px 0">${mkR(i,1,'Exhaust',s.r1e,s.r1s)}${mkR(i,2,'Dehumid',s.r2e,s.r2s)}${mkR(i,3,'Blower',s.r3e,s.r3s)}<button class="btn" onclick="save(${i})" ${isLocal?'':'disabled'}>Save Changes</button></div></div></div>`);
                if(expanded.has(String(i)))document.getElementById(`det-${i}`).classList.add('show');
            });
            updateScheduleModeBadge(sourceLabel);
            lastSchedRenderKey=renderKey;
        }
        function togDet(i,e){if(!['INPUT','SELECT','LABEL'].includes(e.target.tagName))document.getElementById('det-'+i).classList.toggle('show')}
        async function togEn(i,c){
            try{
                const requestId=makeRequestId(`schedule-${i}`);
                const waitAck=waitForDashboardAction(requestId);
                await sendData(i,c,...getS(i,true),requestId);
                const result=await waitAck;
                if(!result.success) throw new Error(result.message||"Schedule update ditolak gateway.");
            }catch(err){
                alert(err&&err.message?err.message:"Schedule update gagal.");
            }
        }
        function getS(i,mem){
            const scheds=getScheduleListForView(scheduleViewSource), s=scheds[i]; if(mem)return[s.id,s.sh,s.sm,s.eh,s.em,s.r1e,s.r1s,s.r2e,s.r2s,s.r3e,s.r3s];
            const gv=id=>document.getElementById(id).value, gc=id=>document.getElementById(id).checked;
            const st=gv('s-'+i).split(':'),en=gv('e-'+i).split(':');
            return[s&&s.id?s.id:0,parseInt(st[0]),parseInt(st[1]),parseInt(en[0]),parseInt(en[1]),gc('e1-'+i),gv('v1-'+i)=='1',gc('e2-'+i),gv('v2-'+i)=='1',gc('e3-'+i),gv('v3-'+i)=='1'];
        }
        async function save(i){
            const c=document.querySelector(`#det-${i}`).parentElement.querySelector('.toggle input').checked;
            try{
                const requestId=makeRequestId(`schedule-${i}`);
                const waitAck=waitForDashboardAction(requestId);
                await sendData(i,c,...getS(i,false),requestId);
                const result=await waitAck;
                if(!result.success) throw new Error(result.message||"Schedule update ditolak gateway.");
                alert(result.message||"Schedule update diterima gateway.");
            }catch(err){
                alert(err&&err.message?err.message:"Schedule update gagal.");
            }
        }
        async function sendData(i,en,id,sh,sm,eh,em,r1e,r1s,r2e,r2s,r3e,r3s,requestId){
            if(!wsD||wsD.readyState!==WebSocket.OPEN) throw new Error('Dashboard belum terhubung');
            if(!isScheduleViewEditable(scheduleViewSource)) throw new Error('Schedule source ini read-only pada mode saat ini.');
            await sendWsJson(wsD,{type:"set_schedule",request_id:requestId||makeRequestId(`schedule-${i}`),idx:i,id:id,en:en,sh:sh,sm:sm,eh:eh,em:em,r1e:r1e,r1s:r1s,r2e:r2e,r2s:r2s,r3e:r3e,r3s:r3s});
        }
        
        let wsT;const box=document.getElementById('term-box');
        function setTermStatus(text,cls){const el=document.getElementById('st-term');if(!el)return;el.textContent=text;el.className=cls}
        function connT(){
            setTermStatus('Conn...','warn');
            wsT=new WebSocket((location.protocol==='https:'?'wss:':'ws:')+'//'+location.host+'/ws');
            wsT.onopen=async ()=>{
                setTermStatus('Live','ok');
                if(hasAdminSession()) await sendWsText(wsT,`auth_token ${adminToken}`);
            };
            wsT.onclose=()=>{setTermStatus('Disc','err');setTimeout(connT,2000)};
            wsT.onerror=()=>setTermStatus('Err','err');
            wsT.onmessage=async e=>{const msg=await decryptGatewayFrame(e.data);if(msg===null)return;const l=document.createElement('div');l.className=msg.startsWith('>')?'msg-tx':'msg-rx';l.textContent=msg;box.appendChild(l);box.scrollTop=box.scrollHeight}
        }
        async function send(){const i=document.getElementById('cmd-in');if(wsT&&i.value.trim()){await sendWsText(wsT,i.value);i.value=''}}
        function cls(){box.innerHTML=''}
        document.getElementById('cmd-in').addEventListener("keypress",e=>{if(e.key==="Enter"){e.preventDefault();send()}});
        document.getElementById('wifi-ssid').addEventListener('change',()=>{
            const ssid=document.getElementById('wifi-ssid').value.trim();
            const passEl=document.getElementById('wifi-pass');
            if(!ssid||passEl.value) return;
            const autoPass=getKnownGatewayPassword(ssid);
            if(autoPass){
                passEl.value=autoPass;
                appendWifiLog('info',`Password default otomatis dipakai untuk ${ssid}.`);
            }
        });
        document.getElementById('up-form').addEventListener('submit',e=>{e.preventDefault();const f=document.getElementById('file').files[0];if(!f)return;withAdminSession(async ()=>{const sha256=await sha256HexFromFile(f);const proof=await buildAdminProof('POST','/doUpdate',{filename:f.name,size:f.size,sha256:sha256});const x=new XMLHttpRequest();x.open('POST','/doUpdate');x.setRequestHeader('X-Admin-Proof',proof);x.setRequestHeader('X-Upload-Filename',f.name);x.setRequestHeader('X-Upload-Size',String(f.size));x.setRequestHeader('X-Upload-SHA256',sha256);x.upload.onprogress=e=>{if(e.lengthComputable)document.getElementById('prog').style.width=Math.round((e.loaded/e.total)*100)+'%'};x.onload=()=>{if(x.status===403) clearAdminSession();alert(x.status===200?'OK, Rebooting':(x.status===403?'Forbidden':'Fail'))};const d=new FormData();d.append('update',f);x.send(d)})});
        initTheme();connD();connT();
    </script>
</body>
</html>
)=====";

void WebSerialClass::begin(AsyncWebServer *server, const char *url)
{
    _ws = new AsyncWebSocket(url);
    _ws->onEvent(onWsEvent);
    server->addHandler(_ws);
    server->on("/crypto.js", HTTP_GET, [](AsyncWebServerRequest *r)
               {
                   AsyncWebServerResponse *response = r->beginResponse(
                       200,
                       "application/javascript",
                       reinterpret_cast<const uint8_t *>(EMBEDDED_CRYPTO_JS),
                       strlen_P(EMBEDDED_CRYPTO_JS));
                   response->addHeader("Cache-Control", "public, max-age=3600");
                   r->send(response);
               });
    server->on("/", HTTP_GET, [](AsyncWebServerRequest *r)
               {
                   AsyncWebServerResponse *response = r->beginResponse(
                       200,
                       "text/html",
                       reinterpret_cast<const uint8_t *>(UNIFIED_HTML),
                       sizeof(UNIFIED_HTML) - 1);
                   response->addHeader("Cache-Control", "no-store");
                   r->send(response);
               });
    server->on("/webserial", HTTP_GET, [](AsyncWebServerRequest *r)
               { r->redirect("/"); });
    server->on("/dashboard", HTTP_GET, [](AsyncWebServerRequest *r)
               { r->redirect("/"); });
    server->on("/update", HTTP_GET, [](AsyncWebServerRequest *r)
               {
                   r->redirect("/");
               });
    Serial.println("Web Ready");
}
void WebSerialClass::onMessage(WebSerialCallback c) { _messageCallback = c; }
void WebSerialClass::print(const String &m)
{
    sendEncryptedWebSerialFrame(_ws, m);
}
void WebSerialClass::print(const char *m)
{
    if (m)
        sendEncryptedWebSerialFrame(_ws, String(m));
}
void WebSerialClass::println(const String &m)
{
    sendEncryptedWebSerialFrame(_ws, m + "\n");
}
void WebSerialClass::println(const char *m)
{
    if (m)
        sendEncryptedWebSerialFrame(_ws, String(m) + "\n");
}
void WebSerialClass::printf(const char *format, ...)
{
    if (!_ws || _ws->count() == 0)
        return;
    static char b[256];
    va_list a;
    va_start(a, format);
    vsnprintf(b, sizeof(b), format, a);
    va_end(a);
    sendEncryptedWebSerialFrame(_ws, String(b));
}
void WebSerialClass::onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *a, uint8_t *d, size_t l)
{
    if (t == WS_EVT_DATA && _messageCallback)
        _messageCallback(c, d, l);
}
size_t WebSerialClass::write(uint8_t c)
{
    if (_ws)
    {
        char b[2] = {(char)c, 0};
        sendEncryptedWebSerialFrame(_ws, String(b));
        return 1;
    }
    return 0;
}
size_t WebSerialClass::write(const uint8_t *b, size_t s)
{
    if (_ws && s > 0)
    {
        String payload;
        payload.reserve(s);
        for (size_t i = 0; i < s; ++i)
            payload += static_cast<char>(b[i]);
        sendEncryptedWebSerialFrame(_ws, payload);
        return s;
    }
    return 0;
}
WebSerialClass WebSerial;

