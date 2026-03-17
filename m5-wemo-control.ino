/**
 * M5StickC Plus2 - Belkin WeMo Wall Outlet Controller (Multi-Device)
 *
 * Controls multiple Belkin WeMo smart plugs via WiFi using UPnP/SOAP.
 *
 * Hardware: M5StickC Plus2 (ESP32-based)
 *
 * Controls:
 *   Button A (M5 front button)  - Toggle current outlet ON/OFF
 *   Button B short press        - Cycle to next WeMo device
 *   Button B long press (1.5s)  - Re-scan for WeMo devices
 *   Button A                   - Wake from sleep
 *
 * Dependencies (install via Arduino Library Manager):
 *   - M5StickCPlus2  by M5Stack
 *   - WiFi           (built-in ESP32)
 *
 * Setup:
 *   1. Set WIFI_SSID and WIFI_PASSWORD in config.h.
 *   2. Optionally hard-code up to MAX_DEVICES static IPs, or leave
 *      STATIC_DEVICES empty to rely entirely on auto-discovery.
 */
#include "config.h"
#include <M5StickCPlus2.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>

// Optional: hard-coded devices. Set count to 0 to use discovery only.
// Format: { "IP", port }  or  { "IP", port, "Friendly Name" }
// A non-null name skips the setup.xml fetch and uses that string directly.
struct StaticDevice { const char* ip; int port; const char* name = nullptr; };
const StaticDevice STATIC_DEVICES[] = {
  // { "192.168.1.42", 49153 },
  // { "192.168.1.43", 49153, "Living Room" },
};
const int STATIC_DEVICE_COUNT = sizeof(STATIC_DEVICES) / sizeof(STATIC_DEVICES[0]);
// ─────────────────────────────────────────────────────────────────────────────

#define MAX_DEVICES     8
#define SSDP_PORT       1900
#define SSDP_TIMEOUT_MS 5000
#define LONG_PRESS_MS   1500

#define SLEEP_TIMEOUT_MS 60000   // idle time before sleep (ms); 0 = never sleep

#define WEMO_CONTROL_PATH "/upnp/control/basicevent1"
#define WEMO_SERVICE_TYPE "urn:Belkin:service:basicevent:1"

// ── Device list ───────────────────────────────────────────────────────────────
struct WemoDevice {
  String ip;
  int    port;
  String name;
  bool   on;
};

WemoDevice devices[MAX_DEVICES];
int deviceCount   = 0;
int currentDevice = 0;

// ── Button B long-press tracking ──────────────────────────────────────────────
unsigned long btnBPressTime = 0;
bool          btnBLongHandled = false;

// ── Sleep tracking ────────────────────────────────────────────────────────────
unsigned long lastActivityTime = 0;

// ── Forward declarations ──────────────────────────────────────────────────────
void     discoverWemo();
void     addDevice(const String& ip, int port, const String& overrideName = "");
String   fetchFriendlyName(const String& ip, int port, const String& locationPath);
bool     setBinaryState(int idx, bool on);
bool     getBinaryState(int idx);
String   sendSoap(int idx, const String& action, const String& body);
void     drawUI();
void     showMessage(const String& msg, uint16_t color = TFT_WHITE);
void     goToSleep();
void     animateToggle(bool toOn);

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  auto cfg = M5.config();
  StickCP2.begin(cfg);

  StickCP2.Display.setRotation(3);
  StickCP2.Display.fillScreen(TFT_BLACK);
  StickCP2.Display.setTextSize(2);
  StickCP2.Display.setTextColor(TFT_WHITE);

  showMessage("Connecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    showMessage("WiFi failed!", TFT_RED);
    delay(3000);
    return;
  }

  showMessage("Scanning for\nWeMo devices...");
  discoverWemo();

  lastActivityTime = millis();
  drawUI();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  StickCP2.update();

  // ── Idle sleep check ───────────────────────────────────────────────────────
  if (SLEEP_TIMEOUT_MS > 0 && millis() - lastActivityTime >= SLEEP_TIMEOUT_MS) {
    goToSleep();
  }

  // ── Button A: toggle current device ────────────────────────────────────────
  if (StickCP2.BtnA.wasPressed()) {
    lastActivityTime = millis();
    if (deviceCount == 0) {
      showMessage("No devices found.\nLong-press B\nto scan.", TFT_ORANGE);
      delay(2000);
      drawUI();
      return;
    }
    bool desired = !devices[currentDevice].on;
    if (setBinaryState(currentDevice, desired)) {
      devices[currentDevice].on = desired;
      animateToggle(desired);
    } else {
      showMessage("Command failed!", TFT_RED);
      delay(2000);
      drawUI();
    }
  }

  // ── Button B: short = cycle, long = rescan ──────────────────────────────────
  if (StickCP2.BtnB.wasPressed()) {
    lastActivityTime = millis();
    btnBPressTime   = millis();
    btnBLongHandled = false;
  }

  if (StickCP2.BtnB.isPressed() && !btnBLongHandled) {
    if (millis() - btnBPressTime >= LONG_PRESS_MS) {
      btnBLongHandled = true;
      showMessage("Rescanning...");
      deviceCount   = 0;
      currentDevice = 0;
      discoverWemo();
      drawUI();
    }
  }

  if (StickCP2.BtnB.wasReleased() && !btnBLongHandled) {
    if (deviceCount > 1) {
      currentDevice = (currentDevice + 1) % deviceCount;
      getBinaryState(currentDevice);
      drawUI();
    } else if (deviceCount == 1) {
      showMessage("Only 1 device\nfound.", TFT_DARKGREY);
      delay(1500);
      drawUI();
    } else {
      showMessage("No devices found.\nLong-press B\nto scan.", TFT_ORANGE);
      delay(2000);
      drawUI();
    }
  }

  delay(50);
}

// ── Sleep ─────────────────────────────────────────────────────────────────────
void animateToggle(bool toOn) {
  auto& d = StickCP2.Display;

  const int cx    = d.width() / 2;
  const int cy    = 86;
  const int tw    = 80;
  const int th    = 34;
  const int tr    = 17;
  const int thumR = 13;
  const int tx    = cx - tw / 2;

  int fromX = toOn ? (tx + 4 + thumR) : (tx + tw - 4 - thumR);
  int toX   = toOn ? (tx + tw - 4 - thumR) : (tx + 4 + thumR);

  const int steps   = 10;
  const int frameMs = 18;

  for (int s = 1; s <= steps; s++) {
    int thumbX = fromX + (toX - fromX) * s / steps;
    // colour switches at the midpoint of the travel
    bool pastMid    = (s * 2 >= steps) == toOn;
    uint16_t trackColor = pastMid ? TFT_GREEN : TFT_DARKGREY;

    d.fillRoundRect(tx, cy - th / 2, tw, th, tr, trackColor);
    d.fillCircle(thumbX, cy, thumR, TFT_WHITE);
    delay(frameMs);
  }
}

void goToSleep() {
  showMessage("Sleeping...", TFT_DARKGREY);
  delay(500);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  StickCP2.Display.setBrightness(0);
  StickCP2.Display.sleep();

  // Soft sleep: throttle CPU and poll for Button A.
  // Avoids PMIC deep-sleep issues that prevent wakeup on battery.
  setCpuFrequencyMhz(10);
  while (true) {
    StickCP2.update();
    if (StickCP2.BtnA.wasPressed()) break;
    delay(100);
  }

  // Flush button state so the wakeup press isn't seen as a toggle in loop()
  delay(50);
  StickCP2.update();

  // Wake up
  setCpuFrequencyMhz(240);
  StickCP2.Display.wakeup();
  StickCP2.Display.setBrightness(100);

  showMessage("Reconnecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    showMessage("WiFi failed!", TFT_RED);
    delay(3000);
  } else {
    showMessage("Refreshing...");
    for (int i = 0; i < deviceCount; i++) getBinaryState(i);
  }

  lastActivityTime = millis();
  drawUI();
}

// ── Discovery ─────────────────────────────────────────────────────────────────
void discoverWemo() {
  for (int i = 0; i < STATIC_DEVICE_COUNT && deviceCount < MAX_DEVICES; i++) {
    addDevice(String(STATIC_DEVICES[i].ip), STATIC_DEVICES[i].port,
              STATIC_DEVICES[i].name ? String(STATIC_DEVICES[i].name) : "");
  }

  WiFiUDP udp;
  udp.begin(0);

  // Search for both standard WeMo switches and Insight switches
  const char* searchTargets[] = {
    "urn:Belkin:device:controllee:1",  // standard switch / wall outlet
    "urn:Belkin:device:insight:1",     // Insight switch
  };

  for (const char* st : searchTargets) {
    String msearch =
      "M-SEARCH * HTTP/1.1\r\n"
      "HOST: 239.255.255.250:1900\r\n"
      "MAN: \"ssdp:discover\"\r\n"
      "MX: 3\r\n"
      "ST: " + String(st) + "\r\n"
      "\r\n";
    udp.beginPacket(IPAddress(239, 255, 255, 250), SSDP_PORT);
    udp.print(msearch);
    udp.endPacket();
    delay(100);
  }

  unsigned long deadline = millis() + SSDP_TIMEOUT_MS;
  char buf[1024];

  while (millis() < deadline && deviceCount < MAX_DEVICES) {
    int len = udp.parsePacket();
    if (len > 0) {
      int bytes = udp.read(buf, sizeof(buf) - 1);
      buf[bytes] = '\0';
      String response(buf);

      if (response.indexOf("Belkin") < 0 &&
          response.indexOf("belkin") < 0 &&
          response.indexOf("WeMo")   < 0 &&
          response.indexOf("wemo")   < 0) {
        continue;
      }

      int locIdx = response.indexOf("LOCATION:");
      if (locIdx < 0) locIdx = response.indexOf("Location:");
      if (locIdx < 0) continue;

      int httpIdx  = response.indexOf("http://", locIdx);
      if (httpIdx < 0) continue;

      int ipStart   = httpIdx + 7;
      int colonIdx  = response.indexOf(":", ipStart);
      int slashIdx  = response.indexOf("/", colonIdx);
      if (colonIdx < 0 || slashIdx < 0) continue;

      String ip   = response.substring(ipStart, colonIdx);
      int    port = response.substring(colonIdx + 1, slashIdx).toInt();
      String path = response.substring(slashIdx);
      path.trim();

      bool duplicate = false;
      for (int i = 0; i < deviceCount; i++) {
        if (devices[i].ip == ip && devices[i].port == port) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        int idx = deviceCount++;
        devices[idx].ip   = ip;
        devices[idx].port = port;
        devices[idx].name = fetchFriendlyName(ip, port, "/setup.xml");
        devices[idx].on   = false;
        getBinaryState(idx);
        showMessage("Found " + String(deviceCount) + " device(s)");
      }
    }
    delay(10);
  }

  udp.stop();
}

void addDevice(const String& ip, int port, const String& overrideName) {
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].ip == ip && devices[i].port == port) return;
  }
  int idx = deviceCount++;
  devices[idx].ip   = ip;
  devices[idx].port = port;
  devices[idx].name = overrideName.length() > 0
                        ? overrideName
                        : fetchFriendlyName(ip, port, "/setup.xml");
  devices[idx].on   = false;
  getBinaryState(idx);
}

String fetchFriendlyName(const String& ip, int port, const String& locationPath) {
  String url = "http://" + ip + ":" + String(port) + locationPath;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    int start = body.indexOf("<friendlyName>");
    int end   = body.indexOf("</friendlyName>", start);
    if (start >= 0 && end > start) {
      http.end();
      return body.substring(start + 14, end);
    }
  }
  http.end();
  return ip;
}

// ── WeMo SOAP ─────────────────────────────────────────────────────────────────
String sendSoap(int idx, const String& action, const String& body) {
  if (idx < 0 || idx >= deviceCount) return "";

  String url = "http://" + devices[idx].ip + ":" +
               String(devices[idx].port) + WEMO_CONTROL_PATH;
  String soapAction = "\"" + String(WEMO_SERVICE_TYPE) + "#" + action + "\"";
  String payload =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>" + body + "</s:Body>"
    "</s:Envelope>";

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPAction", soapAction);
  http.setTimeout(5000);

  int code = http.POST(payload);
  String response = "";
  if (code == 200) response = http.getString();
  http.end();
  return response;
}

bool setBinaryState(int idx, bool on) {
  String body =
    "<u:SetBinaryState xmlns:u=\"" + String(WEMO_SERVICE_TYPE) + "\">"
      "<BinaryState>" + String(on ? "1" : "0") + "</BinaryState>"
    "</u:SetBinaryState>";
  return sendSoap(idx, "SetBinaryState", body).length() > 0;
}

bool getBinaryState(int idx) {
  String body =
    "<u:GetBinaryState xmlns:u=\"" + String(WEMO_SERVICE_TYPE) + "\">"
      "<BinaryState>1</BinaryState>"
    "</u:GetBinaryState>";
  String resp = sendSoap(idx, "GetBinaryState", body);
  if (resp.length() == 0) return false;

  int i = resp.indexOf("<BinaryState>");
  if (i >= 0) {
    devices[idx].on = (resp.charAt(i + 13) == '1');
    return true;
  }
  return false;
}

// ── Display ───────────────────────────────────────────────────────────────────
void drawUI() {
  auto& d = StickCP2.Display;
  d.fillScreen(TFT_BLACK);

  if (deviceCount == 0) {
    d.setTextSize(2);
    d.setTextColor(TFT_ORANGE);
    d.setCursor(4, 20);
    d.print("No devices\nfound.");
    d.setTextSize(1);
    d.setTextColor(TFT_DARKGREY);
    d.setCursor(4, 100);
    d.print("[B long] Rescan");
    return;
  }

  WemoDevice& dev = devices[currentDevice];

  d.setTextSize(2);
  d.setTextColor(TFT_CYAN);
  d.setCursor(4, 4);
  d.print("WeMo Control");

  d.setTextSize(1);
  d.setTextColor(TFT_DARKGREY);
  d.setCursor(d.width() - 30, 8);
  d.printf("%d/%d", currentDevice + 1, deviceCount);

  d.setTextSize(2);
  d.setTextColor(TFT_WHITE);
  d.setCursor(4, 26);
  String name = dev.name;
  if (name.length() > 19) name = name.substring(0, 18) + "~";
  d.print(name);

  d.setTextSize(1);
  d.setTextColor(TFT_DARKGREY);
  d.setCursor(4, 46);
  d.printf("%s:%d", dev.ip.c_str(), dev.port);

  // Toggle icon centered at (d.width()/2, 86)
  {
    const int cx    = d.width() / 2;
    const int cy    = 86;
    const int tw    = 80;   // track width
    const int th    = 34;   // track height
    const int tr    = 17;   // track corner radius (th/2 = pill)
    const int thumR = 13;   // thumb radius
    const int tx    = cx - tw / 2;
    const int ty    = cy - th / 2;

    uint16_t trackColor = dev.on ? TFT_GREEN : TFT_DARKGREY;
    int      thumbX     = dev.on ? (tx + tw - 4 - thumR) : (tx + 4 + thumR);

    d.fillRoundRect(tx, ty, tw, th, tr, trackColor);
    d.fillCircle(thumbX, cy, thumR, TFT_WHITE);
  }

  d.setTextSize(1);
  d.setTextColor(TFT_DARKGREY);
  d.setCursor(4, 118);
  if (deviceCount > 1) {
    d.print("[A] Toggle [B] Next");
  } else {
    d.print("[A] Toggle [B-long] Scan");
  }
}

void showMessage(const String& msg, uint16_t color) {
  auto& d = StickCP2.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextSize(2);
  d.setTextColor(color);
  d.setCursor(4, 20);
  d.println(msg);
}
