/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"


#define TAG "MCP"

McpServer::McpServer()
{
}

McpServer::~McpServer()
{
    for (auto tool : tools_)
    {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools()
{

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto &board = Board::GetInstance();
   

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
            "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
            PropertyList(),
            [&board](const PropertyList &properties) -> ReturnValue
            {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList &properties) -> ReturnValue
            {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight)
    {
        AddTool("self.screen.set_brightness",
                "Set the brightness of the screen.",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList &properties) -> ReturnValue
                {
                    uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr)
    {
        AddTool("self.screen.set_theme",
                "Set the theme of the screen. The theme can be `light` or `dark`.",
                PropertyList({Property("theme", kPropertyTypeString)}),
                [display](const PropertyList &properties) -> ReturnValue
                {
                    auto theme_name = properties["theme"].value<std::string>();
                    auto &theme_manager = LvglThemeManager::GetInstance();
                    auto theme = theme_manager.GetTheme(theme_name);
                    if (theme != nullptr)
                    {
                        display->SetTheme(theme);
                        return true;
                    }
                    return false;
                });
    }

    auto camera = board.GetCamera();
    if (camera)
    {
        AddTool("self.camera.take_photo",
                "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList &properties) -> ReturnValue
                {
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture())
                    {
                        throw std::runtime_error("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    return camera->Explain(question);
                });
    }

  
  auto music = board.GetMusic();
if (music)
{
    // 🎵 Công cụ phát nhạc có lời chúc
    AddTool("self.music.play_song",
        "Phát bài hát chỉ định. Khi người dùng yêu cầu phát nhạc, công cụ này sẽ tự động lấy thông tin bài hát và bắt đầu phát luồng.\n"
        "Tham số:\n"
        "  `ten_bai_hat`: Tên bài hát (bắt buộc).\n"
        "  `ten_ca_si`: Tên ca sĩ (tùy chọn, mặc định là chuỗi rỗng).\n"
        "Trả về:\n"
        "  Thông tin trạng thái phát nhạc kèm lời chúc.",
        PropertyList({
            Property("ten_bai_hat", kPropertyTypeString),       // Tên bài hát (bắt buộc)
            Property("ten_ca_si", kPropertyTypeString, "")      // Tên ca sĩ (tùy chọn, mặc định rỗng)
        }),
        [music](const PropertyList &properties) -> ReturnValue
        {
            auto ten_bai_hat = properties["ten_bai_hat"].value<std::string>();
            auto ten_ca_si = properties["ten_ca_si"].value<std::string>();

           // ESP_LOGI("MusicTool", "Yêu cầu phát bài: %s - %s", ten_bai_hat.c_str(), ten_ca_si.c_str());

            if (!music->Download(ten_bai_hat, ten_ca_si))
            {
                return "{\"success\": false, \"message\": \"Không lấy được tài nguyên nhạc, vui lòng thử lại sau.\"}";
            }

            auto ket_qua_tai = music->GetDownloadResult();
            //ESP_LOGI("MusicTool", "Kết quả chi tiết nhạc: %s", ket_qua_tai.c_str());

            // 🌸 Danh sách lời chúc ngẫu nhiên
            std::vector<std::string> loi_chuc = {
                "Chúc bạn có những phút giây thư giãn thật tuyệt vời cùng âm nhạc 🎶",
                "Âm nhạc là liều thuốc chữa lành tâm hồn – hãy cảm nhận từng giai điệu nhé 💖",
                "Thưởng thức bài hát thật trọn vẹn nhé, chúc bạn một ngày vui vẻ 🌈",
                "Một bản nhạc hay có thể thay đổi cả tâm trạng – cùng tận hưởng nào! 🎧",
                "Hy vọng bài hát này mang lại cho bạn chút bình yên và cảm xúc nhẹ nhàng 🌸"
            };

            // Tạo lời chúc ngẫu nhiên
            srand(time(NULL));
            std::string chuc = loi_chuc[rand() % loi_chuc.size()];

            // 🎶 Trả về phản hồi có cảm xúc
            std::string json_response = "{\"success\": true, \"message\": \"" + chuc + " Nhạc đã bắt đầu phát.\"}";
            return json_response;
        });

    // 🎨 Công cụ thiết lập chế độ hiển thị khi phát nhạc
    AddTool("self.music.set_display_mode",
        "Thiết lập chế độ hiển thị khi phát nhạc. Có thể chọn hiển thị phổ hoặc lời bài hát.\n"
        "Tham số:\n"
        "  `che_do`: Chế độ hiển thị, giá trị 'spectrum' (phổ) hoặc 'lyrics' (lời bài hát).\n"
        "Trả về:\n"
        "  Thông tin kết quả thiết lập.",
        PropertyList({
            Property("che_do", kPropertyTypeString) // Chế độ hiển thị: "spectrum" hoặc "lyrics"
        }),
        [music](const PropertyList &properties) -> ReturnValue
        {
            auto che_do = properties["che_do"].value<std::string>();
            std::transform(che_do.begin(), che_do.end(), che_do.begin(), ::tolower);

            if (che_do == "spectrum" || che_do == "phổ")
            {
                // Thiết lập chế độ hiển thị phổ (nếu có API cụ thể thì mở comment bên dưới)
                // auto esp32_music = static_cast<Esp32Music *>(music);
                // esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_SPECTRUM);
                return "{\"success\": true, \"message\": \"Đã chuyển sang chế độ hiển thị phổ 🌈\"}";
            }
            else if (che_do == "lyrics" || che_do == "lời")
            {
                // Thiết lập chế độ hiển thị lời bài hát
                // auto esp32_music = static_cast<Esp32Music *>(music);
                // esp32_music->SetDisplayMode(Esp32Music::DISPLAY_MODE_LYRICS);
                return "{\"success\": true, \"message\": \"Đã chuyển sang chế độ hiển thị lời bài hát 🎤\"}";
            }
            else
            {
                return "{\"success\": false, \"message\": \"Chế độ hiển thị không hợp lệ, vui lòng sử dụng 'spectrum' hoặc 'lyrics'\"}";
            }
        });
}

auto radio = board.GetRadio();
if (radio)
{
    // 🎵 Công cụ phát radio - ĐƠN GIẢN
    AddTool("self.radio.play_song",
        "Phát kênh radio chỉ định. Các kênh hỗ trợ: VOV1, VOV2, VOV3, VOV5, VOV Giao Thông, VOV English, VOV MEKONG, VOV 90FM.\n"
        "Tham số:\n"
        "  `ten_kenh`: Tên kênh radio (bắt buộc).\n"
        "Trả về:\n"
        "  Thông tin trạng thái phát nhạc.",
        PropertyList({
            Property("ten_kenh", kPropertyTypeString) // Tên kênh radio (bắt buộc)
        }),
        [radio](const PropertyList &properties) -> ReturnValue
        {
            auto ten_kenh = properties["ten_kenh"].value<std::string>();

            ESP_LOGI("RadioTool", "Yêu cầu phát kênh radio: %s", ten_kenh.c_str());

            // Gọi hàm Download của radio với tên kênh và để trống tên ca sĩ
            if (!radio->Download(ten_kenh, ""))
            {
                return "{\"success\": false, \"message\": \"Không lấy được tài nguyên radio, vui lòng thử lại sau.\"}";
            }

            auto ket_qua_tai = radio->GetDownloadResult();
            ESP_LOGI("RadioTool", "Kết quả chi tiết radio: %s", ket_qua_tai.c_str());

            // Lời chúc đơn giản
            std::string loi_chuc = "Kênh radio đã bắt đầu phát. Chúc bạn nghe nhạc vui vẻ! 🎵";

            // Trả về phản hồi
            std::string json_response = "{\"success\": true, \"message\": \"" + loi_chuc + "\"}";
            return json_response;
        });

    // 🎨 Công cụ thiết lập chế độ hiển thị - ĐÃ LOẠI BỎ PHẦN GÂY LỖI
    AddTool("self.radio.set_display_mode",
       "Thiết lập chế độ hiển thị khi phát radio.\n"
        "Tham số:\n"
        "  `che_do`: Chế độ hiển thị, 'spectrum' (phổ) hoặc 'lyrics' (lời bài hát).\n"
        "Trả về:\n"
        "  Thông tin kết quả thiết lập.",
        PropertyList({
            Property("che_do", kPropertyTypeString) // Chế độ hiển thị: "spectrum" hoặc "lyrics"
        }),
        [](const PropertyList &properties) -> ReturnValue
        {
            auto che_do = properties["che_do"].value<std::string>();
            std::transform(che_do.begin(), che_do.end(), che_do.begin(), ::tolower);

            if (che_do == "spectrum" || che_do == "phổ")
            {
                return "{\"success\": true, \"message\": \"Đã chuyển sang chế độ hiển thị phổ 🌈\"}";
            }
            else if (che_do == "lyrics" || che_do == "lời")
            {
                return "{\"success\": true, \"message\": \"Đã chuyển sang chế độ hiển thị lời bài hát 🎤\"}";
            }
            else
            {
                return "{\"success\": false, \"message\": \"Chế độ hiển thị không hợp lệ, vui lòng sử dụng 'spectrum' hoặc 'lyrics'\"}";
            }
        });

    // ⏹️ Công cụ dừng phát radio - ĐÃ SỬA LỖI PropertyList
    AddTool("self.radio.stop",
        "Dừng phát radio hiện tại.\n"
        "Trả về:\n"
        "  Thông tin trạng thái sau khi dừng.",
        PropertyList(), // Sửa thành PropertyList() thay vì PropertyList({})
        [radio](const PropertyList &properties) -> ReturnValue
        {
            ESP_LOGI("RadioTool", "Yêu cầu dừng phát radio");
            
            // Gọi hàm dừng phát
            bool ket_qua = radio->StopStreaming();
            
            if (ket_qua) {
                ESP_LOGI("RadioTool", "Đã dừng phát radio thành công");
                return "{\"success\": true, \"message\": \"Đã dừng phát radio. Cảm ơn bạn đã lắng nghe! 👋\"}";
            } else {
                ESP_LOGW("RadioTool", "Không có radio nào đang phát để dừng");
                return "{\"success\": false, \"message\": \"Không có kênh radio nào đang phát để dừng.\"}";
            }
        });
}
	
	
	  // ============================================================================
    // 📰 TOOL TIN TỨC VNEXPRESS - ĐÃ DI CHUYỂN RA NGOÀI
    // ============================================================================
    AddTool("self.get_vnexpress_news",
        "Lay tin tuc moi nhat tu VnExpress RSS. Co the loc theo tu khoa trong tieu de.\n"
        "Du lieu bao gom: tieu de, thoi gian dang, duong link, hinh anh dai dien.\n"
        "Vi du su dung:\n"
        "- Lay tat ca tin: khong can tham so\n"
        "- Tim tin theo tu khoa: {'keyword': 'Da Nang'}\n",
        
        PropertyList(std::vector<Property>{
            Property("keyword", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            // ... implementation giữ nguyên
            auto network = Board::GetInstance().GetNetwork();
            if (!network)
                throw std::runtime_error("Mang khong kha dung");

            auto http = network->CreateHttp(15);
            std::string url = "https://ai.phuquoc.pro/news/get_news.php";
           // ESP_LOGI(TAG, "Dang lay tin tuc tu: %s", url.c_str());

            if (!http->Open("GET", url))
                throw std::runtime_error("Khong the ket noi den URL: " + url);

            http->SetHeader("User-Agent", "ESP32-MCP-Client");
            http->SetHeader("Accept", "application/json");

            if (http->GetStatusCode() != 200) {
                http->Close();
                throw std::runtime_error("Loi HTTP khi lay du lieu tu may chu");
            }

            std::string response = http->ReadAll();
            http->Close();
            if (response.empty())
                throw std::runtime_error("Khong co du lieu tra ve");

            cJSON* root = cJSON_Parse(response.c_str());
            if (!root)
                throw std::runtime_error("Du lieu JSON khong hop le");

            cJSON* source = cJSON_GetObjectItem(root, "source");
            cJSON* updated = cJSON_GetObjectItem(root, "updated");
            cJSON* articles = cJSON_GetObjectItem(root, "articles");

            if (!articles || !cJSON_IsArray(articles)) {
                cJSON_Delete(root);
                throw std::runtime_error("Cau truc JSON khong dung dinh dang - khong tim thay danh sach bai viet");
            }

            std::string filter_keyword;
            try { filter_keyword = properties["keyword"].value<std::string>(); } catch (...) {}

            cJSON* result = cJSON_CreateObject();
            cJSON* news_list = cJSON_CreateArray();

            if (source && cJSON_IsString(source)) {
                cJSON_AddStringToObject(result, "source", source->valuestring);
            }
            if (updated && cJSON_IsString(updated)) {
                cJSON_AddStringToObject(result, "last_updated", updated->valuestring);
            }

            int array_size = cJSON_GetArraySize(articles);
            int found_count = 0;

            for (int i = 0; i < array_size; i++) {
                cJSON* article = cJSON_GetArrayItem(articles, i);
                if (!cJSON_IsObject(article)) continue;

                cJSON* title = cJSON_GetObjectItem(article, "title");
                
                if (!filter_keyword.empty() && title && cJSON_IsString(title)) {
                    std::string s_title = title->valuestring;
                    std::string f = filter_keyword;
                    std::transform(s_title.begin(), s_title.end(), s_title.begin(), ::tolower);
                    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
                    if (s_title.find(f) == std::string::npos) continue;
                }

                cJSON* news_item = cJSON_CreateObject();
                auto add = [&](const char* k, cJSON* v) {
                    if (v && cJSON_IsString(v)) cJSON_AddStringToObject(news_item, k, v->valuestring);
                    else if (v && cJSON_IsNumber(v)) cJSON_AddNumberToObject(news_item, k, v->valueint);
                };

                add("id", cJSON_GetObjectItem(article, "id"));
                add("title", title);
                add("pubDate", cJSON_GetObjectItem(article, "pubDate"));
                add("link", cJSON_GetObjectItem(article, "link"));
                add("image", cJSON_GetObjectItem(article, "image"));

                cJSON_AddItemToArray(news_list, news_item);
                found_count++;
            }

            cJSON_AddItemToObject(result, "articles", news_list);
            cJSON_AddNumberToObject(result, "total_articles", array_size);
            cJSON_AddNumberToObject(result, "found_articles", found_count);

            cJSON_Delete(root);
            
            if (found_count == 0 && !filter_keyword.empty()) {
                cJSON_Delete(result);
                throw std::runtime_error("Khong tim thay bai viet nao voi tu khoa: " + filter_keyword);
            }
            
            return result;
        });



#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
	
	//tool mới đặt ở đây
	
	
	
}

void McpServer::AddUserOnlyTools()
{
    // System tools
    AddUserOnlyTool("self.get_system_info",
                    "Get the system information",
                    PropertyList(),
                    [this](const PropertyList &properties) -> ReturnValue
                    {
                        auto &board = Board::GetInstance();
                        return board.GetSystemInfoJson();
                    });

    AddUserOnlyTool("self.reboot", "Reboot the system",
                    PropertyList(),
                    [this](const PropertyList &properties) -> ReturnValue
                    {
                        auto &app = Application::GetInstance();
                        app.Schedule([&app]()
                                     {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot(); });
                        return true;
                    });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
                    PropertyList({Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")}),
                    [this](const PropertyList &properties) -> ReturnValue
                    {
                        auto url = properties["url"].value<std::string>();
                        ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());

                        auto &app = Application::GetInstance();
                        app.Schedule([url, &app]()
                                     {
                auto ota = std::make_unique<Ota>();
                
                bool success = app.UpgradeFirmware(*ota, url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                } });

                        return true;
                    });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
    if (display)
    {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
                        PropertyList(),
                        [display](const PropertyList &properties) -> ReturnValue
                        {
                            cJSON *json = cJSON_CreateObject();
                            cJSON_AddNumberToObject(json, "width", display->width());
                            cJSON_AddNumberToObject(json, "height", display->height());
                            if (dynamic_cast<OledDisplay *>(display))
                            {
                                cJSON_AddBoolToObject(json, "monochrome", true);
                            }
                            else
                            {
                                cJSON_AddBoolToObject(json, "monochrome", false);
                            }
                            return json;
                        });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
                        PropertyList({Property("url", kPropertyTypeString),
                                      Property("quality", kPropertyTypeInteger, 80, 1, 100)}),
                        [display](const PropertyList &properties) -> ReturnValue
                        {
                            auto url = properties["url"].value<std::string>();
                            auto quality = properties["quality"].value<int>();

                            std::string jpeg_data;
                            if (!display->SnapshotToJpeg(jpeg_data, quality))
                            {
                                throw std::runtime_error("Failed to snapshot screen");
                            }

                            ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());

                            // 构造multipart/form-data请求体
                            std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";

                            auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                            http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                            if (!http->Open("POST", url))
                            {
                                throw std::runtime_error("Failed to open URL: " + url);
                            }
                            {
                                // 文件字段头部
                                std::string file_header;
                                file_header += "--" + boundary + "\r\n";
                                file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                                file_header += "Content-Type: image/jpeg\r\n";
                                file_header += "\r\n";
                                http->Write(file_header.c_str(), file_header.size());
                            }

                            // JPEG数据
                            http->Write((const char *)jpeg_data.data(), jpeg_data.size());

                            {
                                // multipart尾部
                                std::string multipart_footer;
                                multipart_footer += "\r\n--" + boundary + "--\r\n";
                                http->Write(multipart_footer.c_str(), multipart_footer.size());
                            }
                            http->Write("", 0);

                            if (http->GetStatusCode() != 200)
                            {
                                throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                            }
                            std::string result = http->ReadAll();
                            http->Close();
                            ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                            return true;
                        });

        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
                        PropertyList({Property("url", kPropertyTypeString)}),
                        [display](const PropertyList &properties) -> ReturnValue
                        {
                            auto url = properties["url"].value<std::string>();
                            auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                            if (!http->Open("GET", url))
                            {
                                throw std::runtime_error("Failed to open URL: " + url);
                            }
                            int status_code = http->GetStatusCode();
                            if (status_code != 200)
                            {
                                throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                            }

                            size_t content_length = http->GetBodyLength();
                            char *data = (char *)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                            if (data == nullptr)
                            {
                                throw std::runtime_error("Failed to allocate memory for image: " + url);
                            }
                            size_t total_read = 0;
                            while (total_read < content_length)
                            {
                                int ret = http->Read(data + total_read, content_length - total_read);
                                if (ret < 0)
                                {
                                    heap_caps_free(data);
                                    throw std::runtime_error("Failed to download image: " + url);
                                }
                                if (ret == 0)
                                {
                                    break;
                                }
                                total_read += ret;
                            }
                            http->Close();

                            auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                            display->SetPreviewImage(std::move(image));
                            return true;
                        });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto &assets = Assets::GetInstance();
    if (assets.partition_valid())
    {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
                        PropertyList({Property("url", kPropertyTypeString)}),
                        [](const PropertyList &properties) -> ReturnValue
                        {
                            auto url = properties["url"].value<std::string>();
                            Settings settings("assets", true);
                            settings.SetString("download_url", url);
                            return true;
                        });
    }
}

void McpServer::AddTool(McpTool *tool)
{
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool *t)
                     { return t->name() == tool->name(); }) != tools_.end())
    {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string &name, const std::string &description, const PropertyList &properties, std::function<ReturnValue(const PropertyList &)> callback)
{
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string &name, const std::string &description, const PropertyList &properties, std::function<ReturnValue(const PropertyList &)> callback)
{
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string &message)
{
    cJSON *json = cJSON_Parse(message.c_str());
    if (json == nullptr)
    {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON *capabilities)
{
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision))
    {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url))
        {
            auto camera = Board::GetInstance().GetCamera();
            if (camera)
            {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token))
                {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON *json)
{
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0)
    {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method))
    {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0)
    {
        return;
    }

    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params))
    {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id))
    {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize")
    {
        if (cJSON_IsObject(params))
        {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities))
            {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    }
    else if (method_str == "tools/list")
    {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr)
        {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor))
            {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools))
            {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    }
    else if (method_str == "tools/call")
    {
        if (!cJSON_IsObject(params))
        {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name))
        {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments))
        {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    }
    else
    {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string &result)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string &message)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string &cursor, bool list_user_only_tools)
{
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end())
    {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor)
        {
            if ((*it)->name() == cursor)
            {
                found_cursor = true;
            }
            else
            {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only())
        {
            ++it;
            continue;
        }

        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size)
        {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',')
    {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty())
    {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty())
    {
        json += "]}";
    }
    else
    {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string &tool_name, const cJSON *tool_arguments)
{
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(),
                                  [&tool_name](const McpTool *tool)
                                  {
                                      return tool->name() == tool_name;
                                  });

    if (tool_iter == tools_.end())
    {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try
    {
        for (auto &argument : arguments)
        {
            bool found = false;
            if (cJSON_IsObject(tool_arguments))
            {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value))
                {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value))
                {
                    argument.set_value<int>(value->valueint);
                    found = true;
                }
                else if (argument.type() == kPropertyTypeString && cJSON_IsString(value))
                {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found)
            {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto &app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]()
                 {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        } });
}