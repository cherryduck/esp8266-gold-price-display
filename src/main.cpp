/*
 * ESP8266 Gold/Silver Price Display
 * 
 * Fetches live gold (XAU) or silver (XAG) price in GBP from api.gold-api.com
 * and displays it on a MAX7219 8-digit 7-segment display.
 * 
 * Hardware: D1 Mini (ESP8266) + MAX7219 module
 * Wiring:   DIN->D7(GPIO13), CS->D6(GPIO12), CLK->D5(GPIO14)
 * 
 * Asset selection via WiFiManager custom parameter ("XAU" or "XAG").
 * Open config portal by double-resetting the board within 5 seconds.
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <LedControl.h>
#include <EEPROM.h>

/* ============================================================
 * CONFIGURATION CONSTANTS
 * ============================================================ */

// MAX7219 pin mapping (D1 Mini GPIO)
#define PIN_DIN   13  // D7 -> MOSI on HSPI bus
#define PIN_CS    12  // D6
#define PIN_CLK   14  // D5 -> SCK on HSPI bus

// Display configuration
#define DISPLAY_DEVICES 1  // Number of MAX7219 chips chained (1 = 8 digits)

// API configuration
const char* API_HOST     = "api.gold-api.com";
const int   API_PORT     = 443;  // HTTPS

// Refresh interval: 5 minutes in milliseconds
#define REFRESH_INTERVAL_MS (5UL * 60UL * 1000UL)

// HTTP client timeout: 10 seconds
#define HTTP_TIMEOUT_MS 10000

// Display brightness (0-15)
#define DISPLAY_BRIGHTNESS 8

/* ============================================================
 * ASSET MODE ENUM
 * ============================================================ */

enum AssetMode {
    ASSET_GOLD   = 0,  // XAU/GBP - gold price, 2 decimal places
    ASSET_SILVER = 1   // XAG/GBP - silver price, 3 decimal places (if fits)
};

/* ============================================================
 * GLOBAL STATE
 * ============================================================ */

LedControl lc = LedControl(PIN_DIN, PIN_CLK, PIN_CS, DISPLAY_DEVICES);

float lastPrice       = -1.0f;   // Negative means no valid price yet
bool  hasValidPrice   = false;
unsigned long lastFetchTime = 0;

// Current asset mode (persisted in EEPROM)
AssetMode currentMode = ASSET_GOLD;

// Built-in LED for status indication (active LOW on D1 Mini)
#define LED_BUILTIN_PIN 2  // GPIO2 / D4

/* ============================================================
 * EEPROM PERSISTENCE
 * 
 * EEPROM.begin(4) allocates one flash sector. We store the mode byte
 * at address 0. WiFiManager stores its preferences in NVS/spiffs, not
 * in the same EEPROM-emulated region on ESP8266, so address 0 is safe.
 * ============================================================ */

#define EEPROM_MODE_ADDR  0
#define EEPROM_SIZE       4

void saveModeToEEPROM(AssetMode mode) {
    EEPROM.write(EEPROM_MODE_ADDR, (uint8_t)mode);
    EEPROM.commit();
    Serial.printf("[EEPROM] Saved mode: %s at address %d\n",
                  mode == ASSET_GOLD ? "GOLD" : "SILVER", EEPROM_MODE_ADDR);
}

AssetMode loadModeFromEEPROM() {
    uint8_t val = EEPROM.read(EEPROM_MODE_ADDR);
    
    if (val != ASSET_GOLD && val != ASSET_SILVER) {
        // First boot or invalid value: default to gold
        Serial.println(F("[EEPROM] No valid mode found, defaulting to GOLD"));
        saveModeToEEPROM(ASSET_GOLD);
        return ASSET_GOLD;
    }
    
    Serial.printf("[EEPROM] Loaded mode: %s\n",
                  val == ASSET_GOLD ? "GOLD" : "SILVER");
    return (AssetMode)val;
}

/* ============================================================
 * DOUBLE-RESET DETECTION (opens config portal on demand)
 * 
 * Stores the previous boot time in EEPROM at address 1.
 * If current boot occurs within DOUBLE_RESET_WINDOW_MS of the
 * previous boot, we open the WiFiManager config portal.
 * ============================================================ */

#define DOUBLE_RESET_ADDR       1   // Address for last-boot timestamp (low byte)
#define DOUBLE_RESET_WINDOW_MS  5000  // 5 seconds window

bool detectDoubleReset() {
    unsigned long now = millis();
    
    // Read the stored boot tick from previous session
    uint8_t savedTickLow  = EEPROM.read(DOUBLE_RESET_ADDR);
    uint8_t savedTickHigh = EEPROM.read(DOUBLE_RESET_ADDR + 1);
    uint16_t savedBootTick = ((uint16_t)savedTickHigh << 8) | savedTickLow;
    
    // Current boot tick (millis / 1000, truncated to fit in 16 bits)
    uint16_t currentBootTick = (uint16_t)(now / 1000);
    
    // Calculate delta accounting for millis() wraparound (~49 days)
    int16_t delta = (int16_t)(currentBootTick - savedBootTick);
    
    bool isDoubleReset = false;
    
    if (savedTickLow == 0xFF && savedTickHigh == 0xFF) {
        // First boot ever — EEPROM uninitialized
        Serial.println(F("[BOOT] First boot (EEPROM uninitialized)"));
    } else if (delta >= 0 && delta <= (DOUBLE_RESET_WINDOW_MS / 1000)) {
        // Both boots within the time window.
        // delta == 0 means both in same second — strongest signal.
        Serial.printf("[BOOT] Double-reset detected! Delta: %d seconds\n", delta);
        isDoubleReset = true;
    } else if (delta < -(32768 - (DOUBLE_RESET_WINDOW_MS / 1000))) {
        // millis() wrapped around (~49 days). If the saved tick is just past
        // the wrap boundary, delta wraps negative but is still within window.
        int16_t wrappedDelta = delta + 32768;
        if (wrappedDelta <= (DOUBLE_RESET_WINDOW_MS / 1000)) {
            Serial.printf("[BOOT] Double-reset detected (wrap)! Delta: %d seconds\n", wrappedDelta);
            isDoubleReset = true;
        } else {
            Serial.printf("[BOOT] Normal boot. Saved tick too old.\n");
        }
    } else {
        Serial.printf("[BOOT] Normal boot. Delta: %d seconds\n", delta);
    }
    
    // Save current boot tick for next time
    EEPROM.write(DOUBLE_RESET_ADDR,  (uint8_t)(currentBootTick & 0xFF));
    EEPROM.write(DOUBLE_RESET_ADDR + 1, (uint8_t)((currentBootTick >> 8) & 0xFF));
    EEPROM.commit();
    
    return isDoubleReset;
}

/* ============================================================
 * DISPLAY FUNCTIONS
 * ============================================================ */

// Display a single digit at position pos (0=rightmost, 7=leftmost)
void displayDigit(int pos, uint8_t digit, bool dot);

// Clear all digits on the display
void displayClear();

// Show error pattern: "---- ----" (blinking dashes)
void displayErrorPattern(bool on);

// Show "E r r" pattern for no-price-ever state
void displayErrorIndicator();

// Format and display price on 8 digits (right-justified), with configurable decimals
void displayPrice(float price, int decimalPlaces);

// Show asset indicator: "Au" or "Ag" on the left side of display
void displayAssetIndicator(AssetMode mode);

// Fetch price for a given asset mode
bool fetchAssetPrice(AssetMode mode);

/* ============================================================
 * DISPLAY INITIALIZATION
 * ============================================================ */

void displayInit() {
    Serial.println(F("[DISPLAY] Initializing MAX7219..."));

    for (int i = 0; i < DISPLAY_DEVICES; i++) {
        lc.shutdown(i, false);   // Wake up from power-down mode
        delay(5);                // Brief settle time after wakeup
        lc.setIntensity(i, DISPLAY_BRIGHTNESS);
        lc.clearDisplay(i);      // Clear all digits
    }

    // Diagnostic: write "8.8.8.8.8.8.8.8" to verify every digit works
    Serial.println(F("[DISPLAY] Test pattern: 88888888"));
    for (int i = 0; i < 8; i++) {
        lc.setDigit(0, i, 8, false);
    }
    delay(1500);                 // Hold test pattern so user can see it
    displayClear();

    Serial.println(F("[DISPLAY] MAX7219 initialized"));
}

/* ============================================================
 * DISPLAY FORMATTING AND OUTPUT
 * ============================================================ */

// Display a single digit at position pos (0=rightmost, 7=leftmost)
void displayDigit(int pos, uint8_t digit, bool dot = false) {
    lc.setDigit(0, pos, digit, dot);
}

// Clear all digits on the display
void displayClear() {
    for (int i = 0; i < 8; i++) {
        lc.setChar(0, i, ' ', false);
    }
}

// Show error pattern: "---- ----" (blinking dashes)
void displayErrorPattern(bool on) {
    if (on) {
        for (int i = 0; i < 8; i++) {
            lc.setRow(0, i, 0x40);  // 'g' segment only = dash
        }
    } else {
        displayClear();
    }
}

// Show "E r r" pattern for no-price-ever state
void displayErrorIndicator() {
    displayClear();
    lc.setChar(0, 6, 'E', false);   // Position 6: 'E'
    lc.setChar(0, 4, 'r', false);   // Position 4: 'r'
    lc.setRow(0, 2, 0x40);          // Position 2: dash
}

// Show asset indicator on the left side of display
void displayAssetIndicator(AssetMode mode) {
    displayClear();
    if (mode == ASSET_GOLD) {
        lc.setChar(0, 7, 'A', false);   // Position 7: 'A'
        lc.setChar(0, 6, 'u', false);   // Position 6: 'u'
    } else {
        lc.setChar(0, 7, 'A', false);   // Position 7: 'A'
        lc.setChar(0, 6, 'g', false);   // Position 6: 'g'
    }
}

// Format and display price on 8 digits (right-justified)
void displayPrice(float price, int decimalPlaces = 2) {
    if (price < 0) return;

    displayClear();

    char buf[20];
    
    // Check if the formatted string fits within 8 display positions.
    // Each digit and the decimal point occupy one position.
    // For silver: "999.123" = 7 chars (6 digits + dot) -> fits.
    // If price is >= 1000 with 3 decimals, fall back to 2 decimals.
    int dp = decimalPlaces;
    snprintf(buf, sizeof(buf), "%.*f", dp, price);
    
    // Count total characters that map to display positions (digits + dot)
    int charCount = strlen(buf);
    if (charCount > 8 && dp > 2) {
        dp = 2;
        snprintf(buf, sizeof(buf), "%.2f", price);
    }

    // Count digits (excluding '.') to determine right-justified start position
    int digit_count = 0;
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            digit_count++;
        }
    }

    int pos = digit_count - 1;  // start from rightmost position for first digit

    for (int i = 0; buf[i] != '\0'; i++) {
        char c = buf[i];

        if (c == '.') {
            continue;
        }

        bool dot = (buf[i + 1] == '.');  // dot belongs to this digit

        if (c >= '0' && c <= '9') {
            lc.setDigit(0, pos, c - '0', dot);
            pos--;
        }
    }

    Serial.printf("[DISPLAY] Showing price: %.*f GBP (%s)\n", 
                  dp, price, currentMode == ASSET_GOLD ? "XAU" : "XAG");
}

/* ============================================================
 * PRICE FETCHING (dynamic asset)
 * ============================================================ */

const char* assetSymbol(AssetMode mode) {
    return (mode == ASSET_GOLD) ? "XAU" : "XAG";
}

bool fetchAssetPrice(AssetMode mode) {
    const char* symbol = assetSymbol(mode);
    Serial.printf("[API] Fetching %s/GBP price...\n", symbol);
    
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert verification (saves RAM on ESP8266)
    
    HTTPClient https;
    https.setTimeout(HTTP_TIMEOUT_MS);
    
    // Build dynamic API path: /price/{XAU|XAG}/GBP
    String url = "https://";
    url += API_HOST;
    url += "/price/";
    url += symbol;
    url += "/GBP";
    
    Serial.printf("[API] GET %s\n", url.c_str());
    
    if (!https.begin(client, url)) {
        Serial.println(F("[API] Failed to begin HTTPS connection"));
        return false;
    }
    
    int httpCode = https.GET();
    
    if (httpCode <= 0) {
        Serial.printf("[API] HTTP error: %d\n", httpCode);
        https.end();
        return false;
    }
    
    Serial.printf("[API] HTTP response code: %d\n", httpCode);
    
    if (httpCode != 200) {
        Serial.printf("[API] Unexpected status: %d\n", httpCode);
        https.end();
        return false;
    }
    
    // Parse JSON response
    String payload = https.getString();
    https.end();
    
    Serial.printf("[API] Response (%d bytes): %s\n", payload.length(), payload.c_str());
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    
    if (err) {
        Serial.print(F("[API] JSON parse error: "));
        Serial.println(err.c_str());
        return false;
    }
    
    if (!doc["price"].is<float>()) {
        Serial.println(F("[API] Response missing 'price' field"));
        return false;
    }
    
    float price = doc["price"].as<float>();
    
    if (isnan(price) || price <= 0) {
        Serial.printf("[API] Invalid price value: %.2f\n", price);
        return false;
    }
    
    lastPrice = price;
    hasValidPrice = true;
    lastFetchTime = millis();
    
    Serial.printf("[API] %s/GBP price: %.*f (updated: %s)\n", 
                  symbol,
                  mode == ASSET_GOLD ? 2 : 3, price,
                  doc["updatedAtReadable"] | "unknown");
    
    return true;
}

/* ============================================================
 * WIFIMANAGER WITH CUSTOM ASSET PARAMETER
 * ============================================================ */

// Custom parameter HTML template for asset selection.
// WiFiManagerParameter renders an auto-generated <input id="asset"> PLUS our custom HTML.
// Strategy: make the auto-generated input invisible via inline styles (zero dimensions),
// hide its label by finding it as a sibling, and sync values via JS on change/submit.
// DO NOT use .closest('.field') — that can match a parent container wrapping ALL params.
const char* PARAM_ASSET_HTML = R"rawliteral(
<div style="margin:12px 0; padding:8px; border:1px solid #ccc; border-radius:4px;">
  <label for="assetSel" style="font-weight:bold; margin-right:6px;">Asset:</label>
  <select id="assetSel" name="assetSel" style="padding:4px 8px; font-size:14px;">
    <option value="XAU"%XAUSEL%>Gold (XAU)</option>
    <option value="XAG"%XAGSEL%>Silver (XAG)</option>
  </select>
</div>
<script>
(function(){
  var sel=document.getElementById('assetSel');
  var txt=document.getElementById('asset');
  if(txt){
    // Make auto-generated input invisible (inline styles survive HTML escaping)
    txt.style.width='0';txt.style.height='0';txt.style.overflow='hidden';
    txt.style.border='none';txt.style.padding='0';txt.style.margin='0';
    txt.style.opacity='0';txt.style.position='absolute';txt.style.clip='rect(0,0,0,0)';
    // Hide the label that WiFiManager generates for this input
    var p=txt.previousElementSibling;
    if(p&&p.tagName==='LABEL')p.style.display='none';
  }
  function sync(){if(txt&&sel)txt.value=sel.value;}
  if(sel)sel.addEventListener('change',sync);
  var form=document.querySelector('form');
  if(form)form.addEventListener('submit',sync);
  window.onload=function(){sync();};
})();
</script>
)rawliteral";

void setupWiFi(bool forcePortal = false) {
    Serial.println(F("[WIFI] Starting WiFiManager..."));
    
    // Built-in LED as status indicator during setup
    pinMode(LED_BUILTIN_PIN, OUTPUT);
    digitalWrite(LED_BUILTIN_PIN, HIGH);  // OFF
    
    WiFiManager wm;
    wm.setDebugOutput(true);
    wm.setConnectTimeout(30);
    wm.setConfigPortalTimeout(180);  // Auto-close after 3 minutes if no connection
    
    String portalName = "GoldPrice-Setup";
    
    /* ---- Custom asset parameter ---- */
    
    // Build the HTML with the correct option pre-selected
    String paramHtml = String(PARAM_ASSET_HTML);
    if (currentMode == ASSET_GOLD) {
        paramHtml.replace("%XAUSEL%", " selected");
        paramHtml.replace("%XAGSEL%", "");
    } else {
        paramHtml.replace("%XAUSEL%", "");
        paramHtml.replace("%XAGSEL%", " selected");
    }
    
    // Pre-fill the defaultValue with current mode so getValue() has a fallback.
    // The JS sync ensures it matches whatever the user selects in the dropdown.
    const char* defaultAsset = (currentMode == ASSET_GOLD) ? "XAU" : "XAG";
    
    WiFiManagerParameter paramAsset(
        "asset",                                    // id
        "",                                         // label (hidden, we use custom HTML)
        defaultAsset,                               // defaultValue — synced by JS on change
        4,                                          // length (small text field, hidden by CSS)
        paramHtml.c_str()                           // custom HTML with styled select + JS
    );
    
    wm.addParameter(&paramAsset);
    
    Serial.printf("[WIFI] Starting captive portal: %s\n", portalName.c_str());
    
    // Blink LED while in setup mode
    wm.setAPCallback([](WiFiManager* wm) {
        Serial.printf("[WIFI] AP started. IP: %s\n", 
                      WiFi.softAPIP().toString().c_str());
    });
    
    // If forcePortal is true, skip auto-connect and go straight to config portal
    if (forcePortal) {
        Serial.println(F("[WIFI] Forced config portal mode"));
        wm.setConfigPortalTimeout(300);  // Give more time when manually opened
        
        if (!wm.startConfigPortal(portalName.c_str(), "hermes123")) {
            Serial.println(F("[WIFI] Config portal timed out. Rebooting..."));
            ESP.restart();
            delay(500);
            return;
        }
    } else {
        // Connect to saved credentials or show captive portal if none exist
        if (!wm.autoConnect(portalName.c_str())) {
            Serial.println(F("[WIFI] Failed to connect and hit timeout. Rebooting..."));
            
            // Flash LED rapidly to indicate failure
            for (int i = 0; i < 10; i++) {
                digitalWrite(LED_BUILTIN_PIN, LOW);
                delay(50);
                digitalWrite(LED_BUILTIN_PIN, HIGH);
                delay(50);
            }
            
            // Reset WiFiManager preferences and try again
            wm.resetSettings();
            ESP.restart();
            delay(500);
            return;
        }
    }
    
    /* ---- Validate and persist asset parameter ---- */
    
    const char* assetValue = paramAsset.getValue();
    Serial.printf("[WIFI] Asset parameter received: '%s'\n", assetValue);
    
    AssetMode newMode = currentMode;  // Default: keep previous mode
    
    if (strcmp(assetValue, "XAU") == 0) {
        newMode = ASSET_GOLD;
    } else if (strcmp(assetValue, "XAG") == 0) {
        newMode = ASSET_SILVER;
    } else {
        // Invalid or empty value: keep previous saved mode (already in currentMode)
        Serial.printf("[WIFI] Invalid asset '%s', keeping previous mode: %s\n",
                      assetValue,
                      currentMode == ASSET_GOLD ? "GOLD" : "SILVER");
        newMode = currentMode;
    }
    
    if (newMode != currentMode) {
        Serial.printf("[WIFI] Mode changed from %s to %s\n",
                      currentMode == ASSET_GOLD ? "GOLD" : "SILVER",
                      newMode == ASSET_GOLD ? "GOLD" : "SILVER");
    }
    
    currentMode = newMode;
    saveModeToEEPROM(currentMode);
    
    Serial.println(F("[WIFI] Connected to Wi-Fi!"));
    Serial.printf("[WIFI] IP address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] Signal strength: %d dBm\n", WiFi.RSSI());
    
    // Solid LED = connected
    digitalWrite(LED_BUILTIN_PIN, LOW);  // ON (active low)
}

/* ============================================================
 * SETUP AND LOOP
 * ============================================================ */

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println(F("\n========================================"));
    Serial.println(F("  ESP8266 Gold/Silver Price Display"));
    Serial.println(F("========================================\n"));
    
    // Initialize EEPROM first (needed for double-reset detection)
    EEPROM.begin(EEPROM_SIZE);
    
    // Load persisted asset mode from EEPROM
    currentMode = loadModeFromEEPROM();
    
    // Check for double-reset to open config portal
    bool forcePortal = detectDoubleReset();
    
    // Initialize display first (works without WiFi)
    displayInit();
    
    // Show boot animation: count up 0-7
    for (int i = 0; i < 8; i++) {
        displayClear();
        displayDigit(i, i);
        delay(100);
    }
    displayClear();
    
    // Connect to WiFi (with optional forced config portal)
    setupWiFi(forcePortal);
    
    // Try initial fetch for the selected asset
    if (fetchAssetPrice(currentMode)) {
        int decimals = (currentMode == ASSET_GOLD) ? 2 : 3;
        displayPrice(lastPrice, decimals);
    } else {
        Serial.println(F("[SETUP] Initial price fetch failed, will retry in loop"));
        if (!hasValidPrice) {
            displayErrorIndicator();
        }
    }
    
    // LED off after setup complete (save power)
    digitalWrite(LED_BUILTIN_PIN, HIGH);  // OFF
}

void loop() {
    unsigned long now = millis();
    
    // Check if we need to refresh the price
    bool shouldFetch = false;
    
    if (!hasValidPrice) {
        // No valid price yet: retry every 30 seconds
        shouldFetch = (now - lastFetchTime > 30000UL);
    } else {
        // Normal operation: refresh every REFRESH_INTERVAL_MS
        shouldFetch = (now - lastFetchTime >= REFRESH_INTERVAL_MS);
    }
    
    if (shouldFetch) {
        bool success = fetchAssetPrice(currentMode);
        
        if (success) {
            int decimals = (currentMode == ASSET_GOLD) ? 2 : 3;
            displayPrice(lastPrice, decimals);
            
            // Brief LED flash to indicate successful update
            digitalWrite(LED_BUILTIN_PIN, LOW);   // ON
            delay(200);
            digitalWrite(LED_BUILTIN_PIN, HIGH);  // OFF
            
        } else {
            Serial.println(F("[LOOP] Fetch failed. Keeping last known price."));
            
            if (!hasValidPrice) {
                // Never had a valid price: show error indicator
                displayErrorIndicator();
            }
            // If we have a previous price, just keep displaying it (no change needed)
        }
    }
    
    // Non-blocking delay - yield to ESP8266 watchdog and WiFi tasks
    delay(100);
}
