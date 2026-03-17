# M5StickC Plus2 – Belkin WeMo Wall Outlet Controller

Control multiple Belkin WeMo smart plugs directly from an M5StickC Plus2 using WiFi. Devices are discovered automatically via SSDP/UPnP, or you can hard-code static IPs. Toggle power, cycle through devices, and rescan — all from the two hardware buttons.

---

## Hardware

- [M5StickC Plus2](https://docs.m5stack.com/en/core/M5StickC%20PLUS2) (ESP32-based)
- Belkin WeMo smart plug(s) on the same WiFi network

---

## Dependencies

Install via **Arduino Library Manager**:

| Library | Source |
|---|---|
| `M5StickCPlus2` | M5Stack (Library Manager) |
| `WiFi` | Built-in ESP32 |
| `HTTPClient` | Built-in ESP32 |

---

## WiFi Setup

WiFi credentials are stored in `config.h`, which is **gitignored** so your password is never committed.

**Steps:**

1. Copy the example config file:
   ```
   cp config.h.example config.h
   ```

2. Open `config.h` and replace the placeholder values with your actual WiFi credentials:
   ```cpp
   #define WIFI_SSID     "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"
   ```

3. Save the file. It will not be tracked by git.

> `config.h.example` is committed to the repo as a template. `config.h` is listed in `.gitignore` and must be created locally on each machine you build from.

---

## Optional: Static Device IPs

By default the sketch auto-discovers WeMo devices via SSDP. If discovery is unreliable on your network, you can hard-code device IPs in the sketch:

```cpp
const StaticDevice STATIC_DEVICES[] = {
  { "192.168.1.42", 49153 },
  { "192.168.1.43", 49153 },
};
```

You can also supply a friendly name as a third field, which skips the `setup.xml` fetch and uses your label directly:

```cpp
const StaticDevice STATIC_DEVICES[] = {
  { "192.168.1.42", 49153, "Living Room" },
};
```

Leave the array empty to rely entirely on auto-discovery (the default).

---

## Sleep After Inactivity

The device enters sleep automatically after a configurable period of inactivity to save battery. Press **Button A** to wake it. On wake, WiFi reconnects and device states are refreshed — no full re-scan is needed.

**Configuring the timeout:**

```cpp
#define SLEEP_TIMEOUT_MS 60000   // sleep after 60 seconds of inactivity
```

Change the value to any number of milliseconds. To disable sleep entirely, set it to `0`:

```cpp
#define SLEEP_TIMEOUT_MS 0       // never sleep
```

---

## Controls

| Button | Action |
|---|---|
| **Button A** (front) | Toggle current outlet ON / OFF |
| **Button B** short press | Cycle to next WeMo device |
| **Button B** long press (1.5 s) | Re-scan for WeMo devices |
| **Button A** | Wake from sleep |

---

## Flashing

1. Open the sketch folder in Arduino IDE.
2. Select board: **M5StickC Plus2** (or M5Stick-C-Plus2 depending on your IDE version).
3. Select the correct COM/serial port.
4. Click **Upload**.

The device will connect to WiFi and scan for WeMo plugs automatically on startup.

---

## How It Works

- On boot, the M5StickC Plus2 connects to WiFi and sends an SSDP M-SEARCH broadcast to discover WeMo devices on the local network.
- Each discovered device is queried for its friendly name and current power state via UPnP/SOAP.
- Toggle commands are sent as SOAP requests to the WeMo's `/upnp/control/basicevent1` endpoint.
- Up to 8 devices (`MAX_DEVICES`) are supported simultaneously.
