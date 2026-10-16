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
<div class="tile"><span>BTC price</span><b id="px">-</b></div>
<div class="tile"><span>Block height</span><b id="bh">-</b></div>
<div class="tile wide"><span>Network hashrate / difficulty</span><b id="net" style="font-size:15px">-</b></div>
</div>
<p id="links"></p>

<div id="pinbar" class="card"><label>Admin PIN <small>(needed to change anything)</small></label><input id="pin" type="password" inputmode="numeric" autocomplete="current-password"></div>

<form id="f">
<h2>WiFi &amp; wallet</h2><div class="card">
<label>WiFi name (SSID)</label><input name="ssid" maxlength="63">
<label>WiFi password <small>(blank = keep current)</small></label><input name="wifipass" type="password" maxlength="64">
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
$('rssi').textContent=s.rssi+' dBm';$('heap').textContent=Math.round(s.heap/1024)+' KB';
$('px').textContent=s.btc?'$'+Math.round(s.btc).toLocaleString():'-';$('bh').textContent=s.height||'-';
$('ust').textContent='Update status: '+s.update;$('net').textContent=(s.net_hr||'-')+' / '+(s.net_diff||'-');$('host').textContent=s.host+' · '+s.ip+' · '+s.version;
}).catch(function(){})}
fetch('/api/config').then(function(r){return r.json()}).then(function(c){var f=$('f');
$('au').checked=!!c.auto_up;for(var k in c){var e=f[k];if(!e)continue;if(e.type=='checkbox')e.checked=!!c[k];else e.value=c[k]}
hasPin=c.has_pin;$('pinbar').style.display=hasPin?'block':'none';
if(c.wallet){var pp=c.pool_url.indexOf('public-pool.io')>=0;var a=document.createElement('a');
a.href=pp?'https://web.public-pool.io/#/app/'+encodeURIComponent(c.wallet):'https://mempool.space/address/'+encodeURIComponent(c.wallet);
a.textContent=pp?'Open pool stats ↗':'Open wallet on mempool.space ↗';a.target='_blank';$('links').appendChild(a)}});
load();setInterval(load,5000);
$('f').onsubmit=function(e){e.preventDefault();if(!confirm('Save settings and restart the miner?'))return;
var fd=new FormData($('f'));['night','led','https','daily','auto_up'].forEach(function(n){if(!fd.has(n))fd.append(n,'0')});
post('/api/save',new URLSearchParams(fd),'msg')};
$('tn').onclick=function(){var b=new URLSearchParams();b.append('ntfy',$('f').ntfy.value);post('/api/test-notify',b,'msg')};
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
    s_server.on("/api/portal", HTTP_POST, handlePortal);
    s_server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    s_server.onNotFound([]() { s_server.send(404, "text/plain", "Not found"); });
    s_server.begin();
    s_started = true;

    Serial.printf("[WEB] UI at http://%s/ (http://%s.local/)\n",
                  WiFi.localIP().toString().c_str(), s_hostname);
}

void web_ui_task(void *param) {
    web_ui_start();
    for (;;) {
        if (s_started) s_server.handleClient();
        if (s_restartAt && (int32_t)(millis() - s_restartAt) >= 0) {
            if (s_portalAfter) extras_request_portal();  // reboots
            ESP.restart();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
