// main/boards/esp32s3-oled13/board.cpp
#include "board.h"

Board* create_board() {
    // Khởi tạo và trả về đối tượng Board tùy theo cấu hình.
    return new Board(/* thông số phù hợp với esp32s3-oled13 */);
}
