#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include <esp_task_wdt.h>
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
  Serial.begin(115200);
  delay(1000);

  Serial.println("Starting setup...");

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
    tft.startWrite();
    while (gif.playFrame(true, NULL))
    {
      handleButtons();
      handleQMKCommands();
      if (inMenu) {
        gif.close();
        tft.endWrite();
        return; // Exit to show menu
      }
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