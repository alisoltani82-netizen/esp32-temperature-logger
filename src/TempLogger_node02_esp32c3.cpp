#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <DHT.h>
#include "esp_wifi.h"
#include "lwip/dns.h"   // for dns_setserver
#include "lwip/ip_addr.h"
#include "esp_sleep.h"  // for deep sleep
#include "config.h"     // Contains WiFi credentials and API keys (not in git)

/* ================== CONFIG ================== */
// WiFi and API credentials are defined in config.h (gitignored)

// ESP32-C3 SuperMini pin for DHT22 (GPIO4 is a great alternative)
#define DHTPIN   1
#define DHTTYPE  DHT22

// Send fake data if no sensor connected
const bool SEND_DUMMY = false;

// *** TESTING ONLY: Set to true to bypass sensor and use simulated data ***
// Useful for testing network connectivity without sensor issues
// const bool SEND_DUMMY = true;  // Uncomment this line to test with dummy data

// Send interval (ms). 60 minutes
const unsigned long SAMPLE_MS = 60UL * 60UL * 1000UL; // 3,600,000 ms
/* ============================================ */

DHT dht(DHTPIN, DHTTYPE);
// flag set by Wi-Fi event handler to request a DNS test from loop()
volatile bool needDnsTest = false;

// DNS cache for Cloud Function host (avoid repeated DNS lookups)
static IPAddress cachedIP;
static unsigned long cacheTime = 0;
static bool cacheValid = false;
const unsigned long CACHE_TTL_MS = 3600000; // 1 hour

// Retry-on-failure state (RTC memory survives deep sleep)
RTC_DATA_ATTR bool retryMode = false;
RTC_DATA_ATTR unsigned long retryStartTime = 0;
RTC_DATA_ATTR float lastTempC = 0;
RTC_DATA_ATTR float lastHum = 0;
RTC_DATA_ATTR uint32_t retryCount = 0;
const unsigned long RETRY_INTERVAL_MS = 30000;   // 30 seconds
const unsigned long RETRY_MAX_DURATION_MS = 300000; // 5 minutes

/* ---------- Time helper (CT) ---------- */
String nowISO() {
  struct tm t;
  if (getLocalTime(&t)) {
    char buf[32];
    // Example: 2025-10-01T00:34:12-0500  (%z gives -0500 or -0600)
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &t);
    return String(buf);
  }
  return "t+" + String(millis()/1000) + "s";
}

/* ---------- DNS helper ---------- */
static void forceDNS(const char* primary = "1.1.1.1", const char* secondary = "8.8.8.8") {
  ip_addr_t d1; ip_addr_t d2;
  ipaddr_aton(primary, &d1);
  ipaddr_aton(secondary, &d2);
  dns_setserver(0, &d1);
  dns_setserver(1, &d2);
  // Read back for logging
  const ip_addr_t* r0 = dns_getserver(0);
  const ip_addr_t* r1 = dns_getserver(1);
  char buf0[16]; char buf1[16];
  ipaddr_ntoa_r(r0, buf0, sizeof(buf0));
  ipaddr_ntoa_r(r1, buf1, sizeof(buf1));
  Serial.printf("[DNS] servers set: %s, %s\n", buf0, buf1);
}

/* ---------- Wi-Fi helpers ---------- */
static void printWifiInfo() {
  Serial.printf("[WiFi] IP:%s  GW:%s  DNS:%s\n",
    WiFi.localIP().toString().c_str(),
    WiFi.gatewayIP().toString().c_str(),
    WiFi.dnsIP().toString().c_str());
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    Serial.printf("[WiFi] RSSI:%d dBm  ch:%d  BSSID:%02X:%02X:%02X:%02X:%02X:%02X\n",
      ap.rssi, ap.primary,
      ap.bssid[0], ap.bssid[1], ap.bssid[2],
      ap.bssid[3], ap.bssid[4], ap.bssid[5]);
  }
}

static bool waitForWiFi(uint32_t ms = 20000) {
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < ms) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

static void wifiScanOnce() {
  Serial.println("[WiFi] scan...");
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) { Serial.println("  (no networks)"); return; }
  for (int i = 0; i < n; ++i) {
    Serial.printf("  #%d  SSID:%s  RSSI:%d  ch:%d  %s\n", i,
      WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
      (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "SEC"));
  }
  WiFi.scanDelete();
}

static bool dnsTest(const char* host) {
  IPAddress ip;
  bool ok = WiFi.hostByName(host, ip);
  Serial.printf("[DNS] %s -> %s (%s)\n", host, ip.toString().c_str(), ok ? "OK" : "FAIL");
  return ok;
}

static void onWiFiEvent(WiFiEvent_t event) {
  // 4=CONNECTED (assoc), 7=GOT_IP, 5=DISCONNECTED, 9=DHCP_TIME
  if (event == SYSTEM_EVENT_STA_DISCONNECTED) {
    Serial.println("[WiFi] disconnected — retrying");
    WiFi.reconnect();
  } else if (event == SYSTEM_EVENT_STA_GOT_IP) {
    Serial.println("[WiFi] connected");
    printWifiInfo();
    // Router DHCP may push its own DNS; override here every time.
    forceDNS("1.1.1.1", "8.8.8.8");
    // Request a DNS test from the main loop (non-blocking in event handler)
    needDnsTest = true;
  } else {
    Serial.printf("[WiFiEvent] %d\n", event);
  }
}

static bool wifiBeginClean(const char* ssid, const char* pass, const char* hostname = "node-02") {
  WiFi.mode(WIFI_STA);
  // Allow modem sleep to reduce power and heat. Set moderate TX power for stability.
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.setHostname(hostname);

  wifiScanOnce();

  Serial.printf("[WiFi] connect to '%s'...\n", ssid);
  WiFi.begin(ssid, pass);

  if (!waitForWiFi(20000)) {
    Serial.println("[WiFi] FAIL (no link)");
    return false;
  }

  // We will override DNS in GOT_IP event; just print current info now.
  printWifiInfo();
  return true;
}

/* ---------- HTTPS POST (bounded timeouts + clear logs) ---------- */
static bool postReading(float tempC, float hum) {
  unsigned long postStart = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[POST] skipped: Wi-Fi not connected");
    return false;
  }

  // DNS sanity before TLS (use cache to avoid repeated lookups)
  IPAddress hostIP;
  unsigned long now_ms = millis();
  unsigned long dnsStart = millis();
  if (!cacheValid || (now_ms - cacheTime > CACHE_TTL_MS)) {
    // Cache expired or empty; do DNS lookup
    if (!WiFi.hostByName(FUNCTION_HOST, hostIP)) {
      Serial.println("[POST] DNS lookup failed, aborting");
      return false;
    }
    cachedIP = hostIP;
    cacheTime = now_ms;
    cacheValid = true;
    Serial.printf("[POST] DNS resolved (cached) %s -> %s\n", FUNCTION_HOST, hostIP.toString().c_str());
  } else {
    // Use cached IP
    hostIP = cachedIP;
    Serial.printf("[POST] DNS (from cache) %s -> %s\n", FUNCTION_HOST, hostIP.toString().c_str());
  }
  unsigned long dnsEnd = millis();
  Serial.printf("[POST] DNS lookup took %lu ms\n", dnsEnd - dnsStart);

  WiFiClientSecure client;
  client.setInsecure();          // DEV ONLY. Skip cert verification.
  client.setTimeout(15000);      // Extended socket read timeout for TLS handshake

  HTTPClient http;
  http.setConnectTimeout(8000);  // TCP connect timeout
  http.setTimeout(8000);         // overall HTTP transaction timeout
  http.setReuse(false);

  String ts = nowISO();
  String json = "{";
  json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
  json += "\"timestamp\":\"" + ts + "\",";
  json += "\"temp_c\":" + String(tempC, 1) + ",";
  json += "\"hum_pct\":" + String(hum, 0);
  json += "}";

  // First attempt: connect to IP and include Host header so frontends route correctly
  String url_with_ip = String("https://") + hostIP.toString() + String("/ingest");
  Serial.printf("[POST] Attempting IP connect -> %s (Host=%s)\n", url_with_ip.c_str(), FUNCTION_HOST);
  if (http.begin(client, url_with_ip)) {
    http.addHeader("Host", FUNCTION_HOST);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Key", DEVICE_KEY);
    http.addHeader("Connection", "close");

    Serial.println("[POST-IP] sending...");
    int code = http.POST(json);
    Serial.printf("[POST-IP] code=%d\n", code);
    String resp = http.getString();
    http.end();

    if (code >= 200 && code < 300) {
      Serial.printf("[POST-IP] OK: %s\n", resp.c_str());
      unsigned long postEnd = millis();
      Serial.printf("[POST] total successful POST took %lu ms\n", postEnd - postStart);
      return true;
    }
    Serial.printf("[POST-IP] failed: %s\n", resp.c_str());
    // fall through to hostname attempt
  } else {
    Serial.println("[POST-IP] begin() failed");
  }

  // Second attempt: connect using the hostname (normal mode). This may reveal
  // TLS/SNI issues if the core doesn't set SNI properly.
  Serial.printf("[POST] Falling back to hostname connect -> %s\n", FUNCTION_URL);
  if (http.begin(client, FUNCTION_URL)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Key", DEVICE_KEY);
    http.addHeader("Connection", "close");

    Serial.println("[POST-HOST] sending...");
    int code = http.POST(json);
    Serial.printf("[POST-HOST] code=%d\n", code);
    String resp = http.getString();
    http.end();

    if (code >= 200 && code < 300) {
      Serial.printf("[POST-HOST] OK: %s\n", resp.c_str());
      unsigned long postEnd = millis();
      Serial.printf("[POST] total successful POST took %lu ms\n", postEnd - postStart);
      return true;
    }
    Serial.printf("[POST-HOST] failed: %s\n", resp.c_str());
    return false;
  } else {
    Serial.println("[POST-HOST] begin() failed (TLS handshake likely)");
    return false;
  }
}

/* ================== SETUP / LOOP ================== */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[ESP32-C3] Cloud Function logger boot (node-02)");

  if (!SEND_DUMMY) {
    dht.begin();
    delay(2000); // DHT22 settle
  }

  const bool wifi_ok = wifiBeginClean(WIFI_SSID, WIFI_PASS, "node-02");

  // Only sync time if connected; cap wait to 30s (increased from 15s)
  if (WiFi.status() == WL_CONNECTED) {
    const long gmtOffset_sec = -6 * 3600;   // CT UTC-6
    const int  daylightOffset_sec = 3600;   // +1h in summer
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");

    Serial.print("Syncing time");
    time_t now = time(nullptr);
    unsigned long t0 = millis();
    while (now < 8 * 3600 * 2 && millis() - t0 < 30000) {
      Serial.print(".");
      delay(500);
      now = time(nullptr);
    }
    Serial.println(now >= 8 * 3600 * 2 ? "\nTime synced" : "\nTime sync skipped");
  } else {
    Serial.println("[NET] skip NTP (no Wi-Fi)");
  }

  // With deep sleep, all sensor reading/posting is done in loop()
  // setup() just initializes hardware and network
  Serial.println("[SETUP] Complete. Proceeding to loop() for sensor read/post.");
}

void loop() {
  // Deep sleep implementation: all work done in setup(), loop() just triggers sleep
  unsigned long sleepDuration_us;

  // Handle retry mode
  if (retryMode) {
    retryCount++;
    Serial.printf("[RETRY] Attempt #%u (max duration: 5 min)\n", retryCount);

    // Check if retry window has expired (5 minutes / 30s = 10 attempts)
    if (retryCount >= (RETRY_MAX_DURATION_MS / RETRY_INTERVAL_MS)) {
      Serial.println("[RETRY] Timeout: exceeded 5 minutes. Returning to normal 1-hour schedule.");
      retryMode = false;
      retryCount = 0;
      sleepDuration_us = SAMPLE_MS * 1000ULL; // 1 hour
    } else {
      // Try to post the last reading
      if (postReading(lastTempC, lastHum)) {
        Serial.println("[RETRY] SUCCESS! Returning to normal 1-hour schedule.");
        retryMode = false;
        retryCount = 0;
        sleepDuration_us = SAMPLE_MS * 1000ULL; // 1 hour
      } else {
        Serial.println("[RETRY] Failed, will retry in 30s");
        sleepDuration_us = RETRY_INTERVAL_MS * 1000ULL; // 30 seconds
      }
    }
  } else {
    // Normal mode: read sensor and post
    float tempC, hum;
    if (SEND_DUMMY) {
      tempC = 22.5 + (sin(millis()/60000.0) * 2.0);
      hum   = 50 + (cos(millis()/60000.0) * 5.0);
    } else {
      tempC = dht.readTemperature();
      hum   = dht.readHumidity();
      if (isnan(tempC) || isnan(hum)) {
        delay(1500);
        tempC = dht.readTemperature();
        hum   = dht.readHumidity();
      }
    }

    if (isnan(tempC) || isnan(hum)) {
      Serial.println("[WARN] Sensor read failed, will retry in 30s");
      lastTempC = 22.0;  // fallback value
      lastHum = 50.0;
      retryMode = true;
      retryCount = 0;
      retryStartTime = millis();
      sleepDuration_us = RETRY_INTERVAL_MS * 1000ULL; // 30 seconds
    } else {
      // Store readings in case we need to retry
      lastTempC = tempC;
      lastHum = hum;

      // Try to post
      if (postReading(tempC, hum)) {
        Serial.println("[POST] SUCCESS! Sleeping for 1 hour.");
        sleepDuration_us = SAMPLE_MS * 1000ULL; // 1 hour
      } else {
        // Post failed: enter retry mode
        Serial.println("[POST] FAILED! Entering 5-minute retry mode (30s intervals)");
        retryMode = true;
        retryCount = 0;
        retryStartTime = millis();
        sleepDuration_us = RETRY_INTERVAL_MS * 1000ULL; // 30 seconds
      }
    }
  }

  // Disconnect WiFi before sleep to save power
  Serial.printf("[SLEEP] Going to deep sleep for %llu seconds\n", sleepDuration_us / 1000000ULL);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Configure wake-up timer and enter deep sleep
  esp_sleep_enable_timer_wakeup(sleepDuration_us);
  Serial.flush();
  esp_deep_sleep_start();

  // Never reaches here (device resets on wake)
}
  