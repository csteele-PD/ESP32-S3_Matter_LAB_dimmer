#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define LAB_WIFI_SSID ""
#define LAB_WIFI_PASSWORD ""
#endif

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/server/Server.h>
#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_providers.h>
#include <lib/support/CHIPMemString.h>
#include <platform/CHIPDeviceEvent.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "matter_s3_lab";

static EventGroupHandle_t wifi_events;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
static constexpr gpio_num_t FACTORY_RESET_GPIO = GPIO_NUM_0;
static constexpr uint32_t MATTER_SETUP_PIN = 20202021;
static constexpr uint16_t MATTER_SETUP_DISCRIMINATOR = 3840;
static constexpr char MATTER_VENDOR_NAME[] = "ACLYS_LAB";
static constexpr char MATTER_PRODUCT_NAME[] = "MatterS3Lab";
static constexpr char MATTER_NODE_LABEL[] = "S3 Matter Lab";

static uint16_t lab_endpoint_id;
static bool internal_update;

class LabDeviceInstanceInfoProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    CHIP_ERROR GetVendorName(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, MATTER_VENDOR_NAME);
    }

    CHIP_ERROR GetVendorId(uint16_t &vendor_id) override
    {
        vendor_id = CONFIG_DEVICE_VENDOR_ID;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetProductName(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, MATTER_PRODUCT_NAME);
    }

    CHIP_ERROR GetProductId(uint16_t &product_id) override
    {
        product_id = CONFIG_DEVICE_PRODUCT_ID;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetPartNumber(char *buf, size_t buf_size) override
    {
        (void)buf;
        (void)buf_size;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetProductURL(char *buf, size_t buf_size) override
    {
        (void)buf;
        (void)buf_size;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetProductLabel(char *buf, size_t buf_size) override
    {
        (void)buf;
        (void)buf_size;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetSerialNumber(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, "matter-s3-lab-001");
    }

    CHIP_ERROR GetManufacturingDate(uint16_t &year, uint8_t &month, uint8_t &day) override
    {
        (void)year;
        (void)month;
        (void)day;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetHardwareVersion(uint16_t &hardware_version) override
    {
        hardware_version = 1;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetHardwareVersionString(char *buf, size_t buf_size) override
    {
        return copy_string(buf, buf_size, "ESP32-S3 Supermini");
    }

    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &unique_id_span) override
    {
        static constexpr uint8_t unique_id[] = {
            0x41, 0x43, 0x4C, 0x59, 0x53, 0x2D, 0x53, 0x33,
            0x2D, 0x4C, 0x41, 0x42, 0x2D, 0x30, 0x30, 0x31,
        };

        if (unique_id_span.size() < sizeof(unique_id)) {
            return CHIP_ERROR_BUFFER_TOO_SMALL;
        }

        memcpy(unique_id_span.data(), unique_id, sizeof(unique_id));
        unique_id_span.reduce_size(sizeof(unique_id));
        return CHIP_NO_ERROR;
    }

private:
    static CHIP_ERROR copy_string(char *buf, size_t buf_size, const char *value)
    {
        if (buf_size < strlen(value) + 1) {
            return CHIP_ERROR_BUFFER_TOO_SMALL;
        }

        chip::Platform::CopyString(buf, buf_size, value);
        return CHIP_NO_ERROR;
    }
};

static LabDeviceInstanceInfoProvider lab_device_instance_info_provider;

static void maybe_factory_reset()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << FACTORY_RESET_GPIO;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    vTaskDelay(pdMS_TO_TICKS(50));
    if (gpio_get_level(FACTORY_RESET_GPIO) != 0) {
        return;
    }

    ESP_LOGW(TAG, "BOOT/GPIO0 held low; erasing NVS and restarting");
    ESP_ERROR_CHECK(nvs_flash_erase());
    esp_restart();
}

static void log_manual_pairing_code()
{
    chip::PayloadContents payload;
    payload.version = 0;
    payload.commissioningFlow = chip::CommissioningFlow::kStandard;
    payload.discriminator.SetLongValue(MATTER_SETUP_DISCRIMINATOR);
    payload.setUpPINCode = MATTER_SETUP_PIN;

    char code[chip::kManualSetupLongCodeCharLength + 2] = {};
    chip::MutableCharSpan span(code, sizeof(code));
    CHIP_ERROR err = chip::ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(span);
    if (err == CHIP_NO_ERROR) {
        code[span.size()] = '\0';
        ESP_LOGI(TAG, "Matter manual pairing code=%s", code);
    } else {
        ESP_LOGW(TAG, "Matter manual pairing code generation failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi station started");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = static_cast<const ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Wi-Fi got IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t start_wifi()
{
    if (strlen(LAB_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "main/wifi_secrets.h is missing or LAB_WIFI_SSID is empty");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(wifi_events != nullptr, ESP_ERR_NO_MEM, TAG, "failed to create Wi-Fi event group");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr),
                        TAG,
                        "Wi-Fi event handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr),
                        TAG,
                        "IP event handler register failed");

    wifi_config_t config = {};
    strlcpy(reinterpret_cast<char *>(config.sta.ssid), LAB_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(config.sta.password), LAB_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID=%s", LAB_WIFI_SSID);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "set Wi-Fi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");

    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

static esp_err_t matter_attribute_callback(attribute::callback_type_t type,
                                           uint16_t endpoint_id,
                                           uint32_t cluster_id,
                                           uint32_t attribute_id,
                                           esp_matter_attr_val_t *val,
                                           void *priv_data)
{
    (void)priv_data;

    if ((type != attribute::PRE_UPDATE && type != attribute::POST_UPDATE) ||
        endpoint_id != lab_endpoint_id ||
        val == nullptr ||
        internal_update) {
        return ESP_OK;
    }

    const char *phase = type == attribute::PRE_UPDATE ? "PRE_UPDATE" : "POST_UPDATE";
    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        ESP_LOGI(TAG, "Matter OnOff %s value=%s", phase, val->val.b ? "on" : "off");
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        ESP_LOGI(TAG, "Matter LevelControl %s current_level=%u", phase, val->val.u8);
    }
    return ESP_OK;
}

static void matter_event_callback(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    if (event == nullptr) {
        return;
    }

    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "commissioning window opened");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "commissioning window closed");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "commissioning session started");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "commissioning session stopped");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "fabric committed");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "fabric removed");
        break;
    default:
        break;
    }
}

static esp_err_t start_matter()
{
    node::config_t node_config;
    strlcpy(node_config.root_node.basic_information.node_label,
            MATTER_NODE_LABEL,
            sizeof(node_config.root_node.basic_information.node_label));

    esp_matter::set_custom_device_instance_info_provider(&lab_device_instance_info_provider);

    node_t *node = node::create(&node_config, matter_attribute_callback, nullptr);
    ESP_RETURN_ON_FALSE(node != nullptr, ESP_FAIL, TAG, "Matter node create failed");

    endpoint::dimmable_light::config_t endpoint_config;
    endpoint_config.on_off.on_off = false;
    endpoint_config.level_control.current_level = nullable<uint8_t>(128);
    endpoint_t *endpoint = endpoint::dimmable_light::create(node, &endpoint_config, ENDPOINT_FLAG_NONE, nullptr);
    ESP_RETURN_ON_FALSE(endpoint != nullptr, ESP_FAIL, TAG, "Matter Dimmable Light endpoint create failed");

    lab_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG,
             "Matter identity manufacturer=%s model=%s vendor_id=0x%04X product_id=0x%04X",
             MATTER_VENDOR_NAME,
             MATTER_PRODUCT_NAME,
             CONFIG_DEVICE_VENDOR_ID,
             CONFIG_DEVICE_PRODUCT_ID);
    ESP_LOGI(TAG,
             "starting Matter Dimmable Light endpoint=%u setup_pin=%" PRIu32 " discriminator=%u",
             lab_endpoint_id,
             MATTER_SETUP_PIN,
             MATTER_SETUP_DISCRIMINATOR);
    log_manual_pairing_code();
    ESP_LOGI(TAG,
             "heap before Matter start internal_free=%u internal_largest=%u default_free=%u default_largest=%u chip_stack=%u",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             CONFIG_CHIP_TASK_STACK_SIZE);

    ESP_RETURN_ON_ERROR(esp_matter::start(matter_event_callback), TAG, "Matter start failed");

    const size_t fabric_count = chip::Server::GetInstance().GetFabricTable().FabricCount();
    ESP_LOGI(TAG, "Matter started fabric_count=%u", static_cast<unsigned>(fabric_count));
    return ESP_OK;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-S3 Supermini Matter lab");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    maybe_factory_reset();
    ESP_ERROR_CHECK(start_wifi());
    ESP_ERROR_CHECK(start_matter());
}
