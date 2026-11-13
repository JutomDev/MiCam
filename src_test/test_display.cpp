#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ============================================================================
// KONFIGURACJA PINÓW: Seeed Studio XIAO ESP32-S3 + Ekran ST7789
// ============================================================================
// DIN  -> D2 (GPIO 3)
// CLK  -> D1 (GPIO 2)
// DC   -> D3 (GPIO 4)
// RST  -> D6 (GPIO 43)  [lub D7 / GPIO 44 jeśli podłączone pod D7]
// CS   -> GND (na stałe zmasowane)
// BLK  -> 3.3V (podświetlenie)

#define TFT_SCLK_PIN 2  // D1 (GPIO 2)
#define TFT_MOSI_PIN 3  // D2 (GPIO 3, DIN)
#define TFT_DC_PIN 4    // D3 (GPIO 4)
#define TFT_RST_PIN 43  // D6 (GPIO 43) -> Zmień na 44 jeśli wpięte w D7!
#define TFT_CS_PIN -1   // CS spięty z GND

class LGFX_ST7789_Test : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus_instance;
    lgfx::Panel_ST7789 _panel_instance;

public:
    LGFX_ST7789_Test() {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI3_HOST;
            cfg.spi_mode = 3;           // Mode 3 (CPOL=1, CPHA=1) zalecany gdy CS jest na GND
            cfg.freq_write = 20000000;  // 20 MHz - bezpieczna prędkość dla przewodów
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = TFT_SCLK_PIN;
            cfg.pin_mosi = TFT_MOSI_PIN;
            cfg.pin_miso = -1;
            cfg.pin_dc = TFT_DC_PIN;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = TFT_CS_PIN;
            cfg.pin_rst = TFT_RST_PIN;
            cfg.pin_busy = -1;
            cfg.memory_width = 240;
            cfg.memory_height = 320;
            cfg.panel_width = 240;
            cfg.panel_height = 280;  // Standard 1.69" (240x280) lub 240 dla 1.3"
            cfg.offset_x = 0;
            cfg.offset_y = 20;  // Offset 20 dla matrycy 240x280 w kontrolerze ST7789 240x320
            cfg.offset_rotation = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = false;
            cfg.invert = true;  // Panele ST7789 IPS wymagają invert = true
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel_instance.config(cfg);
        }
    }
};

LGFX_ST7789_Test display;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n==========================================");
    Serial.println("  TEST EKRANU ST7789 (Seeed XIAO ESP32-S3)");
    Serial.println("==========================================");
    Serial.printf("PINY: CLK=D1(%d), DIN=D2(%d), DC=D3(%d), RST=D6(%d), CS=GND\n", TFT_SCLK_PIN, TFT_MOSI_PIN,
                  TFT_DC_PIN, TFT_RST_PIN);

    // Krok 1: Wymuszenie sprzętowego resetu przed inicjalizacją magistrali SPI
    if (TFT_RST_PIN >= 0) {
        Serial.println("-> Wykonywanie twardego resetu linii RST...");
        pinMode(TFT_RST_PIN, OUTPUT);
        digitalWrite(TFT_RST_PIN, HIGH);
        delay(50);
        digitalWrite(TFT_RST_PIN, LOW);
        delay(100);
        digitalWrite(TFT_RST_PIN, HIGH);
        delay(100);
    }

    Serial.println("-> Inicjalizacja LovyanGFX...");
    if (display.init()) {
        Serial.println("-> LovyanGFX zainicjalizowany pomyslnie!");
    } else {
        Serial.println("-> BLAD inicjalizacji LovyanGFX!");
    }

    display.setRotation(0);
    display.fillScreen(TFT_BLACK);

    // Ekran testowy: Informacje i kolory podstawowe
    display.fillRect(0, 0, 240, 40, TFT_RED);
    display.fillRect(0, 40, 240, 40, TFT_GREEN);
    display.fillRect(0, 80, 240, 40, TFT_BLUE);
    display.fillRect(0, 120, 240, 40, TFT_WHITE);
    delay(1500);

    display.fillScreen(TFT_BLACK);
    // Ramka krawędziowa sprawdzająca offsety
    display.drawRect(0, 0, display.width(), display.height(), TFT_YELLOW);
    display.drawRect(1, 1, display.width() - 2, display.height() - 2, TFT_MAGENTA);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);
    display.setCursor(15, 20);
    display.println("EKRAN DZIALA!");

    display.setTextSize(1);
    display.setCursor(15, 60);
    display.println("Seeed XIAO ESP32-S3");
    display.setCursor(15, 80);
    display.printf("DIN (MOSI): D2 (GPIO %d)\n", TFT_MOSI_PIN);
    display.setCursor(15, 95);
    display.printf("CLK (SCLK): D1 (GPIO %d)\n", TFT_SCLK_PIN);
    display.setCursor(15, 110);
    display.printf("DC:         D3 (GPIO %d)\n", TFT_DC_PIN);
    display.setCursor(15, 125);
    display.printf("RST:        D6 (GPIO %d)\n", TFT_RST_PIN);
    display.setCursor(15, 140);
    display.println("CS:         GND");
}

int ballX = 120;
int ballY = 200;
int ballDx = 3;
int ballDy = 2;
const int ballRadius = 8;
uint32_t frameCount = 0;
uint32_t lastFpsTime = 0;
int currentFps = 0;

void loop() {
    // Czyszczenie starej pozycji piłki
    display.fillCircle(ballX, ballY, ballRadius, TFT_BLACK);

    // Fizyka odbijania w ramce
    ballX += ballDx;
    ballY += ballDy;

    if (ballX - ballRadius <= 2 || ballX + ballRadius >= display.width() - 3) {
        ballDx = -ballDx;
    }
    int maxY = min(235, (int)display.height() - 3);
    if (ballY - ballRadius <= 155 || ballY + ballRadius >= maxY) {
        ballDy = -ballDy;
    }

    // Rysowanie nowej pozycji piłki
    display.fillCircle(ballX, ballY, ballRadius, TFT_CYAN);

    // Licznik FPS
    frameCount++;
    if (millis() - lastFpsTime >= 1000) {
        currentFps = frameCount;
        frameCount = 0;
        lastFpsTime = millis();

        display.setTextSize(1);
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.setCursor(15, 225);
        display.printf("Status: OK | FPS: %d   ", currentFps);
    }

    delay(15);
}
