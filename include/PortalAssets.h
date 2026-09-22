#ifndef PORTAL_ASSETS_H
#define PORTAL_ASSETS_H

#include <Arduino.h>

const char PORTAL_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Greenhouse Gateway Setup</title>
    <style>
        :root {
            --primary: #10b981; /* Emerald 500 */
            --primary-dark: #059669; /* Emerald 600 */
            --bg: #f3f4f6; /* Gray 100 */
            --card-bg: #ffffff;
            --text-main: #1f2937; /* Gray 800 */
            --text-muted: #6b7280; /* Gray 500 */
            --border: #e5e7eb; /* Gray 200 */
            --radius: 12px;
            --shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1), 0 2px 4px -1px rgba(0, 0, 0, 0.06);
        }

        * { box-sizing: border-box; margin: 0; padding: 0; font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }
        
        body {
            background-color: var(--bg);
            color: var(--text-main);
            display: flex;
            justify-content: center;
            align-items: flex-start;
            min-height: 100vh;
            padding: 20px;
        }

        .container {
            background: var(--card-bg);
            border-radius: var(--radius);
            box-shadow: var(--shadow);
            width: 100%;
            max-width: 480px;
            overflow: hidden;
            margin-top: 20px;
        }

        header {
            background: var(--primary);
            color: white;
            padding: 20px;
            text-align: center;
        }

        header h1 { font-size: 1.5rem; font-weight: 600; margin-bottom: 5px; }
        header p { font-size: 0.9rem; opacity: 0.9; }

        .content { padding: 20px; }

        .section-title {
            font-size: 0.85rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
            color: var(--text-muted);
            margin-bottom: 10px;
            margin-top: 20px;
            font-weight: 700;
        }
        .section-title:first-child { margin-top: 0; }

        .form-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 6px; font-weight: 500; font-size: 0.95rem; }
        input[type="text"], input[type="password"], input[type="url"] {
            width: 100%;
            padding: 10px 12px;
            border: 1px solid var(--border);
            border-radius: 6px;
            font-size: 1rem;
            transition: border-color 0.2s;
        }
        input:focus { outline: none; border-color: var(--primary); box-shadow: 0 0 0 3px rgba(16, 185, 129, 0.1); }

        .btn {
            display: inline-block;
            width: 100%;
            padding: 12px;
            background: var(--primary);
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 1rem;
            font-weight: 600;
            cursor: pointer;
            transition: background 0.2s;
            text-align: center;
        }
        .btn:hover { background: var(--primary-dark); }
        .btn:disabled { background: var(--text-muted); cursor: not-allowed; }

        .btn-outline {
            background: transparent;
            border: 2px solid var(--primary);
            color: var(--primary);
            margin-top: 10px;
        }
        .btn-outline:hover { background: rgba(16, 185, 129, 0.05); }

        /* Network List */
        .network-list {
            border: 1px solid var(--border);
            border-radius: 8px;
            max-height: 200px;
            overflow-y: auto;
            background: #fafafa;
        }
        .network-item {
            padding: 10px 12px;
            border-bottom: 1px solid var(--border);
            cursor: pointer;
            display: flex;
            justify-content: space-between;
            align-items: center;
            transition: background 0.1s;
        }
        .network-item:last-child { border-bottom: none; }
        .network-item:hover { background: #f0fdf4; }
        .network-item.selected { background: #d1fae5; border-left: 4px solid var(--primary); }
        .network-rssi { font-size: 0.8rem; color: var(--text-muted); }
        .lock-icon { font-size: 0.8rem; margin-right: 5px; }

        .spinner {
            border: 3px solid rgba(0,0,0,0.1);
            border-left-color: var(--primary);
            border-radius: 50%;
            width: 20px;
            height: 20px;
            animation: spin 1s linear infinite;
            display: inline-block;
            margin-right: 8px;
            vertical-align: middle;
        }
        @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }

        .status-msg {
            text-align: center;
            padding: 10px;
            color: var(--text-muted);
            font-size: 0.9rem;
        }
        
        .info-tip {
            font-size: 0.8rem;
            color: var(--text-muted);
            margin-top: 4px;
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>Gateway Setup</h1>
            <p>Configure WiFi & API Connection</p>
        </header>
        <div class="content">
            <form id="configForm" action="/save" method="POST">
                <div class="section-title">WiFi Connection</div>
                
                <div class="form-group">
                    <div id="networkList" class="network-list">
                        <div class="status-msg">
                            <div class="spinner"></div> Scanning networks...
                        </div>
                    </div>
                    <button type="button" class="btn btn-outline" onclick="scanNetworks()">Rescan Networks</button>
                </div>

                <div class="form-group">
                    <label for="ssid">Selected SSID</label>
                    <input type="text" id="ssid" name="ssid" placeholder="Select from list or type manual" required value="%SSID%">
                </div>

                <div class="form-group">
                    <label for="pass">WiFi Password</label>
                    <input type="password" id="pass" name="pass" placeholder="Enter WiFi Password">
                    <div class="info-tip">Leave blank if open network or keeping current password.</div>
                </div>

                <div class="section-title">API Configuration</div>

                <div class="form-group">
                    <label for="token">API Token</label>
                    <input type="text" id="token" name="token" value="%TOKEN%" required>
                </div>

                <div class="form-group">
                    <label for="ta_token">TA API Token</label>
                    <input type="text" id="ta_token" name="ta_token" value="%TA_TOKEN%" required>
                    <div class="info-tip">Dipakai untuk jadwal dan post status di your TA server.</div>
                </div>

                <div class="form-group">
                    <label for="th_url">Threshold URL</label>
                    <input type="url" id="th_url" name="th_url" value="%TH_URL%" required>
                </div>

                <div class="form-group">
                    <label for="nd_url_base">Data Endpoint Base URL</label>
                    <input type="url" id="nd_url_base" name="nd_url_base" value="%ND_URL_BASE%" required>
                    <div class="info-tip">Include protocol (http/https).</div>
                </div>

                <div class="section-title">Device Options</div>

                <div class="form-group">
                    <label for="admin_pass">Admin Password</label>
                    <input type="password" id="admin_pass" name="admin_pass" placeholder="Leave blank to keep current password">
                </div>

                <div class="form-group">
                    <label style="display:flex;align-items:center;gap:10px;">
                        <input type="checkbox" id="gprs_enabled" name="gprs_enabled" %GPRS_CHECKED%>
                        Enable GPRS fallback
                    </label>
                    <div class="info-tip">Aktifkan hanya jika gateway memang diizinkan pindah ke koneksi seluler.</div>
                </div>

                <button type="submit" class="btn">Save & Connect</button>
            </form>
        </div>
    </div>

    <script>
        function scanNetworks() {
            const list = document.getElementById('networkList');
            list.innerHTML = '<div class="status-msg"><div class="spinner"></div> Scanning networks...</div>';
            
            fetch('/scan')
                .then(response => response.json())
                .then(data => {
                    list.innerHTML = '';
                    if(data.length === 0) {
                        list.innerHTML = '<div class="status-msg">No networks found</div>';
                        return;
                    }
                    data.sort((a, b) => b.rssi - a.rssi); // Sort by signal strength
                    data.forEach(net => {
                        const item = document.createElement('div');
                        item.className = 'network-item';
                        // Simple signal calculation
                        let bars = 1;
                        if(net.rssi > -60) bars = 3;
                        else if(net.rssi > -70) bars = 2;
                        
                        const signalIcon = '📶'.repeat(1); // Placeholder for simplicity
                        const lockIcon = net.auth ? '🔒' : '';
                        
                        item.innerHTML = `
                            <div>
                                <span class="lock-icon">${lockIcon}</span>
                                <strong>${net.ssid}</strong>
                            </div>
                            <div class="network-rssi">${net.rssi} dBm</div>
                        `;
                        item.onclick = () => selectNetwork(net.ssid, item);
                        list.appendChild(item);
                    });
                })
                .catch(err => {
                    console.error('Scan failed', err);
                    list.innerHTML = '<div class="status-msg">Scan failed. <a href="#" onclick="scanNetworks()">Try again</a></div>';
                });
        }

        function selectNetwork(ssid, el) {
            document.getElementById('ssid').value = ssid;
            document.querySelectorAll('.network-item').forEach(i => i.classList.remove('selected'));
            if(el) el.classList.add('selected');
            document.getElementById('pass').focus();
        }

        // Initial scan on load
        document.addEventListener('DOMContentLoaded', scanNetworks);
    </script>
</body>
</html>
)=====";

#endif // PORTAL_ASSETS_H
