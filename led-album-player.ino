#include "HomeSpan.h"
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>
#include "hal/gpio_ll.h"
#include <HTTPClient.h>
#include <PxMatrix.h>
#include <ezButton.h>
#include <SPIFFS.h>
#include <JPEGDecoder.h>
#include <WiFiClientSecure.h>

#define PN532_IRQ   -1
#define PN532_RESET -1
#define LIMIT_SWITCH_PIN 27

#define MATRIX_WIDTH 64
#define MATRIX_HEIGHT 64

#define P_LAT 25
#define P_OE 16
#define P_A 19
#define P_B 23
#define P_C 18
#define P_D 5
#define P_E 15
#define P_CLK 14
#define P_R1 13


#define DISPLAY_DRAW_TIME 10 
#define REFRESH_RATE 200 

PxMATRIX display(MATRIX_WIDTH, MATRIX_HEIGHT, P_LAT, P_OE, P_A, P_B, P_C, P_D, P_E);

hw_timer_t * timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR display_updater() {
  portENTER_CRITICAL_ISR(&timerMux);
  display.display(DISPLAY_DRAW_TIME);
  portEXIT_CRITICAL_ISR(&timerMux);
}

TaskHandle_t displayTaskHandle = NULL;

void displayUpdateTask(void * parameter) {
    while(1) {
        portENTER_CRITICAL(&timerMux);
        display.display(DISPLAY_DRAW_TIME);
        portEXIT_CRITICAL(&timerMux);
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void drawJPEGFromSpiffs(const char* filename) {
  if (JpegDec.decodeFsFile(filename)) {
    uint16_t mcu_w = JpegDec.MCUWidth;
    uint16_t mcu_h = JpegDec.MCUHeight;
    
    while (JpegDec.read()) {
      uint16_t *pImg;
      pImg = JpegDec.pImage;
      int mcu_x = JpegDec.MCUx * mcu_w;
      int mcu_y = JpegDec.MCUy * mcu_h;
      
      for (uint16_t y = mcu_y; y < mcu_y + mcu_h; y++) {
        for (uint16_t x = mcu_x; x < mcu_x + mcu_w; x++) {

          uint8_t red = (uint8_t)((((*pImg & 0xF800) >> 11) / 32.0) * 255);
          uint8_t green = (uint8_t)((((*pImg & 0x7E0) >> 6) / 32.0) * 255);
          uint8_t blue = (uint8_t)((((*pImg & 0x1F) / 32.0) * 255));
          
          if (x < MATRIX_WIDTH && y < MATRIX_HEIGHT) {
            display.drawPixelRGB888(x, y, red, green, blue);
          }
          *pImg++;
        }
      }
    }
  }
}


bool downloadAlbumArt(const char* artUrl) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); 
  
  Serial.print("Downloading album art from: ");
  Serial.println(artUrl);
  
  http.begin(client, artUrl);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {

    File f = SPIFFS.open("/cover.jpg", "w");
    if (!f) {
      Serial.println("Failed to open file for writing");
      http.end();
      return false;
    }
    

    WiFiClient *stream = http.getStreamPtr();
    
    uint8_t buffer[1024];
    int totalBytes = http.getSize();
    int remainingBytes = totalBytes;
    
    while (http.connected() && (remainingBytes > 0 || remainingBytes == -1)) {
      size_t size = stream->available();
      if (size) {
        int c = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
        f.write(buffer, c);
        if (remainingBytes > 0) {
          remainingBytes -= c;
        }
      }
      delay(1);
    }
    
    f.close();
    http.end();
    Serial.println("Album art downloaded successfully");
    return true;
  }
  
  Serial.print("HTTP GET failed, error: ");
  Serial.println(httpCode);
  http.end();
  return false;
}

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
ezButton limitSwitch(LIMIT_SWITCH_PIN);

struct DEV_AlbumSwitch : Service::Switch {
  SpanCharacteristic *power;
  int albumId;
  const char* albumName;
  
  DEV_AlbumSwitch(int id, const char* name) : Service::Switch() {
    albumId = id;
    albumName = name;
    power = new Characteristic::On();
    new Characteristic::Name(name);
  }
  
  boolean update() {
    Serial.print("Album switch '");
    Serial.print(albumName);
    Serial.print("' (ID: ");
    Serial.print(albumId);
    Serial.print(") turned ");
    Serial.println(power->getNewVal() ? "ON" : "OFF");
    return(true);
  }
};

DEV_AlbumSwitch *albumSwitches[5]; 
int currentAlbum = -1;

void setup() {
  Serial.begin(115200);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed!");
    return;
  }

  homeSpan.begin(Category::Bridges, "Cassette Player");
  
  limitSwitch.setDebounceTime(50);
  
  Wire.begin(21, 22);
  nfc.begin();
  
  if (!nfc.getFirmwareVersion()) {
    Serial.println("Didn't find PN532 board");
    while (1);
  }
  
  nfc.SAMConfig();

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Cassette Player");
      new Characteristic::Manufacturer("Krystian");
      new Characteristic::SerialNumber("123-ABC");
      new Characteristic::Model("Cassette Player 1.0");
      new Characteristic::FirmwareRevision("1.0");
      

  const char* albumNames[] = {
    "Dark Side of the Moon",
    "Abbey Road",
    "GUTS",
    "The Rise and Fall of a Midwest Princess",
    "A Guide to Love, Loss & Desperation"
  };
  

  for(int i = 0; i < 5; i++) {
    new SpanAccessory();
      new Service::AccessoryInformation();
        new Characteristic::Identify();
        new Characteristic::Name(albumNames[i]);
        new Characteristic::Manufacturer("Krystian");
        new Characteristic::SerialNumber(("Album-" + String(i+1)).c_str());
        new Characteristic::Model("Album Switch");
        new Characteristic::FirmwareRevision("1.0");
        
      albumSwitches[i] = new DEV_AlbumSwitch(i+1, albumNames[i]);
  }

  display.begin(32);
  display.setScanPattern(LINE);
  display.clearDisplay();


  display.setBrightness(128); 
  

  display.fillRect(0, 0, MATRIX_WIDTH, MATRIX_HEIGHT, display.color565(255, 0, 0));


  xTaskCreatePinnedToCore(
    displayUpdateTask, 
    "displayUpdate",   
    4096,              
    NULL,              
    2,                 
    &displayTaskHandle,
    1                  
  );

}

void fetchAndDisplayAlbumArt(const char* artUrl) {
  display.clearDisplay();
  
  if (downloadAlbumArt(artUrl)) {
    drawJPEGFromSpiffs("/cover.jpg");

    SPIFFS.remove("/cover.jpg");
  } else {

    display.fillRect(0, 0, MATRIX_WIDTH, MATRIX_HEIGHT, display.color565(255, 0, 0));
  }
}


bool readNFCTag(int &albumId, String &artUrl) {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;
  
  Serial.println("Scanning for NFC tag...");
  
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    Serial.print("Found NFC tag with UID: ");
    for (uint8_t i = 0; i < uidLength; i++) {
      Serial.print(" 0x"); Serial.print(uid[i], HEX);
    }
    Serial.println("");
    
    String jsonString = "";
    uint8_t data[32];
    

    for (uint8_t page = 4; page < 50; page++) {
      if (nfc.ntag2xx_ReadPage(page, data)) {

        for (uint8_t i = 0; i < 4; i++) {
          char c = data[i];
          if (c >= 32 && c <= 126) {
            if (c == '{') {
              jsonString = "";  
            }
            jsonString += c;
          }
        }
      } else {
        Serial.print("Failed to read page ");
        Serial.println(page);
        break;
      }
    }
    
    Serial.println("Collected JSON string:");
    Serial.println(jsonString);
    

    if (jsonString.indexOf('{') != -1 && jsonString.indexOf('}') != -1) {

      int startPos = jsonString.indexOf('{');
      int endPos = jsonString.indexOf('}');
      String cleanJson = jsonString.substring(startPos, endPos + 1);
      
      Serial.println("Attempting to parse:");
      Serial.println(cleanJson);
      

      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, cleanJson);
      
      if (!error) {
        albumId = doc["id"];
        artUrl = doc["artUrl"].as<String>();
        Serial.print("Successfully read album ID: ");
        Serial.print(albumId);
        Serial.print(" with art URL: ");
        Serial.println(artUrl);
        return true;
      } else {
        Serial.print("JSON parsing failed: ");
        Serial.println(error.c_str());
      }
    } else {
      Serial.println("No complete JSON object found");
    }
  } else {
    Serial.println("No NFC tag detected");
  }
  
  return false;
}

void setAlbum(int albumId) {

  if(currentAlbum >= 0 && currentAlbum < 5) {
    Serial.print("Turning off previous album: ");
    Serial.println(albumSwitches[currentAlbum]->albumName);
    albumSwitches[currentAlbum]->power->setVal(false);
  }
  

  if(albumId > 0 && albumId <= 5) {
    currentAlbum = albumId-1;
    Serial.print("Turning on album: ");
    Serial.println(albumSwitches[currentAlbum]->albumName);
    albumSwitches[currentAlbum]->power->setVal(true);
  }
}

void stopPlayback() {
  if(currentAlbum >= 0 && currentAlbum < 5) {
    Serial.print("Stopping playback of album: ");
    Serial.println(albumSwitches[currentAlbum]->albumName);
    albumSwitches[currentAlbum]->power->setVal(false);
    currentAlbum = -1;
  }
}

void loop() {
  homeSpan.poll();
  limitSwitch.loop();
  
  if(limitSwitch.isPressed()) {
    Serial.println("\n--- Cassette Inserted ---");
    int albumId;
    String artUrl;
    
    if (readNFCTag(albumId, artUrl)) {
      setAlbum(albumId);
      fetchAndDisplayAlbumArt(artUrl.c_str());
    } else {
      Serial.println("Failed to read album from NFC tag");
    }
  }
  
  if(limitSwitch.isReleased()) {
    Serial.println("\n--- Cassette Removed ---");
    stopPlayback();
  }
  
  delay(50);
}