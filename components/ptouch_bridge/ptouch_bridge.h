#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

#include "esp_gap_bt_api.h"
#include "esp_http_server.h"
#include "esp_spp_api.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

namespace esphome {
namespace ptouch_bridge {

enum NumericSensorKind : uint8_t {
  LABELS_PRINTED = 0,
  TAPE_USED = 1,
  FAILED_PRINTS = 2,
  BLUETOOTH_CONNECTIONS = 3,
  CONNECTION_ATTEMPTS = 4,
  NUMERIC_SENSOR_COUNT = 5,
};

enum TextSensorKind : uint8_t {
  CONNECTION_PHASE = 0,
  CONNECTION_DETAIL = 1,
  CURRENT_CARTRIDGE = 2,
  LAST_CARTRIDGE = 3,
  LAST_PRINT_RESULT = 4,
  PRINTER_ADDRESS = 5,
  TEXT_SENSOR_COUNT = 6,
};

enum BinarySensorKind : uint8_t {
  PRINTER_CONNECTED = 0,
  MEDIA_LOADED = 1,
  BINARY_SENSOR_COUNT = 2,
};

enum ButtonKind : uint8_t {
  RECONNECT = 0,
  DISCONNECT = 1,
  REFRESH_STATUS = 2,
};

class PtouchBridge;

class PtouchBridgeButton : public button::Button {
 public:
  PtouchBridgeButton(PtouchBridge *parent, uint8_t kind) : parent_(parent), kind_(kind) {}

 protected:
  void press_action() override;
  PtouchBridge *parent_;
  uint8_t kind_;
};

class PtouchBridge : public Component {
 public:
  void set_printer_address(const std::string &address) { this->printer_address_text_ = address; }
  void set_http_token(const std::string &token) { this->http_token_ = token; }
  void set_numeric_sensor(uint8_t kind, sensor::Sensor *value);
  void set_text_sensor(uint8_t kind, text_sensor::TextSensor *value);
  void set_binary_sensor(uint8_t kind, binary_sensor::BinarySensor *value);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void request_reconnect();
  void request_disconnect();
  void request_status_refresh();

 protected:
  static constexpr size_t STATUS_BYTES = 32;
  static constexpr size_t MAX_PAGE_BYTES = 300 * 1024;
  static constexpr size_t WRITE_CHUNK = 512;
  static constexpr size_t RX_BUFFER_SIZE = 2048;

  static constexpr EventBits_t CONNECTED_BIT = BIT0;
  static constexpr EventBits_t CONNECT_FAILED_BIT = BIT1;
  static constexpr EventBits_t CAN_WRITE_BIT = BIT2;

  struct State {
    char name[64]{};
    char phase[24]{};
    char detail[128]{};
    char current_cartridge[64]{};
    char last_cartridge[64]{};
    char last_print_result[96]{};
    bool paired{false};
    bool connected{false};
    bool media_loaded{false};
    int rssi{0};
    bool has_rssi{false};
    uint32_t labels_printed{0};
    double tape_used_mm{0};
    uint32_t failed_prints{0};
    uint32_t bluetooth_connections{0};
    uint32_t connection_attempts{0};
    uint32_t revision{0};
  };

  void set_state_(const char *phase, const char *detail);
  void set_last_print_result_(const char *result);
  void fail_connection_(const char *detail);
  bool parse_address_();
  esp_err_t initialize_bluetooth_();
  esp_err_t ensure_connected_(uint32_t timeout_ms);
  esp_err_t begin_transaction_(uint32_t timeout_ms);
  esp_err_t send_(const uint8_t *data, size_t length, uint32_t timeout_ms);
  esp_err_t receive_(uint8_t *data, size_t length, uint32_t timeout_ms);
  void end_transaction_();
  void abort_transaction_();
  void disconnect_();
  void begin_connection_();
  void publish_state_();
  void update_status_(const uint8_t frame[STATUS_BYTES]);
  void count_print_(uint32_t tape_length_dots);
  void count_failed_print_(const char *detail);
  static bool terminal_status_(const uint8_t frame[STATUS_BYTES]);
  static std::string cartridge_description_(const uint8_t frame[STATUS_BYTES]);

  bool authorized_(httpd_req_t *request) const;
  esp_err_t start_http_();
  esp_err_t send_information_(httpd_req_t *request, const char *status,
                              const char *error = nullptr);
  static esp_err_t info_handler_(httpd_req_t *request);
  static esp_err_t status_handler_(httpd_req_t *request);
  static esp_err_t page_handler_(httpd_req_t *request);
  static esp_err_t disconnect_handler_(httpd_req_t *request);
  static esp_err_t require_authorization_(httpd_req_t *request);
  static esp_err_t send_status_frame_(httpd_req_t *request,
                                      const uint8_t frame[STATUS_BYTES]);
  static uint32_t tape_length_header_(httpd_req_t *request);

  static void gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void spp_callback_(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
  static PtouchBridge *instance_;

  std::string printer_address_text_;
  std::string http_token_;
  esp_bd_addr_t printer_address_{};
  httpd_handle_t server_{nullptr};
  EventGroupHandle_t events_{nullptr};
  SemaphoreHandle_t transaction_mutex_{nullptr};
  SemaphoreHandle_t write_done_{nullptr};
  StreamBufferHandle_t receive_stream_{nullptr};
  portMUX_TYPE state_lock_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t spp_handle_{0};
  volatile size_t last_write_length_{0};
  volatile esp_spp_status_t last_write_status_{ESP_SPP_FAILURE};
  volatile bool connect_in_progress_{false};
  State state_{};
  uint32_t published_revision_{UINT32_MAX};
  uint32_t last_connect_attempt_ms_{0};
  bool reconnect_requested_{false};
  bool disconnect_requested_{false};
  bool status_refresh_requested_{false};
  bool bluetooth_initialized_{false};

  std::array<sensor::Sensor *, NUMERIC_SENSOR_COUNT> numeric_sensors_{};
  std::array<text_sensor::TextSensor *, TEXT_SENSOR_COUNT> text_sensors_{};
  std::array<binary_sensor::BinarySensor *, BINARY_SENSOR_COUNT> binary_sensors_{};

  friend class PtouchBridgeButton;
};

}  // namespace ptouch_bridge
}  // namespace esphome
