#include "minigame.h"
#include "config.h"
#include "display_ui.h"
#include "buzzer.h"

namespace {

constexpr int SNAKE_COLS = 22;
constexpr int SNAKE_ROWS = 20;
constexpr int SNAKE_CELL = 10;
constexpr int SNAKE_OFFSET_X = 10;
constexpr int SNAKE_OFFSET_Y = 38;
constexpr int SNAKE_MAX_LEN = 128;

struct GridPt {
    int8_t x;
    int8_t y;
};

GridPt snakeBody[SNAKE_MAX_LEN];
int snakeLen = 3;
int8_t snakeDx = 1;
int8_t snakeDy = 0;
int8_t nextSnakeDx = 1;
int8_t nextSnakeDy = 0;
GridPt snakeFood = {10, 10};
uint16_t snakeScore = 0;
uint16_t snakeHighScore = 0;
uint32_t lastSnakeStep = 0;
bool snakeGameOver = false;

void drawSnakeCell(int gx, int gy, uint16_t color, bool isHead = false, bool isFood = false) {
    auto& lcd = DisplayUI::getLcd();
    int px = SNAKE_OFFSET_X + gx * SNAKE_CELL;
    int py = SNAKE_OFFSET_Y + gy * SNAKE_CELL;
    if (isFood) {
        lcd.fillRect(px, py, SNAKE_CELL, SNAKE_CELL, 0x0000);
        lcd.fillRect(px + 2, py + 2, SNAKE_CELL - 4, SNAKE_CELL - 4, 0xF81F);  // Magenta Mii-Treat
        lcd.drawRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, 0xFD20);
    } else if (isHead) {
        lcd.fillRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, 0xFD20);  // Cat Amber Head
        lcd.drawPixel(px + 3, py + 3, 0x0000);
        lcd.drawPixel(px + 6, py + 3, 0x0000);
    } else {
        lcd.fillRect(px, py, SNAKE_CELL, SNAKE_CELL, 0x0000);
        if (color != 0x0000) {
            lcd.fillRect(px + 1, py + 1, SNAKE_CELL - 2, SNAKE_CELL - 2, color);
        }
    }
}

void drawSnakeHeader() {
    auto& lcd = DisplayUI::getLcd();
    char buf[36];
    snprintf(buf, sizeof(buf), "MII-SNAKE  SCORE:%02d  HI:%02d", snakeScore, snakeHighScore);
    lcd.setTextSize(1);
    lcd.setTextColor(0xFD20, 0x10A2);
    lcd.drawCenterString(buf, 120, 12);
}

void spawnSnakeFood() {
    for (int tries = 0; tries < 100; tries++) {
        int fx = random(0, SNAKE_COLS);
        int fy = random(0, SNAKE_ROWS);
        bool collision = false;
        for (int i = 0; i < snakeLen; i++) {
            if (snakeBody[i].x == fx && snakeBody[i].y == fy) {
                collision = true;
                break;
            }
        }
        if (!collision) {
            snakeFood.x = fx;
            snakeFood.y = fy;
            drawSnakeCell(fx, fy, 0, false, true);
            return;
        }
    }
}

}  // namespace

void MiniGame::init() {
    auto& lcd = DisplayUI::getLcd();
    lcd.fillScreen(0x0821);

    // Header bar
    lcd.fillRect(0, 0, TFT_WIDTH, 28, 0x10A2);
    lcd.drawFastHLine(0, 28, TFT_WIDTH, 0xFD20);

    // Playfield border (220x200 inside 240x280)
    lcd.fillRect(SNAKE_OFFSET_X - 2, SNAKE_OFFSET_Y - 2, SNAKE_COLS * SNAKE_CELL + 4, SNAKE_ROWS * SNAKE_CELL + 4,
                 0x0000);
    lcd.drawRect(SNAKE_OFFSET_X - 2, SNAKE_OFFSET_Y - 2, SNAKE_COLS * SNAKE_CELL + 4, SNAKE_ROWS * SNAKE_CELL + 4,
                 0xFD20);

    // Footer instructions
    lcd.setTextSize(1);
    lcd.setTextColor(0x07FF, 0x0821);
    lcd.drawCenterString("JOY: Steer | Click D0: Exit", 120, 252);

    snakeLen = 4;
    snakeDx = 1;
    snakeDy = 0;
    nextSnakeDx = 1;
    nextSnakeDy = 0;
    snakeScore = 0;
    snakeGameOver = false;

    for (int i = 0; i < snakeLen; i++) {
        snakeBody[i].x = 6 - i;
        snakeBody[i].y = 10;
        drawSnakeCell(snakeBody[i].x, snakeBody[i].y, 0x07E0, (i == 0));
    }

    drawSnakeHeader();
    spawnSnakeFood();
    Buzzer::playMiiGreeting();
    lastSnakeStep = millis();
}

bool MiniGame::update(uint32_t now, AnalogJoystick& joystick, const Button& btnShutter) {
    JoyDirection rawJoy = joystick.getRawDirection();
    if (rawJoy == JOY_UP && snakeDy == 0) {
        nextSnakeDx = 0;
        nextSnakeDy = -1;
    } else if (rawJoy == JOY_DOWN && snakeDy == 0) {
        nextSnakeDx = 0;
        nextSnakeDy = 1;
    } else if (rawJoy == JOY_LEFT && snakeDx == 0) {
        nextSnakeDx = -1;
        nextSnakeDy = 0;
    } else if (rawJoy == JOY_RIGHT && snakeDx == 0) {
        nextSnakeDx = 1;
        nextSnakeDy = 0;
    }

    if (snakeGameOver) {
        if (btnShutter.shortClicked) {
            init();
            return false;
        }
        if (btnShutter.longClicked || rawJoy == JOY_LEFT) {
            Buzzer::playUiTick();
            return true;
        }
        return false;
    }

    if (btnShutter.shortClicked || btnShutter.longClicked) {
        Buzzer::playUiTick();
        return true;
    }

    uint32_t stepInterval = (snakeScore < 15) ? (165 - snakeScore * 5) : 90;
    if (now - lastSnakeStep < stepInterval) {
        return false;
    }
    lastSnakeStep = now;

    snakeDx = nextSnakeDx;
    snakeDy = nextSnakeDy;

    int newX = snakeBody[0].x + snakeDx;
    int newY = snakeBody[0].y + snakeDy;

    bool hit = (newX < 0 || newX >= SNAKE_COLS || newY < 0 || newY >= SNAKE_ROWS);
    if (!hit) {
        for (int i = 0; i < snakeLen - 1; i++) {
            if (snakeBody[i].x == newX && snakeBody[i].y == newY) {
                hit = true;
                break;
            }
        }
    }

    if (hit) {
        snakeGameOver = true;
        Buzzer::playCountdownTick();
        auto& lcd = DisplayUI::getLcd();
        lcd.fillRect(36, 105, 168, 70, 0x10A2);
        lcd.drawRect(36, 105, 168, 70, 0xF800);
        lcd.setTextSize(2);
        lcd.setTextColor(0xF800, 0x10A2);
        lcd.drawCenterString("GAME OVER", 120, 115);
        lcd.setTextSize(1);
        lcd.setTextColor(0xFFFF, 0x10A2);
        lcd.drawCenterString("D0: Retry | Hold D0: Exit", 120, 148);
        return false;
    }

    bool ateFood = (newX == snakeFood.x && newY == snakeFood.y);
    GridPt oldTail = snakeBody[snakeLen - 1];

    if (ateFood && snakeLen < SNAKE_MAX_LEN) {
        snakeLen++;
    } else {
        drawSnakeCell(oldTail.x, oldTail.y, 0x0000);
    }

    for (int i = snakeLen - 1; i > 0; i--) {
        snakeBody[i] = snakeBody[i - 1];
    }
    snakeBody[0].x = newX;
    snakeBody[0].y = newY;

    drawSnakeCell(snakeBody[1].x, snakeBody[1].y, 0x07E0, false);
    drawSnakeCell(snakeBody[0].x, snakeBody[0].y, 0xFD20, true);

    if (ateFood) {
        snakeScore++;
        if (snakeScore > snakeHighScore) {
            snakeHighScore = snakeScore;
        }
        Buzzer::playCatMii();
        drawSnakeHeader();
        spawnSnakeFood();
    }

    return false;
}
