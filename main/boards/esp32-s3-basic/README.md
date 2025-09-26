# ESP32-S3 OLED 1.3 inch Board

- MCU: ESP32-S3
- Màn hình: OLED 1.3 inch 128x64 (SSD1306, I2C, địa chỉ 0x3C)
- Âm thanh: ES8311
- Nút nhấn: 1 (Boot)
- Không dùng backlight

## Sơ đồ chân OLED I2C
- SDA: GPIO21
- SCL: GPIO22

## Hướng dẫn build/nạp
1. Biên dịch với cấu hình board là esp32s3-oled13
2. Nạp firmware như bình thường

## Ghi chú
- Nếu dùng SPI hoặc dùng loại màn hình khác, hãy sửa lại config.h và code khởi tạo display cho phù hợp.
