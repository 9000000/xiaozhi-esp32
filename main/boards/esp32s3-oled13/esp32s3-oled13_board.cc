#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/ssd1306_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include <driver/i2c_master.h>

#define TAG "ESP32S3OLED13Board"

class ESP32S3OLED13Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    i2c_master_bus_handle_t oled_i2c_bus_;
    Button boot_button_;
    Ssd1306Display* display_;

    void InitializeI2c() {
        // Codec
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // OLED
        i2c_master_bus_config_t oled_i2c_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = OLED_I2C_SDA_PIN,
            .scl_io_num = OLED_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&oled_i2c_cfg, &oled_i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
    }

    void InitializeDisplay() {
        display_ = new Ssd1306Display(oled_i2c_bus_, OLED_I2C_ADDR, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        display_->Init();
    }

public:
    ESP32S3OLED13Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeDisplay();
        InitializeButtons();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    virtual Backlight* GetBacklight() override {
        return nullptr; // OLED không cần backlight PWM
    }
};

DECLARE_BOARD(ESP32S3OLED13Board);
