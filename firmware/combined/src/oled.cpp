#include "oled.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// OLED 走 ESP32-S3 的第二条硬件 I2C 总线 (Wire1)，与 SHT30 的默认 Wire (GPIO17/18) 隔离。
// 引脚为 OLED_SDA(38) / OLED_SCL(39)，在 OLED::init() 中 begin。
static TwoWire OLED_I2C = TwoWire(1);

namespace
{
constexpr uint8_t OLED_PAGE_BUTTON_PIN = 0; // On-board BOOT button, active LOW
constexpr uint8_t REASON_LINES_PER_PAGE = 3;
constexpr uint8_t DASHBOARD_STATIC_PAGE_COUNT = 3;
constexpr uint8_t REASON_FIRST_PAGE = DASHBOARD_STATIC_PAGE_COUNT;
constexpr unsigned long ENVIRONMENT_PAGE_MS = 6000;
constexpr unsigned long ASSESSMENT_PAGE_MS = 6000;
constexpr unsigned long BASELINE_PAGE_MS = 6000;
constexpr unsigned long REASON_PAGE_MS = 8000;
constexpr unsigned long CUSTOM_PAGE_MS = 8000;
constexpr unsigned long CUSTOM_MIN_DURATION_MS = 5000;
constexpr unsigned long CUSTOM_MAX_DURATION_MS = 120000;
constexpr size_t CUSTOM_MESSAGE_MAX_BYTES = 240;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 40;
constexpr size_t OLED_DATA_CHUNK_SIZE = 127; // Wire frame also carries a control byte
SemaphoreHandle_t oledMutex = nullptr;

class OledGuard
{
public:
    OledGuard()
        : locked_(oledMutex != nullptr &&
                  xSemaphoreTakeRecursive(
                      oledMutex, portMAX_DELAY) == pdTRUE)
    {
    }

    ~OledGuard()
    {
        if (locked_)
            xSemaphoreGiveRecursive(oledMutex);
    }

private:
    bool locked_;
};

bool deadlineReached(unsigned long now, unsigned long deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}
}

uint8_t OLED::cursor_page_ = 0;
uint8_t OLED::cursor_col_ = 0;
bool OLED::dashboard_active_ = false;
uint8_t OLED::dashboard_page_ = 0;
unsigned long OLED::page_started_ms_ = 0;
float OLED::latest_temp_ = 0.0f;
float OLED::latest_humi_ = 0.0f;
float OLED::latest_confidence_ = 0.0f;
unsigned long OLED::latest_sense_interval_ms_ = 0;
bool OLED::latest_cloud_connected_ = false;
String OLED::latest_state_;
String OLED::latest_severity_;
String OLED::latest_reason_;
AdaptiveBaselineStatus OLED::latest_baseline_status_ =
    AdaptiveBaselineStatus::RESET;
bool OLED::latest_baseline_ready_ = false;
uint8_t OLED::latest_baseline_progress_ = 0;
uint32_t OLED::latest_baseline_samples_ = 0;
float OLED::latest_baseline_temp_lower_ = 0.0f;
float OLED::latest_baseline_temp_upper_ = 0.0f;
float OLED::latest_baseline_humi_lower_ = 0.0f;
float OLED::latest_baseline_humi_upper_ = 0.0f;
bool OLED::custom_active_ = false;
String OLED::custom_message_;
uint8_t OLED::custom_page_ = 0;
unsigned long OLED::custom_page_started_ms_ = 0;
unsigned long OLED::custom_expires_ms_ = 0;
bool OLED::alert_active_ = false;
String OLED::alert_message_;
unsigned long OLED::alert_started_ms_ = 0;
bool OLED::status_active_ = false;
String OLED::status_message_;
bool OLED::button_last_raw_ = false;
bool OLED::button_stable_pressed_ = false;
unsigned long OLED::button_changed_ms_ = 0;

// ASCII 5x7 字模（空格到Z，够用）
const uint8_t OLED::font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x00,0x08,0x14,0x22,0x41}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x41,0x22,0x14,0x08,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x06,0x09,0x06,0x00}, // '[' -> 度数符号 (°)，小圆圈显示在字符顶部
};

void OLED::sendCmd(uint8_t cmd)
{
    OLED_I2C.beginTransmission(OLED_ADDR);
    OLED_I2C.write(0x00);  // Co=0, D/C#=0 命令模式
    OLED_I2C.write(cmd);
    OLED_I2C.endTransmission();
}

void OLED::sendData(uint8_t *buf, size_t len)
{
    size_t offset = 0;
    while (offset < len)
    {
        const size_t chunkLength =
            min(len - offset, OLED_DATA_CHUNK_SIZE);
        OLED_I2C.beginTransmission(OLED_ADDR);
        OLED_I2C.write(0x40);  // Co=0, D/C#=1 数据模式
        OLED_I2C.write(buf + offset, chunkLength);
        OLED_I2C.endTransmission();
        offset += chunkLength;
    }
}

bool OLED::init()
{
    if (oledMutex == nullptr)
        oledMutex = xSemaphoreCreateRecursiveMutex();
    OledGuard guard;

    // 启动 OLED 专用 I2C 总线（Wire1: SDA=38, SCL=39）
    OLED_I2C.begin(OLED_SDA, OLED_SCL);

    // SSD1315 初始化序列
    const uint8_t cmds[] = {
        0xAE,        // 关显示
        0xD5, 0x80,  // 时钟分频
        0xA8, 0x3F,  // 多路复用比 64
        0xD3, 0x00,  // 显示偏移
        0x40,        // 起始行
        0x8D, 0x14,  // 开启电荷泵
        0x20, 0x02,  // 页寻址模式，与 setCursor() 的 B0 命令保持一致
        0xA1,        // 列地址翻转
        0xC8,        // COM 扫描方向
        0xDA, 0x12,  // COM 引脚配置
        0x81, 0xCF,  // 对比度
        0xD9, 0xF1,  // 预充电
        0xDB, 0x40,  // VCOMH
        0xA4,        // 全局显示开启
        0xA6,        // 正常显示
        0xAF,        // 开显示
    };

    for (uint8_t cmd : cmds)
        sendCmd(cmd);

    pinMode(OLED_PAGE_BUTTON_PIN, INPUT_PULLUP);
    button_last_raw_ = digitalRead(OLED_PAGE_BUTTON_PIN) == LOW;
    button_stable_pressed_ = button_last_raw_;
    button_changed_ms_ = millis();

    clear();
    // 不在这里打日志：init() 跑在 Serial.begin() 之前，为的是尽早清屏。
    // 调用方拿到返回值后再打印。
    return true;
}

void OLED::clear()
{
    OledGuard guard;

    for (uint8_t page = 0; page < 8; page++)
    {
        sendCmd(0xB0 + page);  // 设置页
        sendCmd(0x00);         // 列低位
        sendCmd(0x10);         // 列高位

        uint8_t blank[128] = {0};
        sendData(blank, 128);
    }
    cursor_page_ = 0;
    cursor_col_ = 0;
}

void OLED::setCursor(uint8_t page, uint8_t col)
{
    cursor_page_ = page;
    cursor_col_ = col;
    sendCmd(0xB0 + page);
    sendCmd(col & 0x0F);
    sendCmd(0x10 | (col >> 4));
}

void OLED::drawChar(char c)
{
    if (c < ' ' || c > '[')   // 字库范围：空格(0x20)~'['(0x5B)，'[' 复用为度数符号
        c = '?';

    const uint8_t *glyph = font5x7[c - ' '];
    uint8_t buf[6];
    for (int i = 0; i < 5; i++)
        buf[i] = glyph[i];
    buf[5] = 0x00;  // 字符间距

    sendData(buf, 6);
    cursor_col_ += 6;
}

void OLED::drawString(const String &s)
{
    for (char c : s)
    {
        if (cursor_col_ + 6 > OLED_WIDTH)
            break;
        drawChar(toupper(c));
    }
}

void OLED::drawWrapped(const String &text,
                       uint8_t firstPage,
                       uint8_t maxPages)
{
    constexpr int CHARS_PER_LINE = OLED_WIDTH / 6;
    String remaining = text;
    remaining.trim();

    for (uint8_t row = 0; row < maxPages && remaining.length() > 0; ++row)
    {
        int take = min(static_cast<int>(remaining.length()), CHARS_PER_LINE);
        if (take < static_cast<int>(remaining.length()))
        {
            const int wordBreak = remaining.lastIndexOf(' ', take);
            if (wordBreak > 0)
                take = wordBreak;
        }

        String line = remaining.substring(0, take);
        line.trim();
        setCursor(firstPage + row, 0);
        drawString(line);

        remaining = remaining.substring(take);
        remaining.trim();
    }
}

uint16_t OLED::wrappedLineCount(const String &text)
{
    constexpr int CHARS_PER_LINE = OLED_WIDTH / 6;
    String remaining = text;
    remaining.trim();
    uint16_t lineCount = 0;

    while (remaining.length() > 0)
    {
        int take = min(static_cast<int>(remaining.length()), CHARS_PER_LINE);
        if (take < static_cast<int>(remaining.length()))
        {
            const int wordBreak = remaining.lastIndexOf(' ', take);
            if (wordBreak > 0)
                take = wordBreak;
        }

        remaining = remaining.substring(take);
        remaining.trim();
        ++lineCount;
    }

    return lineCount > 0 ? lineCount : 1;
}

void OLED::drawWrappedSlice(const String &text,
                            uint16_t firstLine,
                            uint8_t firstPage,
                            uint8_t maxLines,
                            uint8_t pageStride)
{
    constexpr int CHARS_PER_LINE = OLED_WIDTH / 6;
    String remaining = text;
    remaining.trim();
    uint16_t lineIndex = 0;
    uint8_t drawn = 0;

    while (remaining.length() > 0 && drawn < maxLines)
    {
        int take = min(static_cast<int>(remaining.length()), CHARS_PER_LINE);
        if (take < static_cast<int>(remaining.length()))
        {
            const int wordBreak = remaining.lastIndexOf(' ', take);
            if (wordBreak > 0)
                take = wordBreak;
        }

        String line = remaining.substring(0, take);
        line.trim();
        remaining = remaining.substring(take);
        remaining.trim();

        if (lineIndex >= firstLine)
        {
            setCursor(firstPage + drawn * pageStride, 0);
            drawString(line);
            ++drawn;
        }
        ++lineIndex;
    }
}

void OLED::updateDashboard(float temp, float humi,
                           unsigned long senseIntervalMs,
                           const EdgeAssessment &assessment,
                           const AdaptiveBaselineResult &baseline,
                           bool cloudConnected)
{
    OledGuard guard;

    latest_temp_ = temp;
    latest_humi_ = humi;
    latest_confidence_ = assessment.confidence;
    latest_sense_interval_ms_ = senseIntervalMs;
    latest_cloud_connected_ = cloudConnected;
    latest_state_ = assessment.displayLabel;
    latest_severity_ = assessment.severity;
    latest_reason_ = assessment.reason;
    latest_baseline_status_ = baseline.status;
    latest_baseline_ready_ = baseline.ready;
    latest_baseline_progress_ = baseline.progressPct;
    latest_baseline_samples_ = baseline.learnedSamples;
    latest_baseline_temp_lower_ = baseline.temperatureLowerC;
    latest_baseline_temp_upper_ = baseline.temperatureUpperC;
    latest_baseline_humi_lower_ = baseline.humidityLowerPct;
    latest_baseline_humi_upper_ = baseline.humidityUpperPct;
    status_active_ = false;

    if (!dashboard_active_)
    {
        dashboard_active_ = true;
        dashboard_page_ = 0;
        page_started_ms_ = millis();
    }
    else if (dashboard_page_ >= dashboardPageCount())
    {
        dashboard_page_ = 0;
        page_started_ms_ = millis();
    }

    // Alerts and operator messages own the screen while active. Sensor data is
    // still cached here so the normal carousel resumes with fresh values.
    if (!alert_active_ && !custom_active_)
        renderActiveView();
}

uint8_t OLED::dashboardPageCount()
{
    const uint16_t reasonLines = wrappedLineCount(latest_reason_);
    const uint16_t reasonPages =
        (reasonLines + REASON_LINES_PER_PAGE - 1) / REASON_LINES_PER_PAGE;
    const uint16_t totalPages = DASHBOARD_STATIC_PAGE_COUNT + reasonPages;
    return totalPages > 255 ? 255 : static_cast<uint8_t>(totalPages);
}

unsigned long OLED::activePageDurationMs()
{
    if (dashboard_page_ == 0)
        return ENVIRONMENT_PAGE_MS;
    if (dashboard_page_ == 1)
        return ASSESSMENT_PAGE_MS;
    if (dashboard_page_ == 2)
        return BASELINE_PAGE_MS;
    return REASON_PAGE_MS;
}

void OLED::advanceDashboardPage(bool manual)
{
    if (!dashboard_active_)
        return;

    const uint8_t pageCount = dashboardPageCount();
    dashboard_page_ = (dashboard_page_ + 1) % pageCount;
    page_started_ms_ = millis();
    renderActiveView();

    Serial.print(manual ? "[OLED] Manual page " : "[OLED] Auto page ");
    Serial.print(dashboard_page_ + 1);
    Serial.print("/");
    Serial.println(pageCount);
}

uint8_t OLED::customPageCount()
{
    const uint16_t lines = wrappedLineCount(custom_message_);
    const uint16_t pages =
        (lines + REASON_LINES_PER_PAGE - 1) / REASON_LINES_PER_PAGE;
    return pages > 255 ? 255 : static_cast<uint8_t>(pages);
}

void OLED::advanceCustomPage(bool manual)
{
    if (!custom_active_)
        return;

    const uint8_t pageCount = customPageCount();
    custom_page_ = (custom_page_ + 1) % pageCount;
    custom_page_started_ms_ = millis();
    renderActiveView();

    Serial.print(manual ? "[OLED] Manual message page "
                        : "[OLED] Auto message page ");
    Serial.print(custom_page_ + 1);
    Serial.print("/");
    Serial.println(pageCount);
}

void OLED::tick()
{
    OledGuard guard;

    const unsigned long now = millis();

    const bool rawPressed = digitalRead(OLED_PAGE_BUTTON_PIN) == LOW;

    if (rawPressed != button_last_raw_)
    {
        button_last_raw_ = rawPressed;
        button_changed_ms_ = now;
    }

    if (now - button_changed_ms_ >= BUTTON_DEBOUNCE_MS &&
        rawPressed != button_stable_pressed_)
    {
        button_stable_pressed_ = rawPressed;
        if (button_stable_pressed_)
        {
            if (!alert_active_ && custom_active_)
                advanceCustomPage(true);
            else if (!alert_active_ && !status_active_ && dashboard_active_)
                advanceDashboardPage(true);
            return;
        }
    }

    if (alert_active_)
        return;

    if (custom_active_ && deadlineReached(now, custom_expires_ms_))
    {
        custom_active_ = false;
        custom_message_ = "";
        custom_page_ = 0;
        page_started_ms_ = now;
        Serial.println("[OLED] Custom message expired; carousel resumed");
        renderActiveView();
    }

    if (custom_active_)
    {
        if (customPageCount() > 1 &&
            now - custom_page_started_ms_ >= CUSTOM_PAGE_MS)
        {
            advanceCustomPage(false);
        }
        return;
    }

    if (status_active_)
        return;

    if (dashboard_active_ &&
        now - page_started_ms_ >= activePageDurationMs())
    {
        advanceDashboardPage(false);
    }
}

void OLED::renderDashboardPage()
{
    if (!dashboard_active_)
        return;

    const uint8_t pageCount = dashboardPageCount();
    clear();
    if (dashboard_page_ == 0)
    {
        setCursor(0, 0);
        drawString(String("ENV 1/") + String(pageCount) + "  " +
                   (latest_cloud_connected_ ? "CLOUD OK" : "CLOUD --"));
        setCursor(2, 0);
        drawString("TEMPERATURE");
        setCursor(3, 0);
        drawString(String(latest_temp_, 1) + "[C");
        setCursor(5, 0);
        drawString("HUMIDITY");
        setCursor(6, 0);
        drawString(String(latest_humi_, 1) + "%");
        return;
    }

    if (dashboard_page_ == 1)
    {
        setCursor(0, 0);
        drawString(String("ASSESSMENT 2/") + String(pageCount));
        setCursor(2, 0);
        drawString(latest_state_);
        setCursor(4, 0);
        drawString("LEVEL  " + latest_severity_);
        setCursor(6, 0);
        drawString("CONFIDENCE  " +
                   String(latest_confidence_ * 100.0f, 0) + "%");
        return;
    }

    if (dashboard_page_ == 2)
    {
        setCursor(0, 0);
        drawString(String("BASELINE 3/") + String(pageCount));

        if (!latest_baseline_ready_)
        {
            setCursor(2, 0);
            drawString("LEARNING");
            setCursor(4, 0);
            drawString("PROGRESS  " +
                       String(latest_baseline_progress_) + "%");
            setCursor(6, 0);
            drawString("SAMPLES  " +
                       String(latest_baseline_samples_));
            return;
        }

        setCursor(2, 0);
        if (latest_baseline_status_ ==
            AdaptiveBaselineStatus::OUTSIDE_ADAPTIVE_BAND)
        {
            drawString("PATTERN SHIFT");
        }
        else if (latest_baseline_status_ ==
                 AdaptiveBaselineStatus::HARD_LIMIT)
        {
            drawString("HARD LIMIT");
        }
        else
        {
            drawString("ADAPTED");
        }
        setCursor(4, 0);
        drawString("T " + String(latest_baseline_temp_lower_, 1) +
                   " TO " + String(latest_baseline_temp_upper_, 1) + "[C");
        setCursor(6, 0);
        drawString("H " + String(latest_baseline_humi_lower_, 1) +
                   " TO " + String(latest_baseline_humi_upper_, 1) + "%");
        return;
    }

    const uint8_t reasonPageIndex =
        dashboard_page_ - REASON_FIRST_PAGE;
    setCursor(0, 0);
    drawString(String("WHY ") + String(dashboard_page_ + 1) + "/" +
               String(pageCount));
    drawWrappedSlice(latest_reason_,
                     reasonPageIndex * REASON_LINES_PER_PAGE,
                     2,
                     REASON_LINES_PER_PAGE,
                     2);
    // 原因文字占第 2/4/6 行，第 7 行始终空着，正好留给当前上报周期。
    // 它是会变的，所以站在设备前面的人应当看得见它此刻是多少，
    // 否则「自适应」只存在于串口里。
    setCursor(7, 0);
    drawString("REPORT " +
               String(latest_sense_interval_ms_ / 1000.0f, 1) + " s");
}

void OLED::renderCustomPage()
{
    if (!custom_active_)
        return;

    const uint8_t pageCount = customPageCount();
    if (custom_page_ >= pageCount)
        custom_page_ = 0;

    clear();
    setCursor(0, 0);
    drawString(String("MESSAGE ") + String(custom_page_ + 1) + "/" +
               String(pageCount));
    drawWrappedSlice(custom_message_,
                     custom_page_ * REASON_LINES_PER_PAGE,
                     2,
                     REASON_LINES_PER_PAGE,
                     2);
}

void OLED::renderAlert()
{
    if (!alert_active_)
        return;

    clear();
    setCursor(0, 0);
    drawString("! DEVICE ALERT !");
    drawWrapped(alert_message_, 2, 5);
}

void OLED::renderStatus()
{
    if (!status_active_)
        return;

    clear();
    setCursor(0, 0);
    drawString("EDGE ENV MONITOR");
    drawWrapped(status_message_, 2, 5);
}

void OLED::renderActiveView()
{
    if (alert_active_)
    {
        renderAlert();
        return;
    }
    if (custom_active_)
    {
        renderCustomPage();
        return;
    }
    if (status_active_)
    {
        renderStatus();
        return;
    }
    if (dashboard_active_)
        renderDashboardPage();
}

bool OLED::isDisplayableText(const String &text)
{
    if (text.length() == 0 || text.length() > CUSTOM_MESSAGE_MAX_BYTES)
        return false;

    bool hasVisibleCharacter = false;
    for (size_t i = 0; i < text.length(); ++i)
    {
        const uint8_t byte = static_cast<uint8_t>(text[i]);
        const int normalized = toupper(byte);
        if (normalized < ' ' || normalized > 'Z')
            return false;
        if (byte != 0x20)
            hasVisibleCharacter = true;
    }
    return hasVisibleCharacter;
}

bool OLED::showCustomMessage(const String &message,
                             unsigned long durationMs)
{
    OledGuard guard;

    if (alert_active_)
    {
        Serial.println("[OLED] Display busy with critical alert");
        return false;
    }

    if (!isDisplayableText(message) ||
        durationMs < CUSTOM_MIN_DURATION_MS ||
        durationMs > CUSTOM_MAX_DURATION_MS)
    {
        Serial.println("[OLED] Rejected invalid custom message");
        return false;
    }

    custom_message_ = message;
    custom_active_ = true;
    custom_page_ = 0;
    custom_page_started_ms_ = millis();

    const unsigned long fullPassMs =
        static_cast<unsigned long>(customPageCount()) * CUSTOM_PAGE_MS;
    const unsigned long effectiveDurationMs =
        max(durationMs, fullPassMs);
    custom_expires_ms_ = custom_page_started_ms_ + effectiveDurationMs;

    Serial.print("[OLED] Custom message accepted pages=");
    Serial.print(customPageCount());
    Serial.print(" duration_ms=");
    Serial.println(effectiveDurationMs);
    renderActiveView();
    return true;
}

void OLED::showAIResult(const String &result)
{
    OledGuard guard;
    showCustomMessage(result, 30000);
}

void OLED::showAlert(const String &msg)
{
    OledGuard guard;

    if (!alert_active_)
        alert_started_ms_ = millis();

    alert_message_ = msg;
    alert_active_ = true;
    renderActiveView();
}

void OLED::clearAlert()
{
    OledGuard guard;

    if (!alert_active_)
        return;

    const unsigned long now = millis();
    if (custom_active_)
    {
        const unsigned long pausedMs = now - alert_started_ms_;
        custom_page_started_ms_ += pausedMs;
        custom_expires_ms_ += pausedMs;
    }

    alert_active_ = false;
    alert_message_ = "";
    alert_started_ms_ = 0;
    renderActiveView();
}

void OLED::showStatus(const String &msg)
{
    OledGuard guard;

    status_message_ = msg;
    status_active_ = true;
    renderActiveView();
}
