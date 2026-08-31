#include "agora_rtc_protocol.h"
#include "application.h"
#include "board.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "AgoraRTC"

// Global instance pointer for static callback routing
static AgoraRtcProtocol* g_instance = nullptr;

AgoraRtcProtocol::AgoraRtcProtocol() {
    event_group_handle_ = xEventGroupCreate();
    g_instance = this;

    // Allocate lock-free ring buffer in PSRAM for downlink AEC reference
    ref_ring_buffer_ = std::make_unique<LockFreeRingBuffer>(kRefBufferMaxSamples);
    if (!ref_ring_buffer_->IsValid()) {
        ESP_LOGE(TAG, "Failed to allocate ref ring buffer");
    }
}

AgoraRtcProtocol::~AgoraRtcProtocol() {
    CloseAudioChannel(false);
    FiniSdk();
    vEventGroupDelete(event_group_handle_);
    g_instance = nullptr;
}

bool AgoraRtcProtocol::Start() { return true; }

bool AgoraRtcProtocol::InitSdk(const std::string& app_id) {
    if (sdk_initialized_) {
        return true;
    }

    agora_rtc_event_handler_t handler = {};
    handler.on_join_channel_success = OnJoinChannelSuccess;
    handler.on_error = OnError;
    handler.on_user_joined_with_user_account = OnUserJoinedWithUserAccount;
    handler.on_user_offline_with_user_account = OnUserOfflineWithUserAccount;
    handler.on_audio_data = OnAudioData;
    handler.on_user_mute_audio = OnUserMuteAudio;
    handler.on_connection_lost = OnConnectionLost;
    handler.on_reconnecting = OnReconnecting;
    handler.on_rejoin_channel_success = OnRejoinChannelSuccess;

    rtc_service_option_t option = {};
    option.area_code = AREA_CODE_GLOB;
    option.log_cfg.log_level = RTC_LOG_WARNING;
    option.log_cfg.log_disable = false;
    option.use_string_uid = true;

    int ret = agora_rtc_init(app_id.c_str(), &handler, &option);
    if (ret != ERR_OKAY) {
        ESP_LOGE(TAG, "agora_rtc_init failed: %d (%s)", ret, agora_rtc_err_2_str(ret));
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    sdk_initialized_ = true;
    ESP_LOGI(TAG, "Agora RTC SDK initialized, version: %s", agora_rtc_get_version());
    return true;
}

void AgoraRtcProtocol::FiniSdk() {
    if (!sdk_initialized_) {
        return;
    }
    CleanupRtm();
    agora_rtc_fini();
    sdk_initialized_ = false;
    sdk_restart_required_ = false;
    ESP_LOGI(TAG, "Agora RTC SDK finalized");
}

void AgoraRtcProtocol::CleanupRtm() {
    std::string channel;
    bool subscribe_requested;
    bool login_requested;
    {
        std::lock_guard<std::mutex> lock(rtm_mutex_);
        channel = rtm_channel_;
        subscribe_requested = rtm_subscribe_requested_.exchange(false);
        rtm_subscribed_ = false;
        login_requested = rtm_login_requested_.exchange(false);
        rtm_logged_in_ = false;
        local_rtm_uid_.clear();
        rtm_channel_.clear();
        remote_rtm_uid_.clear();
        rtm_session_id_.clear();
    }

    if (subscribe_requested && !channel.empty()) {
        int ret = agora_rtm_unsubscribe(channel.c_str());
        if (ret != ERR_OKAY) {
            sdk_restart_required_ = true;
            ESP_LOGW(TAG, "RTM unsubscribe failed: channel=%s, ret=%d (%s)", channel.c_str(), ret,
                     agora_rtc_err_2_str(ret));
        } else {
            ESP_LOGI(TAG, "RTM unsubscribed: channel=%s", channel.c_str());
        }
    }

    if (login_requested) {
        int ret = agora_rtm_logout();
        if (ret != ERR_OKAY) {
            sdk_restart_required_ = true;
            ESP_LOGW(TAG, "RTM logout failed: %d (%s)", ret, agora_rtc_err_2_str(ret));
        } else {
            ESP_LOGI(TAG, "RTM logged out");
        }
    }

    xEventGroupClearBits(event_group_handle_, AGORA_RTM_LOGIN_EVENT | AGORA_RTM_SUBSCRIBE_EVENT);
}

void AgoraRtcProtocol::StopCurrentConversation() {
    if (!current_conversation_.conversation_id.empty()) {
        device_api_.StopConversation(current_conversation_.conversation_id, "device_hangup");
        current_conversation_ = ConversationInfo{};
    }
    session_id_.clear();
}

bool AgoraRtcProtocol::RunPairingFlow() { return device_api_.HasDeviceToken(); }

bool AgoraRtcProtocol::OpenAudioChannel() {
    if (IsAudioChannelOpened()) {
        ESP_LOGW(TAG, "Already in channel, skip OpenAudioChannel");
        return true;
    }

    connection_id_t stale_conn_id = conn_id_.exchange(CONNECTION_ID_INVALID);
    if (stale_conn_id != CONNECTION_ID_INVALID) {
        ESP_LOGW(TAG, "Cleaning up stale RTC connection before opening a new channel");
        agora_rtc_leave_channel(stale_conn_id);
        agora_rtc_destroy_connection(stale_conn_id);
    }
    joined_ = false;
    CleanupRtm();
    StopCurrentConversation();
    if (sdk_restart_required_.exchange(false) && sdk_initialized_) {
        ESP_LOGW(TAG, "Restarting RTSA SDK after an incomplete RTM cleanup");
        agora_rtc_fini();
        sdk_initialized_ = false;
    }

    error_occurred_ = false;
    xEventGroupClearBits(event_group_handle_,
                         AGORA_JOINED_EVENT | AGORA_RTM_LOGIN_EVENT | AGORA_RTM_SUBSCRIBE_EVENT);

    // Step 1: Call Device API to start conversation and get RTC params
    ESP_LOGI(TAG, "Starting conversation via Device API...");
    ConversationInfo info;
    if (!device_api_.StartConversation(info)) {
        auto err = device_api_.GetLastError();
        ESP_LOGE(TAG, "StartConversation failed, error=%d", (int)err);

        if (err == DeviceApiError::kServerError) {
            ESP_LOGW(TAG, "Server error, retrying in 2s...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (!device_api_.StartConversation(info)) {
                err = device_api_.GetLastError();
                ESP_LOGE(TAG, "StartConversation retry failed, error=%d", (int)err);
                SetError(Lang::Strings::SERVER_NOT_CONNECTED);
                return false;
            }
        } else if (err == DeviceApiError::kUnauthenticated ||
                   err == DeviceApiError::kTokenRevoked || err == DeviceApiError::kNotBound) {
            SetError("设备未绑定，请重新配对");
            return false;
        } else {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
            return false;
        }
    }

    if (info.conversation_id.empty() || info.rtc.app_id.empty() || info.rtc.channel.empty() ||
        info.rtc.uid.empty() || info.rtc.agent_uid.empty()) {
        ESP_LOGE(TAG, "Conversation response is missing required RTC or RTM fields");
        if (!info.conversation_id.empty()) {
            device_api_.StopConversation(info.conversation_id, "device_hangup");
        }
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    current_conversation_ = info;
    session_id_ = info.conversation_id;
    ESP_LOGI(TAG, "Conversation started: id=%s, channel=%s, uid=%s, agent_uid=%s",
             info.conversation_id.c_str(), info.rtc.channel.c_str(), info.rtc.uid.c_str(),
             info.rtc.agent_uid.c_str());
    // Step 2: Initialize SDK with the app_id from server
    if (!InitSdk(info.rtc.app_id)) {
        StopCurrentConversation();
        return false;
    }
    // Step 3: Login RTM using local_uid as RTM UID
    std::string rtm_uid = info.rtc.uid;
    {
        std::lock_guard<std::mutex> lock(rtm_mutex_);
        local_rtm_uid_ = rtm_uid;
        remote_rtm_uid_ = info.rtc.agent_uid;
        rtm_channel_ = info.rtc.channel;
        rtm_session_id_ = info.conversation_id;
    }

    agora_rtm_handler_t rtm_handler = {};
    rtm_handler.on_rtm_event = OnRtmEvent;
    rtm_handler.on_rtm_data = OnRtmData;
    rtm_handler.on_rtm_send_data_result = OnRtmSendDataResult;
    rtm_handler.on_rtm_subscribe_result = OnRtmSubscribeResult;
#if CONFIG_CONNECTION_TYPE_AGORA_RTC
    rtm_handler.on_rtm_subscribe_data = OnRtmSubscribeData;
#endif

    ESP_LOGI(TAG, "Logging in RTM, uid: %s", rtm_uid.c_str());
    rtm_login_requested_ = true;
    int ret = agora_rtm_login(
        rtm_uid.c_str(), info.rtc.token.empty() ? nullptr : info.rtc.token.c_str(), &rtm_handler);
    if (ret != ERR_OKAY) {
        ESP_LOGE(TAG, "agora_rtm_login failed: %d (%s)", ret, agora_rtc_err_2_str(ret));
        rtm_login_requested_ = false;
        CleanupRtm();
        StopCurrentConversation();
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Wait for RTM login event
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, AGORA_RTM_LOGIN_EVENT, pdTRUE,
                                           pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & AGORA_RTM_LOGIN_EVENT) || !rtm_logged_in_) {
        ESP_LOGE(TAG, "RTM login %s", (bits & AGORA_RTM_LOGIN_EVENT) ? "failed" : "timeout");
        CleanupRtm();
        StopCurrentConversation();
        SetError((bits & AGORA_RTM_LOGIN_EVENT) ? Lang::Strings::SERVER_NOT_CONNECTED
                                                : Lang::Strings::SERVER_TIMEOUT);
        return false;
    }
    ESP_LOGI(TAG, "RTM login success");

    // Step 4: Subscribe to the RTM channel and wait for confirmation.
    ESP_LOGI(TAG, "Subscribing RTM channel: %s", info.rtc.channel.c_str());
    rtm_subscribe_requested_ = true;
    ret = agora_rtm_subscribe(info.rtc.channel.c_str());
    if (ret != ERR_OKAY) {
        ESP_LOGE(TAG, "agora_rtm_subscribe failed: %d (%s)", ret, agora_rtc_err_2_str(ret));
        rtm_subscribe_requested_ = false;
        CleanupRtm();
        StopCurrentConversation();
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    bits = xEventGroupWaitBits(event_group_handle_, AGORA_RTM_SUBSCRIBE_EVENT, pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(10000));
    if (!(bits & AGORA_RTM_SUBSCRIBE_EVENT) || !rtm_logged_in_ || !rtm_subscribed_) {
        ESP_LOGE(TAG, "RTM channel subscription %s",
                 (bits & AGORA_RTM_SUBSCRIBE_EVENT) ? "failed" : "timeout");
        CleanupRtm();
        StopCurrentConversation();
        SetError((bits & AGORA_RTM_SUBSCRIBE_EVENT) ? Lang::Strings::SERVER_NOT_CONNECTED
                                                    : Lang::Strings::SERVER_TIMEOUT);
        return false;
    }
    ESP_LOGI(TAG, "RTM channel subscription success: %s", info.rtc.channel.c_str());

    // Step 5: Create connection and join the RTC channel.
    connection_id_t conn_id = CONNECTION_ID_INVALID;
    ret = agora_rtc_create_connection(&conn_id);
    if (ret != ERR_OKAY) {
        ESP_LOGE(TAG, "agora_rtc_create_connection failed: %d (%s)", ret, agora_rtc_err_2_str(ret));
        CleanupRtm();
        StopCurrentConversation();
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    conn_id_ = conn_id;
    rtc_channel_options_t options = {};
    options.auto_subscribe_audio = true;
    options.auto_subscribe_video = false;
    options.enable_audio_jitter_buffer = AGORA_JITTER_BUFFER;
    options.enable_audio_mixer = false;
    options.enable_audio_decode = true;
    options.enable_audio_ai_qos = AGORA_AI_QOS;
    options.enable_audio_downlink_aec = AGORA_CLOUD_AEC;

    options.audio_codec_opt.audio_codec_type = AUDIO_CODEC_TYPE_G722;
    options.audio_codec_opt.pcm_sample_rate = kPcmSampleRate;
    options.audio_codec_opt.pcm_channel_num = 1;
    options.audio_codec_opt.pcm_duration = kPcmFrameDurationMs;

    ESP_LOGI(TAG,
             "Joining channel: %s, uid: %s, ai_qos: %d, jitter: %d, cloud_aec: %d, token: %.8s...",
             info.rtc.channel.c_str(), info.rtc.uid.c_str(), AGORA_AI_QOS,
             options.enable_audio_jitter_buffer, options.enable_audio_downlink_aec,
             info.rtc.token.empty() ? "none" : info.rtc.token.c_str());

    ret = agora_rtc_join_channel_with_user_account(
        conn_id, info.rtc.channel.c_str(), info.rtc.uid.c_str(),
        info.rtc.token.empty() ? nullptr : info.rtc.token.c_str(), &options);
    if (ret != ERR_OKAY) {
        ESP_LOGE(TAG, "agora_rtc_join_channel failed: %d (%s)", ret, agora_rtc_err_2_str(ret));
        conn_id_.exchange(CONNECTION_ID_INVALID);
        agora_rtc_destroy_connection(conn_id);
        CleanupRtm();
        StopCurrentConversation();
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // Wait for join success
    bits = xEventGroupWaitBits(event_group_handle_, AGORA_JOINED_EVENT, pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(10000));
    if (!(bits & AGORA_JOINED_EVENT) || !joined_ || !rtm_logged_in_ || !rtm_subscribed_ ||
        error_occurred_) {
        ESP_LOGE(TAG, "RTC join failed or the RTM session ended while joining");
        conn_id_.exchange(CONNECTION_ID_INVALID);
        agora_rtc_leave_channel(conn_id);
        agora_rtc_destroy_connection(conn_id);
        CleanupRtm();
        StopCurrentConversation();
        if (!error_occurred_) {
            SetError((bits & AGORA_JOINED_EVENT) ? Lang::Strings::SERVER_NOT_CONNECTED
                                                 : Lang::Strings::SERVER_TIMEOUT);
        }
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

void AgoraRtcProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;

    connection_id_t conn_id = conn_id_.exchange(CONNECTION_ID_INVALID);
    if (conn_id != CONNECTION_ID_INVALID) {
        agora_rtc_leave_channel(conn_id);
        agora_rtc_destroy_connection(conn_id);
        ESP_LOGI(TAG, "Left channel and destroyed connection");
    }
    joined_ = false;
    CleanupRtm();

    // Clear ref ring buffer
    if (ref_ring_buffer_) {
        ref_ring_buffer_->Reset();
    }

    // Stop conversation via Device API
    StopCurrentConversation();

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool AgoraRtcProtocol::IsAudioChannelOpened() const {
    return joined_ && rtm_logged_in_ && rtm_subscribed_ && !error_occurred_ && !IsTimeout();
}

bool AgoraRtcProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    connection_id_t conn_id = conn_id_.load();
    if (!joined_ || conn_id == CONNECTION_ID_INVALID) {
        return false;
    }

    if (packet->payload.size() != kPcmBytesPerFrame) {
        return true;
    }

    audio_frame_info_t info = {};
    info.data_type = AUDIO_DATA_TYPE_PCM;

    int ret;

#if AGORA_CLOUD_AEC
    const int16_t* mic_data = (const int16_t*)packet->payload.data();
    size_t mic_samples = packet->payload.size() / sizeof(int16_t);

    std::vector<int16_t> ref_data(mic_samples, 0);
    if (ref_ring_buffer_) {
        ref_ring_buffer_->Read(ref_data.data(), mic_samples);
    }

    std::vector<int16_t> interleaved(mic_samples * 2);
    for (size_t i = 0; i < mic_samples; i++) {
        interleaved[i * 2] = mic_data[i];
        interleaved[i * 2 + 1] = ref_data[i];
    }

    ret = agora_rtc_send_audio_data(conn_id, interleaved.data(),
                                    interleaved.size() * sizeof(int16_t), &info);
#else
    ret = agora_rtc_send_audio_data(conn_id, packet->payload.data(), packet->payload.size(), &info);
#endif
    if (ret != ERR_OKAY) {
        ESP_LOGW(TAG, "send_audio_data failed: %d", ret);
        return false;
    }
    return true;
}

bool AgoraRtcProtocol::SendText(const std::string& text) {
    std::string remote_rtm_uid;
    {
        std::lock_guard<std::mutex> lock(rtm_mutex_);
        remote_rtm_uid = remote_rtm_uid_;
    }
    if (!rtm_logged_in_ || remote_rtm_uid.empty()) {
        ESP_LOGW(TAG, "RTM not available, cannot send text");
        return false;
    }

    int ret = agora_rtm_send_data(remote_rtm_uid.c_str(), text.c_str(), text.size(), ++rtm_msg_id_,
                                  RTM_MESSAGE_TYPE_BINARY, nullptr);
    if (ret != ERR_OKAY) {
        ESP_LOGW(TAG, "send_rtm_data failed: %d (%s)", ret, agora_rtc_err_2_str(ret));
        return false;
    }
    return true;
}

// ==================== RTC Static Callbacks ====================

bool AgoraRtcProtocol::IsCurrentConnection(connection_id_t conn_id) {
    return g_instance && conn_id != CONNECTION_ID_INVALID && g_instance->conn_id_.load() == conn_id;
}

void AgoraRtcProtocol::OnJoinChannelSuccess(connection_id_t conn_id, uint32_t uid, int elapsed_ms) {
    ESP_LOGI(TAG, "Join channel success, uid: %lu, elapsed: %d ms", (unsigned long)uid, elapsed_ms);
    if (IsCurrentConnection(conn_id)) {
        g_instance->joined_ = true;
        g_instance->last_incoming_time_ = std::chrono::steady_clock::now();
        xEventGroupSetBits(g_instance->event_group_handle_, AGORA_JOINED_EVENT);
    }
}

void AgoraRtcProtocol::OnError(connection_id_t conn_id, int code, const char* msg) {
    ESP_LOGE(TAG, "Error %d: %s", code, msg ? msg : "unknown");
    if (g_instance && (conn_id == CONNECTION_ID_INVALID || IsCurrentConnection(conn_id))) {
        g_instance->SetError(msg ? msg : "Agora RTC error");
    }
}

void AgoraRtcProtocol::OnUserJoinedWithUserAccount(connection_id_t conn_id, const user_info_t* user,
                                                   int elapsed_ms) {
    if (!IsCurrentConnection(conn_id)) {
        return;
    }
    ESP_LOGI(TAG, "Remote user joined, account: %s, uid: %lu", user->user_account,
             (unsigned long)user->uid);
}

void AgoraRtcProtocol::OnUserOfflineWithUserAccount(connection_id_t conn_id,
                                                    const user_info_t* user, int reason) {
    if (!IsCurrentConnection(conn_id)) {
        return;
    }
    ESP_LOGI(TAG, "Remote user offline, account: %s, reason: %d", user->user_account, reason);
}

void AgoraRtcProtocol::OnAudioData(connection_id_t conn_id, uint32_t uid, uint16_t sent_ts,
                                   const void* data_ptr, size_t data_len,
                                   const audio_frame_info_t* info_ptr) {
    if (!IsCurrentConnection(conn_id) || !data_ptr || data_len == 0) {
        return;
    }
    g_instance->last_incoming_time_ = std::chrono::steady_clock::now();

    constexpr size_t kBytesPerMillisecond = kPcmSampleRate / 1000 * sizeof(int16_t);
    if (data_len % kBytesPerMillisecond != 0) {
        ESP_LOGW(TAG, "RecvAudio: invalid PCM frame size=%d, skipping", (int)data_len);
        return;
    }
    int frame_duration_ms = data_len / kBytesPerMillisecond;
    if (AGORA_JITTER_BUFFER && frame_duration_ms != kPcmFrameDurationMs) {
        ESP_LOGW(TAG, "RecvAudio: expected %dms jitter frame, got %dms, skipping",
                 kPcmFrameDurationMs, frame_duration_ms);
        return;
    }

    // Store downlink PCM into ref ring buffer for downlink AEC (lock-free write)
    if (g_instance->ref_ring_buffer_) {
        g_instance->ref_ring_buffer_->Write(static_cast<const int16_t*>(data_ptr),
                                            data_len / sizeof(int16_t));
    }

    // Push PCM frame directly to AudioService decode queue
    if (g_instance->on_incoming_audio_) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = kPcmSampleRate;
        packet->frame_duration = frame_duration_ms;
        packet->timestamp = sent_ts;
        packet->payload.assign((uint8_t*)data_ptr, (uint8_t*)data_ptr + data_len);
        g_instance->on_incoming_audio_(std::move(packet));
    }
}

void AgoraRtcProtocol::OnUserMuteAudio(connection_id_t conn_id, uint32_t uid, bool muted) {
    if (!IsCurrentConnection(conn_id)) {
        return;
    }
    ESP_LOGI(TAG, "UserMuteAudio: uid=%lu, muted=%d", (unsigned long)uid, (int)muted);
}

void AgoraRtcProtocol::OnConnectionLost(connection_id_t conn_id) {
    if (!IsCurrentConnection(conn_id)) {
        return;
    }
    ESP_LOGW(TAG, "Connection lost");
    if (g_instance) {
        g_instance->SetError("Connection lost");
    }
}

void AgoraRtcProtocol::OnReconnecting(connection_id_t conn_id) {
    if (IsCurrentConnection(conn_id)) {
        ESP_LOGW(TAG, "Reconnecting...");
    }
}

void AgoraRtcProtocol::OnRejoinChannelSuccess(connection_id_t conn_id, uint32_t uid,
                                              int elapsed_ms) {
    ESP_LOGI(TAG, "Rejoin channel success, uid: %lu", (unsigned long)uid);
    if (IsCurrentConnection(conn_id)) {
        g_instance->joined_ = true;
        g_instance->last_incoming_time_ = std::chrono::steady_clock::now();
    }
}

// ==================== RTM Static Callbacks ====================

void AgoraRtcProtocol::OnRtmEvent(const char* rtm_uid, rtm_event_type_e event_type,
                                  rtm_err_code_e err_code) {
    const char* event_str = "unknown";
    switch (event_type) {
        case RTM_EVENT_TYPE_LOGIN:
            event_str = "LOGIN";
            break;
        case RTM_EVENT_TYPE_KICKOFF:
            event_str = "KICKOFF";
            break;
        case RTM_EVENT_TYPE_EXIT:
            event_str = "EXIT";
            break;
        default:
            break;
    }
    const char* err_str = "unknown";
    switch (err_code) {
        case ERR_RTM_OK:
            err_str = "OK";
            break;
        case ERR_RTM_FAILED:
            err_str = "FAILED";
            break;
        case ERR_RTM_LOGIN_REJECTED:
            err_str = "LOGIN_REJECTED";
            break;
        case ERR_RTM_INVALID_RTM_UID:
            err_str = "INVALID_RTM_UID";
            break;
        case ERR_RTM_LOGIN_INVALID_TOKEN:
            err_str = "INVALID_TOKEN";
            break;
        case ERR_RTM_LOGIN_NOT_AUTHORIZED:
            err_str = "NOT_AUTHORIZED";
            break;
        case ERR_RTM_LOCAL_NETDOWN:
            err_str = "LOCAL_NETDOWN";
            break;
        case ERR_RTM_LOCAL_INTERRUPT:
            err_str = "LOCAL_INTERRUPT";
            break;
        case ERR_RTM_LOCAL_TIMEOUT:
            err_str = "LOCAL_TIMEOUT";
            break;
        case ERR_RTM_SERVER_TIMEOUT:
            err_str = "SERVER_TIMEOUT";
            break;
        default:
            break;
    }
    ESP_LOGI(TAG, "RTM event: uid=%s, type=%s(%d), err=%s(%d)", rtm_uid ? rtm_uid : "null",
             event_str, event_type, err_str, err_code);

    if (!g_instance) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_instance->rtm_mutex_);
        if (!g_instance->rtm_login_requested_ || !rtm_uid ||
            g_instance->local_rtm_uid_ != rtm_uid) {
            ESP_LOGW(TAG, "Ignoring stale RTM event");
            return;
        }

        if (event_type == RTM_EVENT_TYPE_LOGIN) {
            g_instance->rtm_logged_in_ = err_code == ERR_RTM_OK;
            if (err_code != ERR_RTM_OK) {
                ESP_LOGE(TAG, "RTM login failed: %s(%d)", err_str, err_code);
            }
            xEventGroupSetBits(g_instance->event_group_handle_, AGORA_RTM_LOGIN_EVENT);
        } else if (event_type == RTM_EVENT_TYPE_KICKOFF || event_type == RTM_EVENT_TYPE_EXIT) {
            g_instance->rtm_logged_in_ = false;
            g_instance->rtm_subscribed_ = false;
            xEventGroupSetBits(g_instance->event_group_handle_, AGORA_RTM_LOGIN_EVENT);
            if (g_instance->rtm_subscribe_requested_) {
                xEventGroupSetBits(g_instance->event_group_handle_, AGORA_RTM_SUBSCRIBE_EVENT);
            }
            if (g_instance->conn_id_.load() != CONNECTION_ID_INVALID) {
                g_instance->joined_ = false;
                xEventGroupSetBits(g_instance->event_group_handle_, AGORA_JOINED_EVENT);
            }
            ESP_LOGW(TAG, "RTM session ended: %s", event_str);
            g_instance->SetError(event_type == RTM_EVENT_TYPE_KICKOFF ? "RTM kicked off"
                                                                      : "RTM exited");
        }
    }
}

void AgoraRtcProtocol::OnRtmData(const char* rtm_uid, const void* msg, size_t msg_len,
                                 rtm_message_type_e msg_type, const char* custom_type) {
    if (!g_instance || !rtm_uid || !msg || msg_len == 0 || msg_len > kMaxP2pMessageBytes ||
        std::memchr(msg, '\0', msg_len) != nullptr ||
        (msg_type != RTM_MESSAGE_TYPE_BINARY && msg_type != RTM_MESSAGE_TYPE_STRING)) {
        return;
    }

    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(g_instance->rtm_mutex_);
        if (!g_instance->rtm_logged_in_ || g_instance->remote_rtm_uid_ != rtm_uid) {
            ESP_LOGW(TAG, "Ignoring P2P RTM data from unexpected uid: %s", rtm_uid);
            return;
        }
        session_id = g_instance->rtm_session_id_;
    }

    // Print message payload as string (truncated to avoid log flooding)
    std::string payload_str;
    size_t print_len = (msg_len > 512) ? 512 : msg_len;
    payload_str.assign((const char*)msg, print_len);
    if (msg_len > 512) {
        payload_str += "... (" + std::to_string(msg_len) + " bytes total)";
    }
    ESP_LOGI(TAG, "RTM data: from=%s, msg_type=%d, custom_type=%s, len=%zu, payload=[%s]",
             rtm_uid ? rtm_uid : "null", msg_type, custom_type ? custom_type : "null", msg_len,
             payload_str.c_str());

    g_instance->last_incoming_time_ = std::chrono::steady_clock::now();

    if (g_instance->on_incoming_json_) {
        auto root = cJSON_ParseWithLength((const char*)msg, msg_len);
        if (root) {
            {
                std::lock_guard<std::mutex> lock(g_instance->rtm_mutex_);
                if (!g_instance->rtm_logged_in_ || g_instance->rtm_session_id_ != session_id ||
                    g_instance->remote_rtm_uid_ != rtm_uid) {
                    ESP_LOGW(TAG, "Ignoring stale P2P RTM data");
                    cJSON_Delete(root);
                    return;
                }
            }
            g_instance->on_incoming_json_(root);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "RTM data is not valid JSON, len=%zu", msg_len);
        }
    }
}

void AgoraRtcProtocol::OnRtmSendDataResult(const char* rtm_uid, uint32_t msg_id,
                                           rtm_msg_state_e state) {
    if (state != RTM_MSG_STATE_RECEIVED) {
        ESP_LOGW(TAG, "RTM msg_id=%lu state=%d (not received)", (unsigned long)msg_id, state);
    }
}

void AgoraRtcProtocol::OnRtmSubscribeResult(const char* channel_name, rtm_err_code_e err_code) {
    if (!g_instance || !channel_name) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_instance->rtm_mutex_);
        if (!g_instance->rtm_login_requested_ || !g_instance->rtm_logged_in_ ||
            !g_instance->rtm_subscribe_requested_ || g_instance->rtm_channel_ != channel_name) {
            ESP_LOGW(TAG, "Ignoring RTM subscribe result for stale channel: %s", channel_name);
            return;
        }
        g_instance->rtm_subscribed_ = err_code == ERR_RTM_OK;
        ESP_LOGI(TAG, "RTM subscribe result: channel=%s, err=%d", channel_name, err_code);
        xEventGroupSetBits(g_instance->event_group_handle_, AGORA_RTM_SUBSCRIBE_EVENT);
    }
}

#if CONFIG_CONNECTION_TYPE_AGORA_RTC
void AgoraRtcProtocol::OnRtmSubscribeData(const char* channel_name, const char* rtm_uid,
                                          const void* msg, size_t msg_len,
                                          rtm_message_type_e msg_type, const char* custom_type) {
    if (!g_instance || !g_instance->rtm_subscribed_ || !channel_name || !rtm_uid || !msg ||
        msg_len == 0 || msg_len > kMaxVoiceprintMessageBytes ||
        std::memchr(msg, '\0', msg_len) != nullptr) {
        return;
    }

    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(g_instance->rtm_mutex_);
        if (!g_instance->rtm_login_requested_ || !g_instance->rtm_logged_in_ ||
            !g_instance->rtm_subscribe_requested_ || !g_instance->rtm_subscribed_ ||
            g_instance->rtm_channel_ != channel_name || g_instance->remote_rtm_uid_ != rtm_uid) {
            ESP_LOGW(TAG, "Ignoring RTM channel data from unexpected source: channel=%s, uid=%s",
                     channel_name, rtm_uid);
            return;
        }
        session_id = g_instance->rtm_session_id_;
    }

    auto root = cJSON_ParseWithLength(static_cast<const char*>(msg), msg_len);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Ignoring invalid RTM channel JSON: len=%zu", msg_len);
        cJSON_Delete(root);
        return;
    }

    auto object = cJSON_GetObjectItemCaseSensitive(root, "object");
    auto status = cJSON_GetObjectItemCaseSensitive(root, "status");
    bool voiceprint_registered = cJSON_IsString(object) && cJSON_IsString(status) &&
                                 std::strcmp(object->valuestring, "message.sal_status") == 0 &&
                                 std::strcmp(status->valuestring, "VP_REGISTER_SUCCESS") == 0;
    cJSON_Delete(root);

    if (!voiceprint_registered) {
        ESP_LOGD(TAG,
                 "Ignoring unrelated RTM channel data: channel=%s, msg_type=%d, custom_type=%s",
                 channel_name, msg_type, custom_type ? custom_type : "null");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_instance->rtm_mutex_);
        if (!g_instance->rtm_logged_in_ || !g_instance->rtm_subscribed_ ||
            g_instance->rtm_session_id_ != session_id || g_instance->rtm_channel_ != channel_name ||
            g_instance->remote_rtm_uid_ != rtm_uid) {
            ESP_LOGW(TAG, "Ignoring stale voiceprint registration event");
            return;
        }
    }

    ESP_LOGI(TAG, "Voiceprint registration succeeded: channel=%s, uid=%s", channel_name, rtm_uid);
    g_instance->last_incoming_time_ = std::chrono::steady_clock::now();
    if (g_instance->on_voiceprint_registered_) {
        g_instance->on_voiceprint_registered_();
    }
}
#endif
