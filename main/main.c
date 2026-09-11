#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "driver/i2s_std.h"
#include "esp_spiffs.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "cJSON.h"
#include "esp_sntp.h"
#include "freertos/task.h"
#include <inttypes.h>
#include "freertos/queue.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_crc.h"
#include "freertos/semphr.h"


// ===== ESP‑SR AFE‑SR 新版头文件（替换旧版wakenet）=====
#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include  "esp_afe_sr_models.h"

// wifi
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_wifi_types.h"


// I2S硬件引脚 INMP441(录音) + MAX98357(播放)
// #define I2S_BCK_PIN     2
// #define I2S_LRCK_PIN    3
// #define I2S_DIN_PIN     4   // INMP441输出

#define I2S_BCK_PIN     12
#define I2S_LRCK_PIN    11
#define I2S_DIN_PIN     10   // INMP441输出

// I2S‑1 MAX98357播放引脚
#define PLAY_I2S_BCK      6
#define PLAY_I2S_LRCK     7
#define PLAY_I2S_DOUT     5
#define PLAY_I2S_NUM      I2S_NUM_1


#define RECORD_SEC      4        //唤醒后录音时长 3‑5s
#define SAMPLE_RATE     16000
#define SAMPLE_RATE_2     16000

//====================业务配置====================
#define DOUBAO_API_KEY "ark-86d3c4c3-ce15-4186-aa97-e4034d33d396-7b7c8" // 豆包apikey
#define ASR_HTTP_TIMEOUT_MS 10000
#define LLM_HTTP_TIMEOUT_MS 12000
//ASR、LLM工作任务栈大小
#define NET_WORK_TASK_STACK (16*1024)
//录音完成后，把PCM数据通过队列交给网络任务
typedef struct {
    bool valid;
    int16_t *pcm_buf;
    uint32_t pcm_samples;
} audio_job_t;
static QueueHandle_t g_audio_job_queue = NULL; //修复：QueueHandle_t，不要xQueueHandle
//================================================

//====================业务配置====================
#define DOUBAO_API_KEY "ark-86d3c4c3-ce15-4186-aa97-e4034d33d396-7b7c8" // 豆包apikey
#define ASR_HTTP_TIMEOUT_MS 10000
#define LLM_HTTP_TIMEOUT_MS 12000

//==== Python中转后端配置，改成你电脑实际局域网IP ====
#define PY_BACKEND_SUBMIT_URL "http://192.168.1.6:8000/audio_submit"
#define PY_BACKEND_QUERY_URL  "http://192.168.1.6:8000/audio_query"
#define PY_BACKEND_LLM_URL    "http://192.168.1.6:8000/audio_infer"
#define PY_BACKEND_UPLOAD_PCM_URL "http://192.168.1.6:8000/upload_pcm_bin"

#define PY_HTTP_TIMEOUT_MS     15000
#define ASR_MAX_POLL_COUNT     20
#define ASR_POLL_INTERVAL_MS   400

//ASR、LLM工作任务栈大小
#define NET_WORK_TASK_STACK (16*1024)

// ---播放队列与互斥锁---
typedef struct {
    uint8_t *pcm_ptr;
    size_t pcm_bytes;
} play_req_t;

static QueueHandle_t g_play_req_queue = NULL;
static SemaphoreHandle_t g_play_mutex = NULL;

static bool g_afe_ready = false;
static TaskHandle_t voice_task_handle = NULL;

static const char *TAG = "VOICE_ASSIST";

static i2s_chan_handle_t i2s_tx_handle;
static i2s_chan_handle_t i2s_rx_handle;
static bool wifi_is_connected = false;
static bool is_wakeup_busy = false;
static time_t s_system_timestamp = 0;

static i2s_chan_handle_t i2s_tx1_handle;  // I2S1 播放 MAX98357

// AFE‑SR全局句柄
static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;

static void afe_debug_pcm_hook(const int16_t *data, int data_size);

static void voice_assist_task(void *arg);
static void afe_init_task(void *arg);

static void net_work_task(void *arg);
// static esp_err_t asr_send_pcm_get_text(const int16_t *pcm, uint32_t samples, char *out_text, size_t out_buf_len);
static esp_err_t asr_send_pcm_get_text(const int16_t *pcm, uint32_t samples, char *out_asr_text, size_t asr_buf_len, char *out_llm_text, size_t llm_buf_len);
static esp_err_t llm_doubao_get_reply(const char *user_query, char *out_reply, size_t out_buf_len);

static esp_err_t i2s_play_init(void);
static void play_beep_test(void);
static esp_err_t http_fetch_tts_pcm(const char *text, uint8_t *buf, size_t buf_size, size_t *out_bytes);

static void audio_player_task(void *arg);


// I2S初始化：仅录音，去掉播放
static esp_err_t i2s_duplex_init(void)
{
    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_LRCK_PIN,
            .din  = I2S_DIN_PIN,
            .dout = I2S_GPIO_UNUSED,
        }
    };
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_handle,&rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_handle));
    return ESP_OK;
}

/**
 * @brief I2S1 播放初始化，16000Hz，立体声，给MAX98357
 */
static esp_err_t i2s_play_init(void)
{
    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_2),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = PLAY_I2S_BCK,
            .ws   = PLAY_I2S_LRCK,
            .dout = PLAY_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        }
    };
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(PLAY_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;  // 放大DMA单块帧数，增加硬件缓冲！！

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx1_handle, NULL));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx1_handle, &tx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx1_handle));
    ESP_LOGI(TAG,"✅I2S1播放通道初始化完成");
    return ESP_OK;
}

// static void play_beep_test(void)
// {
//     ESP_LOGI(TAG,"🔊触发1s蜂鸣状态");
//     g_play_state = PLAY_BEEP_1S;
//     g_play_start_tick = xTaskGetTickCount();
// }
static void play_beep_test(void)
{
    ESP_LOGI(TAG,"🔊触发1s蜂鸣");
#define BEEP_SEC     1
#define BEEP_FREQ    440
#define BEEP_VOL     8000
    const size_t beep_sample_cnt = SAMPLE_RATE * BEEP_SEC;
    size_t beep_bytes = beep_sample_cnt * sizeof(int16_t);

    // PSRAM分配蜂鸣音频缓冲区
    uint8_t *beep_buf = (uint8_t *)heap_caps_malloc(beep_bytes, MALLOC_CAP_SPIRAM);
    if(beep_buf == NULL)
    {
        ESP_LOGE(TAG,"蜂鸣malloc PSRAM失败");
        return;
    }
    //生成正弦蜂鸣pcm
    for(size_t i = 0; i < beep_sample_cnt; i++)
    {
        int16_t val = (int16_t)( BEEP_VOL * sinf( 2.0F * (float)M_PI * BEEP_FREQ * i / (float)SAMPLE_RATE ) );
        *( (int16_t*)(beep_buf + i*sizeof(int16_t)) ) = val;
    }

    play_req_t req;
    req.pcm_ptr = beep_buf;
    req.pcm_bytes = beep_bytes;

    //投递到播放队列；队列满则直接释放内存，不播放
    BaseType_t ret = xQueueSend(g_play_req_queue, &req, pdMS_TO_TICKS(0));
    if(ret != pdTRUE)
    {
        ESP_LOGE(TAG,"播放队列已满，蜂鸣无法播放，释放buffer");
        heap_caps_free(beep_buf);
    }
}

/**
 * @brief POST文本给Python后端 /tts_text2pcm，获取裸16000‑16bit单声道pcm二进制
 * @param text 待合成语音的文本
 * @param buf 接收pcm的缓冲区
 * @param buf_size 缓冲区总字节
 * @param out_bytes 返回实际收到pcm字节数
 */
#if 0
static esp_err_t http_fetch_tts_pcm(const char *text, uint8_t *buf, size_t buf_size, size_t *out_bytes)
{
    *out_bytes = 0;
    if(text == NULL || buf == NULL || buf_size == 0)
        return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", text);
    char *post_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(post_body == NULL)
        return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = "http://192.168.1.6:8000/tts_text2pcm",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(post_body));
    if(err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        free(post_body);
        return err;
    }
    esp_http_client_write(client, post_body, strlen(post_body));
    free(post_body);

    int status = esp_http_client_fetch_headers(client);
    if(status != 200)
    {
        ESP_LOGE(TAG,"TTS http status code=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_ret;
    size_t total = 0;
    while((read_ret = esp_http_client_read_response(client, (char *)(buf + total), (int)(buf_size - total))) > 0)
    {
        total += read_ret;
        if(total >= buf_size)
            break;
    }
    *out_bytes = total;
    ESP_LOGI(TAG,"✅收到TTS PCM字节：%zu", total);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}
#endif
static esp_err_t http_fetch_tts_pcm(const char *text, uint8_t *buf, size_t buf_size, size_t *out_bytes)
{
    *out_bytes = 0;
    if(text == NULL || buf == NULL || buf_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", text);
    char *post_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(post_body == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t cfg = {
        .url = "http://192.168.1.6:8000/tts_text2pcm",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, strlen(post_body));
    if(err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        free(post_body);
        return err;
    }
    esp_http_client_write(client, post_body, strlen(post_body));
    free(post_body);

    int status = esp_http_client_fetch_headers(client);
    int http_status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,"TTS http_status_code=%d", http_status_code);
    if(http_status_code != 200)
    {
        ESP_LOGE(TAG,"TTS接口返回非200");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

//======== 修改：160KB PSRAM缓冲区 =========
#define TTS_JSON_BUF_LEN (512*1024U)
// heap_caps_malloc 指定从PSRAM分配！！！！
    char *json_buf = (char *)heap_caps_malloc(TTS_JSON_BUF_LEN, MALLOC_CAP_SPIRAM);
    if(json_buf == NULL)
    {
        ESP_LOGE(TAG,"json_buf heap_caps_malloc PSRAM failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    memset(json_buf,0,TTS_JSON_BUF_LEN);
    size_t max_can_read = TTS_JSON_BUF_LEN - 2;
    int read_ret;
    size_t total_json = 0;
    while((read_ret = esp_http_client_read_response(client, json_buf + total_json, (int)(max_can_read - total_json)))>0)
    {
        total_json += read_ret;
        if(total_json >= max_can_read)
        {
            ESP_LOGW(TAG,"TTS json buffer reach max limit, data truncated!!!");
            break;
        }
    }
    json_buf[total_json] = '\0';

//=======新增调试打印，重点看这两行日志=======
    ESP_LOGI(TAG,"TTS resp total_json=%zu", total_json);
    ESP_LOGI(TAG,"TTS resp head:%.300s", json_buf);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    cJSON *j_root = cJSON_Parse(json_buf);
    heap_caps_free(json_buf);
    if(j_root == NULL)
    {
        ESP_LOGE(TAG,"TTS JSON解析失败");
        return ESP_FAIL;
    }

    //取出三个字段
    cJSON *j_b64      = cJSON_GetObjectItemCaseSensitive(j_root, "pcm_b64");
    cJSON *j_pcm_len  = cJSON_GetObjectItemCaseSensitive(j_root, "pcm_len");
    cJSON *j_crc32    = cJSON_GetObjectItemCaseSensitive(j_root, "crc32");

    if(!cJSON_IsString(j_b64) || j_b64->valuestring == NULL ||
       !cJSON_IsNumber(j_pcm_len) ||
       !cJSON_IsNumber(j_crc32))
    {
        ESP_LOGE(TAG,"TTS json字段缺失！需要pcm_b64/pcm_len/crc32");
        cJSON_Delete(j_root);
        return ESP_FAIL;
    }

    uint32_t expect_pcm_len = (uint32_t)j_pcm_len->valuedouble;
    uint32_t expect_crc32   = (uint32_t)j_crc32->valuedouble;

    ESP_LOGI(TAG,"【服务端下发校验】expect_pcm_len=%"PRIu32" , expect_crc32=%"PRIu32, expect_pcm_len, expect_crc32);

    //提前防护：输出缓冲区不够就直接退出，不做解码
    if(expect_pcm_len > buf_size)
    {
        ESP_LOGE(TAG,"❌输出缓冲区不足！expect:%"PRIu32"  buf_max:%zu", expect_pcm_len, buf_size);
        cJSON_Delete(j_root);
        return ESP_FAIL;
    }

    size_t b64_len = strlen(j_b64->valuestring);
    size_t decode_out_len = 0;
    int mbed_ret = mbedtls_base64_decode(buf, buf_size, &decode_out_len,
            (const unsigned char*)j_b64->valuestring, b64_len);

    if(mbed_ret != 0)
    {
        ESP_LOGE(TAG,"❌base64解码失败 ret=%d",mbed_ret);
        cJSON_Delete(j_root);
        return ESP_FAIL;
    }

    //ESP32本地对解码出来的裸PCM计算CRC32
    uint32_t local_crc = esp_crc32_le(0, buf, decode_out_len);
    ESP_LOGI(TAG,"【本地校验结果】decode_len=%zu local_crc=%"PRIu32" expect_crc=%"PRIu32,
            decode_out_len, local_crc, expect_crc32);

    //双重校验：长度相等 + crc32相等
    if( (decode_out_len == expect_pcm_len) && (local_crc == expect_crc32) )
    {
        ESP_LOGI(TAG,"✅【校验全部通过】PCM数据完整无损");
    }
    else
    {
        ESP_LOGE(TAG,"❌【校验失败】数据损坏！decode_len:%zu expect:%"PRIu32" , local_crc:%"PRIu32" expect:%"PRIu32,
            decode_out_len, expect_pcm_len, local_crc, expect_crc32);
        cJSON_Delete(j_root);
        return ESP_FAIL;
    }

    cJSON_Delete(j_root);
    *out_bytes = decode_out_len;
    ESP_LOGI(TAG,"✅收到TTS PCM字节：%zu", decode_out_len);
    return ESP_OK;
}

// 新版AFE‑SR唤醒初始化（替代旧版esp‑sr wakenet）
static esp_err_t esp_afe_sr_wake_init(void)
{
    esp_log_level_set("AFE_SR", ESP_LOG_VERBOSE);
    esp_log_level_set("WAKENET", ESP_LOG_VERBOSE);
    esp_log_level_set("VAD", ESP_LOG_VERBOSE);

    static srmodel_list_t *sr_model_list = NULL;
    sr_model_list = esp_srmodel_init("model");
    if(sr_model_list == NULL) {
        ESP_LOGE(TAG,"sr_model_init fail! check model partition & flash full image");
        return ESP_FAIL;
    }

    afe_config_t *afe_cfg = afe_config_init("M", sr_model_list, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if(afe_cfg == NULL) {
        ESP_LOGE(TAG,"afe_config_init return NULL");
        return ESP_FAIL;
    }

    afe_cfg->afe_perferred_core = 1;
    afe_cfg->afe_perferred_priority = 4;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg->afe_linear_gain = 2.0f;

    afe_cfg->debug_init = true;
    afe_cfg->wakenet_model_name = "wn9s_nihaoxiaozhi";

    afe_config_print(afe_cfg);

    afe_handle = esp_afe_handle_from_config(afe_cfg);
    if(afe_handle == NULL) {
        ESP_LOGE(TAG,"esp_afe_handle_from_config NULL");
        return ESP_FAIL;
    }

    afe_data = afe_handle->create_from_config(afe_cfg);
    if(afe_data == NULL) {
        ESP_LOGE(TAG,"afe create_from_config fail, check PSRAM enabled");
        return ESP_FAIL;
    }

    int ret = afe_handle->set_wakenet_threshold(afe_data, 1, 0.55f);
    ESP_LOGI(TAG,"set wakenet threshold ret:%d", ret);

    // =========新增注册debug hook=========
    afe_debug_hook_t hook_cfg = {
        .hook_type = AFE_DEBUG_HOOK_FETCH_TASK_IN,   // AFE后处理完成后，给到fetch之前的音频
        .hook_callback = afe_debug_pcm_hook
    };
    // afe_handle->set_debug_hook(afe_data, &hook_cfg);
    // ====================================

    ESP_LOGI(TAG,"AFE‑SR唤醒服务启动，等待 nihaoxiaozhi");
    return ESP_OK;
}

static void afe_debug_pcm_hook(const int16_t *data, int data_size)
{
    // data：AFE链路的PCM帧，data_size 为样本个数，指针临时有效，回调结束失效
    // 禁止在这里频繁ESP_LOG，会造成音频卡顿、WDT重启
    static uint32_t frame_cnt = 0;
    frame_cnt ++;

    // 每50帧打印一次简单统计，不要每帧打
    if ((frame_cnt % 50) == 0) {
        int32_t sum_abs = 0;
        for(int i = 0; i < data_size; i++) {
            sum_abs += abs(data[i]);
        }
        int avg = sum_abs / data_size;
        // ESP_LOGI("AFE_HOOK", "hook frame_cnt=%u, samples=%d, avg_abs=%d", frame_cnt, data_size, avg);
        ESP_LOGI("AFE_HOOK", "hook frame_cnt=%"PRIu32", samples=%d, avg_abs=%d", frame_cnt, data_size, avg);
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI("WIFI","WIFI_EVENT_STA_START 开始连接wifi");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW("WIFI","WIFI_EVENT_STA_DISCONNECTED 断开，自动重连");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WIFI","✅WiFi获取IP成功！ ip:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "车城之家313",
            .password = "QCC789@313",
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );
}

void sntp_time_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void print_wifi_status(void)
{
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if(ret == ESP_OK){
        ESP_LOGI("WIFI_STATUS","✅已连接AP: %s, rssi:%d", ap_info.ssid, ap_info.rssi);
    }else{
        ESP_LOGW("WIFI_STATUS","❌未连接WiFi, ret=%d", ret);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG,"====系统启动====");
    
    // ============网络初始化动============
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_sta();
    sntp_time_sync();

    uint32_t wait_ms = 0;
    const uint32_t MAX_WAIT_MS = 8000;
    wifi_ap_record_t ap_info;
    while(wait_ms < MAX_WAIT_MS)
    {
        if(esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        wait_ms += 200;
    }
    print_wifi_status();
    // ===================================

    ESP_ERROR_CHECK(i2s_duplex_init());
    ESP_ERROR_CHECK(i2s_play_init());

    //====新建播放队列：最多2个播放请求====
    g_play_req_queue = xQueueCreate(2, sizeof(play_req_t));
    g_play_mutex = xSemaphoreCreateMutex();

    //新建独立I2S播放任务，core1，高优先级7，和AFE‑SR同core但完全隔离业务
    xTaskCreatePinnedToCore(audio_player_task, "audio_player", 8192, NULL,4,NULL,0);
    ESP_LOGI(TAG,"audio_player_task create ok");

    //新建音频任务队列，最多1个任务，避免堆积
    g_audio_job_queue = xQueueCreate(1, sizeof(audio_job_t));

    xTaskCreatePinnedToCore(net_work_task, "net_task", NET_WORK_TASK_STACK, NULL, 2, NULL, 0);
    ESP_LOGI(TAG,"net_work_task create OK!");
    xTaskCreatePinnedToCore(afe_init_task, "afe_init", 8192, NULL, 3, NULL, 1);
    ESP_LOGI(TAG,"afe_init_task create OK!");

    ESP_LOGI(TAG,"app_main: waiting g_afe_ready ...");
    while(!g_afe_ready)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG,"app_main: g_afe_ready == true, now create voice_assist_task bind to core1");

    BaseType_t ret_create;
    ret_create = xTaskCreatePinnedToCore(
            voice_assist_task,
            "voice_task",
            8192,
            NULL,
            5,
            &voice_task_handle,
            1   // ✅绑定Core1，现在是Core0上下文调用，不会触发SMP bug
    );
    if(ret_create == pdPASS)
    {
        ESP_LOGI(TAG,"✅voice_assist_task 创建成功, handle=%p", voice_task_handle);
    }
    else
    {
        ESP_LOGE(TAG,"❌voice_assist_task 创建失败! ret=%d pdPASS=%d pdFAIL=%d",
                ret_create, (int)pdPASS, (int)pdFAIL);
    }

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void voice_assist_task(void *arg)
{
    ESP_LOGI(TAG,"✅voice_assist_task 已经启动！");
    // #define AFE_FRAME_SAMPLES 320
    int AFE_FRAME_SAMPLES;

    //==================== 从正常版本移植：软件环形缓冲 ====================
#define RING_BUF_STEREO_CNT 2048
    static int32_t ring_buf[RING_BUF_STEREO_CNT];
    static uint32_t ring_wr_idx = 0;
    static uint32_t ring_rd_idx = 0;

    AFE_FRAME_SAMPLES = afe_handle->get_feed_chunksize(afe_data);
    ESP_LOGI(TAG,"AFE feed chunk size = %d", AFE_FRAME_SAMPLES);

    // AFE‑SR要求输入：int16_t 16bit PCM
    static int16_t mic_frame[512];

    #define RECORD_TOTAL_SAMPLES  (RECORD_SEC * SAMPLE_RATE)
    static int16_t record_buf[RECORD_TOTAL_SAMPLES];
    uint32_t record_write_idx = 0;

    size_t read_bytes;
    bool wakeup_triggered = false;
    uint32_t record_sample_cnt = 0;
    const uint32_t record_total_samples = RECORD_SEC * SAMPLE_RATE;

    // 用于控制音量打印频率：每N帧打印一次，不要每一帧都打
    const uint32_t print_level_interval = 100;
    uint32_t frame_cnt = 0;

    while(1)
    {
        //====================【重点1：小块读取，不再一次性读大buffer，解决超时】====================
        int32_t temp_i2s_buf[128];
        esp_err_t ret = i2s_channel_read(i2s_rx_handle, temp_i2s_buf, sizeof(temp_i2s_buf), &read_bytes, pdMS_TO_TICKS(50));
    
        const char *err_str = esp_err_to_name(ret);
        // ESP_LOGW(TAG, "I2S_READ: ret=%d | %s | read_bytes=%zu", ret, err_str, read_bytes);

        if(ret == ESP_OK || ret == ESP_ERR_TIMEOUT)
        {
            if(read_bytes > 0)
            {
                uint32_t sample_in = read_bytes / sizeof(int32_t);
                for(uint32_t i=0; i<sample_in; i++)
                {
                    uint32_t next_wr = (ring_wr_idx + 1) % RING_BUF_STEREO_CNT;
                    if(next_wr != ring_rd_idx)
                    {
                        ring_buf[ring_wr_idx] = temp_i2s_buf[i];
                        ring_wr_idx = next_wr;
                    }
                    else
                    {
                        // ESP_LOGW(TAG,"ring buffer full, drop old sample");
                        //丢弃一个旧样本，腾出位置
                        ring_rd_idx = (ring_rd_idx + 1) % RING_BUF_STEREO_CNT;
                        ring_buf[ring_wr_idx] = temp_i2s_buf[i];
                        ring_wr_idx = next_wr;
                    }
                }
            }
        }
        else
        {
            ESP_LOGW(TAG,"i2s hardware error ret=%d", ret);
        }

        // 计算环形缓冲可用立体声样本
        uint32_t avail_samples;
        if(ring_wr_idx >= ring_rd_idx)
            avail_samples = ring_wr_idx - ring_rd_idx;
        else
            avail_samples = (RING_BUF_STEREO_CNT - ring_rd_idx) + ring_wr_idx;

        // if(avail_samples >= AFE_FRAME_SAMPLES * 2)
        // {
        //     // ESP_LOGW(TAG,"ring_buf overrun drop old frame");
        //     ring_rd_idx = (ring_rd_idx + AFE_FRAME_SAMPLES * 2) % RING_BUF_STEREO_CNT;
        // }

        // 攒够一帧数据才处理
        if(avail_samples >= AFE_FRAME_SAMPLES * 2)
        {
            uint32_t temp_rd = ring_rd_idx;

            int16_t pcm_max = -32768;
            int16_t pcm_min = 32767;

            // 从ringbuf取出左声道，组装mic_frame
            for(int i=0; i < AFE_FRAME_SAMPLES; i++)
            {
                //取左声道，跳过右声道
                int32_t val = ring_buf[ring_rd_idx];
                ring_rd_idx = (ring_rd_idx + 2) % RING_BUF_STEREO_CNT;

                //移位，后面再来修改这个移位，本次不动
                val = val >> 8;
                val = val >> 8;
                if(val > 32767) val = 32767;
                if(val < -32768) val = -32768;
                mic_frame[i] = (int16_t)val;

                if(mic_frame[i] > pcm_max) pcm_max = mic_frame[i];
                if(mic_frame[i] < pcm_min) pcm_min = mic_frame[i];
            }

            // ========= 只有读到新帧之后，才计算麦克风音量电平 =========
            frame_cnt++;
            if (frame_cnt >= print_level_interval)
            {
                frame_cnt = 0;
                // ESP_LOGI(TAG, "ring sample[0] = %" PRId32, ring_buf[temp_rd]);
                // ESP_LOGI(TAG,"pcm range, max:%d min:%d", pcm_max, pcm_min); //打印幅值
                                
                int32_t sum_abs = 0;
                for (int i = 0; i < AFE_FRAME_SAMPLES; i++)
                {
                    sum_abs += abs(mic_frame[i]);
                }
                uint32_t mic_level = (uint32_t)(sum_abs / AFE_FRAME_SAMPLES);
                ESP_LOGI(TAG, "mic average level: %lu", mic_level);
            }
 
            //喂给AFE
            afe_handle->feed(afe_data, mic_frame);
            afe_fetch_result_t *res = afe_handle->fetch(afe_data);
            if(res != NULL && res->ret_value != ESP_FAIL)
            {
                //新增调试打印！看每一次事件的状态
                // ESP_LOGI(TAG,"afe fetch get event, wakeup_state=%d vad_state=%d", res->wakeup_state, res->vad_state);
                    
                if(res->data != NULL && res->data_size > 0)
                {
                    int sample_cnt = res->data_size / sizeof(int16_t); //字节转int16样本个数
                    int32_t sum_abs = 0;
                    for(int i=0; i < sample_cnt; i++){
                        sum_abs += abs(res->data[i]);
                    }
                    int avg_out = sum_abs / sample_cnt;
                    // ESP_LOGI("AFE_OUT","processed mic avg_abs: %d", avg_out);
                }

                if(!wakeup_triggered)
                {
                    if(res->wakeup_state == WAKENET_DETECTED || res->wakeup_state == WAKENET_CHANNEL_VERIFIED)
                    {
                        ESP_LOGI(TAG,"========唤醒词 你好小智 触发！========");
                        wakeup_triggered = true;
                        record_sample_cnt = 0;
                        record_write_idx = 0;
                        is_wakeup_busy = true;
                    }
                }
                else
                {
                    if(record_sample_cnt < record_total_samples)
                    {
                        // 拷贝AFE输出降噪后的pcm
                        if(res->data != NULL && res->data_size > 0)
                        {
                            int frame_sample = res->data_size / sizeof(int16_t);
                            int copy_len = ((record_write_idx + frame_sample) > RECORD_TOTAL_SAMPLES)
                                            ? (RECORD_TOTAL_SAMPLES - record_write_idx)
                                            : frame_sample;
                            memcpy(&record_buf[record_write_idx], res->data, copy_len * sizeof(int16_t));
                            record_write_idx += copy_len;
                        }

                        record_sample_cnt += AFE_FRAME_SAMPLES;
                        if(record_sample_cnt >= record_total_samples)
                        {
                            ESP_LOGI(TAG,"录音采集完成，准备进行云端ASR");
                            audio_job_t job={0};
                            job.valid = true;
                            job.pcm_buf = record_buf;
                            job.pcm_samples = record_total_samples;
                            //发送到队列，如果队列满直接丢弃本次录音
                            if(xQueueSend(g_audio_job_queue,&job,0) != pdTRUE)
                            {
                                ESP_LOGW(TAG,"audio job queue full, drop this record");
                            }

                            afe_handle->enable_wakenet(afe_data);
                            wakeup_triggered = false;
                            is_wakeup_busy = false;
                            ESP_LOGI(TAG,"回到休眠等待唤醒词");
                        }
                    }
                }
            }
            else
            {
                // 大部分帧fetch返回NULL，这里不会打印，属于正常现象
                ESP_LOGI(TAG,"afe fetch NULL!");
                taskYIELD();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void afe_init_task(void *arg)
{
    ESP_LOGI(TAG,"afe_init_task start");
    esp_err_t ret = esp_afe_sr_wake_init();
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG,"afe init fail");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,"afe_init_task: AFE init complete, set g_afe_ready=true");
    g_afe_ready = true;

    // //AFE初始化完成，延时再创建语音采集任务
    // vTaskDelay(pdMS_TO_TICKS(200));

    // ESP_LOGI(TAG,"free heap: %u bytes", (unsigned int)esp_get_free_heap_size());

    // TaskHandle_t voice_task_handle = NULL;
    // BaseType_t ret_create;

    // ret_create = xTaskCreatePinnedToCore(
    //         voice_assist_task,
    //         "voice_task",
    //         // 24*1024,
    //         8192,
    //         NULL,
    //         5,
    //         &voice_task_handle,   // 接收任务句柄，传NULL就丢弃句柄
    //         1
    // );

    // if(ret_create == pdPASS)
    // {
    //     ESP_LOGI(TAG,"✅voice_assist_task 创建成功, handle=%p", voice_task_handle);
    // }
    // else
    // {
    //     ESP_LOGE(TAG,"❌voice_assist_task 创建失败! ret=%d", ret_create);
    // }

    //本初始化任务使命完成，自我销毁
    vTaskDelete(NULL);
}

static void net_work_task(void *arg)
{
    audio_job_t job;
    char asr_result[512]={0};
    char llm_reply[1024]={0};
    for(;;)
    {
        ESP_LOGI(TAG,"11111111111111111");
        memset(&job,0,sizeof(job));
        BaseType_t ret = xQueueReceive(g_audio_job_queue,&job,portMAX_DELAY);
        if(ret != pdTRUE || !job.valid) continue;
        ESP_LOGI(TAG,"net_work_task: receive audio job, samples:%"PRIu32, job.pcm_samples);
        //【步骤2】判断WiFi是否在线
        wifi_ap_record_t ap_info;
        esp_err_t wifi_ret = esp_wifi_sta_get_ap_info(&ap_info);
        if(wifi_ret != ESP_OK)
        {
            ESP_LOGE(TAG,"WiFi未连接！网络异常");
            //TODO：TTS播放“网络异常”
            continue;
        }
        ESP_LOGI(TAG,"222222222222222");

        //【步骤3】PCM发给Python中转后端，一次性拿到ASR文本 + LLM回答
        memset(asr_result,0,sizeof(asr_result));
        memset(llm_reply,0,sizeof(llm_reply));
        esp_err_t backend_err = asr_send_pcm_get_text(job.pcm_buf, job.pcm_samples,
                                                        asr_result, sizeof(asr_result),
                                                        llm_reply, sizeof(llm_reply));
        if(backend_err != ESP_OK || strlen(asr_result)==0)
        {
            ESP_LOGE(TAG,"后端处理失败或者ASR识别为空");
            //TODO TTS播放“识别失败，请再说一遍”
            continue;
        }
        ESP_LOGI(TAG,"✅ASR识别用户问句：%s", asr_result);
        ESP_LOGI(TAG,"✅豆包LLM回复：%s", llm_reply);
        // ESP_LOGI(TAG,"🔊播放提示蜂鸣音");
        // play_beep_test();   //临时测试：播放1秒滴滴提示音，确认喇叭硬件通路正常
        ESP_LOGI(TAG,"333333333333333");
        
    #define TTS_BUF_SIZE  (384*1024U)
        uint8_t *tts_recv_buf = (uint8_t *)heap_caps_malloc(TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if(tts_recv_buf != NULL && strlen(llm_reply) > 0)
        {
            size_t pcm_bytes = 0;
            esp_err_t tts_err = http_fetch_tts_pcm(llm_reply, tts_recv_buf, TTS_BUF_SIZE, &pcm_bytes);
            if(tts_err == ESP_OK && pcm_bytes > sizeof(int16_t))
            {
                ESP_LOGI(TAG,"🔊准备入队TTS播放请求");
                play_req_t req;
                req.pcm_ptr = tts_recv_buf;
                req.pcm_bytes = pcm_bytes;
                //发送到播放队列，如果队列满，释放内存，丢弃本次播放
                if(xQueueSend(g_play_req_queue, &req, pdMS_TO_TICKS(0)) != pdTRUE)
                {
                    ESP_LOGE(TAG,"播放队列满，丢弃TTS音频");
                    heap_caps_free(tts_recv_buf);
                    play_beep_test();
                }
            }
            else
            {
                ESP_LOGE(TAG,"TTS获取pcm失败，降级播放蜂鸣");
                play_beep_test();
                heap_caps_free(tts_recv_buf);
            }
        }
        else
        {
            ESP_LOGE(TAG,"malloc tts buffer失败，降级播放蜂鸣");
            play_beep_test();
        }
    }
    vTaskDelete(NULL);
}

/**
 * @brief POST json到指定url，返回响应json字符串
 * @param url 请求地址
 * @param post_json 请求json字符串
 * @param out_resp 输出缓冲区
 * @param out_resp_len 输出缓冲区大小
 * @return ESP_OK成功
 */
static esp_err_t http_post_json(const char *url, const char *post_json, char *out_resp, size_t out_resp_len)
{
    if(!url || !post_json || !out_resp || out_resp_len == 0)
        return ESP_ERR_INVALID_ARG;
    *out_resp = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = PY_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == NULL) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Content-Type", "application/json");


    esp_err_t err = esp_http_client_open(client, strlen(post_json));
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG,"esp_http_client_open fail %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    //分块发送json字符串
    size_t total = strlen(post_json);
    size_t offset = 0;
    const size_t chunk_sz = 4096;
    while(offset < total)
    {
        size_t wlen = ( (offset+chunk_sz) > total ) ? (total-offset) : chunk_sz;
        int w = esp_http_client_write(client, post_json + offset, wlen);
        if(w <=0)
        {
            ESP_LOGE(TAG,"esp_http_client_write fail");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        offset += w;
    }

    //读取响应头
    err = esp_http_client_fetch_headers(client);
    if(err < 0)
    {
        ESP_LOGE(TAG,"fetch_headers fail %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,"http status_code=%d", status_code);

    int buf_max = (int)out_resp_len -1;
    int total_read =0;
    int r;
    while( (r = esp_http_client_read_response(client, out_resp + total_read, buf_max - total_read)) > 0 )
    {
        total_read += r;
    }
    out_resp[total_read] = '\0';
    ESP_LOGI(TAG,"HTTP RESP RAW:[%s]", out_resp);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t http_post_raw_binary(const char *url, const uint8_t *bin_data, size_t bin_len, char *out_resp, size_t out_resp_len)
{
    if(!url || !bin_data || bin_len ==0 || !out_resp || out_resp_len ==0)
        return ESP_ERR_INVALID_ARG;
    *out_resp = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = PY_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == NULL) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    esp_err_t err = esp_http_client_open(client, bin_len);
    if(err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return err;
    }

    size_t offset = 0;
    const size_t chunk_sz = 4096;
    while(offset < bin_len)
    {
        size_t wlen = ((offset + chunk_sz) > bin_len) ? (bin_len - offset) : chunk_sz;
        int w = esp_http_client_write(client, (const char*)(bin_data + offset), wlen);
        if(w <= 0)
        {
            ESP_LOGE(TAG,"write binary fail");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        offset += w;
    }

    err = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG,"http status_code=%d", status_code);

    int buf_max = (int)out_resp_len - 1;
    int total_read = 0;
    int r;
    while( (r = esp_http_client_read_response(client, out_resp + total_read, buf_max - total_read)) >0 )
    {
        total_read += r;
    }
    out_resp[total_read] = '\0';
    ESP_LOGI(TAG,"HTTP RESP RAW:[%s]", out_resp);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t asr_send_pcm_get_text(const int16_t *pcm, uint32_t samples,
                                       char *out_asr_text, size_t asr_buf_len,
                                       char *out_llm_text, size_t llm_buf_len)
{
    if(!pcm || !out_asr_text || !out_llm_text || asr_buf_len ==0 || llm_buf_len ==0)
        return ESP_ERR_INVALID_ARG;
    *out_asr_text = '\0';
    *out_llm_text = '\0';
    uint32_t pcm_bytes = samples * sizeof(int16_t);
    ESP_LOGI(TAG,"pcm_bytes=%"PRIu32", samples=%"PRIu32, pcm_bytes, samples);

    char task_id[64] = {0};
    char resp_buf[1024] = {0};

    //====【关键】直接上传原始PCM二进制，不再base64，不再组装大JSON====
    esp_err_t err = http_post_raw_binary(PY_BACKEND_UPLOAD_PCM_URL,
                                         (const uint8_t *)pcm,
                                         pcm_bytes,
                                         resp_buf, sizeof(resp_buf));
    if(err != ESP_OK){
        ESP_LOGE(TAG,"upload pcm bin http fail");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,"submit resp_buf len=%zu", strlen(resp_buf));
    cJSON *submit_resp = cJSON_Parse(resp_buf);
    if(submit_resp == NULL){
        ESP_LOGE(TAG,"parse submit resp json fail! resp:[%s]", resp_buf);
        return ESP_FAIL;
    }
    cJSON *tid_obj = cJSON_GetObjectItemCaseSensitive(submit_resp, "task_id");
    if(!cJSON_IsString(tid_obj) || tid_obj->valuestring == NULL){
        ESP_LOGE(TAG,"submit return empty task_id");
        cJSON_Delete(submit_resp);
        return ESP_FAIL;
    }
    strncpy(task_id, tid_obj->valuestring, sizeof(task_id)-1);
    cJSON_Delete(submit_resp);
    ESP_LOGI(TAG,"get task_id=%s", task_id);

    // =========【下面代码完全不动：轮询 /audio_query 逻辑原样保留】=========
    bool asr_ok = false;
    for(int poll=0; poll < ASR_MAX_POLL_COUNT; poll++)
    {
        memset(resp_buf,0,sizeof(resp_buf));
        cJSON *query_json = cJSON_CreateObject();
        cJSON_AddStringToObject(query_json, "task_id", task_id);
        char *query_body = cJSON_PrintUnformatted(query_json);
        cJSON_Delete(query_json);
        if(query_body == NULL) return ESP_ERR_NO_MEM;
        err = http_post_json(PY_BACKEND_QUERY_URL, query_body, resp_buf, sizeof(resp_buf));
        free(query_body);
        if(err != ESP_OK){
            ESP_LOGE(TAG,"audio_query http fail");
            return ESP_FAIL;
        }
        cJSON *query_resp = cJSON_Parse(resp_buf);
        if(query_resp == NULL){
            ESP_LOGE(TAG,"parse query json fail");
            return ESP_FAIL;
        }
        cJSON *status_obj = cJSON_GetObjectItemCaseSensitive(query_resp, "status");
        cJSON *text_obj  = cJSON_GetObjectItemCaseSensitive(query_resp, "text");
        cJSON *msg_obj   = cJSON_GetObjectItemCaseSensitive(query_resp, "msg");
        if(cJSON_IsString(status_obj))
        {
            if(strcmp(status_obj->valuestring, "success") == 0)
            {
                if(cJSON_IsString(text_obj))
                {
                    strncpy(out_asr_text, text_obj->valuestring, asr_buf_len -1);
                    out_asr_text[asr_buf_len-1] = '\0';
                }
                asr_ok = true;
                cJSON_Delete(query_resp);
                break;
            }
            else if(strcmp(status_obj->valuestring, "error") == 0)
            {
                ESP_LOGE(TAG,"audio_query error: %s", cJSON_IsString(msg_obj)?msg_obj->valuestring:"null");
                cJSON_Delete(query_resp);
                return ESP_FAIL;
            }
            // pending，继续轮询
        }
        cJSON_Delete(query_resp);
        vTaskDelay(pdMS_TO_TICKS(ASR_POLL_INTERVAL_MS));
    }
    if(!asr_ok){
        ESP_LOGE(TAG,"ASR poll timeout");
        return ESP_FAIL;
    }
    if(strlen(out_asr_text) == 0){
        ESP_LOGW(TAG,"asr text is empty");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,"ASR result: %s", out_asr_text);

    // =========【下面代码完全不动：调用 /audio_infer 获取LLM回复原样保留】=========
    memset(resp_buf,0,sizeof(resp_buf));
    cJSON *llm_json = cJSON_CreateObject();
    cJSON_AddStringToObject(llm_json, "asr_text", out_asr_text);
    char *llm_body = cJSON_PrintUnformatted(llm_json);
    cJSON_Delete(llm_json);
    if(llm_body == NULL) return ESP_ERR_NO_MEM;
    err = http_post_json(PY_BACKEND_LLM_URL, llm_body, resp_buf, sizeof(resp_buf));
    free(llm_body);
    if(err != ESP_OK){
        ESP_LOGE(TAG,"audio_infer http fail");
        return ESP_FAIL;
    }
    cJSON *llm_resp = cJSON_Parse(resp_buf);
    if(llm_resp == NULL){
        ESP_LOGE(TAG,"parse llm resp json fail");
        return ESP_FAIL;
    }
    cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(llm_resp, "err");
    if(cJSON_IsString(err_obj) && strlen(err_obj->valuestring) > 0)
    {
        ESP_LOGE(TAG,"audio_infer err=%s", err_obj->valuestring);
        cJSON_Delete(llm_resp);
        return ESP_FAIL;
    }
    cJSON *llm_text_obj = cJSON_GetObjectItemCaseSensitive(llm_resp, "llm_text");
    if(cJSON_IsString(llm_text_obj))
    {
        strncpy(out_llm_text, llm_text_obj->valuestring, llm_buf_len -1);
        out_llm_text[llm_buf_len-1] = '\0';
    }
    cJSON_Delete(llm_resp);
    ESP_LOGI(TAG,"LLM reply: %s", out_llm_text);
    return ESP_OK;
}

static esp_err_t llm_doubao_get_reply(const char *user_query, char *out_reply, size_t out_buf_len)
{
    if(!user_query || !out_reply || out_buf_len ==0)
        return ESP_ERR_INVALID_ARG;
    *out_reply = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "doubao-seed-2.0-lite"); //全部英文减号
    cJSON *msg_arr = cJSON_AddArrayToObject(root, "messages");

    cJSON *msg1 = cJSON_CreateObject();
    cJSON_AddStringToObject(msg1,"role","user");
    cJSON_AddStringToObject(msg1,"content", user_query);
    cJSON_AddItemToArray(msg_arr, msg1);

    char *post_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(!post_body)
        return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = "https://ark.cn-beijing.volces.com/api/v3/chat/completions",
        .method = HTTP_METHOD_POST,
        .timeout_ms = LLM_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == NULL){
        free(post_body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client,"Content-Type","application/json"); //半角横杠！
    char auth_header[128];
    snprintf(auth_header,sizeof(auth_header),"Bearer %s", DOUBAO_API_KEY);
    esp_http_client_set_header(client,"Authorization", auth_header);

    esp_err_t err = esp_http_client_set_post_field(client, post_body, strlen(post_body));
    int status_code = 0;
    if(err == ESP_OK)
    {
        err = esp_http_client_perform(client);
    }

    if(err == ESP_OK)
    {
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG,"LLM http status_code:%d", status_code);
        if(status_code == 200)
        {
            int content_len = esp_http_client_get_content_length(client);
            if(content_len <=0) content_len = 4096;
            char *resp_buf = malloc(content_len + 1);
            if(resp_buf != NULL)
            {
                int total_read = 0;
                int r;
                while((r = esp_http_client_read(client, resp_buf + total_read, content_len - total_read)) > 0)
                {
                    total_read += r;
                }
                resp_buf[total_read] = '\0';

                cJSON *resp_json = cJSON_Parse(resp_buf);
                if(resp_json != NULL)
                {
                    cJSON *choices = cJSON_GetObjectItemCaseSensitive(resp_json, "choices");
                    if(cJSON_IsArray(choices) && cJSON_GetArraySize(choices) >0)
                    {
                        cJSON *choice0 = cJSON_GetArrayItem(choices,0);
                        cJSON *msg_obj = cJSON_GetObjectItemCaseSensitive(choice0, "message");
                        cJSON *content = cJSON_GetObjectItemCaseSensitive(msg_obj, "content");
                        if(cJSON_IsString(content) && content->valuestring != NULL)
                        {
                            strncpy(out_reply, content->valuestring, out_buf_len - 1);
                            out_reply[out_buf_len -1] = '\0';
                        }
                    }
                    cJSON_Delete(resp_json);
                }
                free(resp_buf);
            }
        }else{
            ESP_LOGE(TAG,"LLM http error code:%d", status_code);
            err = ESP_FAIL;
        }
    }

    esp_http_client_cleanup(client);
    free(post_body);
    return err;
}

static void audio_player_task(void *arg)
{
    ESP_LOGI(TAG, "audio_player_task start");
    (void)arg;
#define FRAME_SAMP 256
    // 单帧输出缓冲区，不需要双buf乒乓！i2s_channel_write内部自带拷贝DMA缓冲
    static int16_t out_buf[FRAME_SAMP * 2];

    play_req_t req;
    memset(&req, 0, sizeof(req));
    size_t offset = 0;

    for(;;)
    {
        // 阻塞等待播放请求！不要pdMS_TO_TICKS(0)非阻塞！
        BaseType_t q_ret = xQueueReceive(g_play_req_queue, &req, pdMS_TO_TICKS(10));
        if(q_ret == pdTRUE)
        {
            //收到新播放请求，重置偏移
            offset = 0;
            ESP_LOGI(TAG,"🔊audio_player_task 收到播放请求 pcm_bytes=%zu", req.pcm_bytes);
        }

        bool is_playing = (req.pcm_ptr != NULL && req.pcm_bytes > 0);
        if(is_playing)
        {
            //填充一帧立体声：单声道转立体声，左右相同
            for(size_t i=0; i < FRAME_SAMP; i++)
            {
                if(offset + sizeof(int16_t) <= req.pcm_bytes)
                {
                    int16_t sample = *(int16_t*)(req.pcm_ptr + offset);
                    out_buf[i*2]   = sample;
                    out_buf[i*2+1] = sample;
                    offset += sizeof(int16_t);
                }
                else
                {
                    out_buf[i*2] = 0;
                    out_buf[i*2+1] = 0;
                }
            }

            size_t written = 0;
            esp_err_t ret_i2s = i2s_channel_write(i2s_tx1_handle,
                    out_buf,
                    FRAME_SAMP*2*sizeof(int16_t),
                    &written,
                    pdMS_TO_TICKS(500));

            if(ret_i2s != ESP_OK || written != FRAME_SAMP*2*sizeof(int16_t))
            {
                ESP_LOGW(TAG,"I2S write warning ret=%d written=%zu", ret_i2s, written);
            }

            //播放全部完成，释放PSRAM
            if(offset >= req.pcm_bytes)
            {
                ESP_LOGI(TAG,"🔊TTS播放完成，释放PSRAM缓冲区");
                heap_caps_free(req.pcm_ptr);
                req.pcm_ptr = NULL;
                req.pcm_bytes = 0;
                offset = 0;
            }
        }
        else
        {
            //没有播放任务，输出静音帧
            memset(out_buf, 0, sizeof(out_buf));
            size_t written = 0;
            i2s_channel_write(i2s_tx1_handle, out_buf, sizeof(out_buf), &written, pdMS_TO_TICKS(500));
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}




