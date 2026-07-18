#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

static DNSServer sDns;
static WebServer sServer(80);
static PortalSubmitCallback sOnSubmit = nullptr;
static bool sRunning = false;

static String sApSsid;
static String sApPassword;

static const IPAddress kApIp(192, 168, 4, 1);

// --- Access point identity --------------------------------------------------

// Ambiguous glyphs (0/O, 1/l/I) are left out: the password is shown on the
// e-paper as a fallback for anyone typing it by hand.
static String randomPassword(size_t length) {
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
    String out;
    out.reserve(length);
    for (size_t i = 0; i < length; i++) {
        out += alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    return out;
}

static String deviceSuffix() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[5];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return String(buf);
}

// --- Pages ------------------------------------------------------------------

// Escapes the few characters that would break out of an HTML attribute or
// element. SSIDs are attacker-influenced in the sense that any nearby network
// name lands in this page.
static String escapeHtml(const String &in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

static const char kStyle[] PROGMEM = R"CSS(
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
     margin:0;padding:24px;background:#f4f4f2;color:#111;line-height:1.5}
.card{max-width:420px;margin:0 auto;background:#fff;border-radius:14px;
      padding:24px;box-shadow:0 1px 3px rgba(0,0,0,.12)}
h1{font-size:20px;margin:0 0 4px}
p.sub{margin:0 0 20px;color:#666;font-size:14px}
label{display:block;font-size:13px;font-weight:600;margin:16px 0 6px}
select,input{width:100%;padding:12px;font-size:16px;border:1px solid #ccc;
             border-radius:8px;background:#fff}
button{width:100%;margin-top:22px;padding:14px;font-size:16px;font-weight:600;
       color:#fff;background:#111;border:0;border-radius:8px}
.tz{margin-top:16px;font-size:13px;color:#666}
</style>
)CSS";

static void handleRoot() {
    // Cached scan results would go stale while the user is standing there, so
    // scan on each load. Takes a couple of seconds.
    int found = WiFi.scanNetworks();

    String page = F("<!doctype html><html><head><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>Quote of the Day setup</title>");
    page += FPSTR(kStyle);
    page += F("</head><body><div class='card'>"
              "<h1>Connect to Wi-Fi</h1>"
              "<p class='sub'>Pick your home network so the display can fetch the date.</p>"
              "<form method='POST' action='/save'>"
              "<label for='ssid'>Network</label>"
              "<select id='ssid' name='ssid'>");

    if (found <= 0) {
        page += F("<option value=''>-- no networks found --</option>");
    }
    for (int i = 0; i < found; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        String safe = escapeHtml(ssid);
        page += "<option value='" + safe + "'>" + safe + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    WiFi.scanDelete();

    page += F("</select>"
              "<label for='pass'>Password</label>"
              "<input id='pass' name='pass' type='password' autocomplete='off'>"
              "<input type='hidden' id='tz' name='tz'>"
              "<input type='hidden' id='tzname' name='tzname'>"
              "<p class='tz' id='tzlabel'>Detecting timezone&hellip;</p>"
              "<button type='submit'>Save</button>"
              "</form></div>");

    // The browser already knows the timezone, so we never ask for it. This
    // derives a POSIX TZ string (what configTzTime wants) by finding this
    // year's DST transitions, rather than shipping a 470-zone lookup table.
    page += F(R"JS(
<script>
function tzPosix(){
  try{
    var Y=new Date().getFullYear();
    var offAt=function(t){return -new Date(t).getTimezoneOffset();};
    var janT=Date.UTC(Y,0,1), julT=Date.UTC(Y,6,1);
    var jan=offAt(janT), jul=offAt(julT);
    var std=Math.min(jan,jul), dst=Math.max(jan,jul);
    var fmt=function(m){var s=m<=0?'+':'-';m=Math.abs(m);
      var h=Math.floor(m/60),r=m%60;return s+h+(r?':'+(r<10?'0':'')+r:'');};
    var abbr=function(t,fb){try{
        var p=new Intl.DateTimeFormat('en-US',{timeZoneName:'short'}).formatToParts(new Date(t));
        for(var i=0;i<p.length;i++) if(p[i].type=='timeZoneName'){
          var v=p[i].value.replace(/[^A-Za-z]/g,'');
          if(v.length>=3&&v!='GMT'&&v!='UTC') return v;
        }}catch(e){} return fb;};
    var stdName=abbr(jan===std?janT:julT,'STD');
    var dstName=abbr(jan===dst?janT:julT,'DST');
    if(std===dst) return stdName+fmt(std);
    var pts=[],prev=jan;
    for(var t=janT+86400000;t<Date.UTC(Y+1,0,1);t+=86400000){
      var o=offAt(t);
      if(o!==prev){
        var lo=t-86400000,hi=t;
        while(hi-lo>60000){var mid=Math.floor((lo+hi)/2);
          if(offAt(mid)===prev) lo=mid; else hi=mid;}
        pts.push(hi); prev=o;
      }
    }
    if(pts.length<2) return stdName+fmt(std);
    var rule=function(t){var d=new Date(t);
      var m=d.getMonth()+1,dow=d.getDay(),dom=d.getDate();
      var last=new Date(d.getFullYear(),m,0).getDate();
      var w=(dom+7>last)?5:Math.ceil(dom/7);
      return 'M'+m+'.'+w+'.'+dow+'/'+d.getHours();};
    var s=null,e=null;
    for(var i=0;i<pts.length;i++){ if(offAt(pts[i])===dst) s=pts[i]; else e=pts[i]; }
    if(s===null||e===null) return stdName+fmt(std);
    return stdName+fmt(std)+dstName+fmt(dst)+','+rule(s)+','+rule(e);
  }catch(err){ return ''; }
}
var name='';
try{ name=Intl.DateTimeFormat().resolvedOptions().timeZone||''; }catch(e){}
var posix=tzPosix();
document.getElementById('tz').value=posix;
document.getElementById('tzname').value=name;
document.getElementById('tzlabel').textContent =
  posix ? ('Timezone detected: '+(name||posix)) : 'Timezone could not be detected.';
</script>
)JS");

    page += F("</body></html>");
    sServer.send(200, "text/html", page);
}

static void handleSave() {
    String ssid = sServer.arg("ssid");
    String pass = sServer.arg("pass");
    String tz = sServer.arg("tz");
    String tzName = sServer.arg("tzname");

    if (ssid.isEmpty()) {
        sServer.sendHeader("Location", "/");
        sServer.send(302, "text/plain", "");
        return;
    }

    String page = F("<!doctype html><html><head><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>Saved</title>");
    page += FPSTR(kStyle);
    page += F("</head><body><div class='card'><h1>Saved</h1>"
              "<p class='sub'>The display is connecting now. This setup network "
              "will disappear in a moment - that is expected.</p>"
              "<p class='sub'>Check the screen: it shows the quote once connected, "
              "or an error if the password was wrong.</p>"
              "</div></body></html>");
    sServer.send(200, "text/html", page);

    // Let the response reach the browser before the caller retunes the radio.
    sServer.client().flush();
    delay(200);

    if (sOnSubmit) sOnSubmit(ssid, pass, tz, tzName);
}

// Anything else redirects to the portal root, which is what triggers the
// "sign in to network" sheet on iOS and Android.
static void handleNotFound() {
    sServer.sendHeader("Location", String("http://") + kApIp.toString() + "/");
    sServer.send(302, "text/plain", "");
}

// --- Lifecycle --------------------------------------------------------------

void portalBegin(PortalSubmitCallback onSubmit) {
    if (sRunning) return;

    sOnSubmit = onSubmit;

    sApSsid = String("QuoteDisplay-") + deviceSuffix();
    sApPassword = randomPassword(8);

    // AP_STA rather than AP: the station interface is what makes scanning for
    // the user's own networks possible.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
    WiFi.softAP(sApSsid.c_str(), sApPassword.c_str());

    sDns.setErrorReplyCode(DNSReplyCode::NoError);
    sDns.start(53, "*", kApIp);  // wildcard: every lookup lands on us

    // Registered once for the lifetime of the process: WebServer appends to a
    // handler list rather than replacing, so calling this on every return to
    // setup mode would pile up duplicate routes.
    static bool routesRegistered = false;
    if (!routesRegistered) {
        sServer.on("/", handleRoot);
        sServer.on("/save", HTTP_POST, handleSave);
        sServer.onNotFound(handleNotFound);
        routesRegistered = true;
    }
    sServer.begin();

    sRunning = true;
    Serial.printf("[portal] AP \"%s\" password \"%s\" at %s\n",
                  sApSsid.c_str(), sApPassword.c_str(), kApIp.toString().c_str());
}

void portalLoop() {
    if (!sRunning) return;
    sDns.processNextRequest();
    sServer.handleClient();
}

void portalStop() {
    if (!sRunning) return;
    sServer.stop();
    sDns.stop();
    WiFi.softAPdisconnect(true);
    sRunning = false;
    Serial.println("[portal] stopped");
}

String portalSsid() {
    return sApSsid;
}

String portalPassword() {
    return sApPassword;
}

// In the Wi-Fi QR format these characters carry meaning and must be escaped
// inside a field value.
static String escapeQrValue(const String &in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

String portalQrPayload() {
    // Standard Wi-Fi network QR format. Fields are separated by COLONS -
    // "WIFI:T:WPA;S:name;P:key;;". Using '=' instead produces a code that scans
    // perfectly and then reports "no usable data", because the payload is not
    // recognised as network credentials.
    return "WIFI:T:WPA;S:" + escapeQrValue(sApSsid) +
           ";P:" + escapeQrValue(sApPassword) + ";;";
}
