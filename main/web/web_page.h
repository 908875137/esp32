/*
 * web_page.h
 *
 * Embedded HTML page for WiFi provisioning.
 * This string is embedded directly in the firmware binary.
 */
#pragma once

/* Minimal responsive HTML for provisioning */
#define WEB_PROVISION_HTML \
"<!DOCTYPE html>" \
"<html lang=\"zh-CN\">" \
"<head>" \
"<meta charset=\"UTF-8\">" \
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" \
"<title>ThinkINK 配网</title>" \
"<style>" \
"*{box-sizing:border-box;margin:0;padding:0}" \
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;" \
"background:#1a1a2e;color:#eaeaea;display:flex;justify-content:center;" \
"align-items:center;min-height:100vh;padding:16px}" \
".card{background:#16213e;border-radius:12px;padding:32px 24px;" \
"max-width:380px;width:100%;box-shadow:0 4px 24px rgba(0,0,0,.4)}" \
"h1{text-align:center;font-size:1.4rem;margin-bottom:24px;color:#fff}" \
"label{display:block;font-size:.85rem;margin-bottom:6px;color:#b0b0b0}" \
"input{width:100%;padding:10px 12px;border:1px solid #334;" \
"border-radius:8px;background:#0f3460;color:#fff;font-size:1rem;" \
"margin-bottom:16px;outline:none}" \
"input:focus{border-color:#e94560}" \
"button{width:100%;padding:12px;border:none;border-radius:8px;" \
"background:#e94560;color:#fff;font-size:1rem;font-weight:600;" \
"cursor:pointer;margin-top:8px}" \
"button:hover{background:#d63850}" \
"#msg{text-align:center;margin-top:16px;font-size:.85rem;display:none}" \
"#msg.ok{color:#4ecca3;display:block}" \
"#msg.err{color:#e94560;display:block}" \
"</style>" \
"</head>" \
"<body>" \
"<div class=\"card\">" \
"<h1>ThinkINK 配网</h1>" \
"<form id=\"fm\">" \
"<label for=\"ssid\">WiFi 名称</label>" \
"<input id=\"ssid\" name=\"ssid\" type=\"text\" required autofocus>" \
"<label for=\"pass\">WiFi 密码</label>" \
"<input id=\"pass\" name=\"password\" type=\"password\" required>" \
"<button type=\"submit\">保存并连接</button>" \
"</form>" \
"<div id=\"msg\"></div>" \
"</div>" \
"<script>" \
"document.getElementById('fm').addEventListener('submit',async function(e){" \
"e.preventDefault();const m=document.getElementById('msg');" \
"m.className='';m.textContent='正在连接...';m.style.display='block';" \
"try{" \
"const body='ssid='+encodeURIComponent(document.getElementById('ssid').value)" \
"+'&password='+encodeURIComponent(document.getElementById('pass').value);" \
"const r=await fetch('/save',{method:'POST'," \
"headers:{'Content-Type':'application/x-www-form-urlencoded'}," \
"body:body});" \
"if(r.ok){const t=await r.text();m.className='ok';m.textContent=t}" \
"else{const t=await r.text();m.className='err';m.textContent=t}}" \
"catch(e){m.className='err';m.textContent='请求失败: '+e.message}" \
"});" \
"</script>" \
"</body>" \
"</html>"

