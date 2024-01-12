#ifndef _UI_H
#define _UI_H

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp32/rom/crc.h>
#include "main.h"
#include "pins.h"
#include "uart.h"
#include "mon.h"
#include "encoder.h"
#include "strips.h" 
#include "gamma.h" 

// color theme ----------------------------------------
#define COLOR_HIGHLIGHT 0xfffef7
// #define COLOR_HIGHLIGHT 0xFFFFFB
#define COLOR_SHADOW 0x000000 
// screen
#define COLOR_SCREEN_BG COLOR_SHADOW
// info bar
#define COLOR_INFO_TEXT 0x7e7f7d
#define COLOR_INFO_CHARGE 0xf5bd34
#define COLOR_INFO_BATT_LOW  0xe83a37
#define COLOR_INFO_BG COLOR_SHADOW
#define COLOR_INFO_SHADOW COLOR_SHADOW
// txt terminal
#define COLOR_TERM_RECENT_RX_TEXT COLOR_HIGHLIGHT
#define COLOR_TERM_RX_TEXT 0xafb1ac
#define COLOR_TERM_RECENT_TX_TEXT 0xf5bd34
#define COLOR_TERM_TX_TEXT 0xad8428
// chart default values
#define COLOR_CHART_PALETTE {0xe83a37, 0xf5bd34, 0x6ebe77, 0x3697d4, 0xdc4894}
#define COLOR_CHART_BG 0x004e84
#define COLOR_CHART_SERIES_NAME_TEXT COLOR_SHADOW
#define COLOR_CHART_SERIES_VALUE_TEXT COLOR_HIGHLIGHT
#define COLOR_CHART_WAITING_TITLE_TEXT COLOR_HIGHLIGHT
#define COLOR_CHART_WAITING_CONTENT_TEXT COLOR_HIGHLIGHT
#define COLOR_CHART_WAITING_CONTENT_BG 0xe5b225
// ----------------------------------------

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define TERM_LIME_MAX 40
#define CHART_WIDTH_PIXEL 274
#define CHART_RESET_AT_IDLE_LOOP 3
#define CHART_VALUE_SCALE_DEFAULT 100 // avoid float in chart data: 0.01*100 -> 1
#define CHART_VALUE_FMT_DEFAULT "%.1f"
#define CHART_COUNT_MAX 5 // no more screen space for more than 5 charts
#define UI_REFRESH_DELAY 20 // up to 50 lines/s
#define BACKLIGHT_PWM_TOLERANCE 20 // do not change backlight pwm within +-N

#define ENV_LIGHT_PWM_MIN 85
#define ENV_LIGHT_PWM_MAX 245

// #define ENV_LIGHT_PWM_MIN 255
// #define ENV_LIGHT_PWM_MAX 255

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Bus_SPI _bus_instance;
    lgfx::Light_PWM _light_instance;

public:
    LGFX(void)
    {
        {
            auto cfg = _bus_instance.config();

            cfg.spi_host = SPI3_HOST; // 使用するSPIを選択  ESP32-S2,C3 : SPI2_HOST or SPI3_HOST / ESP32 : VSPI_HOST or HSPI_HOST
            // ※ ESP-IDFバージョンアップに伴い、VSPI_HOST , HSPI_HOSTの記述は非推奨になるため、エラーが出る場合は代わりにSPI2_HOST , SPI3_HOSTを使用してください。
            cfg.spi_mode = 0;                  // SPI通信モードを設定 (0 ~ 3)
            cfg.freq_write = 80000000;         // 送信時のSPIクロック (最大80MHz, 80MHzを整数で割った値に丸められます)
            cfg.freq_read = 16000000;          // 受信時のSPIクロック
            cfg.spi_3wire = true;              // 受信をMOSIピンで行う場合はtrueを設定
            cfg.use_lock = true;               // トランザクションロックを使用する場合はtrueを設定
            cfg.dma_channel = SPI_DMA_CH_AUTO; // 使用するDMAチャンネルを設定 (0=DMA不使用 / 1=1ch / 2=ch / SPI_DMA_CH_AUTO=自動設定)
            // ※ ESP-IDFバージョンアップに伴い、DMAチャンネルはSPI_DMA_CH_AUTO(自動設定)が推奨になりました。1ch,2chの指定は非推奨になります。
            cfg.pin_sclk = PIN_LCD_SCL;                       // SPIのSCLKピン番号を設定
            cfg.pin_mosi = PIN_LCD_SDA;                      // SPIのMOSIピン番号を設定
            cfg.pin_miso = -1;                      // SPIのMISOピン番号を設定 (-1 = disable)
            cfg.pin_dc = PIN_LCD_DC;                        // SPIのD/Cピン番号を設定  (-1 = disable)
                                                    // SDカードと共通のSPIバスを使う場合、MISOは省略せず必ず設定してください。
                                                    //*/
            _bus_instance.config(cfg);              // 設定値をバスに反映します。
            _panel_instance.setBus(&_bus_instance); // バスをパネルにセットします。
        }

        {                                        // 表示パネル制御の設定を行います。
            auto cfg = _panel_instance.config(); // 表示パネル設定用の構造体を取得します。

            cfg.pin_cs = PIN_LCD_CS;   // CSが接続されているピン番号   (-1 = disable)
            cfg.pin_rst = PIN_LCD_RES;  // RSTが接続されているピン番号  (-1 = disable)
            cfg.pin_busy = -1; // BUSYが接続されているピン番号 (-1 = disable)

            // ※ 以下の設定値はパネル毎に一般的な初期値が設定されていますので、不明な項目はコメントアウトして試してみてください。
            // 90 rotate
            cfg.panel_width = SCREEN_HEIGHT;    // 実際に表示可能な幅
            cfg.panel_height = SCREEN_WIDTH;   // 実際に表示可能な高さ
            cfg.offset_x = 0;         // パネルのX方向オフセット量
            cfg.offset_y = 0;         // パネルのY方向オフセット量
            cfg.offset_rotation = 0;  // 回転方向の値のオフセット 0~7 (4~7は上下反転)
            cfg.dummy_read_pixel = 8; // ピクセル読出し前のダミーリードのビット数
            cfg.dummy_read_bits = 1;  // ピクセル以外のデータ読出し前のダミーリードのビット数
            cfg.readable = true;      // データ読出しが可能な場合 trueに設定
            cfg.invert = true;        // パネルの明暗が反転してしまう場合 trueに設定
            cfg.rgb_order = false;    // パネルの赤と青が入れ替わってしまう場合 trueに設定
            cfg.dlen_16bit = false;   // 16bitパラレルやSPIでデータ長を16bit単位で送信するパネルの場合 trueに設定
            cfg.bus_shared = false;   // SDカードとバスを共有している場合 trueに設定(drawJpgFile等でバス制御を行います)

            // 以下はST7735やILI9163のようにピクセル数が可変のドライバで表示がずれる場合にのみ設定してください。
            //    cfg.memory_width     =   240;  // ドライバICがサポートしている最大の幅
            //    cfg.memory_height    =   320;  // ドライバICがサポートしている最大の高さ

            _panel_instance.config(cfg);
        }

        //*
        {                                        // バックライト制御の設定を行います。（必要なければ削除）
            auto cfg = _light_instance.config(); // バックライト設定用の構造体を取得します。

            cfg.pin_bl = PIN_LCD_BLK;      // バックライトが接続されているピン番号
            cfg.invert = false;  // バックライトの輝度を反転させる場合 true
            cfg.freq = 5000;     // バックライトのPWM周波数
            cfg.pwm_channel = 1; // 使用するPWMのチャンネル番号

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance); // バックライトをパネルにセットします。
        }
        //*/

        setPanel(&_panel_instance); // 使用するパネルをセットします。
    }
};

extern SemaphoreHandle_t lvgl_mutex;
extern bool toggle_chart_terminal;

void ui_init(void);

#endif