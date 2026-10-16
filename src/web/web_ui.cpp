/*
 * SparkMiner - Web UI Implementation
 * Serves a live status page and a settings editor on port 80 while mining,
 * so WiFi/wallet/pool can be changed without a factory reset.
 * Changes (save, restart, firmware update) need the admin PIN once one is set.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <board_config.h>
#include "web_ui.h"
#include "../config/nvs_config.h"
#include "../extras/extras.h"
#include "../stats/monitor.h"
#include "../display/display.h"
#include "../mining/miner.h"
#include "../display/touch_cyd.h"
#include "../extras/fw_update.h"
#include "../config/wifi_store.h"
#include "../extras/history.h"

static WebServer s_server(80);
static bool s_started = false;
static volatile uint32_t s_restartAt = 0;   // millis() at which to reboot (0 = none)
static volatile bool s_portalAfter = false; // reboot into the setup hotspot instead
static char s_hostname[24];

// Firmware upload state
static bool s_otaAuthorized = false;
static bool s_otaFailed = false;
static size_t s_otaBytes = 0;
static bool s_otaHolding = false;   // Core-1 miner parked for the image hash check

static void otaHold(bool on) {
    if (on && !s_otaHolding) { miner_sha_hold(); s_otaHolding = true; }
    if (!on && s_otaHolding) { miner_sha_release(); s_otaHolding = false; }
}

// ============================================================
// Page
// ============================================================

static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SparkMiner</title>
<style>
:root{--bg:#0d0d0d;--card:#181818;--line:#2c2c2c;--fg:#f2f2f2;--mut:#9a9a9a;--acc:#ff6800;--ok:#3ccf6e;--bad:#ff5050}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 -apple-system,Helvetica,Arial,sans-serif}
main{max-width:640px;margin:0 auto;padding:16px}h1{color:var(--acc);font-size:22px;margin:4px 0 14px}
h2{font-size:13px;color:var(--mut);margin:20px 0 6px;text-transform:uppercase;letter-spacing:.06em}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}.tile{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 12px}
.tile b{display:block;font-size:20px;font-variant-numeric:tabular-nums;word-break:break-all}.tile span{color:var(--mut);font-size:12px}
.wide{grid-column:1/-1}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px;background:var(--bad)}.dot.on{background:var(--ok)}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:4px 12px 12px}label{display:block;color:var(--mut);font-size:12px;margin:10px 0 3px}
input,select{width:100%;padding:9px;border-radius:7px;border:1px solid var(--line);background:#0d0d0d;color:var(--fg);font-size:15px}
input[type=checkbox]{width:auto;margin-right:8px;vertical-align:middle}label.ck{color:var(--fg);font-size:14px;margin-top:12px}
.row{display:grid;grid-template-columns:2fr 1fr;gap:8px}.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}
button{margin-top:12px;width:100%;padding:11px;border:0;border-radius:8px;background:var(--acc);color:#000;font-weight:600;font-size:15px}
button.sec{background:#2c2c2c;color:var(--fg)}small{color:var(--mut)}.msg{color:var(--ok);margin-top:8px;min-height:1em}.err{color:var(--bad)}
#pinbar{display:none;margin-bottom:6px}a{color:var(--acc)}
.chart{position:relative;margin-bottom:6px}.chart svg{display:block;width:100%;height:auto;touch-action:pan-y}
.ct{font-size:12px;color:var(--mut);margin:8px 0 2px}.ct b{color:var(--fg);font-weight:600}
#tip{position:absolute;pointer-events:none;background:#000c;border:1px solid var(--line);border-radius:6px;padding:6px 8px;font-size:12px;display:none;white-space:nowrap;z-index:2}
.odds{display:grid;grid-template-columns:1fr auto;gap:6px 12px;font-size:14px}.odds b{text-align:right;font-variant-numeric:tabular-nums}
</style></head><body><main>
<h1>&#9889; SparkMiner</h1>
<div class="grid">
<div class="tile"><span>Hashrate</span><b id="hr">-</b></div>
<div class="tile"><span>Best difficulty</span><b id="best">-</b></div>
<div class="tile"><span>Shares (accepted / rejected)</span><b id="sh">-</b></div>
<div class="tile"><span>Blocks found</span><b id="blk">-</b></div>
<div class="tile"><span>Uptime</span><b id="up">-</b></div>
<div class="tile"><span>Pool ping</span><b id="ping">-</b></div>
<div class="tile wide"><span>Pool</span><b id="pool" style="font-size:15px">-</b></div>
<div class="tile"><span>WiFi signal</span><b id="rssi">-</b></div>
<div class="tile"><span>Free memory</span><b id="heap">-</b></div>
<div class="tile"><span>Chip temperature</span><b id="tmp">-</b></div>
<div class="tile"><span>WiFi network</span><b id="wn" style="font-size:15px">-</b></div>
<div class="tile"><span>BTC price</span><b id="px">-</b></div>
<div class="tile"><span>Block height</span><b id="bh">-</b></div>
<div class="tile wide"><span>Network hashrate / difficulty</span><b id="net" style="font-size:15px">-</b></div>
</div>
<p id="links"></p>

<h2>Last 24 hours</h2><div class="card" id="hist" style="position:relative">
<div id="tip"></div>
<div class="ct">Hashrate, kH/s <b id="cv0"></b></div><div class="chart" id="c0"></div>
<div class="ct">Chip temperature, &deg;C <b id="cv1"></b></div><div class="chart" id="c1"></div>
<div class="ct">WiFi signal, dBm <b id="cv2"></b></div><div class="chart" id="c2"></div>
<small id="hnote">Shaded = offline (WiFi or pool down). One point every 5 minutes.</small>
</div>

<h2>Odds of finding a block</h2><div class="card"><div class="odds" id="odds" style="padding-top:10px"><span>Waiting for network data...</span></div>
<small style="display:block;margin-top:8px">A block needs one share above the network difficulty. Every hash is an independent draw, so past luck doesn't change future odds.</small></div>

<div id="pinbar" class="card"><label>Admin PIN <small>(needed to change anything)</small></label><input id="pin" type="password" inputmode="numeric" autocomplete="current-password"></div>

<form id="f">
<h2>Wallet &amp; pool</h2><div class="card">
<label>BTC wallet address <small>(receive address only &mdash; never a seed phrase)</small></label><input name="wallet" maxlength="120" autocomplete="off" spellcheck="false">
<label>Worker name</label><input name="worker" maxlength="31">
<div class="row"><div><label>Pool URL</label><input name="pool_url" maxlength="80"></div><div><label>Port</label><input name="pool_port" type="number" min="1" max="65535"></div></div>
<label>Pool password <small>(blank = keep)</small></label><input name="pool_pass" maxlength="64">
<div class="row"><div><label>Backup pool URL</label><input name="bk_pool_url" maxlength="80"></div><div><label>Port</label><input name="bk_pool_port" type="number" min="1" max="65535"></div></div>
<label>Backup wallet <small>(blank = same as main)</small></label><input name="bk_wallet" maxlength="120" spellcheck="false">
</div>

<h2>Screen &amp; LED</h2><div class="card">
<div class="row"><div><label>Timezone</label><select name="tz" id="tz"></select></div><div><label>Brightness %</label><input name="brightness" type="number" min="1" max="100"></div></div>
<div class="row"><div><label>Screen rotation</label><select name="rotation"><option value="0">0&deg;</option><option value="1">90&deg;</option><option value="2">180&deg;</option><option value="3">270&deg;</option></select></div>
<div><label>Color theme</label><select name="invert"><option value="0">Dark</option><option value="1">Light</option></select></div></div>
<label class="ck"><input type="checkbox" name="night" value="1">Night mode (dim screen, LED off)</label>
<div class="row3"><div><label>From (hour)</label><input name="n_start" type="number" min="0" max="23"></div><div><label>To (hour)</label><input name="n_end" type="number" min="0" max="23"></div><div><label>Night brightness %</label><input name="n_bright" type="number" min="0" max="100"></div></div>
<small>0% turns the screen off. Tap the screen to light it up for a minute.</small>
<label>Turn the screen off when not touched for</label><select name="scr_off"><option value="0">Never</option><option value="1">1 minute</option><option value="5">5 minutes</option><option value="15">15 minutes</option><option value="30">30 minutes</option><option value="60">1 hour</option></select>
<small>Tap the screen or press BOOT to wake it (the first tap only wakes).</small>
<label class="ck"><input type="checkbox" name="auto_br" value="1">Auto brightness from room light</label>
<label class="ck"><input type="checkbox" name="dark_off" value="1">Screen off while the room is dark</label>
<small>Light sensor now: <b id="lt">-</b> &middot; lit: <b id="lb">-</b> &middot; dark: <b id="ld">-</b></small>
<div class="row" style="grid-template-columns:1fr 1fr"><button type="button" class="sec" id="lcb">Room is lit now</button><button type="button" class="sec" id="lcd">Room is dark now</button></div>
<small>Calibrate once: press "lit" with the lights on, then "dark" with them off.</small>
<label class="ck"><input type="checkbox" name="led" value="1">Status LED (red blink = no WiFi, blue blink = no pool, green flash = share)</label>
<label class="ck"><input type="checkbox" name="https" value="1">Live BTC price &amp; network stats (HTTPS from the miner)</label>
</div>

<h2>Phone notifications</h2><div class="card">
<label>ntfy topic <small>(install the ntfy app and subscribe to the same topic &mdash; pick something hard to guess)</small></label><input name="ntfy" maxlength="64" autocomplete="off" spellcheck="false" placeholder="e.g. sparkminer-8f3k2q">
<label class="ck"><input type="checkbox" name="daily" value="1">Daily report at 09:00</label>
<small>Always sent: block found, pool down &gt;10 min, back online after an outage, miner restarted.</small>
<button type="button" class="sec" id="tn">Send test notification</button>
</div>

<h2>Security</h2><div class="card">
<label>New admin PIN <small>(4&ndash;8 digits; blank = keep; "-" = remove)</small></label><input name="new_pin" type="password" inputmode="numeric" maxlength="8" autocomplete="new-password">
</div>

<button type="submit">Save &amp; restart</button>
<div class="msg" id="msg"></div>
</form>

<h2>WiFi networks</h2><div class="card">
<p style="margin:10px 0 4px"><small>Joins the strongest saved network in range. On the miner: hold the screen 3 s for the WiFi menu.</small></p>
<div id="wl"></div>
<label>Add network</label>
<div class="row"><div><input id="ns" list="nl" placeholder="WiFi name" maxlength="32" autocomplete="off"><datalist id="nl"></datalist></div>
<div><button type="button" class="sec" id="sc" style="margin-top:0">Scan</button></div></div>
<input id="np" type="password" placeholder="Password (blank for open WiFi)" maxlength="64" style="margin-top:8px" autocomplete="new-password">
<button type="button" class="sec" id="na">Save network</button>
<div class="msg" id="msg3"></div>
<p style="margin:10px 0 0"><small>Setup hotspot password: <b id="app">(enter PIN)</b></small></p>
</div>

<h2>Maintenance</h2><div class="card">
<label class="ck"><input type="checkbox" form="f" name="auto_up" value="1" id="au">Install updates automatically (checked every 6 h)</label>
<small id="ust">Update status: -</small>
<button type="button" class="sec" id="cu">Check for update now</button>
<button type="button" class="sec" id="wp">Open WiFi setup hotspot</button>
<button type="button" class="sec" id="rb">Restart</button>
<label>Firmware update (.bin)</label><input type="file" id="fw" accept=".bin">
<button type="button" class="sec" id="up">Upload firmware</button>
<div class="msg" id="msg2"></div>
</div>
<p><small id="host"></small></p>
</main>
<script>
var $=function(i){return document.getElementById(i)},hasPin=false;
for(var i=-12;i<=14;i++){var o=document.createElement('option');o.value=i;o.text='UTC'+(i>=0?'+':'')+i;$('tz').appendChild(o)}
function hr(h){var u=['H/s','kH/s','MH/s','GH/s'],k=0;while(h>=1000&&k<3){h/=1000;k++}return h.toFixed(k?1:0)+' '+u[k]}
function up(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+h+'h '+m+'m'}
function num(x){return x>=1e6?x.toExponential(2):(+x).toFixed(x<10?4:1)}
function say(id,t,bad){$(id).textContent=t;$(id).className='msg'+(bad?' err':'')}
function post(url,body,id,okText){return fetch(url,{method:'POST',headers:{'X-PIN':$('pin').value},body:body})
.then(function(r){return r.text().then(function(t){say(id,okText&&r.ok?okText:t,!r.ok);return r.ok})}).catch(function(){say(id,'Miner not reachable',1)})}
function load(){fetch('/api/status').then(function(r){return r.json()}).then(function(s){
$('hr').textContent=hr(s.hashrate);$('best').textContent=num(s.best);$('sh').textContent=s.accepted+' / '+s.rejected;
$('blk').textContent=s.blocks;$('up').textContent=up(s.uptime);$('ping').textContent=s.ping+' ms';
$('pool').innerHTML='<span class="dot'+(s.pool_ok?' on':'')+'"></span>'+(s.pool||'-');
$('rssi').textContent=s.rssi+' dBm';$('tmp').textContent=Math.round(s.temp)+' \u00b0C';$('wn').textContent=s.ssid||'-';$('heap').textContent=Math.round(s.heap/1024)+' KB';
$('px').textContent=s.btc?'$'+Math.round(s.btc).toLocaleString():'-';$('bh').textContent=s.height||'-';
$('ust').textContent='Update status: '+s.update;odds(s);$('lt').textContent=s.light<0?'n/a':s.light;$('lb').textContent=s.light_bright||'-';$('ld').textContent=s.light_dark||'-';$('net').textContent=(s.net_hr||'-')+' / '+(s.net_diff||'-');$('host').textContent=s.host+' · '+s.ip+' · '+s.version;
}).catch(function(){})}
fetch('/api/config').then(function(r){return r.json()}).then(function(c){var f=$('f');
$('au').checked=!!c.auto_up;for(var k in c){var e=f[k];if(!e)continue;if(e.type=='checkbox')e.checked=!!c[k];else e.value=c[k]}
hasPin=c.has_pin;$('pinbar').style.display=hasPin?'block':'none';
if(c.wallet){var pp=c.pool_url.indexOf('public-pool.io')>=0;var a=document.createElement('a');
a.href=pp?'https://web.public-pool.io/#/app/'+encodeURIComponent(c.wallet):'https://mempool.space/address/'+encodeURIComponent(c.wallet);
a.textContent=pp?'Open pool stats ↗':'Open wallet on mempool.space ↗';a.target='_blank';$('links').appendChild(a)}});
load();setInterval(load,5000);
$('f').onsubmit=function(e){e.preventDefault();if(!confirm('Save settings and restart the miner?'))return;
var fd=new FormData($('f'));['night','led','https','daily','auto_up','auto_br','dark_off'].forEach(function(n){if(!fd.has(n))fd.append(n,'0')});
post('/api/save',new URLSearchParams(fd),'msg')};
$('tn').onclick=function(){var b=new URLSearchParams();b.append('ntfy',$('f').ntfy.value);post('/api/test-notify',b,'msg')};
function esc(t){var d=document.createElement('div');d.textContent=t;return d.innerHTML}
function wifi(){fetch('/api/wifi',{headers:{'X-PIN':$('pin').value}}).then(function(r){return r.json()}).then(function(w){
var h='';w.saved.forEach(function(n){h+='<div style="display:flex;align-items:center;justify-content:space-between;padding:6px 0;border-bottom:1px solid var(--line)"><span>'+(n==w.current?'<span class="dot on"></span>':'')+esc(n)+'</span><button type="button" class="sec" data-ssid="'+esc(n)+'" style="width:auto;margin:0;padding:6px 12px">Forget</button></div>'});
$('wl').innerHTML=h||'<small>No saved networks</small>';if(w.ap_pass)$('app').textContent=w.ap_pass;
Array.prototype.forEach.call($('wl').querySelectorAll('button'),function(b){b.onclick=function(){if(!confirm('Forget '+b.dataset.ssid+'?'))return;
var p=new URLSearchParams();p.append('ssid',b.dataset.ssid);post('/api/wifi/remove',p,'msg3').then(wifi)}})})}
wifi();$('pin').addEventListener('change',wifi);
$('sc').onclick=function(){say('msg3','Scanning...');fetch('/api/wifi/scan').then(function(r){return r.json()}).then(function(l){
$('nl').innerHTML=l.map(function(n){return '<option value="'+esc(n.ssid)+'">'+n.rssi+' dBm'+(n.saved?' (saved)':'')+'</option>'}).join('');
say('msg3',l.length+' networks found - pick one in the name box')})};
$('na').onclick=function(){var p=new URLSearchParams();p.append('ssid',$('ns').value);p.append('pass',$('np').value);
post('/api/wifi/add',p,'msg3').then(function(ok){if(ok){$('np').value='';wifi()}})};
// ---- 24 h charts: three small multiples on one time axis, synced crosshair
var H=null;
function chart(id,vals,fmt,i){var el=$(id),W=el.clientWidth||600,h=96,pl=34,pr=6,pt=6,pb=i==2?20:6;
var t=H.t,n=t.length,t0=t[0],t1=t[n-1],sp=Math.max(1,t1-t0);var v=vals.filter(function(x){return x!=null});
var lo=Math.min.apply(null,v),hi=Math.max.apply(null,v);if(hi-lo<1){hi+=1;lo-=1}var pad=(hi-lo)*.12;lo-=pad;hi+=pad;if(i==0){lo=0;hi=Math.max.apply(null,v)*1.15||1}
function X(tt){return pl+(tt-t0)/sp*(W-pl-pr)}function Y(y){return pt+(hi-y)/(hi-lo)*(h-pt-pb)}
var st=Math.pow(10,Math.floor(Math.log10((hi-lo)/3))),m=(hi-lo)/3/st;st*=m>5?10:m>2?5:m>1?2:1;var ticks=[];for(var tv=Math.ceil(lo/st)*st;tv<=hi;tv+=st)ticks.push(tv);
var g='',k;for(k=0;k<ticks.length;k++){var gv=ticks[k];g+='<line x1="'+pl+'" x2="'+(W-pr)+'" y1="'+Y(gv)+'" y2="'+Y(gv)+'" stroke="#2c2c2c"/><text x="'+(pl-4)+'" y="'+(Y(gv)+4)+'" fill="#9a9a9a" font-size="10" text-anchor="end">'+Math.round(gv)+'</text>'}
var off='';for(k=0;k<n;k++)if(!H.pool[k]||!H.rssi[k]){var x0=X(t[k]-H.interval/2),x1=X(t[k]+H.interval/2);off+='<rect x="'+x0+'" y="'+pt+'" width="'+Math.max(1,x1-x0)+'" height="'+(h-pt-pb)+'" fill="#ff505026"/>'}
var d='',pen=false;for(k=0;k<n;k++){if(vals[k]==null||(k&&t[k]-t[k-1]>H.interval*2.5)){pen=false;if(vals[k]==null)continue}d+=(pen?'L':'M')+X(t[k]).toFixed(1)+' '+Y(vals[k]).toFixed(1);pen=true}
var ax='';if(i==2){var hr=3600;for(var tt=Math.ceil(t0/(6*hr))*6*hr;tt<=t1;tt+=6*hr){var dt=new Date(tt*1000);ax+='<text x="'+X(tt)+'" y="'+(h-4)+'" fill="#9a9a9a" font-size="10" text-anchor="middle">'+('0'+dt.getHours()).slice(-2)+':00</text>'}}
el.innerHTML='<svg viewBox="0 0 '+W+' '+h+'" width="'+W+'" height="'+h+'">'+off+g+'<path d="'+d+'" fill="none" stroke="#ff6800" stroke-width="2" stroke-linejoin="round"/>'+ax+'<line class="xh" y1="'+pt+'" y2="'+(h-pb)+'" stroke="#f2f2f2" stroke-width="1" visibility="hidden"/><circle class="dt" r="4" fill="#ff6800" stroke="#0d0d0d" stroke-width="2" visibility="hidden"/></svg>';
el._X=X;el._Y=Y;el._v=vals}
var SER=[{k:'hr',f:function(v){return v>=1000?(v/1000).toFixed(2)+' MH/s':Math.round(v)+' kH/s'}},{k:'temp',f:function(v){return Math.round(v)+' \u00b0C'}},{k:'rssi',f:function(v){return Math.round(v)+' dBm'}}];
function drawHist(){if(!H)return;if(H.t.length<2){$('hnote').textContent='Collecting data - the first points appear within 10 minutes.';return}
SER.forEach(function(s,i){var vals=H[s.k].map(function(v,j){return(s.k=='rssi'&&!H.rssi[j])||(s.k=='hr'&&!H.pool[j])?null:v});chart('c'+i,vals,s.f,i);
var last=vals.filter(function(x){return x!=null});$('cv'+i).textContent=last.length?s.f(last[last.length-1]):''})}
function hover(e){if(!H||H.t.length<2)return;var box=$('c0').getBoundingClientRect(),px=e.clientX-box.left,el0=$('c0');
var W=box.width,pl=34,pr=6,t=H.t,f=(px-pl)/(W-pl-pr);if(f<0||f>1){hide();return}var tt=t[0]+f*(t[t.length-1]-t[0]),j=0;
for(var k=1;k<t.length;k++)if(Math.abs(t[k]-tt)<Math.abs(t[j]-tt))j=k;var html=new Date(t[j]*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'});
SER.forEach(function(s,i){var el=$('c'+i),svg=el.querySelector('svg');if(!svg)return;var sc=svg.viewBox.baseVal.width/box.width,x=el._X(t[j]);
var xh=svg.querySelector('.xh'),dt=svg.querySelector('.dt');xh.setAttribute('x1',x);xh.setAttribute('x2',x);xh.setAttribute('visibility','visible');
var v=el._v[j];if(v!=null){dt.setAttribute('cx',x);dt.setAttribute('cy',el._Y(v));dt.setAttribute('visibility','visible')}else dt.setAttribute('visibility','hidden');
html+='<br>'+['Hashrate','Temp','Signal'][i]+': '+(v==null?'offline':s.f(v))});
var tip=$('tip'),hb=$('hist').getBoundingClientRect();tip.innerHTML=html;tip.style.display='block';
var left=e.clientX-hb.left+12;if(left+130>hb.width)left=e.clientX-hb.left-140;tip.style.left=left+'px';tip.style.top=(e.clientY-hb.top+12)+'px'}
function hide(){$('tip').style.display='none';Array.prototype.forEach.call(document.querySelectorAll('.xh,.dt'),function(n){n.setAttribute('visibility','hidden')})}
['c0','c1','c2'].forEach(function(id){var el=$(id);el.addEventListener('pointermove',hover);el.addEventListener('pointerdown',hover);el.addEventListener('pointerleave',hide)});
function loadHist(){fetch('/api/history').then(function(r){return r.json()}).then(function(h){H=h;drawHist()}).catch(function(){})}
loadHist();setInterval(loadHist,300000);window.addEventListener('resize',drawHist);

// ---- Odds (same maths as the miner's Odds screen)
function big(v){var u=[[1e18,'quintillion'],[1e15,'quadrillion'],[1e12,'trillion'],[1e9,'billion'],[1e6,'million']];
for(var i=0;i<u.length;i++)if(v>=u[i][0])return (v/u[i][0]).toFixed(v/u[i][0]<10?1:0)+' '+u[i][1];return Math.round(v).toLocaleString()}
function odds(s){if(!s.net_diff_raw||!s.hashrate)return;var E=s.net_diff_raw*4294967296/s.hashrate,day=1-Math.exp(-86400/E),yr=1-Math.exp(-31557600/E);
var rows=[['Expected wait','~'+big(E/31557600)+' years'],['Chance today','1 in '+big(1/day)],['Chance this year','1 in '+big(1/yr)],
['Lottery 1st prize (1 in 1,000,000)',big(1e-6/yr)+'x more likely'],['Your best share',(+s.best).toPrecision(3)+' of '+big(s.net_diff_raw)+' needed'],
['Work done so far',(s.hashes/(s.net_diff_raw*4294967296)*100).toExponential(1)+'% of one expected block']];
$('odds').innerHTML=rows.map(function(r){return '<span>'+r[0]+'</span><b>'+r[1]+'</b>'}).join('')}

$('lcb').onclick=function(){var p=new URLSearchParams();p.append('which','bright');post('/api/light-cal',p,'msg')};
$('lcd').onclick=function(){var p=new URLSearchParams();p.append('which','dark');post('/api/light-cal',p,'msg')};
$('cu').onclick=function(){post('/api/check-update','','msg2')};
$('rb').onclick=function(){if(confirm('Restart the miner?'))post('/api/restart','','msg2')};
$('wp').onclick=function(){if(confirm('Reboot into the WiFi setup hotspot? Mining pauses until it is set up or 5 minutes pass.'))post('/api/portal','','msg2')};
$('up').onclick=function(){var f=$('fw').files[0];if(!f){say('msg2','Choose a .bin file first',1);return}
if(!confirm('Install '+f.name+' ('+Math.round(f.size/1024)+' KB)?'))return;say('msg2','Uploading...');
var fd=new FormData();fd.append('firmware',f,f.name);post('/api/update',fd,'msg2')};
</script></body></html>)HTML";

// ============================================================
// Helpers
// ============================================================

// Append s to out as a JSON string literal (with quotes)
static void jsonStr(String &out, const char *s) {
    out += '"';
    for (; s && *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    out += '"';
}

static void copyArg(const char *name, char *dst, size_t cap, bool keepIfBlank = false) {
    if (!s_server.hasArg(name)) return;
    String v = s_server.arg(name);
    v.trim();
    if (keepIfBlank && v.length() == 0) return;
    strncpy(dst, v.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

static bool argFlag(const char *name, bool current) {
    return s_server.hasArg(name) ? s_server.arg(name).toInt() == 1 : current;
}

static bool pinOk() {
    const char *pin = extras_get()->pin;
    if (!pin[0]) return true;
    return s_server.header("X-PIN") == pin;
}

// Reject a request without the right PIN (slowed down to make guessing tedious)
static bool requirePin() {
    if (pinOk()) return true;
    delay(1500);
    s_server.send(403, "text/plain", "Wrong or missing PIN.");
    return false;
}

static bool validTopic(const String &t) {
    if (t.length() > 64) return false;
    for (size_t i = 0; i < t.length(); i++) {
        char c = t[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    }
    return true;
}

// ============================================================
// Handlers
// ============================================================

static void handleRoot() {
    s_server.send_P(200, "text/html", PAGE_HTML);
}

static void handleStatus() {
    const display_data_t *d = monitor_get_data();
    char buf[64];
    String j;
    j.reserve(640);
    j += "{\"hashrate\":";  j += String(d->hashRate, 1);
    j += ",\"best\":";      snprintf(buf, sizeof(buf), "%.6g", d->bestDifficulty); j += buf;
    j += ",\"accepted\":";  j += d->sharesAccepted;
    j += ",\"rejected\":";  j += d->sharesRejected;
    j += ",\"blocks\":";    j += d->blocksFound;
    j += ",\"hashes\":";    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)d->totalHashes); j += buf;
    j += ",\"uptime\":";    j += d->uptimeSeconds;
    j += ",\"ping\":";      j += d->avgLatency;
    j += ",\"pool_ok\":";   j += d->poolConnected ? "true" : "false";
    j += ",\"pool\":";      jsonStr(j, d->poolName);
    j += ",\"rssi\":";      j += (int)WiFi.RSSI();
    j += ",\"ssid\":";      jsonStr(j, WiFi.SSID().c_str());
    j += ",\"temp\":";      snprintf(buf, sizeof(buf), "%.1f", temperatureRead()); j += buf;
    j += ",\"heap\":";      j += ESP.getFreeHeap();
    j += ",\"btc\":";       snprintf(buf, sizeof(buf), "%.0f", d->btcPrice); j += buf;
    j += ",\"height\":";    j += d->blockHeight;
    j += ",\"net_hr\":";    jsonStr(j, d->networkHashrate);
    j += ",\"net_diff\":";  jsonStr(j, d->networkDifficulty);
    j += ",\"ip\":";        jsonStr(j, WiFi.localIP().toString().c_str());
    j += ",\"host\":";      snprintf(buf, sizeof(buf), "%s.local", s_hostname); jsonStr(j, buf);
    j += ",\"version\":";   jsonStr(j, AUTO_VERSION);
    j += ",\"reset_reason\":"; j += (int)esp_reset_reason();
    j += ",\"build\":";     j += (uint32_t)FW_BUILD;
    j += ",\"screen_on\":"; j += extras_is_dimmed() ? "false" : "true";
    j += ",\"light\":";     j += extras_light();
    j += ",\"light_bright\":"; j += extras_get()->lightBright;
    j += ",\"light_dark\":"; j += extras_get()->lightDark;
    j += ",\"net_diff_raw\":"; snprintf(buf, sizeof(buf), "%.6g", d->networkDifficultyRaw); j += buf;
    j += ",\"update\":";    jsonStr(j, fw_update_state());
    #ifdef HAVE_TOUCH_POSITION
        uint16_t th[4][2];
        touch_history(th);
        j += ",\"taps\":[";
        for (int i = 0; i < 4; i++) {
            if (i) j += ",";
            j += "["; j += th[i][0]; j += ","; j += th[i][1]; j += "]";
        }
        j += "]";
    #endif
    j += "}";
    s_server.send(200, "application/json", j);
}

// Current settings, without secrets (passwords and PIN are write-only)
static void handleConfig() {
    miner_config_t *c = nvs_config_get();
    extras_config_t *x = extras_get();
    String j;
    j.reserve(800);
    j += "{\"ssid\":";          jsonStr(j, c->ssid);
    j += ",\"wallet\":";        jsonStr(j, c->wallet);
    j += ",\"worker\":";        jsonStr(j, c->workerName);
    j += ",\"pool_url\":";      jsonStr(j, c->poolUrl);
    j += ",\"pool_port\":";     j += c->poolPort;
    j += ",\"bk_pool_url\":";   jsonStr(j, c->backupPoolUrl);
    j += ",\"bk_pool_port\":";  j += c->backupPoolPort;
    j += ",\"bk_wallet\":";     jsonStr(j, c->backupWallet);
    j += ",\"tz\":";            j += c->timezoneOffset;
    j += ",\"brightness\":";    j += c->brightness;
    j += ",\"rotation\":";      j += c->rotation;
    j += ",\"invert\":";        j += c->invertColors ? 1 : 0;
    j += ",\"https\":";         j += c->enableHttpsStats ? 1 : 0;
    j += ",\"night\":";         j += x->nightEnabled ? 1 : 0;
    j += ",\"n_start\":";       j += x->nightStart;
    j += ",\"n_end\":";         j += x->nightEnd;
    j += ",\"n_bright\":";      j += x->nightBright;
    j += ",\"led\":";           j += x->ledEnabled ? 1 : 0;
    j += ",\"ntfy\":";          jsonStr(j, x->ntfyTopic);
    j += ",\"daily\":";         j += x->notifyDaily ? 1 : 0;
    j += ",\"auto_up\":";       j += x->autoUpdate ? 1 : 0;
    j += ",\"scr_off\":";       j += x->screenOffMin;
    j += ",\"auto_br\":";       j += x->autoBright ? 1 : 0;
    j += ",\"dark_off\":";      j += x->darkOff ? 1 : 0;
    j += ",\"has_pin\":";       j += x->pin[0] ? "true" : "false";
    j += "}";
    s_server.send(200, "application/json", j);
}

static void handleSave() {
    if (!requirePin()) return;
    miner_config_t *c = nvs_config_get();
    extras_config_t *x = extras_get();

    // Validate before touching anything
    if (s_server.hasArg("wallet") && s_server.arg("wallet").length() < 20) {
        s_server.send(400, "text/plain", "Wallet address looks too short - nothing saved.");
        return;
    }
    if (s_server.hasArg("ssid") && s_server.arg("ssid").length() == 0) {
        s_server.send(400, "text/plain", "WiFi name cannot be empty - nothing saved.");
        return;
    }
    String topic = s_server.arg("ntfy");
    topic.trim();
    if (!validTopic(topic)) {
        s_server.send(400, "text/plain", "ntfy topic: use only letters, digits, - and _ - nothing saved.");
        return;
    }
    String newPin = s_server.arg("new_pin");
    newPin.trim();
    bool removePin = newPin == "-";
    if (newPin.length() && !removePin) {
        bool digits = newPin.length() >= 4 && newPin.length() <= 8;
        for (size_t i = 0; digits && i < newPin.length(); i++) digits = isdigit((unsigned char)newPin[i]);
        if (!digits) {
            s_server.send(400, "text/plain", "PIN must be 4-8 digits - nothing saved.");
            return;
        }
    }

    copyArg("ssid", c->ssid, sizeof(c->ssid));
    copyArg("wifipass", c->wifiPassword, sizeof(c->wifiPassword), true);
    copyArg("wallet", c->wallet, sizeof(c->wallet));
    copyArg("worker", c->workerName, sizeof(c->workerName));
    copyArg("pool_url", c->poolUrl, sizeof(c->poolUrl));
    copyArg("pool_pass", c->poolPassword, sizeof(c->poolPassword), true);
    copyArg("bk_pool_url", c->backupPoolUrl, sizeof(c->backupPoolUrl));
    copyArg("bk_wallet", c->backupWallet, sizeof(c->backupWallet));

    int port = s_server.arg("pool_port").toInt();
    if (port > 0 && port <= 65535) c->poolPort = port;
    port = s_server.arg("bk_pool_port").toInt();
    if (port > 0 && port <= 65535) c->backupPoolPort = port;

    if (s_server.hasArg("tz")) c->timezoneOffset = constrain(s_server.arg("tz").toInt(), -12, 14);
    if (s_server.hasArg("brightness")) c->brightness = constrain(s_server.arg("brightness").toInt(), 1, 100);
    if (s_server.hasArg("rotation")) c->rotation = constrain(s_server.arg("rotation").toInt(), 0, 3);
    if (s_server.hasArg("invert")) c->invertColors = s_server.arg("invert").toInt() == 1;
    c->enableHttpsStats = argFlag("https", c->enableHttpsStats);

    x->nightEnabled = argFlag("night", x->nightEnabled);
    if (s_server.hasArg("n_start")) x->nightStart = constrain(s_server.arg("n_start").toInt(), 0, 23);
    if (s_server.hasArg("n_end")) x->nightEnd = constrain(s_server.arg("n_end").toInt(), 0, 23);
    if (s_server.hasArg("n_bright")) x->nightBright = constrain(s_server.arg("n_bright").toInt(), 0, 100);
    x->ledEnabled = argFlag("led", x->ledEnabled);
    x->autoUpdate = argFlag("auto_up", x->autoUpdate);
    x->autoBright = argFlag("auto_br", x->autoBright);
    x->darkOff = argFlag("dark_off", x->darkOff);
    if (s_server.hasArg("scr_off")) x->screenOffMin = constrain(s_server.arg("scr_off").toInt(), 0, 240);
    x->notifyDaily = argFlag("daily", x->notifyDaily);
    if (s_server.hasArg("ntfy")) strlcpy(x->ntfyTopic, topic.c_str(), sizeof(x->ntfyTopic));
    if (removePin) x->pin[0] = '\0';
    else if (newPin.length()) strlcpy(x->pin, newPin.c_str(), sizeof(x->pin));

    if (!nvs_config_save(c) || !extras_save()) {
        s_server.send(500, "text/plain", "Failed to save settings.");
        return;
    }
    Serial.println("[WEB] Settings saved - restarting");
    s_server.send(200, "text/plain",
        "Saved. Restarting... If the WiFi details are wrong, the miner opens its setup hotspot again.");
    s_restartAt = millis() + 1500;
}

static void handleTestNotify() {
    if (!requirePin()) return;
    extras_config_t *x = extras_get();
    String topic = s_server.arg("ntfy");
    topic.trim();
    if (!topic.length() || !validTopic(topic)) {
        s_server.send(400, "text/plain", "Enter a valid ntfy topic first.");
        return;
    }
    // Try the topic typed in the form without saving it
    char saved[sizeof(x->ntfyTopic)];
    strlcpy(saved, x->ntfyTopic, sizeof(saved));
    strlcpy(x->ntfyTopic, topic.c_str(), sizeof(x->ntfyTopic));
    bool ok = extras_notify("SparkMiner test", "Notifications are working.", "white_check_mark", 3);
    strlcpy(x->ntfyTopic, saved, sizeof(x->ntfyTopic));
    s_server.send(ok ? 200 : 502, "text/plain",
        ok ? "Sent - check the ntfy app. Remember to Save." : "Could not reach ntfy.sh.");
}

// Saved networks (passwords never leave the miner)
static void handleWifiList() {
    String j = "{\"current\":";
    jsonStr(j, WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "");
    j += ",\"saved\":[";
    for (int i = 0; i < wifi_store_count(); i++) {
        if (i) j += ",";
        jsonStr(j, wifi_store_get(i)->ssid);
    }
    j += "]";
    if (pinOk()) { j += ",\"ap_pass\":"; jsonStr(j, extras_ap_password()); }
    j += "}";
    s_server.send(200, "application/json", j);
}

static void handleWifiScan() {
    int n = WiFi.scanNetworks(false, false);
    String j = "[";
    int out = 0;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        bool dup = false;
        for (int k = 0; k < i; k++) if (WiFi.SSID(k) == ssid) dup = true;   // Sorted strongest first
        if (dup) continue;
        if (out++) j += ",";
        j += "{\"ssid\":"; jsonStr(j, ssid.c_str());
        j += ",\"rssi\":"; j += WiFi.RSSI(i);
        j += ",\"saved\":"; j += wifi_store_find(ssid.c_str()) ? "true" : "false";
        j += "}";
    }
    j += "]";
    WiFi.scanDelete();
    s_server.send(200, "application/json", j);
}

static void handleWifiAdd() {
    if (!requirePin()) return;
    String ssid = s_server.arg("ssid"), pass = s_server.arg("pass");
    if (!ssid.length() || ssid.length() > 32) { s_server.send(400, "text/plain", "WiFi name must be 1-32 characters."); return; }
    if (pass.length() && (pass.length() < 8 || pass.length() > 64)) {
        s_server.send(400, "text/plain", "WiFi passwords are 8-64 characters (blank for open networks).");
        return;
    }
    if (!wifi_store_put(ssid.c_str(), pass.c_str())) { s_server.send(500, "text/plain", "Could not save."); return; }
    s_server.send(200, "text/plain", "Saved. The miner will use it whenever it is the strongest saved network in range.");
}

static void handleWifiRemove() {
    if (!requirePin()) return;
    String ssid = s_server.arg("ssid");
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid && wifi_store_count() == 1) {
        s_server.send(400, "text/plain", "That is the only saved network - add another one first.");
        return;
    }
    s_server.send(wifi_store_remove(ssid.c_str()) ? 200 : 404, "text/plain", "Removed.");
}

static void handleHistory() {
    String j;
    history_json(j);
    s_server.send(200, "application/json", j);
}

static void handleLightCal() {
    if (!requirePin()) return;
    if (extras_light() < 0) { s_server.send(400, "text/plain", "No light sensor reading yet."); return; }
    bool bright = s_server.arg("which") == "bright";
    extras_light_calibrate(bright);
    String msg = String(bright ? "Lit" : "Dark") + " level saved (" + extras_light() + ").";
    if (extras_get()->lightBright == extras_get()->lightDark) msg += " Do the other one too.";
    s_server.send(200, "text/plain", msg);
}

static void handleCheckUpdate() {
    if (!requirePin()) return;
    fw_update_request();
    s_server.send(200, "text/plain", "Checking for an update - watch the status line.");
}

static void handleRestart() {
    if (!requirePin()) return;
    s_server.send(200, "text/plain", "Restarting...");
    s_restartAt = millis() + 1000;
}

static void handlePortal() {
    if (!requirePin()) return;
    s_server.send(200, "text/plain",
        "Rebooting into the setup hotspot - join the SparkMiner_ WiFi shown on the screen.");
    s_portalAfter = true;
    s_restartAt = millis() + 1000;
}

// Firmware upload: body chunks arrive here before handleUpdateDone runs
static void handleUpdateUpload() {
    HTTPUpload &up = s_server.upload();
    if (up.status == UPLOAD_FILE_START) {
        s_otaAuthorized = pinOk();
        s_otaFailed = false;
        s_otaBytes = 0;
        if (!s_otaAuthorized) return;
        Serial.printf("[OTA] Receiving %s\n", up.filename.c_str());
        // A single-app partition table has no spare slot: the "next" partition is
        // the running one, and erasing it aborts. Refuse instead.
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (!next || next == esp_ota_get_running_partition()) {
            Serial.println("[OTA] No spare OTA slot - flash once over USB with the OTA partition table");
            s_otaFailed = true;
            return;
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
            s_otaFailed = true;
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!s_otaAuthorized || s_otaFailed) return;
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.printError(Serial);
            s_otaFailed = true;
        }
        s_otaBytes += up.currentSize;
    } else if (up.status == UPLOAD_FILE_END) {
        if (!s_otaAuthorized || s_otaFailed) return;
        // Image verification hashes with the SHA unit the miner drives directly
        otaHold(true);
        if (!Update.end(true)) {
            Update.printError(Serial);
            s_otaFailed = true;
        }
        otaHold(false);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        s_otaFailed = true;
        otaHold(false);
    }
}

static void handleUpdateDone() {
    otaHold(false);  // In case the upload ended without an END/ABORT event
    if (!s_otaAuthorized) {
        delay(1500);
        s_server.send(403, "text/plain", "Wrong or missing PIN.");
        return;
    }
    if (s_otaFailed || !Update.isFinished()) {
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (!next || next == esp_ota_get_running_partition()) {
            s_server.send(500, "text/plain",
                "Update failed: this board has no spare firmware slot. Flash once over USB first.");
            return;
        }
        String err = String("Update failed: ") + Update.errorString();
        s_server.send(500, "text/plain", err);
        return;
    }
    Serial.printf("[OTA] Update OK (%u bytes) - restarting\n", (unsigned)s_otaBytes);
    s_server.send(200, "text/plain", "Update installed. Restarting...");
    s_restartAt = millis() + 1000;
}



// ============================================================
// Public API
// ============================================================

void web_ui_start() {
    if (s_started || WiFi.status() != WL_CONNECTED) return;

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_hostname, sizeof(s_hostname), "sparkminer-%02x%02x", mac[4], mac[5]);
    if (MDNS.begin(s_hostname)) {
        MDNS.addService("http", "tcp", 80);
    }

    static const char *headers[] = {"X-PIN"};
    s_server.collectHeaders(headers, 1);

    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/api/status", HTTP_GET, handleStatus);
    s_server.on("/api/config", HTTP_GET, handleConfig);
    s_server.on("/api/save", HTTP_POST, handleSave);
    s_server.on("/api/test-notify", HTTP_POST, handleTestNotify);
    s_server.on("/api/restart", HTTP_POST, handleRestart);
    s_server.on("/api/check-update", HTTP_POST, handleCheckUpdate);
    s_server.on("/api/wifi", HTTP_GET, handleWifiList);
    s_server.on("/api/history", HTTP_GET, handleHistory);
    s_server.on("/api/light-cal", HTTP_POST, handleLightCal);
    s_server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    s_server.on("/api/wifi/add", HTTP_POST, handleWifiAdd);
    s_server.on("/api/wifi/remove", HTTP_POST, handleWifiRemove);
    s_server.on("/api/portal", HTTP_POST, handlePortal);
    s_server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    s_server.onNotFound([]() { s_server.send(404, "text/plain", "Not found"); });
    s_server.begin();
    s_started = true;

    Serial.printf("[WEB] UI at http://%s/ (http://%s.local/)\n",
                  WiFi.localIP().toString().c_str(), s_hostname);
}

void web_ui_task(void *param) {
    for (;;) {
        if (!s_started) web_ui_start();   // No-op until WiFi is connected
        if (s_started) s_server.handleClient();
        if (s_restartAt && (int32_t)(millis() - s_restartAt) >= 0) {
            history_save();
            if (s_portalAfter) extras_request_portal();  // reboots
            ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
