#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include <esp_task_wdt.h>
#include <WiFi.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_wifi.h"
// FreeRTOS semaphore for SPI/TFT/SD mutual exclusion
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define SD_CS_PIN 5
#define SD_MOSI 22
#define SD_MISO 19
#define SD_SCK 21
#define BUTTON_UP 14      // Up/Previous button
#define BUTTON_DOWN 13    // Down/Next button  
#define BUTTON_SELECT 15  // Select button

// UART pins for QMK communication
#define QMK_RX_PIN 16     // ESP32 receives from RP2040
#define QMK_TX_PIN 17     // ESP32 sends to RP2040

// Create second serial port for QMK communication
HardwareSerial QMKSerial(2);

AnimatedGIF gif;
File gifFile;
TFT_eSPI tft = TFT_eSPI(); 
// Inclusive display end coordinates (used by GIFDraw.ino)
int displayXEnd;
int displayYEnd;
// Use a dedicated SPIClass instance for SD operations so we can control transactions
SPIClass sdSPI(VSPI);

// Mutex used to serialize access to SPI/TFT/SD to avoid concurrency races
SemaphoreHandle_t spiMutex = NULL;

// Array to store GIF filenames (for browsing only)
String gifFiles[20];
int gifCount = 0;
int currentGifIndex = 0;

// Button handling
bool buttonUpPressed = false;
bool buttonDownPressed = false;
bool buttonSelectPressed = false;
unsigned long lastButtonPress = 0;
unsigned long buttonSelectPressStart = 0;
bool buttonSelectHeld = false;

// Menu system
bool inMenu = false;
int menuSelection = 0;
unsigned long menuTimeout = 0;
#define MENU_TIMEOUT 5000  // 5 seconds
#define BUTTON_HOLD_TIME 500  // 500ms to trigger menu

// Maximum GIF size allowed (1.5 MB)
#define MAX_GIF_SIZE_BYTES (1536UL * 1024UL)

// Current loaded GIF
String currentLoadedGif = "";
String currentGifPath = ""; // Path to current GIF (SD or SPIFFS)

// Debug popup state (multi-line, resizable centered overlay)
bool debugPopupActive = false;
unsigned long debugPopupUntil = 0;
String debugPopupTitle = "";   // e.g. "DEBUG:" or "SOCD"
String debugPopupLine1 = "";   // first body line
String debugPopupLine2 = "";   // second body line (optional)
unsigned long debugPopupDefaultMs = 1500; // default popup time
int debugPopupPaddingX = 22; // a few pixels wider
int debugPopupPaddingY = 12;
bool debugPopupHighlightKeycode = false;
uint16_t debugPopupLine1Color = TFT_WHITE;
// Request flag to safely activate a popup at a safe point in the GIF playback loop
bool debugPopupRequest = false;
// When true the next GIF frame will be drawn as a full opaque frame (transparent pixels replaced
// with the GIF background) so it completely overwrites any overlays. Cleared after one frame.
bool forceFullFrame = false;
// Per-line horizontal offsets (pixels). Can be negative or positive to shift text left/right.
int debugPopupOffsetTitleX = 0;
int debugPopupOffsetLine1X = 0;
int debugPopupOffsetLine2X = 0;
// Per-popup local offsets (set when creating a popup; if 0, globals are used)
int debugPopupLocalOffsetTitleX = 0;
int debugPopupLocalOffsetLine1X = 0;
int debugPopupLocalOffsetLine2X = 0;

// ═══════════════════════════════════════════════════════════════════════════════
// MANUAL TEXT OFFSET TABLE - Edit the X_OFFSET values below to position each text string
// ═══════════════════════════════════════════════════════════════════════════════
// HOW TO USE:
// 1. Find the exact text string you want to move (title, line1, or line2)
// 2. Set X_OFFSET: positive = shift RIGHT, negative = shift LEFT
// 3. All text is left-aligned by default at the left edge of the popup box
// 4. Compile and upload to see changes
// 
// Example entries (add your own below):
struct ManualOffset {
  const char* text;      // Exact text string to match
  int X_OFFSET;          // Pixels to shift: negative=left, positive=right
};

ManualOffset manualOffsets[] = {
  // Add your text strings and offsets here:
  // {"Keypress detection", 10},      
  {"Snappy Tappy", -13},                             
  {"SOCD: ON", -2},                 
  {"SOCD: OFF", -4},                 
  {"ADC printing", -13},              
  {"KEYCODE", 3},                     
  {"Raw Keycodes", -13},            
  {"Press Detect", -13},
  {"Disabled!", -2},                  

  
  // Your offsets below (copy exact text from Serial Monitor when popups appear):
  
};

int manualOffsetCount = sizeof(manualOffsets) / sizeof(manualOffsets[0]);

// Lookup X_OFFSET for a given text string
int getManualOffset(const String &text) {
  // Try exact match first
  for (int i = 0; i < manualOffsetCount; i++) {
    if (text.equals(manualOffsets[i].text)) {
      return manualOffsets[i].X_OFFSET;
    }
  }
  // Try case-insensitive trimmed match
  String lookupText = text;
  lookupText.trim();
  lookupText.toUpperCase();
  for (int i = 0; i < manualOffsetCount; i++) {
    String entryText = String(manualOffsets[i].text);
    entryText.trim();
    entryText.toUpperCase();
    if (lookupText.equals(entryText)) {
      return manualOffsets[i].X_OFFSET;
    }
  }
  
  // Special rule: any string starting with "0x" (hex keycodes) gets +8px offset
  if (text.startsWith("0x") || text.startsWith("0X")) {
    return 8;
  }
  
  return 0; // no offset found, use default (left edge)
}

// GIF loading state
bool pendingGifLoad = false;
int pendingGifIndex = -1;

// Track last serial-announced playing GIF to avoid spamming the serial port
String lastPlayedSerial = "";

// Function declarations
void handleButtons();
void handleQMKCommands();
void enterMenu();
void exitMenu();
void selectCurrentGif();
void drawMenu();
void playCurrentGif();
void scanAllGifs();
void *fileOpen(const char *filename, int32_t *pFileSize);
void fileClose(void *pHandle);
int32_t fileRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen);
int32_t fileSeek(GIFFILE *pFile, int32_t iPosition);
bool copyFile(const char *srcPath, const char *dstPath);

void setup()
{
  // ========== POWER OPTIMIZATION - DISABLE UNUSED RADIOS ==========
  // This MUST be done before Serial.begin() for maximum effect
  
  // Completely disable WiFi
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_wifi_deinit();
  
  // Completely disable Bluetooth and release memory
  btStop();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  esp_bt_mem_release(ESP_BT_MODE_BTDM);
  
  // Reduce CPU frequency for power savings
  // 160MHz is a good balance - try 80MHz if you want more savings
  // setCpuFrequencyMhz(160); // was 240
  
  // ========== END POWER OPTIMIZATION ==========
  
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting setup...");
  Serial.printf("CPU Frequency: %d MHz (power optimized)\n", getCpuFrequencyMhz());
  Serial.println("WiFi and Bluetooth disabled for low power operation");

  // Initialize buttons
  pinMode(BUTTON_UP, INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_SELECT, INPUT_PULLUP);

  // Initialize QMK Serial communication
  QMKSerial.begin(115200, SERIAL_8N1, QMK_RX_PIN, QMK_TX_PIN);
  Serial.println("QMK Serial initialized on pins 16(RX)/17(TX)");

  // Hardware reset for TFT (if RST pin is connected to GPIO 4)
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  delay(10);
  digitalWrite(4, HIGH);
  delay(10);

  tft.begin();
  Serial.println("Initializing TFT...");
  tft.writecommand(0x01); // Software reset
  delay(150);
  tft.writecommand(0x11); // Sleep out
  delay(120);
  tft.init();
  delay(100);
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  // Set inclusive end coordinates for display (used by GIFDraw clipping)
  displayXEnd = tft.width() - 1;
  displayYEnd = tft.height() - 1;

  // Initialize SD card with custom SPI pins
  Serial.println("SD card initialization...");
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS_PIN);
  bool sdInitialized = false;
  for (int attempts = 0; attempts < 3; attempts++) {
    if (SD.begin(SD_CS_PIN, sdSPI)) {
      sdInitialized = true;
      Serial.println("SD card initialized successfully!");
      break;
    }
    Serial.printf("SD initialization attempt %d failed, retrying...\n", attempts + 1);
    delay(1000);
  }
  if (!sdInitialized) {
    Serial.println("SD card initialization failed after 3 attempts!");
    tft.fillScreen(TFT_RED);
    tft.setCursor(10, 10);
    tft.print("SD CARD FAILED");
    return;
  }

  // Initialize SPIFFS
  Serial.println("Initialize SPIFFS...");
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed!");
    tft.fillScreen(TFT_RED);
    tft.setCursor(10, 30);
    tft.print("SPIFFS FAILED");
    return;
  }
  Serial.println("SPIFFS initialized successfully.");

  // Scan SD for GIF files (but don't auto-play)
  scanAllGifs();
  if (gifCount == 0) {
    Serial.println("No GIF files found on SD!");
    tft.fillScreen(TFT_RED);
    tft.setCursor(10, 50);
    tft.print("NO GIFS FOUND ON SD");
    delay(2000);
  }

  // Check for existing SPIFFS GIF (/current.gif or numbered)
  bool foundExistingGif = false;
  String existingSpiffsPath = "";
  if (SPIFFS.exists("/current.gif")) {
    existingSpiffsPath = "/current.gif";
    foundExistingGif = true;
    Serial.println("Found existing SPIFFS GIF: /current.gif");
  } else {
    for (int i = 0; i < gifCount && !foundExistingGif; i++) {
      String testPath = "/gif_" + String(i) + ".gif";
      if (SPIFFS.exists(testPath.c_str())) {
        existingSpiffsPath = testPath;
        foundExistingGif = true;
        Serial.printf("Found existing SPIFFS GIF: %s\n", testPath.c_str());
        break;
      }
    }
  }

  if (foundExistingGif) {
    File existingGif = SPIFFS.open(existingSpiffsPath.c_str(), FILE_READ);
    if (existingGif && existingGif.size() > 0) {
      Serial.printf("Using existing SPIFFS GIF (%d bytes) at %s\n", existingGif.size(), existingSpiffsPath.c_str());
      size_t existingSize = existingGif.size();
      existingGif.close();
      // try to match by size
      currentGifIndex = 0;
      for (int i = 0; i < gifCount; i++) {
        String sdPath = "/" + gifFiles[i];
        File sdGif = SD.open(sdPath.c_str());
        if (sdGif && sdGif.size() == existingSize) {
          currentGifIndex = i;
          currentLoadedGif = gifFiles[i];
          currentGifPath = existingSpiffsPath;
          Serial.printf("Matched existing SPIFFS GIF to index %d: %s\n", i, gifFiles[i].c_str());
          sdGif.close();
          break;
        }
        if (sdGif) sdGif.close();
      }
    } else {
      Serial.printf("Existing SPIFFS GIF corrupted at %s, using first SD GIF\n", existingSpiffsPath.c_str());
      existingGif.close();
      currentGifIndex = 0;
      currentLoadedGif = gifFiles[0];
      currentGifPath = "/" + gifFiles[0];
    }
  } else {
    Serial.println("No existing SPIFFS GIF found, opening menu for selection...");
    currentGifIndex = 0;
    // Important: Don't set currentGifPath to an SD path as we never want to play from SD
    // Only set placeholders until user makes a selection
    currentLoadedGif = "";
    currentGifPath = "";
    
    // Set flag to enter menu after setup
    inMenu = true;
    menuSelection = 0;
    menuTimeout = millis() + 60000; // Longer timeout (60 seconds) on initial startup
  }

  // Initialize the GIF
  Serial.println("Starting animation...");
  gif.begin(BIG_ENDIAN_PIXELS);

  // Create mutex for SPI/TFT/SD serialization
  spiMutex = xSemaphoreCreateMutex();
  if (!spiMutex) {
    Serial.println("Failed to create SPI mutex!");
  }

  tft.fillScreen(TFT_BLACK);
  
  // If we're starting in menu mode, draw the menu right away
  if (inMenu) {
    drawMenu();
  }
}

void loop()
{
  handleButtons();
  handleQMKCommands();
  
  if (inMenu) {
    // Menu mode - handle timeout
    if (millis() - menuTimeout > MENU_TIMEOUT) {
      exitMenu();
    }
  } else {
    // GIF playback mode - no more pending loads, direct SD card playback
    playCurrentGif();
  }
}

void handleQMKCommands() {
  if (QMKSerial.available()) {
    String command = QMKSerial.readStringUntil('\n');
    command.trim();
    
    Serial.printf("QMK Command received: %s\n", command.c_str());
    // Allow new incoming RAW/DEBUG messages to overwrite the current popup immediately
    // (do not ignore them) — the popup drawing/wait loop will redraw while paused.

  // First, detect RAW_KEYCODE anywhere (some code may send it without [DEBUG])
    if (command.indexOf("RAW_KEYCODE") >= 0) {
      int idx = command.indexOf("0x");
      String code = "";
      if (idx >= 0) {
        // capture 0x + up to 4 hex digits
        code += "0x";
        int pos = idx + 2;
        for (int i = 0; i < 4 && pos < (int)command.length(); i++, pos++) {
          char c = command.charAt(pos);
          if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
            code += c;
          } else {
            break;
          }
        }
      }
      // Draw immediately without pausing GIF playback
      debugPopupTitle = "KEYCODE";
      debugPopupLine1 = code;
      debugPopupLine2 = "";
  // initialize per-popup local offsets to inherit global defaults (can be overridden per-popup)
  debugPopupLocalOffsetTitleX = debugPopupOffsetTitleX;
  debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
  debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
      debugPopupHighlightKeycode = true;
      debugPopupLine1Color = TFT_YELLOW;
      debugPopupUntil = millis() + 750; // visible briefly
      // Acquire mutex and draw inline immediately so playback continues
      if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
      tft.startWrite();
      drawDebugPopupInline();
      tft.endWrite();
      if (spiMutex) xSemaphoreGive(spiMutex);
      Serial.println("Triggered immediate RAW_KEYCODE popup");
      return;
    }

    // Also accept raw SOCD messages that may come without the [DEBUG] prefix
    // e.g. "SOCD: Enabled" or "SOCD: Disabled"
    String upRaw = command;
    upRaw.toUpperCase();
      if (upRaw.indexOf("SOCD:") >= 0) {
        bool socdOn = (upRaw.indexOf("ENABLED") >= 0 || upRaw.indexOf("ON") >= 0);
        debugPopupTitle = "Snappy Tappy";
        if (socdOn) {
          debugPopupLine1 = "SOCD: ON";
          debugPopupLine1Color = TFT_GREEN;
        } else {
          debugPopupLine1 = "SOCD: OFF";
          debugPopupLine1Color = TFT_RED;
        }
        debugPopupLine2 = "";
        debugPopupLocalOffsetTitleX = -20;
        debugPopupLocalOffsetLine1X = 0;
        debugPopupLocalOffsetLine2X = 0;
        debugPopupHighlightKeycode = false;
        debugPopupUntil = millis() + debugPopupDefaultMs;
        // Draw immediately without pausing GIF
        if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
        tft.startWrite();
        drawDebugPopupInline();
        tft.endWrite();
        if (spiMutex) xSemaphoreGive(spiMutex);
        Serial.println("Triggered immediate SOCD popup (raw)");
        return;
      }

      // Also accept raw ADC printing messages that may come without the [DEBUG] prefix
      // e.g. "ADC printing: ON" or "ADC printing: OFF"
      if (upRaw.indexOf("ADC PRINTING") >= 0) {
        bool adcOn = (upRaw.indexOf("ON") >= 0 || upRaw.indexOf("ENABLED") >= 0);
        debugPopupTitle = "ADC printing";
        if (adcOn) {
          debugPopupLine1 = "Enabled!";
          debugPopupLine1Color = TFT_GREEN;
        } else {
          debugPopupLine1 = "Disabled!";
          debugPopupLine1Color = TFT_RED;
        }
        debugPopupLine2 = "";
        // use global offsets by default; you can tweak these per-message below
        debugPopupLocalOffsetTitleX = debugPopupOffsetTitleX - 30;
        debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
        debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
        debugPopupHighlightKeycode = false;
        debugPopupUntil = millis() + debugPopupDefaultMs;
        // Draw immediately without pausing GIF (prevents flicker)
        if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
        tft.startWrite();
        drawDebugPopupInline();
        tft.endWrite();
        if (spiMutex) xSemaphoreGive(spiMutex);
        Serial.println("Triggered immediate ADC popup (raw)");
        return;
      }

    // Detect debug messages coming from UART and trigger the on-screen popup
    // Supported formats handled here (examples):
    //  "[DEBUG] ADC printing: ON"
    //  "[DEBUG] ADC printing: OFF"
    //  "[DEBUG] SOCD: Enabled"  or "[DEBUG] SOCD Enabled"
    if (command.startsWith("[DEBUG]")) {
      // Normalize to uppercase for easier detection but keep original for display if we echo
      String up = command;
      up.toUpperCase();

      // ADC printing ON/OFF
      if (up.indexOf("ADC PRINTING") >= 0) {
        // Friendly title for ADC state
        debugPopupTitle = "ADC printing";
        if (up.indexOf("ON") >= 0) {
          debugPopupLine1 = "Enabled!";
          debugPopupLine2 = "";
          debugPopupLine1Color = TFT_GREEN;
        } else {
          debugPopupLine1 = "Disabled!";
          debugPopupLine2 = "";
          debugPopupLine1Color = TFT_RED;
        }
  debugPopupLocalOffsetTitleX = (debugPopupOffsetTitleX -30);
  debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
  debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
  debugPopupUntil = millis() + debugPopupDefaultMs;
  debugPopupRequest = true;
        debugPopupHighlightKeycode = false;
        Serial.println("Triggered debug popup: ADC printing state");
        return; // consumed
      }

      // SOCD messages - show a clear single-line popup "SOCD: ON" or "SOCD: OFF"
      if (up.indexOf("SOCD") >= 0) {
        bool socdOn = false;
        // consider variants that indicate enabled/on
        if (up.indexOf("ENABLED") >= 0 || up.indexOf("ENABLE") >= 0 || up.indexOf("ON") >= 0) {
          socdOn = true;
        }

        // Friendly title and a single body line indicating ON/OFF
  debugPopupTitle = "Snap Tap";
        if (socdOn) {
          debugPopupLine1 = "SOCD: ON";
          debugPopupLine1Color = TFT_GREEN;
        } else {
          debugPopupLine1 = "SOCD: OFF";
          debugPopupLine1Color = TFT_RED;
        }
        debugPopupLine2 = ""; // single-line body for SOCD
  debugPopupLocalOffsetTitleX = (-20);
  debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
  debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
        debugPopupHighlightKeycode = false;
        debugPopupUntil = millis() + debugPopupDefaultMs;
        debugPopupRequest = true;
        Serial.println("Triggered debug popup: SOCD state");
        return; // consumed
      }

      // Key printing ON/OFF (from QMK toggle_key_debug)
      if (up.indexOf("KEY PRINTING") >= 0) {
        debugPopupTitle = "Press Detect";
        if (up.indexOf("ON") >= 0) {
          debugPopupLine1 = "Enabled!";
          debugPopupLine1Color = TFT_GREEN;
        } else {
          debugPopupLine1 = "Disabled!";
          debugPopupLine1Color = TFT_RED;
        }
        debugPopupLine2 = "";
        debugPopupLocalOffsetTitleX = debugPopupOffsetTitleX;
        debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
        debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
        debugPopupHighlightKeycode = false;
        debugPopupUntil = millis() + debugPopupDefaultMs;
        // Draw immediately without pausing GIF
        if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
        tft.startWrite();
        drawDebugPopupInline();
        tft.endWrite();
        if (spiMutex) xSemaphoreGive(spiMutex);
        Serial.println("Triggered immediate Key Printing popup");
        return;
      }

      // Raw keycodes ON/OFF (from QMK toggle_raw_debug)
      if (up.indexOf("RAW KEYCODES") >= 0) {
        debugPopupTitle = "Raw Keycodes";
        if (up.indexOf("ON") >= 0) {
          debugPopupLine1 = "Enabled!";
          debugPopupLine1Color = TFT_GREEN;
        } else {
          debugPopupLine1 = "Disabled!";
          debugPopupLine1Color = TFT_RED;
        }
        debugPopupLine2 = "";
        debugPopupLocalOffsetTitleX = debugPopupOffsetTitleX;
        debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
        debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
        debugPopupHighlightKeycode = false;
        debugPopupUntil = millis() + debugPopupDefaultMs;
        // Draw immediately without pausing GIF
        if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
        tft.startWrite();
        drawDebugPopupInline();
        tft.endWrite();
        if (spiMutex) xSemaphoreGive(spiMutex);
        Serial.println("Triggered immediate Raw Keycodes popup");
        return;
      }

      // RAW_KEYCODE messages (format from attachment: "RAW_KEYCODE: 0x%04X\n")
      if (up.indexOf("RAW_KEYCODE") >= 0) {
        // find '0x' and capture the hex token
        int idx = command.indexOf("0x");
        String code = "";
        if (idx >= 0) {
          // take up to 6 chars (0x + up to 4 hex digits)
          int len = min(6, (int)command.length() - idx);
          code = command.substring(idx, idx + len);
          code.trim();
        } else {
          // fallback: show rest of message after RAW_KEYCODE
          code = command.substring(command.indexOf("RAW_KEYCODE") + 11);
          code.trim();
        }
  // Friendly custom title for debug path
  debugPopupTitle = "Keypress";
    debugPopupLine1 = "detection";
    debugPopupLine2 = code;
  debugPopupLocalOffsetTitleX = debugPopupOffsetTitleX;
  debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
  debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
  debugPopupUntil = millis() + 750; // visible briefly
    debugPopupHighlightKeycode = true;
    debugPopupLine1Color = TFT_YELLOW;
        // Draw immediately without pausing GIF (like ADC popups)
        if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
        tft.startWrite();
        drawDebugPopupInline();
        tft.endWrite();
        if (spiMutex) xSemaphoreGive(spiMutex);
        Serial.println("Triggered immediate RAW_KEYCODE popup");
        return;
      }

      // Unknown debug: generic fallback
      int colonPos = command.indexOf(':', 7);
      debugPopupTitle = "DEBUG";
      if (colonPos >= 7) {
        debugPopupLine1 = command.substring(colonPos + 1);
        debugPopupLine1.trim();
      } else {
        debugPopupLine1 = command.substring(7);
        debugPopupLine1.trim();
      }
  debugPopupLine2 = "";
  debugPopupLocalOffsetTitleX = debugPopupOffsetTitleX;
  debugPopupLocalOffsetLine1X = debugPopupOffsetLine1X;
  debugPopupLocalOffsetLine2X = debugPopupOffsetLine2X;
  debugPopupUntil = millis() + debugPopupDefaultMs;
      debugPopupHighlightKeycode = false;
      debugPopupLine1Color = TFT_WHITE;
      // Draw immediately to avoid flicker
      if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
      tft.startWrite();
      drawDebugPopupInline();
      tft.endWrite();
      if (spiMutex) xSemaphoreGive(spiMutex);
      Serial.println("Triggered immediate generic debug popup");
      return;
    }
    
    if (command == "MENU_OPEN") {
      if (!inMenu) {
        enterMenu();
        QMKSerial.println("MENU_OPENED");
      } else {
        QMKSerial.println("MENU_ALREADY_OPEN");
      }
    }
    else if (command == "MENU_CLOSE") {
      if (inMenu) {
        exitMenu();
        QMKSerial.println("MENU_CLOSED");
      } else {
        QMKSerial.println("MENU_NOT_OPEN");
      }
    }
    else if (command == "MENU_UP") {
      if (inMenu) {
        int total = gifCount + 1; // include Clear GIF
        menuSelection = (menuSelection - 1 + total) % total;
        Serial.printf("QMK UP - selection: %d\n", menuSelection);
        menuTimeout = millis(); // Reset timeout
        drawMenu();
        QMKSerial.printf("MENU_POS:%d\n", menuSelection);
      } else {
        QMKSerial.println("MENU_NOT_OPEN");
      }
    }
    else if (command == "MENU_DOWN") {
      if (inMenu) {
        int total = gifCount + 1; // include Clear GIF
        menuSelection = (menuSelection + 1) % total;
        Serial.printf("QMK DOWN - selection: %d\n", menuSelection);
        menuTimeout = millis(); // Reset timeout
        drawMenu();
        QMKSerial.printf("MENU_POS:%d\n", menuSelection);
      } else {
        QMKSerial.println("MENU_NOT_OPEN");
      }
    }
    else if (command == "MENU_SELECT") {
      if (inMenu) {
        Serial.printf("QMK SELECT - choosing GIF %d\n", menuSelection);
        selectCurrentGif();
        QMKSerial.printf("GIF_SELECTED:%s\n", gifFiles[currentGifIndex].c_str());
      } else {
        QMKSerial.println("MENU_NOT_OPEN");
      }
    }
    else if (command == "STATUS") {
      // Send current status to QMK
      QMKSerial.printf("STATUS:MENU=%s,GIF=%s,POS=%d,COUNT=%d\n", 
                       inMenu ? "OPEN" : "CLOSED",
                       gifFiles[currentGifIndex].c_str(),
                       menuSelection,
                       gifCount);
    }
    else {
      QMKSerial.println("UNKNOWN_COMMAND");
    }
  }
}

void handleButtons() {
  bool buttonUpCurrentlyPressed = (digitalRead(BUTTON_UP) == LOW);
  bool buttonDownCurrentlyPressed = (digitalRead(BUTTON_DOWN) == LOW);
  bool buttonSelectCurrentlyPressed = (digitalRead(BUTTON_SELECT) == LOW);
  
  // Handle UP button
  if (buttonUpCurrentlyPressed && !buttonUpPressed && (millis() - lastButtonPress > 200)) {
    buttonUpPressed = true;
    lastButtonPress = millis();
    
    if (inMenu) {
      int total = gifCount + 1; // include Clear GIF
      menuSelection = (menuSelection - 1 + total) % total;
      Serial.printf("UP pressed - selection: %d\n", menuSelection);
      menuTimeout = millis(); // Reset timeout
      drawMenu();
    }
  }
  else if (!buttonUpCurrentlyPressed) {
    buttonUpPressed = false;
  }
  
  // Handle DOWN button  
  if (buttonDownCurrentlyPressed && !buttonDownPressed && (millis() - lastButtonPress > 200)) {
    buttonDownPressed = true;
    lastButtonPress = millis();
    
    if (inMenu) {
      int total = gifCount + 1; // include Clear GIF
      menuSelection = (menuSelection + 1) % total;
      Serial.printf("DOWN pressed - selection: %d\n", menuSelection);
      menuTimeout = millis(); // Reset timeout
      drawMenu();
    }
  }
  else if (!buttonDownCurrentlyPressed) {
    buttonDownPressed = false;
  }
  
  // Handle SELECT button (with hold detection)
  if (buttonSelectCurrentlyPressed && !buttonSelectPressed) {
    buttonSelectPressed = true;
    buttonSelectPressStart = millis();
    buttonSelectHeld = false;
  }
  
  // Check for hold
  if (buttonSelectCurrentlyPressed && buttonSelectPressed && !buttonSelectHeld) {
    if (millis() - buttonSelectPressStart > BUTTON_HOLD_TIME) {
      buttonSelectHeld = true;
      if (!inMenu) {
        enterMenu();
      }
    }
  }
  
  // Handle release
  if (!buttonSelectCurrentlyPressed && buttonSelectPressed) {
    buttonSelectPressed = false;
    
    // Quick press (not held)
    if (!buttonSelectHeld) {
      if (inMenu) {
        Serial.printf("SELECT pressed - choosing GIF %d\n", menuSelection);
        selectCurrentGif();
      }
    }
  }
}

void enterMenu() {
  Serial.println("Entering menu mode");
  inMenu = true;
  int totalItems = gifCount + 1; // include Clear GIF
  menuSelection = min(currentGifIndex, totalItems - 1); // Start with current GIF selected but clamp
  menuTimeout = millis();
  drawMenu();
}

void exitMenu() {
  Serial.println("Exiting menu mode");
  inMenu = false;
  tft.fillScreen(TFT_BLACK);
}

void selectCurrentGif() {
  // Handle selecting the special 'Clear GIF' menu item
  if (menuSelection == gifCount) {
    Serial.println("Clear GIF selected - deleting /current.gif from SPIFFS");
    if (SPIFFS.exists("/current.gif")) {
      SPIFFS.remove("/current.gif");
      Serial.println("Deleted /current.gif from SPIFFS");
    } else {
      Serial.println("No /current.gif present in SPIFFS");
    }
    tft.fillScreen(TFT_YELLOW);
    tft.setCursor(10, 60);
    tft.setTextColor(TFT_BLACK);
    tft.print("SPIFFS cleared");
    delay(800);
    currentLoadedGif = "";
    currentGifPath = "";
    enterMenu(); // Go back to menu
    return;
  }

  // Regular GIF selection from SD
  if (menuSelection < 0 || menuSelection >= gifCount) {
    Serial.printf("Invalid selection: %d\n", menuSelection);
    return;
  }

  String sdPath = "/" + gifFiles[menuSelection];
  Serial.printf("Selected SD GIF for copy/play: %s (index %d)\n", sdPath.c_str(), menuSelection);

  // Stop any ongoing GIF playback
  Serial.println("Stopping current GIF playback (if any)");
  gif.close();
  tft.dmaWait(); // Wait for any DMA to finish
  delay(50);

  // Now, use the copyFile function
  if (copyFile(sdPath.c_str(), "/current.gif")) {
    Serial.println("File copied successfully.");
    tft.fillScreen(TFT_GREEN);
    tft.setCursor(10, 60);
    tft.setTextColor(TFT_BLACK);
    tft.print("GIF Saved!");
    delay(1000);

    // Update current state to play from SPIFFS
    currentGifIndex = menuSelection;
    currentLoadedGif = gifFiles[menuSelection];
    currentGifPath = "/current.gif";
    exitMenu(); // Exit menu to start playback
  } else {
    Serial.println("File copy failed.");
    tft.fillScreen(TFT_RED);
    tft.setCursor(10, 60);
    tft.print("Copy Failed!");
    delay(1000);
    drawMenu(); // Go back to the menu
  }
}

void drawMenu() {
  // Guard drawing with the SPI mutex to avoid concurrent SPI access causing partial frames
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  tft.startWrite();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(10, 5);
  tft.setTextSize(1);
  tft.print("Select GIF:");

  int yPos = 25;
  int startIndex = 0;
  int maxVisible = 7; // number of visible items including Clear GIF
  int totalItems = gifCount + 1; // gifs + Clear GIF

  // Compute startIndex so selected item is visible and acts like a scrolling list
  if (menuSelection >= maxVisible) {
    startIndex = menuSelection - maxVisible + 1;
  }
  // Clamp startIndex so we don't go past the end
  if (startIndex > (totalItems - maxVisible)) {
    startIndex = max(0, totalItems - maxVisible);
  }

  int endIndex = min(totalItems, startIndex + maxVisible);

  for (int i = startIndex; i < endIndex; i++) {
    tft.setCursor(10, yPos);

    // Handle Clear GIF as the final item (i == gifCount)
    if (i == gifCount) {
      // Clear GIF entry
      if (i == menuSelection) {
        // Highlight with yellow background when selected
        tft.fillRect(8, yPos - 2, tft.width() - 16, 13, TFT_YELLOW);
        tft.setTextColor(TFT_BLACK);
        tft.print("> Clear GIF");
      } else {
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.print("  Clear GIF");
      }
    } else {
      // Regular GIF entry
      // Highlight selected item with inverted colors
      if (i == menuSelection) {
        tft.setTextColor(TFT_BLACK, TFT_WHITE); // Inverted colors
        tft.print("> ");
      } else {
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.print("  ");
      }

      // Remove path and extension for cleaner display
      String displayName = gifFiles[i];
      displayName.replace("/", "");
      displayName.replace(".gif", "");
      displayName.replace(".GIF", "");

      tft.print(displayName);

      // Show if this is currently loaded
      if (i == currentGifIndex) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.print(" (current)");
      }
    }

    yPos += 13;
  }

  // Show current selection info (bottom-left), exclude Clear GIF from counter (hide when Clear is selected)
  if (menuSelection < gifCount) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(10, tft.height() - 12);
    tft.printf("Selection: %d/%d", menuSelection + 1, gifCount);
  }

  // Draw a thin 'pong'-like cursor on the right when needed (no visible track)
  int scrollX = tft.width() - 10;
  int scrollTop = 25;
  // leave a bottom margin so scrollbar never overlaps selection text
  int bottomMargin = 18;
  int maxScrollHeight = (tft.height() - scrollTop - bottomMargin);
  int scrollHeight = min(maxVisible * 13, maxScrollHeight);
  if (scrollHeight < 20) scrollHeight = 20; // minimum area
  if (totalItems > maxVisible) {
    int cursorW = 4; // thin cursor
    int thumbH = 6; // fixed short height

    // compute slot height for visible items so the thumb moves in discrete steps per selection
    float slotH = (float)scrollHeight / (float)maxVisible;
    int indexInWindow = menuSelection - startIndex;
    if (indexInWindow < 0) indexInWindow = 0;
    if (indexInWindow >= maxVisible) indexInWindow = maxVisible - 1;
    int thumbY = scrollTop + (int)(indexInWindow * slotH + (slotH - thumbH) / 2.0);

    // Clamp to scroll area
    if (thumbY < scrollTop) thumbY = scrollTop;
    if (thumbY > (scrollTop + scrollHeight - thumbH)) thumbY = scrollTop + scrollHeight - thumbH;

    // Draw only the thumb (no track) in white on top of everything
    tft.fillRect(scrollX, thumbY, cursorW, thumbH, TFT_WHITE);
  } else {
    // when no scrolling needed, draw a short centered cursor for visual hint
    int cursorW = 4;
    int thumbH = 6;
    int thumbY = scrollTop + (scrollHeight - thumbH) / 2;
    tft.fillRect(scrollX, thumbY, cursorW, thumbH, TFT_WHITE);
  }
  tft.endWrite();
  if (spiMutex) xSemaphoreGive(spiMutex);
  
}

void playCurrentGif() {
  // Check if the current GIF file exists before trying to play it
  if (currentGifPath == "") {
    // No GIF path set, enter menu
    Serial.println("No GIF path set, entering menu");
    enterMenu();
    return;
  }

  bool isSpiffsPath = currentGifPath.startsWith("/gif_") || currentGifPath.equals("/current.gif");
  
  // IMPORTANT: NEVER play from SD - always show menu if no SPIFFS GIF
  if (!isSpiffsPath) {
    Serial.println("SD path detected, switching to menu instead");
    enterMenu();
    return;
  }
  
  bool fileExists = SPIFFS.exists(currentGifPath.c_str());
  
  if (!fileExists) {
    Serial.printf("SPIFFS GIF not found: %s, opening menu\n", currentGifPath.c_str());
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(10, 40);
    tft.setTextColor(TFT_RED);
    tft.printf("SPIFFS GIF not found");
    tft.setCursor(10, 60);
    tft.print("Opening menu...");
    delay(1000);
    enterMenu();
    return;
  }

  // Use appropriate callback functions for SPIFFS
  bool gifOpened = false;
  // Ensure AnimatedGIF internal state is reset before opening a new file
  gif.close();
  delay(50); // give some time for previous operations to settle
  gif.begin(BIG_ENDIAN_PIXELS);
  yield();
  
  // Only announce playing GIF when it changes (or first time)
  if (lastPlayedSerial != currentLoadedGif) {
    lastPlayedSerial = currentLoadedGif;
    Serial.printf("Playing: %s\n", currentLoadedGif.c_str());
  }
  // Ensure any TFT DMA is complete before opening file
  tft.dmaWait();
  delay(20);
  gifOpened = gif.open(currentGifPath.c_str(), fileOpen, fileClose, fileRead, fileSeek, GIFDraw);

  if (currentLoadedGif != "" && gifOpened)
  {
    // Reset any popup state to avoid accidental blocking of GIF drawing
    debugPopupActive = false;
    debugPopupRequest = false;
    tft.startWrite();

    // Main playback loop that respects popup-pauses
    while (true) {
      // If a popup has been requested, activate it at a safe point (between frame writes)
      if (debugPopupRequest) {
        // finish current write block and wait for any DMA from the GIF draw to complete
        tft.endWrite();
        tft.dmaWait();

        // acquire mutex and draw the popup once to ensure it appears intact
        if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
        tft.startWrite();
        debugPopupActive = true;
        debugPopupRequest = false;
        drawDebugPopupInline();
        tft.endWrite();
        if (spiMutex) xSemaphoreGive(spiMutex);
      }

      // If popup active, pause GIF playback until popup expires
      if (debugPopupActive) {
        // finish current write block and wait for any DMA from the GIF draw to complete
        tft.endWrite();
        tft.dmaWait();

        // draw overlay in its own protected transaction so it stays on top
        if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
        tft.startWrite();
        drawDebugPopupInline();
        tft.endWrite();
        if (spiMutex) xSemaphoreGive(spiMutex);

        // Wait (non-blocking) until popup expires while still processing input
        while (debugPopupActive && millis() < debugPopupUntil) {
          handleButtons();
          handleQMKCommands();
          // redraw the overlay while holding the mutex so new messages overwrite immediately
          if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
          tft.startWrite();
          drawDebugPopupInline();
          tft.endWrite();
          if (spiMutex) xSemaphoreGive(spiMutex);
          // small delay to yield CPU
          delay(10);
        }

  // popup expired, make sure it's deactivated
  debugPopupActive = false;
  // small safety delay so the box fully disappears before GIF resumes drawing
  delay(5);
  // Ensure the next frame fully overwrites any residual overlay (make it opaque)
  forceFullFrame = true;
  // restart the write block for continuing playback
  tft.startWrite();
      }

      // If not paused, advance one frame
      if (!gif.playFrame(true, NULL)) break; // playback finished
      // If we forced a full-frame to overwrite overlays, clear the flag now so only one frame is full
      if (forceFullFrame) {
        forceFullFrame = false;
      }

      // let input be processed
      handleButtons();
      handleQMKCommands();

      if (inMenu) {
        gif.close();
        tft.endWrite();
        return; // Exit to show menu
      }

      // Tiny delay to avoid starving other tasks
      delay(1);
    }
    gif.close();
    tft.endWrite();
  }
  else {
    Serial.printf("Failed to open GIF for playback: %s\n", currentGifPath.c_str());
    delay(1000);
  }
}

void scanAllGifs() {
  Serial.println("Scanning SD card for GIF files...");
  File root = SD.open("/");
  
  gifCount = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    
    String fileName = entry.name();
    if (fileName.endsWith(".gif") || fileName.endsWith(".GIF")) {
      size_t fsize = entry.size();
      if (fsize > MAX_GIF_SIZE_BYTES) {
        Serial.printf("Skipping large GIF (>%u bytes): %s\n", (unsigned)MAX_GIF_SIZE_BYTES, fileName.c_str());
      } else {
        if (gifCount < 20) { // Prevent array overflow
          gifFiles[gifCount] = fileName; // Store just the filename, not full path
          Serial.printf("Found GIF: %s (%u bytes)\n", fileName.c_str(), (unsigned)fsize);
          gifCount++;
        }
      }
    }
    entry.close();
  }
  root.close();
  
  Serial.printf("Total GIFs found: %d\n", gifCount);
}

// Callback functions for the AnimatedGIF library (SPIFFS)
void *fileOpen(const char *filename, int32_t *pFileSize)
{
  // Acquire SPI/TFT/SD mutex while performing filesystem operations
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  gifFile = SPIFFS.open(filename, FILE_READ);
  if (gifFile) {
    *pFileSize = gifFile.size();
  } else {
    Serial.println("Failed to open GIF file from SPIFFS!");
    *pFileSize = 0;
  }
  if (spiMutex) xSemaphoreGive(spiMutex);
  return &gifFile;
}

void fileClose(void *pHandle)
{
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  gifFile.close();
  if (spiMutex) xSemaphoreGive(spiMutex);
}

int32_t fileRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;

  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  gifFile.seek(pFile->iPos);
  int32_t bytesRead = gifFile.read(pBuf, iBytesRead);
  pFile->iPos += bytesRead;
  if (spiMutex) xSemaphoreGive(spiMutex);
  return bytesRead;
}

int32_t fileSeek(GIFFILE *pFile, int32_t iPosition)
{
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  gifFile.seek(pFile->iPos);
  if (spiMutex) xSemaphoreGive(spiMutex);
  return iPosition;
}

// Function to copy a file from SD to SPIFFS with progress
bool copyFile(const char *srcPath, const char *dstPath) {
  // Clear a slightly larger area to avoid residual edge pixels from previous screens
  tft.fillRect(6, 36, tft.width() - 12, 64, TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(12, 44);
  tft.print("Copying to flash...");
  // blank line for separation
  tft.setCursor(12, 54);
  tft.print(" ");
  // Draw a centered progress bar (leave 6px margin left/right)
  int barX = 12;
  int barY = 64;
  int barW = tft.width() - 24; // safe width
  int barH = 18;
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  tft.startWrite();
  tft.drawRect(barX - 1, barY - 1, barW + 2, barH + 2, TFT_WHITE);
  tft.endWrite();
  if (spiMutex) xSemaphoreGive(spiMutex);

  File srcFile = SD.open(srcPath);
  if (!srcFile) {
    Serial.println("Failed to open source file for reading");
    return false;
  }

  // Check the file size before copying
  size_t fileSize = srcFile.size();
  if (fileSize > MAX_GIF_SIZE_BYTES) {
    Serial.printf("Source GIF too large (%u bytes) - max allowed is %u bytes\n", (unsigned)fileSize, (unsigned)MAX_GIF_SIZE_BYTES);
    srcFile.close();
    return false;
  }
  File dstFile = SPIFFS.open(dstPath, FILE_WRITE);
  if (!dstFile) {
    Serial.println("Failed to open destination file for writing");
    srcFile.close();
    return false;
  }
  size_t bufferSize = 512;
  uint8_t buffer[bufferSize];
  size_t bytesCopied = 0;

  while (srcFile.available()) {
    int bytesRead = srcFile.read(buffer, bufferSize);
    if (bytesRead > 0) {
      dstFile.write(buffer, bytesRead);
      bytesCopied += bytesRead;

      // Update progress bar, clamp to barW
      int progress = (int)(((float)bytesCopied / fileSize) * 100);
      if (progress > 100) progress = 100;
      int fillW = (progress * barW) / 100;
      if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
      tft.startWrite();
      if (fillW > 0) {
        tft.fillRect(barX, barY, fillW, barH, TFT_GREEN);
      }
      // Small percentage text under the bar (invert background for readability)
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(barX + barW - 40, barY + barH + 4);
      tft.printf("%3d%%", progress);
      tft.endWrite();
      if (spiMutex) xSemaphoreGive(spiMutex);

      yield(); // Allow other tasks to run
    } else {
      break;
    }
  }

  srcFile.close();
  dstFile.close();

  Serial.printf("Copied %u bytes\n", bytesCopied);
  return bytesCopied == fileSize;
}

// Callback functions for SD card access
void *fileOpenSD(const char *filename, int32_t *pFileSize)
{
  // Acquire mutex to serialize SPI access
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  sdSPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  gifFile = SD.open(filename, FILE_READ);
  if (gifFile) {
    *pFileSize = gifFile.size();
    Serial.printf("Opened SD GIF: %s (%d bytes)\n", filename, *pFileSize);
  } else {
    Serial.printf("Failed to open GIF file from SD: %s\n", filename);
    *pFileSize = 0;
  }
  sdSPI.endTransaction();
  if (spiMutex) xSemaphoreGive(spiMutex);
  return &gifFile;
}

void fileCloseSD(void *pHandle)
{
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  sdSPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  gifFile.close();
  sdSPI.endTransaction();
  if (spiMutex) xSemaphoreGive(spiMutex);
}

int32_t fileReadSD(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  int32_t iBytesRead = iLen;
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos;
  if (iBytesRead <= 0)
    return 0;

  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  // Perform read inside SPI transaction
  sdSPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  gifFile.seek(pFile->iPos);
  int32_t bytesRead = gifFile.read(pBuf, iBytesRead);
  pFile->iPos += bytesRead;
  sdSPI.endTransaction();
  if (spiMutex) xSemaphoreGive(spiMutex);

  return bytesRead;
}

int32_t fileSeekSD(GIFFILE *pFile, int32_t iPosition)
{
  if (iPosition < 0)
    iPosition = 0;
  else if (iPosition >= pFile->iSize)
    iPosition = pFile->iSize - 1;
  pFile->iPos = iPosition;
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  sdSPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  gifFile.seek(pFile->iPos);
  sdSPI.endTransaction();
  if (spiMutex) xSemaphoreGive(spiMutex);
  return iPosition;
}

// Draw a debug popup assuming tft.startWrite() has already been called
void drawDebugPopupInline() {
  // NOTE: this function only draws the popup; the caller is responsible for checking
  // `debugPopupActive` and `debugPopupUntil` and for guarding with the SPI mutex and
  // startWrite/endWrite as needed. This allows immediate draws without pausing GIF.
  if (millis() > debugPopupUntil) {
    // Nothing to draw if expired
    return;
  }

  // Choose font size
  tft.setTextSize(1);
  // Use textBounds for accurate widths when available
  int16_t x1, y1; uint16_t w1, h1;
  int16_t x2, y2; uint16_t w2, h2;
  int16_t x3, y3; uint16_t w3, h3;
  w1 = w2 = w3 = 0;
  // Title
  #ifdef TFT_eSPI_h
  tft.getTextBounds(debugPopupTitle, 0, 0, &x1, &y1, &w1, &h1);
  tft.getTextBounds(debugPopupLine1, 0, 0, &x2, &y2, &w2, &h2);
  tft.getTextBounds(debugPopupLine2, 0, 0, &x3, &y3, &w3, &h3);
  #else
  w1 = debugPopupTitle.length() * 8;
  w2 = debugPopupLine1.length() * 8;
  w3 = debugPopupLine2.length() * 8;
  h1 = h2 = h3 = 10;
  #endif

  int contentW = max((int)w1, max((int)w2, (int)w3));
  // Cap box width to 70% of screen width (slightly wider box)
  int maxBoxW = tft.width() * 70 / 100;
  int boxW = contentW + debugPopupPaddingX * 2;
  // If content is wider than allowed, try a simple wrap: split long body line at a space and move remainder to line2
  int allowedContentW = maxBoxW - debugPopupPaddingX * 2;
  if (boxW > maxBoxW) {
    // attempt to split the longest body line (prefer line1)
    if (w2 > allowedContentW && debugPopupLine1.length() > 10) {
      // split debugPopupLine1 roughly in half at nearest space
      int mid = debugPopupLine1.length() / 2;
      int splitPos = -1;
      for (int i = mid; i >= 0; i--) if (debugPopupLine1.charAt(i) == ' ') { splitPos = i; break; }
      if (splitPos < 0) for (int i = mid; i < (int)debugPopupLine1.length(); i++) if (debugPopupLine1.charAt(i) == ' ') { splitPos = i; break; }
      if (splitPos > 0) {
        String a = debugPopupLine1.substring(0, splitPos);
        String b = debugPopupLine1.substring(splitPos + 1);
        // move remainder into line2 (push existing line2 down if present)
        debugPopupLine1 = a;
        if (debugPopupLine2.length() > 0) {
          // append to existing line2 with a separator
          debugPopupLine2 = b + " " + debugPopupLine2;
        } else {
          debugPopupLine2 = b;
        }
        // recompute bounds for new lines
        #ifdef TFT_eSPI_h
        tft.getTextBounds(debugPopupLine1, 0, 0, &x2, &y2, &w2, &h2);
        tft.getTextBounds(debugPopupLine2, 0, 0, &x3, &y3, &w3, &h3);
        #else
        w2 = debugPopupLine1.length() * 8;
        w3 = debugPopupLine2.length() * 8;
        #endif
        contentW = max((int)w1, max((int)w2, (int)w3));
        boxW = contentW + debugPopupPaddingX * 2;
        if (boxW <= maxBoxW) {
          // good, wrapped successfully
        } else {
          boxW = maxBoxW; // give up, cap width
        }
      } else {
        boxW = maxBoxW; // no spaces to split, cap width
      }
    } else {
      boxW = maxBoxW;
    }
  }
  int boxH = debugPopupPaddingY * 2; // add lines height below

  // line height from measured text (use h2 if available)
  int lineH = max(10, (int)h2);
  int lines = 0;
  if (debugPopupTitle.length() > 0) lines++;
  if (debugPopupLine1.length() > 0) lines++;
  if (debugPopupLine2.length() > 0) lines++;
  if (lines == 0) return; // nothing to draw

  boxH += lines * lineH + (lines - 1) * 4; // small interline spacing

  int boxX = (tft.width() - boxW) / 2;
  int boxY = (tft.height() - boxH) / 2; // center box

  // Opaque dark background to avoid any dissolve artifact
  uint16_t bgCol = tft.color565(8, 8, 8); // near-black
  uint16_t borderCol = TFT_WHITE;

  // Draw filled rounded box and border
  tft.fillRoundRect(boxX, boxY, boxW, boxH, 8, bgCol);
  tft.drawRoundRect(boxX, boxY, boxW, boxH, 8, borderCol);

  // Draw text lines centered
  tft.setTextColor(TFT_WHITE, bgCol);
  tft.setTextSize(1);
  int curY = boxY + debugPopupPaddingY;
  int centerX = boxX + boxW / 2;

  // Left-align all text and apply manual X_OFFSET from the table
  bool usedDrawString = false;
  #ifdef TFT_eSPI_h
  tft.setTextDatum(TL_DATUM); // top-left alignment
  int baseX = boxX + debugPopupPaddingX; // left edge of content area
  
  if (debugPopupTitle.length() > 0) {
    tft.setTextColor(TFT_WHITE, bgCol);
    int offsetX = getManualOffset(debugPopupTitle);
    Serial.printf("[POPUP] Title: \"%s\" | X_OFFSET: %d\n", debugPopupTitle.c_str(), offsetX);
    tft.drawString(debugPopupTitle, baseX + offsetX, curY);
    curY += lineH + 6;
    usedDrawString = true;
  }
  if (debugPopupLine1.length() > 0) {
    uint16_t col = debugPopupHighlightKeycode ? TFT_YELLOW : debugPopupLine1Color;
    tft.setTextColor(col, bgCol);
    int offsetX = getManualOffset(debugPopupLine1);
    Serial.printf("[POPUP] Line1: \"%s\" | X_OFFSET: %d\n", debugPopupLine1.c_str(), offsetX);
    tft.drawString(debugPopupLine1, baseX + offsetX, curY);
    curY += lineH + 6;
    usedDrawString = true;
  }
  if (debugPopupLine2.length() > 0) {
    tft.setTextColor(TFT_WHITE, bgCol);
    int offsetX = getManualOffset(debugPopupLine2);
    Serial.printf("[POPUP] Line2: \"%s\" | X_OFFSET: %d\n", debugPopupLine2.c_str(), offsetX);
    tft.drawString(debugPopupLine2, baseX + offsetX, curY);
    usedDrawString = true;
  }
  #endif

  // Fallback: left-align with manual offsets (for non-TFT_eSPI)
  if (!usedDrawString) {
    int baseX = boxX + debugPopupPaddingX; // left edge
    
    if (debugPopupTitle.length() > 0) {
      int offsetX = getManualOffset(debugPopupTitle);
      Serial.printf("[POPUP] Title: \"%s\" | X_OFFSET: %d\n", debugPopupTitle.c_str(), offsetX);
      tft.setTextColor(TFT_WHITE, bgCol);
      tft.setCursor(baseX + offsetX, curY);
      tft.print(debugPopupTitle);
      curY += lineH + 6;
    }
    if (debugPopupLine1.length() > 0) {
      int offsetX = getManualOffset(debugPopupLine1);
      Serial.printf("[POPUP] Line1: \"%s\" | X_OFFSET: %d\n", debugPopupLine1.c_str(), offsetX);
      uint16_t col = debugPopupHighlightKeycode ? TFT_YELLOW : debugPopupLine1Color;
      tft.setTextColor(col, bgCol);
      tft.setCursor(baseX + offsetX, curY);
      tft.print(debugPopupLine1);
      curY += lineH + 6;
    }
    if (debugPopupLine2.length() > 0) {
      int offsetX = getManualOffset(debugPopupLine2);
      Serial.printf("[POPUP] Line2: \"%s\" | X_OFFSET: %d\n", debugPopupLine2.c_str(), offsetX);
      tft.setTextColor(TFT_WHITE, bgCol);
      tft.setCursor(baseX + offsetX, curY);
      tft.print(debugPopupLine2);
    }
  }
}

// Draw debug popup safely from anywhere (handles mutex/startWrite)
void drawDebugPopup() {
  if (!debugPopupActive) return;
  if (spiMutex) xSemaphoreTake(spiMutex, portMAX_DELAY);
  tft.startWrite();
  drawDebugPopupInline();
  tft.endWrite();
  if (spiMutex) xSemaphoreGive(spiMutex);
}