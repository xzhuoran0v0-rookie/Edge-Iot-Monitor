#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "adaptive_baseline.h"
#include "edge_reasoner.h"

// OLED 使用第二条硬件 I2C 总线 (Wire1)，与 SHT30 (默认 Wire, GPIO17/18) 隔离
#define OLED_SDA 38   // Wire1 SDA
#define OLED_SCL 39   // Wire1 SCL
#define OLED_ADDR 0x3c

#define OLED_WIDTH 128
#define OLED_HEIGHT 64

/**
 * @brief SSD1315 OLED 显示驱动
 *
 * 功能：
 * - 初始化OLED
 * - 显示温湿度数据
 * - 显示AI分析结果
 *
 */
class OLED
{
public:
    /**
     * @brief 初始化OLED
     * @return ture=成功
     */
    static bool init();

    /**
     * @brief 清屏
     */
    static void clear();

    /**
     * @brief Store the latest values and redraw the active carousel page.
     */
    static void updateDashboard(float temp, float humi,
                                const EdgeAssessment &assessment,
                                const AdaptiveBaselineResult &baseline,
                                bool cloudConnected);

    /**
     * @brief Handle automatic page timing and the BOOT-button page advance.
     *
     * Call frequently from loop(); it never delays.
     */
    static void tick();

    /**
     * @brief 显示 AI 分析结果（截断显示）
     * @param result AI 输出文本
     */
    static void showAIResult(const String &result);

    /**
     * @brief Display a paginated operator message above the normal carousel.
     *
     * The message must contain 1-240 characters supported by the built-in
     * 5x7 font (ASCII space through 'Z' after uppercasing). The normal
     * dashboard keeps receiving fresh data in the background and resumes
     * when the message expires. A critical alert always has higher priority.
     */
    static bool showCustomMessage(const String &message,
                                  unsigned long durationMs);

    /**
     * @brief Display a sticky critical alert until clearAlert() is called.
     */
    static void showAlert(const String &msg);
    static void clearAlert();

    /**
     * @brief 显示状态信息（WiFi、连接等）
     * @param msg 状态文本
     */
    static void showStatus(const String &msg);

private:
    static void sendCmd(uint8_t cmd);
    static void sendData(uint8_t *buf, size_t len);
    static void setCursor(uint8_t page, uint8_t col);

    // 字模数据（ASCII 5x7）
    static const uint8_t font5x7[][5];
    static void drawChar(char c);
    static void drawString(const String &s);
    static void drawWrapped(const String &text,
                            uint8_t firstPage,
                            uint8_t maxPages);
    static void drawWrappedSlice(const String &text,
                                 uint16_t firstLine,
                                 uint8_t firstPage,
                                 uint8_t maxLines,
                                 uint8_t pageStride);
    static uint16_t wrappedLineCount(const String &text);
    static void renderDashboardPage();
    static void renderCustomPage();
    static void renderAlert();
    static void renderStatus();
    static void renderActiveView();
    static void advanceDashboardPage(bool manual);
    static void advanceCustomPage(bool manual);
    static unsigned long activePageDurationMs();
    static uint8_t dashboardPageCount();
    static uint8_t customPageCount();
    static bool isDisplayableText(const String &text);

    static uint8_t cursor_page_;
    static uint8_t cursor_col_;
    static bool dashboard_active_;
    static uint8_t dashboard_page_;
    static unsigned long page_started_ms_;
    static float latest_temp_;
    static float latest_humi_;
    static float latest_confidence_;
    static bool latest_cloud_connected_;
    static String latest_state_;
    static String latest_severity_;
    static String latest_reason_;
    static AdaptiveBaselineStatus latest_baseline_status_;
    static bool latest_baseline_ready_;
    static uint8_t latest_baseline_progress_;
    static uint32_t latest_baseline_samples_;
    static float latest_baseline_temp_lower_;
    static float latest_baseline_temp_upper_;
    static float latest_baseline_humi_lower_;
    static float latest_baseline_humi_upper_;
    static bool custom_active_;
    static String custom_message_;
    static uint8_t custom_page_;
    static unsigned long custom_page_started_ms_;
    static unsigned long custom_expires_ms_;
    static bool alert_active_;
    static String alert_message_;
    static unsigned long alert_started_ms_;
    static bool status_active_;
    static String status_message_;
    static bool button_last_raw_;
    static bool button_stable_pressed_;
    static unsigned long button_changed_ms_;
};
