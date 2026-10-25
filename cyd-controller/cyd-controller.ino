#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <JPEGDEC.h>

// Configurazione Wi-Fi dello Scout32
const char* ssid = "Scout32";
const char* password = "";

// Endpoint per cattura e comandi
const String captureUrl = "http://192.168.4.1/capture";
const String controlUrl = "http://192.168.4.1/control?var=car&val=";
const String lightUrl   = "http://192.168.4.1/control?var=flash&val=";

// Configurazione Hardware CYD
#define CYD_BACKLIGHT_PIN 21
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

TFT_eSPI tft = TFT_eSPI();
SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
JPEGDEC jpeg;

// --- AREE SCHERMO (Portrait 240x320) ---
const int videoX = 0; 
const int videoY = 0;   
const int videoMaxWidth = 240;
const int videoMaxHeight = 320; 

// Scala hardware 1 (1/2) -> 160x120
const int scaledW = 160;
const int scaledH = 100;

// Centratura automatica di base
const int baseOffsetX = (videoMaxWidth - scaledW) / 2;   // 40
const int baseOffsetY = (videoMaxHeight - scaledH) / 2; // 46

// -----------------------------------------------------------------
// REGOLAZIONE FINE DELLA CENTRATURA (Modifica questi valori se serve)
// -----------------------------------------------------------------
const int customOffsetX = -40; // Positivo sposta a destra, negativo a sinistra
const int customOffsetY = -130; // Positivo sposta in basso, negativo in alto
// -----------------------------------------------------------------

const int offsetX = baseOffsetX + customOffsetX;
const int offsetY = baseOffsetY + customOffsetY;

// Struttura per i pulsanti touch
struct Button {
  int x, y, w, h;
  const char* label;
  int dir; // 0=UP, 1=DOWN, 2=LEFT, 3=RIGHT, 4=LIGHT
  int cmdVal;
};

Button btnAvanti   = {95,  220, 50, 28, "AVANTI", 0, 3};
Button btnSinistra = {35,  253, 50, 28, "SINISTRA", 2, 2};
Button btnLuce     = {95,  253, 50, 28, "LUCE", 4, 0};
Button btnDestra   = {155, 253, 50, 28, "DESTRA", 3, 1};
Button btnIndietro = {95,  286, 50, 28, "INDIETRO", 1, 4};

int lastCommand = -1;
unsigned long lastTouchTime = 0;
bool lightState = false; 
bool buttonStates[5] = {false, false, false, false, false};

// Funzione inline per correggere il contrasto sui pixel RGB565
inline uint16_t adjustContrast(uint16_t color) {
    uint8_t r = (color >> 11) & 0x1F;
    uint8_t g = (color >> 5) & 0x3F;
    uint8_t b = color & 0x1F;

    r = (r > 4) ? (r * 26 / 31) : 0;
    g = (g > 8) ? (g * 52 / 63) : 0;
    b = (b > 4) ? (b * 26 / 31) : 0;

    return (r << 11) | (g << 5) | b;
}

// Callback veloce con elaborazione in-place e pushImage a blocchi
int JPEGDraw(JPEGDRAW* pDraw) {
    int destY = offsetY + pDraw->y;
    if (destY >= videoMaxHeight) return 0;

    int totalPixels = pDraw->iWidth * pDraw->iHeight;
    for (int i = 0; i < totalPixels; i++) {
        pDraw->pPixels[i] = adjustContrast(pDraw->pPixels[i]);
    }

    tft.setSwapBytes(true); 
    tft.pushImage(videoX + offsetX + pDraw->x, videoY + destY, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
    return 1;
}

void drawButton(Button b, bool pressed) {
  uint16_t bgColor = pressed ? TFT_BLUE : 0x1084; 
  uint16_t borderColor = pressed ? TFT_WHITE : TFT_CYAN;

  tft.fillRect(b.x, b.y, b.w, b.h, bgColor);
  tft.drawRect(b.x, b.y, b.w, b.h, borderColor);

  int cx = b.x + b.w / 2;
  int cy = b.y + b.h / 2;
  uint16_t color = TFT_WHITE;

  if (b.dir == 0) {
    tft.fillTriangle(cx, cy - 6, cx - 8, cy + 4, cx + 8, cy + 4, color);
  } else if (b.dir == 1) {
    tft.fillTriangle(cx, cy + 6, cx - 8, cy - 4, cx + 8, cy - 4, color);
  } else if (b.dir == 2) {
    tft.fillTriangle(cx - 6, cy, cx + 4, cy - 8, cx + 4, cy + 8, color);
  } else if (b.dir == 3) {
    tft.fillTriangle(cx + 6, cy, cx - 4, cy - 8, cx - 4, cy + 8, color);
  } else if (b.dir == 4) {
    tft.fillRect(cx - 6, cy + 1, 12, 3, color); 
    tft.fillRect(cx - 6, cy + 4, 3, 3, color);  
  }
}

void drawControlPanel() {
  tft.fillRect(0, videoMaxHeight, 240, 320 - videoMaxHeight, TFT_BLACK);
  
  drawButton(btnAvanti, false);
  drawButton(btnSinistra, false);
  drawButton(btnLuce, false);
  drawButton(btnDestra, false);
  drawButton(btnIndietro, false);
}

void sendCommand(String url) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(url);
    http.GET();
    http.end();
  }
}

bool isPressed(Button b, int touchX, int touchY) {
  return (touchX >= b.x && touchX <= (b.x + b.w) && touchY >= b.y && touchY <= (b.y + b.h));
}

void setup() {
    Serial.begin(115200);

    pinMode(CYD_BACKLIGHT_PIN, OUTPUT);
    digitalWrite(CYD_BACKLIGHT_PIN, HIGH);

    tft.init();
    tft.setRotation(0); 
    tft.fillScreen(TFT_BLACK);
    tft.invertDisplay(true);

    mySpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(mySpi);
    ts.setRotation(0); 

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

    drawControlPanel();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient httpVideo;
        httpVideo.begin(captureUrl);
        int httpResponseCode = httpVideo.GET();

        if (httpResponseCode > 0) {
            int totalLen = httpVideo.getSize();
            WiFiClient *stream = httpVideo.getStreamPtr();

            if (totalLen > 0 && totalLen < 50000) {
                uint8_t *jpegBuffer = (uint8_t*)malloc(totalLen);
                if (jpegBuffer) {
                    int bytesRead = stream->readBytes(jpegBuffer, totalLen);
                    if (bytesRead > 0) {
                        if (jpeg.openRAM(jpegBuffer, bytesRead, JPEGDraw)) {
                            jpeg.decode(0, 0, 1); 
                            jpeg.close();
                        }
                    }
                    free(jpegBuffer);
                }
            }
        }
        httpVideo.end();
    }

    bool touched = false;
    if (ts.tirqTouched() && ts.touched()) {
        TS_Point p = ts.getPoint();
        
        int touchX = map(p.x, 200, 3700, 0, 240);
        int touchY = map(p.y, 240, 3800, 0, 320);

        bool aState = isPressed(btnAvanti, touchX, touchY);
        bool sState = isPressed(btnSinistra, touchX, touchY);
        bool lState = isPressed(btnLuce, touchX, touchY);
        bool dState = isPressed(btnDestra, touchX, touchY);
        bool iState = isPressed(btnIndietro, touchX, touchY);

        if (aState) { sendCommand(controlUrl + String(btnAvanti.cmdVal)); lastCommand = btnAvanti.cmdVal; touched = true; }
        else if (iState) { sendCommand(controlUrl + String(btnIndietro.cmdVal)); lastCommand = btnIndietro.cmdVal; touched = true; }
        else if (sState) { sendCommand(controlUrl + String(btnSinistra.cmdVal)); lastCommand = btnSinistra.cmdVal; touched = true; }
        else if (dState) { sendCommand(controlUrl + String(btnDestra.cmdVal)); lastCommand = btnDestra.cmdVal; touched = true; }
        else if (lState) {
            lightState = !lightState; 
            sendCommand(lightUrl + (lightState ? "127" : "0")); 
            drawButton(btnLuce, true);
            delay(100); 
            touched = true;
        }

        if (aState != buttonStates[0]) { drawButton(btnAvanti, aState); buttonStates[0] = aState; }
        if (sState != buttonStates[1]) { drawButton(btnSinistra, sState); buttonStates[1] = sState; }
        if (lState != buttonStates[2]) { drawButton(btnLuce, lState); buttonStates[2] = lState; }
        if (dState != buttonStates[3]) { drawButton(btnDestra, dState); buttonStates[3] = dState; }
        if (iState != buttonStates[4]) { drawButton(btnIndietro, iState); buttonStates[4] = iState; }

        if (touched) {
            lastTouchTime = millis();
        }
    } else {
        for (int i = 0; i < 5; i++) {
            if (buttonStates[i]) { 
                buttonStates[i] = false;
                if (i == 0) drawButton(btnAvanti, false);
                if (i == 1) drawButton(btnSinistra, false);
                if (i == 2) drawButton(btnLuce, false);
                if (i == 3) drawButton(btnDestra, false);
                if (i == 4) drawButton(btnIndietro, false);
            }
        }
    }

    if (!touched) {
        if (lastCommand != 5 && (millis() - lastTouchTime > 150)) {
            sendCommand(controlUrl + "5");
            lastCommand = 5;
        }
    }
}