#include "esp32_radio.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>  // Cho hàm isdigit
#include <thread>   // Cho so sánh thread ID
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Radio"

// ========== Các hàm xác thực đơn giản cho ESP32 ==========

/**
 * @brief Lấy địa chỉ MAC của thiết bị
 * @return Chuỗi địa chỉ MAC
 */
static std::string get_device_mac() {
    return SystemInfo::GetMacAddress();
}

/**
 * @brief Lấy ID chip của thiết bị
 * @return Chuỗi ID chip
 */
static std::string get_device_chip_id() {
    // Sử dụng địa chỉ MAC làm ID chip, loại bỏ dấu phân cách
    std::string mac = SystemInfo::GetMacAddress();
    // Xóa tất cả dấu hai chấm
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief Tạo khóa động
 * @param timestamp Thời gian
 * @return Chuỗi khóa động
 */
static std::string generate_dynamic_key(int64_t timestamp) {
    // Khóa bí mật (hãy thay đổi để khớp với máy chủ)
    const std::string secret_key = "your-esp32-secret-key-2024";
    
    // Lấy thông tin thiết bị
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // Kết hợp dữ liệu: MAC:chipID:timestamp:secret_key
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;
    
    // Hash SHA256
    unsigned char hash[32];
    mbedtls_sha256((unsigned char*)data.c_str(), data.length(), hash, 0);
    
    // Chuyển đổi sang chuỗi hex (16 byte đầu tiên)
    std::string key;
    for (int i = 0; i < 16; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }
    
    return key;
}

/**
 * @brief Thêm tiêu đề xác thực vào yêu cầu HTTP
 * @param http Con trỏ HTTP client
 */
static void add_auth_headers(Http* http) {
    // Lấy timestamp hiện tại
    int64_t timestamp = esp_timer_get_time() / 1000000;  // Chuyển đổi sang giây
    
    // Tạo khóa động
    std::string dynamic_key = generate_dynamic_key(timestamp);
    
    // Lấy thông tin thiết bị
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();
    
    // Thêm tiêu đề xác thực
    if (http) {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);
        
        ESP_LOGI(TAG, "Đã thêm tiêu đề xác thực - MAC: %s, ChipID: %s, Timestamp: %lld", 
                 mac.c_str(), chip_id.c_str(), timestamp);
    }
}

// Hàm mã hóa URL
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // Mã hóa khoảng trắng thành '+' hoặc '%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// Hàm trợ giúp xây dựng URL với tham số
static std::string buildUrlWithParams(const std::string& base_url, const std::string& path, const std::string& query) {
    std::string result_url = base_url + path + "?";
    size_t pos = 0;
    size_t amp_pos = 0;
    
    while ((amp_pos = query.find("&", pos)) != std::string::npos) {
        std::string param = query.substr(pos, amp_pos - pos);
        size_t eq_pos = param.find("=");
        
        if (eq_pos != std::string::npos) {
            std::string key = param.substr(0, eq_pos);
            std::string value = param.substr(eq_pos + 1);
            result_url += key + "=" + url_encode(value) + "&";
        } else {
            result_url += param + "&";
        }
        
        pos = amp_pos + 1;
    }
    
    // Xử lý tham số cuối cùng
    std::string last_param = query.substr(pos);
    size_t eq_pos = last_param.find("=");
    
    if (eq_pos != std::string::npos) {
        std::string key = last_param.substr(0, eq_pos);
        std::string value = last_param.substr(eq_pos + 1);
        result_url += key + "=" + url_encode(value);
    } else {
        result_url += last_param;
    }
    
    return result_url;
}

Esp32Radio::Esp32Radio() : last_downloaded_data_(), current_Radio_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_url_(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false) {
    ESP_LOGI(TAG, "Trình phát Radio đã được khởi tạo với chế độ hiển thị phổ mặc định");
    InitializeMp3Decoder();
}

Esp32Radio::~Esp32Radio() {
    ESP_LOGI(TAG, "Hủy trình phát Radio - dừng tất cả hoạt động");
    
    // Dừng tất cả hoạt động
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    // Thông báo cho tất cả các thread đang chờ
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // Chờ thread download kết thúc, timeout 5 giây
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Đang chờ thread download kết thúc (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();
        
        // Chờ thread kết thúc
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 5) {
                ESP_LOGW(TAG, "Thread download timeout sau 5 giây");
                break;
            }
            
            // Đặt lại cờ dừng để đảm bảo thread có thể phát hiện
            is_downloading_ = false;
            
            // Thông báo biến điều kiện
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // Kiểm tra xem thread đã kết thúc chưa
            if (!download_thread_.joinable()) {
                thread_finished = true;
            }
            
            // In thông tin chờ định kỳ
            if (elapsed > 0 && elapsed % 1 == 0) {
                ESP_LOGI(TAG, "Vẫn đang chờ thread download kết thúc... (%ds)", (int)elapsed);
            }
        }
        
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Thread download đã kết thúc");
    }
    
    // Chờ thread phát kết thúc, timeout 3 giây
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Đang chờ thread phát kết thúc (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();
        
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 3) {
                ESP_LOGW(TAG, "Thread phát timeout sau 3 giây");
                break;
            }
            
            // Đặt lại cờ dừng
            is_playing_ = false;
            
            // Thông báo biến điều kiện
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // Kiểm tra xem thread đã kết thúc chưa
            if (!play_thread_.joinable()) {
                thread_finished = true;
            }
        }
        
        if (play_thread_.joinable()) {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Thread phát đã kết thúc");
    }
    
    // Chờ thread lời bài hát kết thúc
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Đang chờ thread lời bài hát kết thúc");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Thread lời bài hát đã kết thúc");
    }
    
    // Dọn dẹp buffer và bộ giải mã MP3
    ClearAudioBuffer();
    CleanupMp3Decoder();
    
    ESP_LOGI(TAG, "Trình phát Radio đã được hủy thành công");
}

bool Esp32Radio::Download(const std::string& song_name, const std::string& artist_name) {
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Bắt đầu lấy thông tin Radio cho: %s", song_name.c_str());

    // Xóa dữ liệu download trước đó
    last_downloaded_data_.clear();

    // Lưu tên bài hát để hiển thị sau này
    current_song_name_ = song_name;

    // Bước 1: Yêu cầu stream_pcm API để lấy thông tin audio
    std::string base_url = "https://ai.daongoc.vn/radio/";
    std::string full_url = base_url + "stream_pcm.php?song=" + url_encode(song_name);

    //ESP_LOGI(TAG, "URL yêu cầu: %s", full_url.c_str());

    // Sử dụng HTTP client từ Board
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    // Đặt tiêu đề yêu cầu cơ bản
    http->SetHeader("User-Agent", "ESP32-Radio-Player/1.0");
    http->SetHeader("Accept", "application/json");

    // Thêm tiêu đề xác thực ESP32
    add_auth_headers(http.get());

    // Mở kết nối GET
    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "Không thể kết nối đến Radio API");
        return false;
    }

    // Kiểm tra mã trạng thái phản hồi
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET thất bại với mã trạng thái: %d", status_code);
        http->Close();
        return false;
    }

    // Đọc dữ liệu phản hồi
    last_downloaded_data_ = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d", status_code, last_downloaded_data_.length());
    ESP_LOGD(TAG, "Phản hồi chi tiết Radio hoàn chỉnh: %s", last_downloaded_data_.c_str());

    // Kiểm tra phản hồi xác thực đơn giản (tùy chọn)
    if (last_downloaded_data_.find("ESP32动态密钥验证失败") != std::string::npos) {
        ESP_LOGE(TAG, "Xác thực thất bại cho bài hát: %s", song_name.c_str());
        return false;
    }

    if (!last_downloaded_data_.empty()) {
        // Phân tích JSON phản hồi để trích xuất URL audio
        cJSON* response_json = cJSON_Parse(last_downloaded_data_.c_str());
        if (response_json) {
            cJSON* artist = cJSON_GetObjectItem(response_json, "artist");
            cJSON* title = cJSON_GetObjectItem(response_json, "title");
            cJSON* audio_url = cJSON_GetObjectItem(response_json, "audio_url");
            cJSON* lyric_url = cJSON_GetObjectItem(response_json, "lyric_url");

            if (cJSON_IsString(artist)) {
                ESP_LOGI(TAG, "Nghệ sĩ: %s", artist->valuestring);
            }
            if (cJSON_IsString(title)) {
                ESP_LOGI(TAG, "Tiêu đề: %s", title->valuestring);
            }

            // Kiểm tra audio_url hợp lệ
            if (cJSON_IsString(audio_url) && audio_url->valuestring && strlen(audio_url->valuestring) > 0) {
               // ESP_LOGI(TAG, "Đường dẫn Audio URL: %s", audio_url->valuestring);

                std::string audio_path = audio_url->valuestring;

                // Xử lý URL tuyệt đối / proxy / tương đối
                if (audio_path.rfind("http://", 0) == 0 || audio_path.rfind("https://", 0) == 0) {
                    current_Radio_url_ = audio_path; // giữ nguyên nếu là URL tuyệt đối
                } 
                else if (audio_path.find("radio_proxy.php") != std::string::npos) {
                    current_Radio_url_ = base_url + audio_path; // proxy
                } 
                else {
                    current_Radio_url_ = base_url + audio_path; // URL tương đối
                }

                //ESP_LOGI(TAG, "Audio URL cuối cùng: %s", current_Radio_url_.c_str());
                //ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
               // ESP_LOGI(TAG, "Bắt đầu phát streaming cho: %s", song_name.c_str());
                song_name_displayed_ = false;  
                StartStreaming(current_Radio_url_);

                // Xử lý lyric URL
                if (cJSON_IsString(lyric_url) && lyric_url->valuestring && strlen(lyric_url->valuestring) > 0) {
                    std::string lyric_path = lyric_url->valuestring;

                    if (lyric_path.rfind("http://", 0) == 0 || lyric_path.rfind("https://", 0) == 0) {
                        current_lyric_url_ = lyric_path;
                    } 
                    else if (lyric_path.find("radio_proxy.php") != std::string::npos) {
                        current_lyric_url_ = base_url + lyric_path;
                    } 
                    else {
                        current_lyric_url_ = base_url + lyric_path;
                    }

                    ESP_LOGI(TAG, "Lyric URL cuối cùng: %s", current_lyric_url_.c_str());

                    if (display_mode_ == DISPLAY_MODE_LYRICS) {
                        if (is_lyric_running_) {
                            is_lyric_running_ = false;
                            if (lyric_thread_.joinable()) lyric_thread_.join();
                        }
                        is_lyric_running_ = true;
                        current_lyric_index_ = -1;
                        lyrics_.clear();
                        lyric_thread_ = std::thread(&Esp32Radio::LyricDisplayThread, this);
                    }
                } else {
                    ESP_LOGW(TAG, "Không tìm thấy URL lời bài hát cho bài hát này");
                }

                cJSON_Delete(response_json);
                return true;
            } else {
                ESP_LOGE(TAG, "Audio URL không tìm thấy hoặc trống cho bài hát: %s", song_name.c_str());
                cJSON_Delete(response_json);
                return false;
            }
        } else {
            ESP_LOGE(TAG, "Không thể phân tích phản hồi JSON");
        }
    } else {
        ESP_LOGE(TAG, "Phản hồi trống từ Radio API");
    }

    return false;
}

std::string Esp32Radio::GetDownloadResult() {
    return last_downloaded_data_;
}

// Bắt đầu phát streaming
bool Esp32Radio::StartStreaming(const std::string& Radio_url) {
    if (Radio_url.empty()) {
        ESP_LOGE(TAG, "Radio URL trống");
        return false;
    }
    
    ESP_LOGD(TAG, "Bắt đầu streaming cho URL: %s", Radio_url.c_str());
    
    // Dừng phát và download trước đó
    is_downloading_ = false;
    is_playing_ = false;
    
    // Chờ các thread trước đó kết thúc hoàn toàn
    if (download_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // Thông báo thread thoát
        }
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();  // Thông báo thread thoát
        }
        play_thread_.join();
    }
    
    // Xóa buffer
    ClearAudioBuffer();
    
    // Cấu hình kích thước stack thread để tránh tràn stack
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;  // Kích thước stack 8KB
    cfg.prio = 5;           // Độ ưu tiên trung bình
    cfg.thread_name = "audio_stream";
    esp_pthread_set_cfg(&cfg);
    
    // Bắt đầu thread download
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Radio::DownloadAudioStream, this, Radio_url);
    
    // Bắt đầu thread phát (sẽ chờ buffer có đủ dữ liệu)
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Radio::PlayAudioStream, this);
    
    ESP_LOGI(TAG, "Các thread streaming đã khởi động thành công");
    
    return true;
}

// Dừng streaming
bool Esp32Radio::StopStreaming() {
    ESP_LOGI(TAG, "Đang dừng Radio streaming - trạng thái hiện tại: downloading=%d, playing=%d", 
            is_downloading_.load(), is_playing_.load());

    // Đặt lại sample rate về giá trị gốc
    ResetSampleRate();
    
    // Kiểm tra xem có streaming nào đang diễn ra không
    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "Không có streaming nào đang diễn ra");
        return true;
    }
    
    // Dừng cờ download và phát
    is_downloading_ = false;
    is_playing_ = false;
    
    // Xóa hiển thị tên bài hát
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");  // Xóa hiển thị tên bài hát
        ESP_LOGI(TAG, "Đã xóa hiển thị tên bài hát");
    }
    
    // Thông báo cho tất cả các thread đang chờ
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // Chờ thread kết thúc
    if (download_thread_.joinable()) {
        download_thread_.join();
        ESP_LOGI(TAG, "Thread download đã join trong StopStreaming");
    }
    
    // Chờ thread phát kết thúc, sử dụng cách an toàn hơn
    if (play_thread_.joinable()) {
        // Đặt cờ dừng trước
        is_playing_ = false;
        
        // Thông báo biến điều kiện, đảm bảo thread có thể thoát
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        
        // Sử dụng cơ chế timeout để chờ thread kết thúc, tránh deadlock
        bool thread_finished = false;
        int wait_count = 0;
        const int max_wait = 100; // Tối đa chờ 1 giây
        
        while (!thread_finished && wait_count < max_wait) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
            
            // Kiểm tra xem thread có thể join không
            if (!play_thread_.joinable()) {
                thread_finished = true;
                break;
            }
        }
        
        if (play_thread_.joinable()) {
            if (wait_count >= max_wait) {
                ESP_LOGW(TAG, "Thread phát join timeout, detaching thread");
                play_thread_.detach();
            } else {
                play_thread_.join();
                ESP_LOGI(TAG, "Thread phát đã join trong StopStreaming");
            }
        }
    }
    
    // Sau khi thread hoàn toàn kết thúc, chỉ dừng hiển thị FFT trong chế độ phổ
    if (display && display_mode_ == DISPLAY_MODE_SPECTRUM) {
        display->stopFft();
        ESP_LOGI(TAG, "Đã dừng hiển thị FFT trong StopStreaming (chế độ phổ)");
    } else if (display) {
        ESP_LOGI(TAG, "Không ở chế độ phổ, bỏ qua dừng FFT trong StopStreaming");
    }
    
    ESP_LOGI(TAG, "Tín hiệu dừng Radio streaming đã được gửi");
    return true;
}

// Download audio stream
void Esp32Radio::DownloadAudioStream(const std::string& Radio_url) {
    ESP_LOGD(TAG, "Bắt đầu download audio stream từ: %s", Radio_url.c_str());
    
    // Xác thực tính hợp lệ của URL
    if (Radio_url.empty() || Radio_url.find("http") != 0) {
        ESP_LOGE(TAG, "Định dạng URL không hợp lệ: %s", Radio_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    // Đặt tiêu đề yêu cầu cơ bản
    http->SetHeader("User-Agent", "ESP32-Radio-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-");  // Hỗ trợ tiếp tục tải
    
    // Thêm tiêu đề xác thực ESP32
    add_auth_headers(http.get());
    
    if (!http->Open("GET", Radio_url)) {
        ESP_LOGE(TAG, "Không thể kết nối đến URL stream Radio");
        is_downloading_ = false;
        return;
    }
    
    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 206) {  // 206 cho nội dung một phần
        ESP_LOGE(TAG, "HTTP GET thất bại với mã trạng thái: %d", status_code);
        http->Close();
        is_downloading_ = false;
        return;
    }
    
    ESP_LOGI(TAG, "Đã bắt đầu download audio stream, trạng thái: %d", status_code);
    
    // Đọc dữ liệu audio theo từng khối
    const size_t chunk_size = 4096;  // 4KB mỗi khối
    char buffer[chunk_size];
    size_t total_downloaded = 0;
    
    while (is_downloading_ && is_playing_) {
        int bytes_read = http->Read(buffer, chunk_size);
        if (bytes_read < 0) {
            ESP_LOGE(TAG, "Không thể đọc dữ liệu audio: mã lỗi %d", bytes_read);
            break;
        }
        if (bytes_read == 0) {
            ESP_LOGI(TAG, "Download audio stream hoàn tất, tổng: %d bytes", total_downloaded);
            break;
        }
        
        // In thông tin khối dữ liệu
        if (bytes_read >= 16) {
            // Có thể in hex nếu cần debug
        } else {
            ESP_LOGI(TAG, "Khối dữ liệu quá nhỏ: %d bytes", bytes_read);
        }
        
        // Thử phát hiện định dạng file (kiểm tra header)
        if (total_downloaded == 0 && bytes_read >= 4) {
            if (memcmp(buffer, "ID3", 3) == 0) {
                ESP_LOGI(TAG, "Phát hiện file MP3 với ID3 tag");
            } else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0) {
                ESP_LOGI(TAG, "Phát hiện header file MP3");
            } else if (memcmp(buffer, "RIFF", 4) == 0) {
                ESP_LOGI(TAG, "Phát hiện file WAV");
            } else if (memcmp(buffer, "fLaC", 4) == 0) {
                ESP_LOGI(TAG, "Phát hiện file FLAC");
            } else if (memcmp(buffer, "OggS", 4) == 0) {
                ESP_LOGI(TAG, "Phát hiện file OGG");
            } else {
                ESP_LOGI(TAG, "Định dạng audio không xác định, 4 byte đầu: %02X %02X %02X %02X", 
                        (unsigned char)buffer[0], (unsigned char)buffer[1], 
                        (unsigned char)buffer[2], (unsigned char)buffer[3]);
            }
        }
        
        // Tạo khối dữ liệu audio
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Không thể cấp phát bộ nhớ cho khối audio");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);
        
        // Chờ buffer có không gian
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });
            
            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                
                // Thông báo cho thread phát có dữ liệu mới
                buffer_cv_.notify_one();
                
                if (total_downloaded % (256 * 1024) == 0) {  // In tiến độ mỗi 256KB
                    ESP_LOGI(TAG, "Đã download %d bytes, kích thước buffer: %d", total_downloaded, buffer_size_);
                }
            } else {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }
    
    http->Close();
    is_downloading_ = false;
    
    // Thông báo cho thread phát download hoàn tất
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "Thread download audio stream đã kết thúc");
}

// Phát audio stream
void Esp32Radio::PlayAudioStream() {
    ESP_LOGI(TAG, "Bắt đầu phát audio stream");
    

    // Khởi tạo biến theo dõi thời gian
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->output_enabled()) {
        ESP_LOGE(TAG, "Audio codec không khả dụng hoặc chưa kích hoạt");
        is_playing_ = false;
        return;
    }

    // 🔊 Đặt âm lượng to nhất ngay sau khi chắc chắn codec hợp lệ
    codec->SetOutputVolume(300);  // 400 = âm lượng tối đa
    ESP_LOGI(TAG, "Âm lượng đã được đặt ở mức tối đa");
    
    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "Bộ giải mã MP3 chưa được khởi tạo");
        is_playing_ = false;
        return;
    }
    
    // Chờ buffer có đủ dữ liệu để bắt đầu phát
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this] { 
            return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty()); 
        });
    }
    
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Bắt đầu phát với kích thước buffer: %d", buffer_size_);
    
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = nullptr;
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    
    // Cấp phát buffer đầu vào MP3
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Không thể cấp phát buffer đầu vào MP3");
        is_playing_ = false;
        return;
    }
    
    // Đánh dấu đã xử lý ID3 tag chưa
    bool id3_processed = false;
    
    while (is_playing_) {
        // Kiểm tra trạng thái thiết bị, chỉ phát nhạc khi ở trạng thái nhàn rỗi
        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        
        // Chuyển đổi trạng thái: đang nói -> đang nghe -> trạng thái chờ -> phát nhạc
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
            if (current_state == kDeviceStateSpeaking) {
                ESP_LOGI(TAG, "Thiết bị đang ở trạng thái nói, chuyển sang trạng thái nghe để phát Radio");
            }
            if (current_state == kDeviceStateListening) {
                ESP_LOGI(TAG, "Thiết bị đang ở trạng thái nghe, chuyển sang trạng thái chờ để phát Radio");
            }
            // Chuyển trạng thái
            app.ToggleChatState(); // Chuyển thành trạng thái chờ
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        } else if (current_state != kDeviceStateIdle) { // Không phải trạng thái chờ, chặn không cho phát nhạc
            ESP_LOGD(TAG, "Trạng thái thiết bị là %d, tạm dừng phát Radio", current_state);
            // Nếu không phải trạng thái nhàn rỗi, tạm dừng phát
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // Kiểm tra trạng thái thiết bị đã thông qua, hiển thị tên bài hát đang phát
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                // Định dạng hiển thị tên bài hát thành 《tên bài hát》 đang phát...
                std::string formatted_song_name = "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Đang hiển thị tên bài hát: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // Khởi động chức năng hiển thị tương ứng dựa trên chế độ hiển thị
            if (display) {
                if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
                    display->start();
                    ESP_LOGI(TAG, "Đã gọi display start() cho hiển thị phổ");
                } else {
                    ESP_LOGI(TAG, "Chế độ hiển thị lời bài hát đang hoạt động, tắt hiển thị FFT");
                }
            }
        }
        
        // Nếu cần thêm dữ liệu MP3, đọc từ buffer
        if (bytes_left < 4096) {  // Giữ ít nhất 4KB dữ liệu để giải mã
            AudioChunk chunk;
            
            // Lấy dữ liệu audio từ buffer
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        // Download hoàn tất và buffer trống, kết thúc phát
                        ESP_LOGI(TAG, "Phát kết thúc, tổng đã phát: %d bytes", total_played);
                        break;
                    }
                    // Chờ dữ liệu mới
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }
                
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                
                // Thông báo cho thread download buffer có không gian
                buffer_cv_.notify_one();
            }
            
            // Thêm dữ liệu mới vào buffer đầu vào MP3
            if (chunk.data && chunk.size > 0) {
                // Di chuyển dữ liệu còn lại về đầu buffer
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                
                // Kiểm tra không gian buffer
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                
                // Sao chép dữ liệu mới
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
                
                // Kiểm tra và bỏ qua ID3 tag (chỉ xử lý một lần khi bắt đầu)
                if (!id3_processed && bytes_left >= 10) {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0) {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Đã bỏ qua ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }
                
                // Giải phóng bộ nhớ chunk
                heap_caps_free(chunk.data);
            }
        }
        
        // Thử tìm từ đồng bộ khung MP3
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "Không tìm thấy từ đồng bộ MP3, bỏ qua %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }
        
        // Nhảy đến vị trí đồng bộ
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        // Giải mã khung MP3
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        
        if (decode_result == 0) {
            // Giải mã thành công, lấy thông tin khung
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // Kiểm tra tính hợp lệ cơ bản của thông tin khung, tránh lỗi chia cho 0
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Thông tin khung không hợp lệ: rate=%d, channels=%d, bỏ qua", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // Tính thời lượng khung hiện tại (mili giây)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            
            // Cập nhật thời gian phát hiện tại
            current_play_time_ms_ += frame_duration_ms;
            
            ESP_LOGD(TAG, "Khung %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // Cập nhật hiển thị lời bài hát
            int buffer_latency_ms = 600; // Giá trị điều chỉnh thực tế
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
            // Gửi dữ liệu PCM đến hàng đợi giải mã audio của Application
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                // Nếu là stereo, chuyển đổi sang mono
                if (mp3_frame_info_.nChans == 2) {
                    // Chuyển đổi stereo sang mono: trộn kênh trái và phải
                    int stereo_samples = mp3_frame_info_.outputSamps;  // Tổng số mẫu bao gồm cả stereo
                    int mono_samples = stereo_samples / 2;  // Số mẫu mono thực tế
                    
                    mono_buffer.resize(mono_samples);
                    
                    for (int i = 0; i < mono_samples; ++i) {
                        // Trộn kênh trái và phải (L + R) / 2
                        int left = pcm_buffer[i * 2];      // Kênh trái
                        int right = pcm_buffer[i * 2 + 1]; // Kênh phải
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }
                    
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Đã chuyển đổi stereo sang mono: %d -> %d samples", 
                            stereo_samples, mono_samples);
                } else if (mp3_frame_info_.nChans == 1) {
                    // Đã là mono, không cần chuyển đổi
                    ESP_LOGD(TAG, "Audio đã là mono: %d samples", final_sample_count);
                } else {
                    ESP_LOGW(TAG, "Số kênh không được hỗ trợ: %d, xử lý như mono", 
                            mp3_frame_info_.nChans);
                }
                
                // Tạo AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = 60;  // Sử dụng độ dài khung mặc định của Application
                packet.timestamp = 0;
                
                // Chuyển đổi dữ liệu PCM int16_t sang mảng byte uint8_t
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

                if (final_pcm_data_fft == nullptr) {
                    final_pcm_data_fft = (int16_t*)heap_caps_malloc(
                        final_sample_count * sizeof(int16_t),
                        MALLOC_CAP_SPIRAM
                    );
                }
                
                memcpy(
                    final_pcm_data_fft,
                    final_pcm_data,
                    final_sample_count * sizeof(int16_t)
                );
                
                ESP_LOGD(TAG, "Đang gửi %d mẫu PCM (%d bytes, rate=%d, channels=%d->1) đến Application", 
                        final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                
                // Gửi đến hàng đợi giải mã audio của Application
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;
                
                // In tiến độ phát
                if (total_played % (128 * 1024) == 0) {
                    ESP_LOGI(TAG, "Đã phát %d bytes, kích thước buffer: %d", total_played, buffer_size_);
                }
            }
            
        } else {
            // Giải mã thất bại
            ESP_LOGW(TAG, "Giải mã MP3 thất bại với lỗi: %d", decode_result);
            
            // Bỏ qua một số byte và tiếp tục thử
            if (bytes_left > 1) {
                read_ptr++;
                bytes_left--;
            } else {
                bytes_left = 0;
            }
        }
    }
    
    // Dọn dẹp
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
    }
    
    // Dọn dẹp cơ bản khi phát kết thúc, nhưng không gọi StopStreaming để tránh thread tự chờ
    ESP_LOGI(TAG, "Phát audio stream đã kết thúc, tổng đã phát: %d bytes", total_played);
    ESP_LOGI(TAG, "Đang thực hiện dọn dẹp cơ bản từ thread phát");
    
    // Dừng cờ phát
    is_playing_ = false;
    
    // Chỉ dừng hiển thị FFT trong chế độ hiển thị phổ
    if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->stopFft();
			codec->SetOutputVolume(100);  // Hạ âm lượng về 100
            ESP_LOGI(TAG, "Đã dừng hiển thị FFT từ thread phát (chế độ phổ)");
        }
    } else {
        ESP_LOGI(TAG, "Không ở chế độ phổ, bỏ qua dừng FFT");
    }
}

// Xóa audio buffer
void Esp32Radio::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "Đã xóa audio buffer");
}

// Khởi tạo bộ giải mã MP3
bool Esp32Radio::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Không thể khởi tạo bộ giải mã MP3");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "Đã khởi tạo bộ giải mã MP3 thành công");
    return true;
}

// Dọn dẹp bộ giải mã MP3
void Esp32Radio::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "Đã dọn dẹp bộ giải mã MP3");
}

// Đặt lại sample rate về giá trị gốc
void Esp32Radio::ResetSampleRate() {
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 && 
        codec->output_sample_rate() != codec->original_output_sample_rate()) {
        ESP_LOGI(TAG, "Đặt lại sample rate: từ %d Hz về giá trị gốc %d Hz", 
                codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1)) {  // -1 nghĩa là đặt lại về giá trị gốc
            ESP_LOGI(TAG, "Đã đặt lại sample rate về giá trị gốc: %d Hz", codec->output_sample_rate());
        } else {
            ESP_LOGW(TAG, "Không thể đặt lại sample rate về giá trị gốc");
        }
    }
}

// Bỏ qua ID3 tag ở đầu file MP3
size_t Esp32Radio::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // Kiểm tra header ID3v2 tag "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // Tính kích thước tag (định dạng synchsafe integer)
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // Header ID3v2 (10 byte) + nội dung tag
    size_t total_skip = 10 + tag_size;
    
    // Đảm bảo không vượt quá kích thước dữ liệu có sẵn
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Đã tìm thấy ID3v2 tag, bỏ qua %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// Tải lời bài hát
bool Esp32Radio::DownloadLyrics(const std::string& lyric_url) {
    ESP_LOGI(TAG, "Đang tải lời bài hát từ: %s", lyric_url.c_str());
    
    // Kiểm tra URL có trống không
    if (lyric_url.empty()) {
        ESP_LOGE(TAG, "URL lời bài hát trống!");
        return false;
    }
    
    // Thêm logic thử lại
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5;  // Tối đa cho phép 5 lần chuyển hướng
    
    while (retry_count < max_retries && !success && redirect_count < max_redirects) {
        if (retry_count > 0) {
            ESP_LOGI(TAG, "Thử lại tải lời bài hát (lần thử %d của %d)", retry_count + 1, max_retries);
            // Tạm dừng trước khi thử lại
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // Sử dụng HTTP client từ Board
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGE(TAG, "Không thể tạo HTTP client để tải lời bài hát");
            retry_count++;
            continue;
        }
        
        // Đặt tiêu đề yêu cầu cơ bản
        http->SetHeader("User-Agent", "ESP32-Radio-Player/1.0");
        http->SetHeader("Accept", "text/plain");
        
        // Thêm tiêu đề xác thực ESP32
        add_auth_headers(http.get());
        
        // Mở kết nối GET
        ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Không thể mở kết nối HTTP cho lời bài hát");
            retry_count++;
            continue;
        }
        
        // Kiểm tra mã trạng thái HTTP
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Mã trạng thái HTTP tải lời bài hát: %d", status_code);
        
        // Xử lý chuyển hướng
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            ESP_LOGW(TAG, "Nhận trạng thái chuyển hướng %d nhưng không thể theo dõi chuyển hướng", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // Mã trạng thái không phải 200 series được coi là lỗi
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "HTTP GET thất bại với mã trạng thái: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }
        
        // Đọc phản hồi
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;
        
        ESP_LOGD(TAG, "Bắt đầu đọc nội dung lời bài hát");
        
        while (true) {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;
                
                // In tiến độ download định kỳ
                if (total_read % 4096 == 0) {
                    ESP_LOGD(TAG, "Đã tải %d bytes", total_read);
                }
            } else if (bytes_read == 0) {
                // Kết thúc bình thường, không còn dữ liệu
                ESP_LOGD(TAG, "Tải lời bài hát hoàn tất, tổng bytes: %d", total_read);
                success = true;
                break;
            } else {
                // bytes_read < 0, có thể là vấn đề đã biết của ESP-IDF
                // Nếu đã đọc được một số dữ liệu, coi như download thành công
                if (!lyric_content.empty()) {
                    ESP_LOGW(TAG, "HTTP read trả về %d, nhưng chúng tôi có dữ liệu (%d bytes), tiếp tục", bytes_read, lyric_content.length());
                    success = true;
                    break;
                } else {
                    ESP_LOGE(TAG, "Không thể đọc dữ liệu lời bài hát: mã lỗi %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }
        
        http->Close();
        
        if (read_error) {
            retry_count++;
            continue;
        }
        
        // Nếu đọc dữ liệu thành công, thoát khỏi vòng lặp thử lại
        if (success) {
            break;
        }
    }
    
    // Kiểm tra xem có vượt quá số lần thử lại tối đa không
    if (retry_count >= max_retries) {
        ESP_LOGE(TAG, "Không thể tải lời bài hát sau %d lần thử", max_retries);
        return false;
    }
    
    // Ghi lại vài byte đầu của dữ liệu để debug
    if (!lyric_content.empty()) {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Xem trước nội dung lời bài hát (%d bytes): %s", lyric_content.length(), preview.c_str());
    } else {
        ESP_LOGE(TAG, "Không thể tải lời bài hát hoặc lời bài hát trống");
        return false;
    }
    
    ESP_LOGI(TAG, "Đã tải lời bài hát thành công, kích thước: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// Phân tích lời bài hát
bool Esp32Radio::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Đang phân tích nội dung lời bài hát");
    
    // Sử dụng khóa để bảo vệ truy cập mảng lyrics_
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    lyrics_.clear();
    
    // Chia nội dung lời bài hát theo dòng
    std::istringstream stream(lyric_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // Loại bỏ ký tự xuống dòng ở cuối dòng
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // Bỏ qua dòng trống
        if (line.empty()) {
            continue;
        }
        
        // Phân tích định dạng LRC: [mm:ss.xx]văn bản lời
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos) {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // Kiểm tra xem có phải là thẻ siêu dữ liệu thay vì dấu thời gian không
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = tag_or_time.substr(0, colon_pos);
                    
                    // Kiểm tra bên trái dấu hai chấm có phải là thời gian (số) không
                    bool is_time_format = true;
                    for (char c : left_part) {
                        if (!isdigit(c)) {
                            is_time_format = false;
                            break;
                        }
                    }
                    
                    // Nếu không phải định dạng thời gian, bỏ qua dòng này (thẻ siêu dữ liệu)
                    if (!is_time_format) {
                        ESP_LOGD(TAG, "Bỏ qua thẻ siêu dữ liệu: [%s]", tag_or_time.c_str());
                        continue;
                    }
                    
                    // Là định dạng thời gian, phân tích dấu thời gian
                    try {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // Xử lý văn bản lời bài hát an toàn, đảm bảo mã hóa UTF-8 đúng
                        std::string safe_lyric_text;
                        if (!content.empty()) {
                            // Tạo bản sao an toàn và xác thực chuỗi
                            safe_lyric_text = content;
                            // Đảm bảo chuỗi kết thúc bằng null
                            safe_lyric_text.shrink_to_fit();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));
                        
                        if (!safe_lyric_text.empty()) {
                            // Giới hạn độ dài đầu ra log, tránh vấn đề cắt ngắn ký tự tiếng Trung
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Đã phân tích lời: [%d ms] %s", timestamp_ms, log_text.c_str());
                        } else {
                            ESP_LOGD(TAG, "Đã phân tích lời: [%d ms] (trống)", timestamp_ms);
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Không thể phân tích thời gian: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }
    
    // Sắp xếp theo dấu thời gian
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Đã phân tích %d dòng lời bài hát", lyrics_.size());
    return !lyrics_.empty();
}

// Thread hiển thị lời bài hát
void Esp32Radio::LyricDisplayThread() {
    ESP_LOGI(TAG, "Thread hiển thị lời bài hát đã bắt đầu");
    
    if (!DownloadLyrics(current_lyric_url_)) {
        ESP_LOGE(TAG, "Không thể tải hoặc phân tích lời bài hát");
        is_lyric_running_ = false;
        return;
    }
    
    // Kiểm tra định kỳ xem có cần cập nhật hiển thị không (tần số có thể giảm)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Thread hiển thị lời bài hát đã kết thúc");
}

void Esp32Radio::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    // Tìm lời bài hát nên hiển thị hiện tại
    int new_lyric_index = -1;
    
    // Bắt đầu tìm từ chỉ số lời bài hát hiện tại, tăng hiệu quả
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    // Tìm kiếm thuận: tìm lời bài hát cuối cùng có dấu thời gian nhỏ hơn hoặc bằng thời gian hiện tại
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // Dấu thời gian đã vượt quá thời gian hiện tại
        }
    }
    
    // Nếu không tìm thấy (có thể thời gian hiện tại sớm hơn lời bài hát đầu tiên), hiển thị trống
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    // Nếu chỉ số lời bài hát thay đổi, cập nhật hiển thị
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            std::string lyric_text;
            
            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
                lyric_text = lyrics_[current_lyric_index_].second;
            }
            
            // Hiển thị lời bài hát
            display->SetChatMessage("lyric", lyric_text.c_str());
            
            ESP_LOGD(TAG, "Cập nhật lời tại %lldms: %s", 
                    current_time_ms, 
                    lyric_text.empty() ? "(không có lời)" : lyric_text.c_str());
        }
    }
}

// Triển khai phương thức điều khiển chế độ hiển thị
void Esp32Radio::SetDisplayMode(DisplayMode mode) {
    DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;
    
    ESP_LOGI(TAG, "Chế độ hiển thị đã thay đổi từ %s sang %s", 
            (old_mode == DISPLAY_MODE_SPECTRUM) ? "PHỔ" : "LỜI BÀI HÁT",
            (mode == DISPLAY_MODE_SPECTRUM) ? "PHỔ" : "LỜI BÀI HÁT");
}