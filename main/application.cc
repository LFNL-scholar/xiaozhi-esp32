// 自定义库
#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"

// 系统库
#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

// 定义了日志标签TAG，用于标识日志的来源模块为Application
#define TAG "Application"

// 二进制资源引用
extern const char p3_err_reg_start[] asm("_binary_err_reg_p3_start");
extern const char p3_err_reg_end[] asm("_binary_err_reg_p3_end");
extern const char p3_err_pin_start[] asm("_binary_err_pin_p3_start");
extern const char p3_err_pin_end[] asm("_binary_err_pin_p3_end");
extern const char p3_err_wificonfig_start[] asm("_binary_err_wificonfig_p3_start");
extern const char p3_err_wificonfig_end[] asm("_binary_err_wificonfig_p3_end");

// 定义了一个静态常量字符串数组，用于表示程序的不同状态
static const char* const STATE_STRINGS[] = {
    "unknown",          // 状态未知
    "starting",         // 系统正在启动中
    "configuring",      // 系统正在进行配置
    "idle",             // 系统空闲中
    "connecting",       // 系统正在尝试建立连接
    "listening",        // 系统正在监听输入或等待事件
    "speaking",         // 系统正在输出信息或执行活动
    "upgrading",        // 系统正在升级
    "fatal_error",      // 系统遇到了严重错误
    "invalid_state"     // 无效状态
};

// 构造函数，负责初始化 Application 类实例所需的资源
Application::Application() {
    event_group_ = xEventGroupCreate(); // 创建事件组
    background_task_ = new BackgroundTask(4096 * 8); // 初始化后台任务

    ota_.SetCheckVersionUrl(CONFIG_OTA_VERSION_URL); // 设置 OTA（固件升级）的版本检查 URL
    ota_.SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str()); // 设置 HTTP 请求头
}

// 析构函数，负责释放 Application 类实例占用的资源
Application::~Application() {
    // 释放后台任务
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

// 检查新版本固件并处理升级操作，通过 OTA（Over-The-Air）功能实现远程升级设备固件
void Application::CheckNewVersion() {
    // 获取设备板卡和显示模块实例
    auto& board = Board::GetInstance(); // 获取 Board 单例，代表设备硬件平台
    auto display = board.GetDisplay();  // 获取设备显示模块的指针，便于显示升级信息
    // Check if there is a new firmware version available
    ota_.SetPostData(board.GetJson()); // 设置 POST 数据

    while (true) {
        // 进入循环检测版本
        if (ota_.CheckVersion()) {
            // 检查是否有新版本
            if (ota_.HasNewVersion()) {
                // Wait for the chat state to be idle | 等待设备空闲状态
                do {
                    vTaskDelay(pdMS_TO_TICKS(3000)); // 每隔 3 秒检查一次设备状态，确保设备处于空闲状态（kDeviceStateIdle）后才能开始升级
                } while (GetDeviceState() != kDeviceStateIdle);

                // Use main task to do the upgrade, not cancelable
                // 调度升级任务
                Schedule([this, &board, display]() {
                    // 设置设备状态为升级中
                    SetDeviceState(kDeviceStateUpgrading);
                    
                    // 更新显示器状态，显示下载图标和新版本号提示
                    display->SetIcon(FONT_AWESOME_DOWNLOAD);
                    display->SetStatus("新版本 " + ota_.GetFirmwareVersion());

                    // 预先关闭音频输出，避免升级过程有音频操作
                    board.GetAudioCodec()->EnableOutput(false);
                    // 清空音频解码队列
                    {
                        // 使用互斥锁保护操作，防止多线程竞争
                        std::lock_guard<std::mutex> lock(mutex_);
                        audio_decode_queue_.clear();
                    }
                    background_task_->WaitForCompletion(); // 等待后台任务完成
                    delete background_task_;
                    background_task_ = nullptr; // 停止后台任务并释放资源，确保升级过程的独占性
                    vTaskDelay(pdMS_TO_TICKS(1000));

                    // 开始固件升级
                    // 通过回调函数更新下载进度和速度，并显示在设备显示屏上
                    ota_.StartUpgrade([display](int progress, size_t speed) {
                        char buffer[64];
                        snprintf(buffer, sizeof(buffer), "%d%% %zuKB/s", progress, speed / 1024);
                        display->SetStatus(buffer);
                    });

                    // If upgrade success, the device will reboot and never reach here
                    // 处理升级失败
                    display->SetStatus("更新失败");
                    ESP_LOGI(TAG, "Firmware upgrade failed...");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart();
                });
            } else {
                // 没有新版本的处理
                ota_.MarkCurrentVersionValid(); // 标记当前版本为有效，表明没有新版本需要升级
                display->ShowNotification("版本 " + ota_.GetCurrentVersion()); // 在显示屏上提示当前版本信息
            }
            return;
        }

        // Check again in 60 seconds | 60 秒后重新检查
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

// 在设备遇到错误或特定的警告消息时进行提醒和提示
void Application::Alert(const std::string& title, const std::string& message) {
    ESP_LOGW(TAG, "Alert: %s, %s", title.c_str(), message.c_str()); // 输出警告级别的日志
    auto display = Board::GetInstance().GetDisplay();
    display->ShowNotification(message); // 显示通知
    
    // 根据不同消息播放音频
    if (message == "PIN is not ready") {    // 当设备的 PIN（个人识别码）没有准备好时
        PlayLocalFile(p3_err_pin_start, p3_err_pin_end - p3_err_pin_start);
    } else if (message == "Configuring WiFi") {    // 当设备正在配置 WiFi 时
        PlayLocalFile(p3_err_wificonfig_start, p3_err_wificonfig_end - p3_err_wificonfig_start);
    } else if (message == "Registration denied") {    // 当设备注册被拒绝时
        PlayLocalFile(p3_err_reg_start, p3_err_reg_end - p3_err_reg_start);
    }
}

// 用于播放存储在本地的音频文件
// 该函数通过解析嵌入的二进制数据，提取音频数据并将其放入解码队列中
void Application::PlayLocalFile(const char* data, size_t size) {
    ESP_LOGI(TAG, "PlayLocalFile: %zu bytes", size); // 输出一条信息级别的日志，显示要播放的音频文件的大小
    SetDecodeSampleRate(16000); // 设置解码采样率 16 kHz
    // 遍历数据并解析音频包
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);
        
        // 提取有效负载（音频数据）
        auto payload_size = ntohs(p3->payload_size);
        std::vector<uint8_t> opus;
        opus.resize(payload_size);
        memcpy(opus.data(), p3->payload, payload_size);
        p += payload_size;

        // 将音频数据放入解码队列
        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(opus));
    }
}
// 用于切换设备的聊天状态
// 具体表现为根据设备当前的状态来启动或停止与远程设备的音频通信
void Application::ToggleChatState() {
    Schedule([this]() {
        // 检查 protocol_ 是否初始化
        if (!protocol_) {
            // 输出一条错误日志并退出函数
            ESP_LOGE(TAG, "Protocol not initialized");
            return;
        }

        // 检查设备当前是否处于空闲状态（没有进行任何通信）
        if (device_state_ == kDeviceStateIdle) {
            SetDeviceState(kDeviceStateConnecting); // 将设备状态设置为连接中
            // 尝试打开音频通道进行音频通信
            if (!protocol_->OpenAudioChannel()) {
                // 如果失败，发送错误提示并恢复设备状态为空闲状态
                Alert("Error", "Failed to open audio channel");
                SetDeviceState(kDeviceStateIdle);
                return;
            }

            // 设置监听状态，指示设备应该保持监听（用于等待语音输入）
            keep_listening_ = true;
            protocol_->SendStartListening(kListeningModeAutoStop); // 通知协议开始监听
            SetDeviceState(kDeviceStateListening); // 将设备状态设置为监听中
        } else if (device_state_ == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone); // 如果设备在说话中，则中止说话
        } else if (device_state_ == kDeviceStateListening) {
            protocol_->CloseAudioChannel(); // 如果设备在监听中，关闭音频通道
        }
    });
}

// 启动设备的监听功能，并根据当前设备的状态采取相应的操作
void Application::StartListening() {
    Schedule([this]() {
        if (!protocol_) {
            ESP_LOGE(TAG, "Protocol not initialized");
            return;
        }
        
        keep_listening_ = false;
        if (device_state_ == kDeviceStateIdle) {
            if (!protocol_->IsAudioChannelOpened()) { // 如果音频通道未打开
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) { // 尝试打开音频通道
                    SetDeviceState(kDeviceStateIdle);
                    Alert("Error", "Failed to open audio channel");
                    return;
                }
            }
            protocol_->SendStartListening(kListeningModeManualStop); // 通知协议进入监听模式
            SetDeviceState(kDeviceStateListening);
        } else if (device_state_ == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone); // 中止当前的说话操作
            protocol_->SendStartListening(kListeningModeManualStop); // 切换到监听模式
            // FIXME: Wait for the speaker to empty the buffer | 等待音频缓冲区清空
            vTaskDelay(pdMS_TO_TICKS(120));
            SetDeviceState(kDeviceStateListening);
        }
    });
}

// 用于停止设备的监听操作
void Application::StopListening() {
    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

// 用于启动应用，负责初始化系统的核心组件并启动主要功能
void Application::Start() {
    auto& board = Board::GetInstance(); // 获取 Board 单例实例
    SetDeviceState(kDeviceStateStarting); // 表示设备正在启动

    /* Setup the display */
    // 调用 GetDisplay 初始化显示屏接口
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec(); // 获取音频编解码器
    opus_decode_sample_rate_ = codec->output_sample_rate();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(opus_decode_sample_rate_, 1);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);

    // 配置采样率转换器
    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

    // 注册音频事件回调
    codec->OnInputReady([this, codec]() {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xEventGroupSetBitsFromISR(event_group_, AUDIO_INPUT_READY_EVENT, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE;
    });
    codec->OnOutputReady([this]() {
        BaseType_t higher_priority_task_woken = pdFALSE;
        xEventGroupSetBitsFromISR(event_group_, AUDIO_OUTPUT_READY_EVENT, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE;
    });

    codec->Start(); // 启动音频编解码器

    /* Start the main loop */
    // 启动主任务
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->MainLoop();
        vTaskDelete(NULL);
    }, "main_loop", 4096 * 2, this, 2, nullptr);

    /* Wait for the network to be ready */
    // 网络启动
    board.StartNetwork();

    // Check for new firmware version or get the MQTT broker address
    // 检查固件更新
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->CheckNewVersion();
        vTaskDelete(NULL);
    }, "check_new_version", 4096 * 2, this, 1, nullptr);

#if CONFIG_IDF_TARGET_ESP32S3   // 音频处理和唤醒词检测（仅针对 ESP32-S3）
    // 音频处理初始化
    audio_processor_.Initialize(codec->input_channels(), codec->input_reference());
    audio_processor_.OnOutput([this](std::vector<int16_t>&& data) {
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                Schedule([this, opus = std::move(opus)]() {
                    protocol_->SendAudio(opus);
                });
            });
        });
    });

    wake_word_detect_.Initialize(codec->input_channels(), codec->input_reference());
    wake_word_detect_.OnVadStateChange([this](bool speaking) {
        Schedule([this, speaking]() {
            if (device_state_ == kDeviceStateListening) {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        });
    });

    wake_word_detect_.OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            if (device_state_ == kDeviceStateIdle) {
                SetDeviceState(kDeviceStateConnecting);
                wake_word_detect_.EncodeWakeWordData();

                if (!protocol_->OpenAudioChannel()) {
                    ESP_LOGE(TAG, "Failed to open audio channel");
                    SetDeviceState(kDeviceStateIdle);
                    wake_word_detect_.StartDetection();
                    return;
                }
                
                std::vector<uint8_t> opus;
                // Encode and send the wake word data to the server
                while (wake_word_detect_.GetWakeWordOpus(opus)) {
                    protocol_->SendAudio(opus);
                }
                // Set the chat state to wake word detected
                protocol_->SendWakeWordDetected(wake_word);
                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
                keep_listening_ = true;
                SetDeviceState(kDeviceStateListening);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            }

            // Resume detection
            wake_word_detect_.StartDetection();
        });
    });
    wake_word_detect_.StartDetection();
#endif

    // Initialize the protocol
    display->SetStatus("初始化协议");

// 根据编译选项选择使用 WebSocket 或 MQTT 作为通信协议
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    protocol_ = std::make_unique<WebsocketProtocol>();
#else
    protocol_ = std::make_unique<MqttProtocol>();
#endif

    // 协议事件注册
    protocol_->OnNetworkError([this](const std::string& message) {
        Alert("Error", std::move(message));
    });
    protocol_->OnIncomingAudio([this](std::vector<uint8_t>&& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_state_ == kDeviceStateSpeaking) {
            audio_decode_queue_.emplace_back(std::move(data));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "服务器的音频采样率 %d 与设备输出的采样率 %d 不一致，重采样后可能会失真",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        SetDecodeSampleRate(protocol_->server_sample_rate());
        // 物联网设备描述符
        last_iot_states_.clear();
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        background_task_->WaitForCompletion();
                        if (keep_listening_) {
                            protocol_->SendStartListening(kListeningModeAutoStop);
                            SetDeviceState(kDeviceStateListening);
                        } else {
                            SetDeviceState(kDeviceStateIdle);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (text != NULL) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    display->SetChatMessage("assistant", text->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (text != NULL) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                display->SetChatMessage("user", text->valuestring);
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (emotion != NULL) {
                display->SetEmotion(emotion->valuestring);
            }
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (commands != NULL) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
        }
    });

    SetDeviceState(kDeviceStateIdle);
}

void Application::Schedule(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    main_tasks_.push_back(std::move(callback));
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_,
            SCHEDULE_EVENT | AUDIO_INPUT_READY_EVENT | AUDIO_OUTPUT_READY_EVENT,
            pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & AUDIO_INPUT_READY_EVENT) {
            InputAudio();
        }
        if (bits & AUDIO_OUTPUT_READY_EVENT) {
            OutputAudio();
        }
        if (bits & SCHEDULE_EVENT) {
            mutex_.lock();
            std::list<std::function<void()>> tasks = std::move(main_tasks_);
            mutex_.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    last_output_time_ = std::chrono::steady_clock::now();
    Board::GetInstance().GetAudioCodec()->EnableOutput(true);
}

void Application::OutputAudio() {
    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    if (device_state_ == kDeviceStateListening) {
        audio_decode_queue_.clear();
        return;
    }

    last_output_time_ = now;
    auto opus = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();

    background_task_->Schedule([this, codec, opus = std::move(opus)]() mutable {
        if (aborted_) {
            return;
        }

        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(opus), pcm)) {
            return;
        }

        // Resample if the sample rate is different
        if (opus_decode_sample_rate_ != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }
        
        codec->OutputData(pcm);
    });
}

void Application::InputAudio() {
    auto codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> data;
    if (!codec->InputData(data)) {
        return;
    }

    if (codec->input_sample_rate() != 16000) {
        if (codec->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    }
    
#if CONFIG_IDF_TARGET_ESP32S3
    if (audio_processor_.IsRunning()) {
        audio_processor_.Input(data);
    }
    if (wake_word_detect_.IsDetectionRunning()) {
        wake_word_detect_.Feed(data);
    }
#else
    if (device_state_ == kDeviceStateListening) {
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                Schedule([this, opus = std::move(opus)]() {
                    protocol_->SendAudio(opus);
                });
            });
        });
    }
#endif
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto display = Board::GetInstance().GetDisplay();
    auto led = Board::GetInstance().GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus("待命");
            display->SetEmotion("neutral");
#ifdef CONFIG_IDF_TARGET_ESP32S3
            audio_processor_.Stop();
#endif
            break;
        case kDeviceStateConnecting:
            display->SetStatus("连接中...");
            break;
        case kDeviceStateListening:
            display->SetStatus("聆听中...");
            display->SetEmotion("neutral");
            ResetDecoder();
            opus_encoder_->ResetState();
#if CONFIG_IDF_TARGET_ESP32S3
            audio_processor_.Start();
#endif
            UpdateIotStates();
            break;
        case kDeviceStateSpeaking:
            display->SetStatus("说话中...");
            ResetDecoder();
#if CONFIG_IDF_TARGET_ESP32S3
            audio_processor_.Stop();
#endif
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::SetDecodeSampleRate(int sample_rate) {
    if (opus_decode_sample_rate_ == sample_rate) {
        return;
    }

    opus_decode_sample_rate_ = sample_rate;
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(opus_decode_sample_rate_, 1);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decode_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decode_sample_rate_, codec->output_sample_rate());
        output_resampler_.Configure(opus_decode_sample_rate_, codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
    auto& thing_manager = iot::ThingManager::GetInstance();
    auto states = thing_manager.GetStatesJson();
    if (states != last_iot_states_) {
        last_iot_states_ = states;
        protocol_->SendIotStates(states);
    }
}
