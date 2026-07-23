#include "ptouch_bridge.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"

namespace esphome {
namespace ptouch_bridge {

static const char *const TAG = "ptouch_bridge";
PtouchBridge *PtouchBridge::instance_ = nullptr;

void PtouchBridgeButton::press_action() {
  switch (this->kind_) {
    case RECONNECT:
      this->parent_->request_reconnect();
      break;
    case DISCONNECT:
      this->parent_->request_disconnect();
      break;
    case REFRESH_STATUS:
      this->parent_->request_status_refresh();
      break;
  }
}

void PtouchBridge::set_numeric_sensor(uint8_t kind, sensor::Sensor *value) {
  if (kind < this->numeric_sensors_.size())
    this->numeric_sensors_[kind] = value;
}

void PtouchBridge::set_text_sensor(uint8_t kind, text_sensor::TextSensor *value) {
  if (kind < this->text_sensors_.size())
    this->text_sensors_[kind] = value;
}

void PtouchBridge::set_binary_sensor(uint8_t kind, binary_sensor::BinarySensor *value) {
  if (kind < this->binary_sensors_.size())
    this->binary_sensors_[kind] = value;
}

float PtouchBridge::get_setup_priority() const { return setup_priority::AFTER_WIFI; }

bool PtouchBridge::parse_address_() {
  unsigned int bytes[ESP_BD_ADDR_LEN];
  if (std::sscanf(this->printer_address_text_.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &bytes[0], &bytes[1],
                  &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != ESP_BD_ADDR_LEN)
    return false;
  for (size_t i = 0; i < ESP_BD_ADDR_LEN; i++)
    this->printer_address_[i] = bytes[i];
  return true;
}

void PtouchBridge::setup() {
  ESP_LOGCONFIG(TAG, "Setting up P-touch bridge");
  instance_ = this;
  std::memset(&this->state_, 0, sizeof(this->state_));
  this->set_state_("starting", "Initializing Bluetooth Classic");
  this->set_last_print_result_("No print attempted this boot");

  if (!this->parse_address_()) {
    ESP_LOGE(TAG, "Invalid Bluetooth printer address");
    this->mark_failed();
    return;
  }

  this->events_ = xEventGroupCreate();
  this->transaction_mutex_ = xSemaphoreCreateMutex();
  this->write_done_ = xSemaphoreCreateBinary();
  this->receive_stream_ = xStreamBufferCreate(RX_BUFFER_SIZE, 1);
  if (this->events_ == nullptr || this->transaction_mutex_ == nullptr || this->write_done_ == nullptr ||
      this->receive_stream_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate Bluetooth resources");
    this->mark_failed();
    return;
  }
  xEventGroupSetBits(this->events_, CAN_WRITE_BIT);

  esp_err_t error = this->start_http_();
  if (error != ESP_OK) {
    this->set_state_("error", esp_err_to_name(error));
    ESP_LOGE(TAG, "HTTP server initialization failed: %s", esp_err_to_name(error));
    this->mark_failed();
    return;
  }
  this->set_state_("starting", "Waiting for Wi-Fi before starting Bluetooth");
  this->publish_state_();
}

esp_err_t PtouchBridge::initialize_bluetooth_() {
  esp_err_t error = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE)
    return error;

  esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  if ((error = esp_bt_controller_init(&controller_config)) != ESP_OK)
    return error;
  if ((error = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK)
    return error;

  esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
  if ((error = esp_bluedroid_init_with_cfg(&bluedroid_config)) != ESP_OK)
    return error;
  if ((error = esp_bluedroid_enable()) != ESP_OK)
    return error;
  if ((error = esp_bt_gap_register_callback(gap_callback_)) != ESP_OK)
    return error;

  esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
  if ((error = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability))) != ESP_OK)
    return error;
  esp_bt_pin_code_t pin = {};
  if ((error = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin)) != ESP_OK)
    return error;
  return this->initialize_spp_();
}

esp_err_t PtouchBridge::initialize_spp_() {
  esp_err_t error = esp_spp_register_callback(spp_callback_);
  if (error != ESP_OK)
    return error;
  this->spp_ready_ = false;
  this->spp_uninitialized_ = false;
  esp_spp_cfg_t config = {
      .mode = ESP_SPP_MODE_CB,
      .enable_l2cap_ertm = true,
      .tx_buffer_size = 0,
  };
  return esp_spp_enhanced_init(&config);
}

void PtouchBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "P-touch Wi-Fi/Bluetooth bridge:");
  ESP_LOGCONFIG(TAG, "  Printer address: %s", this->printer_address_text_.c_str());
  ESP_LOGCONFIG(TAG, "  HTTP authentication: %s", this->http_token_.empty() ? "disabled" : "enabled");
  ESP_LOGCONFIG(TAG, "  Counters: RAM-only; Home Assistant retains history");
}

void PtouchBridge::loop() {
  if (this->is_failed())
    return;

  const uint32_t now = millis();
  if (!this->bluetooth_initialized_) {
    if (!network::is_connected()) {
      this->wifi_connected_since_ms_ = 0;
      this->publish_state_();
      return;
    }
    if (this->wifi_connected_since_ms_ == 0) {
      this->wifi_connected_since_ms_ = now;
      this->set_state_("starting", "Waiting for Wi-Fi to settle");
      this->publish_state_();
      return;
    }
    if (now - this->wifi_connected_since_ms_ < WIFI_SETTLE_MS) {
      this->publish_state_();
      return;
    }
    esp_err_t error = this->initialize_bluetooth_();
    if (error != ESP_OK) {
      this->set_state_("error", esp_err_to_name(error));
      ESP_LOGE(TAG, "Bluetooth initialization failed: %s", esp_err_to_name(error));
      this->mark_failed();
      return;
    }
    this->bluetooth_initialized_ = true;
    ESP_LOGI(TAG, "Bluetooth initialized after Wi-Fi was stable for %u ms", WIFI_SETTLE_MS);
  }

  if (this->spp_reset_requested_ || this->spp_resetting_ || this->spp_reinit_after_ms_ != 0) {
    this->service_spp_reset_(now);
    this->publish_state_();
    return;
  }

  if (!this->spp_ready_) {
    this->publish_state_();
    return;
  }

  if (this->disconnect_requested_) {
    this->disconnect_requested_ = false;
    this->disconnect_();
  }
  if (this->reconnect_requested_) {
    this->reconnect_requested_ = false;
    this->disconnect_();
    this->last_connect_attempt_ms_ = 0;
  }

  EventBits_t bits = xEventGroupGetBits(this->events_);
  if (this->connect_in_progress_ && now - this->last_connect_attempt_ms_ >= CONNECT_TIMEOUT_MS) {
    this->request_spp_reset_("Bluetooth connection attempt timed out");
    this->service_spp_reset_(now);
    this->publish_state_();
    return;
  }
  if (!(bits & CONNECTED_BIT) && !this->connect_in_progress_ &&
      (this->last_connect_attempt_ms_ == 0 || now - this->last_connect_attempt_ms_ >= 5000)) {
    this->begin_connection_();
  }

  if (this->status_refresh_requested_ && !this->status_refresh_running_ &&
      uxSemaphoreGetCount(this->transaction_mutex_) > 0) {
    this->status_refresh_requested_ = false;
    this->status_refresh_running_ = true;
    BaseType_t created = xTaskCreate(
        [](void *argument) {
          auto *self = static_cast<PtouchBridge *>(argument);
          uint8_t frame[STATUS_BYTES];
          esp_err_t error = self->begin_transaction_(15000);
          if (error == ESP_OK) {
            const uint8_t zeros[100] = {};
            static const uint8_t command[] = {0x1B, '@', 0x1B, 'i', 'S'};
            error = self->send_(zeros, sizeof(zeros), 10000);
            if (error == ESP_OK)
              error = self->send_(command, sizeof(command), 10000);
            if (error == ESP_OK)
              error = self->receive_(frame, sizeof(frame), 5000);
            if (error == ESP_OK) {
              self->update_status_(frame);
              self->end_transaction_();
            } else {
              self->abort_transaction_();
            }
          }
          self->status_refresh_running_ = false;
          vTaskDelete(nullptr);
        },
        "ptouch_status", 4096, this, 1, nullptr);
    if (created != pdPASS) {
      this->status_refresh_running_ = false;
      this->status_refresh_requested_ = true;
      ESP_LOGE(TAG, "Could not start printer status task");
    }
  }

  this->publish_state_();
}

void PtouchBridge::request_spp_reset_(const char *detail) {
  if (this->spp_reset_requested_ || this->spp_resetting_)
    return;
  ESP_LOGW(TAG, "%s; resetting SPP", detail);
  this->fail_connection_(detail);
  portENTER_CRITICAL(&this->state_lock_);
  this->state_.bluetooth_recoveries++;
  strlcpy(this->state_.last_recovery_reason, detail, sizeof(this->state_.last_recovery_reason));
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
  this->spp_reset_requested_ = true;
}

void PtouchBridge::service_spp_reset_(uint32_t now) {
  if (this->spp_reset_requested_ && !this->spp_resetting_) {
    this->spp_reset_requested_ = false;
    this->spp_resetting_ = true;
    this->spp_ready_ = false;
    this->spp_uninitialized_ = false;
    this->spp_reset_started_ms_ = now;
    this->set_state_("recovering", "Resetting Bluetooth SPP");
    esp_err_t error = esp_spp_deinit();
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "Could not deinitialize SPP: %s; rebooting", esp_err_to_name(error));
      App.safe_reboot();
    }
    return;
  }

  if (this->spp_resetting_) {
    if (!this->spp_uninitialized_) {
      if (now - this->spp_reset_started_ms_ >= SPP_RESET_TIMEOUT_MS) {
        ESP_LOGE(TAG, "SPP reset timed out; rebooting");
        App.safe_reboot();
      }
      return;
    }
    this->spp_resetting_ = false;
    this->spp_uninitialized_ = false;
    this->spp_reinit_after_ms_ = now + SPP_REINIT_DELAY_MS;
    this->set_state_("recovering", "Restarting Bluetooth SPP");
    ESP_LOGI(TAG, "SPP deinitialized; scheduling reinitialization");
    return;
  }

  if (this->spp_reinit_after_ms_ != 0 &&
      static_cast<int32_t>(now - this->spp_reinit_after_ms_) >= 0) {
    esp_err_t error = this->initialize_spp_();
    if (error == ESP_OK) {
      this->spp_reinit_after_ms_ = 0;
      this->set_state_("recovering", "Waiting for Bluetooth SPP");
      ESP_LOGI(TAG, "SPP reinitialization requested");
    } else {
      this->spp_reinit_after_ms_ = now + 5000;
      ESP_LOGE(TAG, "Could not reinitialize SPP: %s; retrying", esp_err_to_name(error));
    }
  }
}

void PtouchBridge::request_reconnect() { this->reconnect_requested_ = true; }
void PtouchBridge::request_disconnect() { this->disconnect_requested_ = true; }
void PtouchBridge::request_status_refresh() { this->status_refresh_requested_ = true; }

void PtouchBridge::set_state_(const char *phase, const char *detail) {
  portENTER_CRITICAL(&this->state_lock_);
  if (phase != nullptr)
    strlcpy(this->state_.phase, phase, sizeof(this->state_.phase));
  if (detail != nullptr)
    strlcpy(this->state_.detail, detail, sizeof(this->state_.detail));
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
}

void PtouchBridge::set_last_print_result_(const char *result) {
  portENTER_CRITICAL(&this->state_lock_);
  strlcpy(this->state_.last_print_result, result, sizeof(this->state_.last_print_result));
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
}

void PtouchBridge::fail_connection_(const char *detail) {
  this->connect_in_progress_ = false;
  portENTER_CRITICAL(&this->state_lock_);
  this->state_.connected = false;
  strlcpy(this->state_.phase, "disconnected", sizeof(this->state_.phase));
  strlcpy(this->state_.detail, detail, sizeof(this->state_.detail));
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
  xEventGroupClearBits(this->events_, CONNECTED_BIT);
  xEventGroupSetBits(this->events_, CONNECT_FAILED_BIT | CAN_WRITE_BIT);
  xSemaphoreGive(this->write_done_);
}

void PtouchBridge::begin_connection_() {
  this->last_connect_attempt_ms_ = millis();
  this->connect_in_progress_ = true;
  xEventGroupClearBits(this->events_, CONNECTED_BIT | CONNECT_FAILED_BIT);
  portENTER_CRITICAL(&this->state_lock_);
  this->state_.connection_attempts++;
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
  this->set_state_("searching", "Finding printer SPP service");
  esp_err_t error = esp_spp_start_discovery(this->printer_address_);
  if (error != ESP_OK)
    this->fail_connection_(esp_err_to_name(error));
}

esp_err_t PtouchBridge::ensure_connected_(uint32_t timeout_ms) {
  if (xEventGroupGetBits(this->events_) & CONNECTED_BIT)
    return ESP_OK;
  if (!this->connect_in_progress_)
    this->begin_connection_();
  EventBits_t result = xEventGroupWaitBits(this->events_, CONNECTED_BIT | CONNECT_FAILED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
  if (result & CONNECTED_BIT)
    return ESP_OK;
  if (!(result & CONNECT_FAILED_BIT)) {
    this->request_spp_reset_("Timed out connecting to Bluetooth printer");
    return ESP_ERR_TIMEOUT;
  }
  return ESP_FAIL;
}

esp_err_t PtouchBridge::begin_transaction_(uint32_t timeout_ms) {
  if (xSemaphoreTake(this->transaction_mutex_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  uint8_t discard[64];
  while (xStreamBufferReceive(this->receive_stream_, discard, sizeof(discard), 0) != 0) {
  }
  esp_err_t error = this->ensure_connected_(timeout_ms);
  if (error != ESP_OK)
    xSemaphoreGive(this->transaction_mutex_);
  return error;
}

esp_err_t PtouchBridge::send_(const uint8_t *data, size_t length, uint32_t timeout_ms) {
  size_t sent = 0;
  while (sent < length) {
    if (!(xEventGroupGetBits(this->events_) & CONNECTED_BIT))
      return ESP_ERR_INVALID_STATE;
    EventBits_t writable = xEventGroupWaitBits(this->events_, CAN_WRITE_BIT | CONNECT_FAILED_BIT, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(timeout_ms));
    if (!(writable & CAN_WRITE_BIT) || (writable & CONNECT_FAILED_BIT))
      return ESP_ERR_TIMEOUT;
    while (xSemaphoreTake(this->write_done_, 0) == pdTRUE) {
    }
    size_t chunk = std::min(length - sent, WRITE_CHUNK);
    this->last_write_length_ = 0;
    this->last_write_status_ = ESP_SPP_FAILURE;
    esp_err_t error = esp_spp_write(this->spp_handle_, chunk, const_cast<uint8_t *>(data + sent));
    if (error != ESP_OK)
      return error;
    if (xSemaphoreTake(this->write_done_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
      return ESP_ERR_TIMEOUT;
    if (this->last_write_status_ != ESP_SPP_SUCCESS || this->last_write_length_ == 0)
      return ESP_FAIL;
    sent += this->last_write_length_;
  }
  return ESP_OK;
}

esp_err_t PtouchBridge::receive_(uint8_t *data, size_t length, uint32_t timeout_ms) {
  size_t received = 0;
  TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
  while (received < length) {
    TickType_t now = xTaskGetTickCount();
    if (now >= deadline)
      return ESP_ERR_TIMEOUT;
    size_t count =
        xStreamBufferReceive(this->receive_stream_, data + received, length - received, deadline - now);
    if (count == 0)
      return ESP_ERR_TIMEOUT;
    received += count;
  }
  return ESP_OK;
}

void PtouchBridge::end_transaction_() { xSemaphoreGive(this->transaction_mutex_); }

void PtouchBridge::abort_transaction_() {
  this->disconnect_();
  xSemaphoreGive(this->transaction_mutex_);
}

void PtouchBridge::disconnect_() {
  uint32_t handle = this->spp_handle_;
  if (handle == 0 && this->connect_in_progress_) {
    this->request_spp_reset_("Bluetooth connection cancelled");
    return;
  }
  this->spp_handle_ = 0;
  this->connect_in_progress_ = false;
  this->fail_connection_("Bluetooth printer disconnected");
  if (handle != 0)
    esp_spp_disconnect(handle);
}

void PtouchBridge::gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  auto *self = instance_;
  if (self == nullptr)
    return;
  switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        portENTER_CRITICAL(&self->state_lock_);
        self->state_.paired = true;
        strlcpy(self->state_.name, reinterpret_cast<const char *>(param->auth_cmpl.device_name),
                sizeof(self->state_.name));
        self->state_.revision++;
        portEXIT_CRITICAL(&self->state_lock_);
        ESP_LOGI(TAG, "Bluetooth authentication succeeded");
      } else {
        ESP_LOGW(TAG, "Bluetooth authentication failed: %d", param->auth_cmpl.stat);
      }
      break;
    case ESP_BT_GAP_CFM_REQ_EVT:
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
      esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
      break;
    }
    default:
      break;
  }
}

void PtouchBridge::spp_callback_(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  auto *self = instance_;
  if (self == nullptr)
    return;
  switch (event) {
    case ESP_SPP_INIT_EVT:
      if (param->init.status == ESP_SPP_SUCCESS) {
        self->spp_ready_ = true;
        esp_bt_gap_set_device_name(App.get_name().c_str());
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        self->set_state_("disconnected", "Waiting for printer");
        ESP_LOGI(TAG, "Bluetooth SPP initialized");
      } else {
        self->fail_connection_("Bluetooth SPP initialization failed");
      }
      break;
    case ESP_SPP_UNINIT_EVT:
      self->spp_ready_ = false;
      self->spp_uninitialized_ = true;
      ESP_LOGI(TAG, "Bluetooth SPP uninitialized: %d", param->uninit.status);
      break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
      if (self->spp_reset_requested_ || self->spp_resetting_)
        break;
      if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
        self->set_state_("connecting", "Opening printer SPP channel");
        esp_err_t error = esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER, param->disc_comp.scn[0],
                                          self->printer_address_);
        if (error != ESP_OK)
          self->request_spp_reset_(esp_err_to_name(error));
      } else {
        self->fail_connection_("Printer SPP service was not found");
      }
      break;
    case ESP_SPP_CL_INIT_EVT:
      ESP_LOGI(TAG, "SPP client initiation: status=%d handle=%lu", param->cl_init.status,
               static_cast<unsigned long>(param->cl_init.handle));
      if (param->cl_init.status != ESP_SPP_SUCCESS)
        self->request_spp_reset_("Could not initiate printer SPP connection");
      break;
    case ESP_SPP_OPEN_EVT:
      self->connect_in_progress_ = false;
      if (self->spp_reset_requested_ || self->spp_resetting_)
        break;
      if (param->open.status == ESP_SPP_SUCCESS) {
        self->spp_handle_ = param->open.handle;
        portENTER_CRITICAL(&self->state_lock_);
        self->state_.connected = true;
        self->state_.paired = true;
        self->state_.bluetooth_connections++;
        strlcpy(self->state_.phase, "ready", sizeof(self->state_.phase));
        strlcpy(self->state_.detail, "Bluetooth printer connected", sizeof(self->state_.detail));
        self->state_.revision++;
        portEXIT_CRITICAL(&self->state_lock_);
        xEventGroupClearBits(self->events_, CONNECT_FAILED_BIT);
        xEventGroupSetBits(self->events_, CONNECTED_BIT | CAN_WRITE_BIT);
        // A background connection has no browser request to obtain media
        // information. Queue the same status transaction as the manual
        // refresh button. If this connection belongs to an existing status
        // or print transaction, update_status_ clears the queued duplicate.
        self->status_refresh_requested_ = true;
        ESP_LOGI(TAG, "Printer SPP connection opened");
      } else {
        self->request_spp_reset_("Could not open printer SPP channel");
      }
      break;
    case ESP_SPP_CLOSE_EVT:
      self->spp_handle_ = 0;
      if (!self->spp_resetting_)
        self->fail_connection_("Bluetooth printer disconnected");
      break;
    case ESP_SPP_DATA_IND_EVT:
      if (xStreamBufferSend(self->receive_stream_, param->data_ind.data, param->data_ind.len, 0) !=
          param->data_ind.len)
        ESP_LOGE(TAG, "Bluetooth receive buffer overflow");
      break;
    case ESP_SPP_WRITE_EVT:
      self->last_write_status_ = param->write.status;
      self->last_write_length_ = param->write.len;
      if (param->write.cong)
        xEventGroupClearBits(self->events_, CAN_WRITE_BIT);
      else
        xEventGroupSetBits(self->events_, CAN_WRITE_BIT);
      xSemaphoreGive(self->write_done_);
      break;
    case ESP_SPP_CONG_EVT:
      if (param->cong.cong)
        xEventGroupClearBits(self->events_, CAN_WRITE_BIT);
      else
        xEventGroupSetBits(self->events_, CAN_WRITE_BIT);
      break;
    default:
      break;
  }
}

std::string PtouchBridge::cartridge_description_(const uint8_t frame[STATUS_BYTES]) {
  if ((frame[8] & 0x01) != 0 || frame[10] == 0)
    return "No cassette";
  const char *type = frame[11] == 1 ? "laminated" : frame[11] == 3 ? "non-laminated" : "unknown type";
  const char *tape = "unknown";
  switch (frame[24]) {
    case 1: tape = "white"; break;
    case 3: tape = "clear"; break;
    case 4: tape = "red"; break;
    case 5: tape = "blue"; break;
    case 6: tape = "yellow"; break;
    case 7: tape = "green"; break;
    case 8: tape = "black"; break;
  }
  const char *text = "unknown";
  switch (frame[25]) {
    case 1: text = "white"; break;
    case 4: text = "red"; break;
    case 5: text = "blue"; break;
    case 8: text = "black"; break;
    case 10: text = "gold"; break;
  }
  char value[64];
  std::snprintf(value, sizeof(value), "%u mm %s, %s on %s", frame[10], type, text, tape);
  return value;
}

void PtouchBridge::update_status_(const uint8_t frame[STATUS_BYTES]) {
  if (std::memcmp(frame, "\x80\x20" "B0", 4) != 0)
    return;
  this->status_refresh_requested_ = false;
  std::string cartridge = cartridge_description_(frame);
  bool loaded = (frame[8] & 0x01) == 0 && frame[10] != 0;
  portENTER_CRITICAL(&this->state_lock_);
  this->state_.media_loaded = loaded;
  strlcpy(this->state_.current_cartridge, cartridge.c_str(), sizeof(this->state_.current_cartridge));
  if (loaded)
    strlcpy(this->state_.last_cartridge, cartridge.c_str(), sizeof(this->state_.last_cartridge));
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
}

bool PtouchBridge::terminal_status_(const uint8_t frame[STATUS_BYTES]) {
  return frame[8] != 0 || frame[9] != 0 || frame[18] == 1 || frame[18] == 2;
}

void PtouchBridge::count_print_(uint32_t tape_length_dots) {
  portENTER_CRITICAL(&this->state_lock_);
  this->state_.labels_printed++;
  this->state_.tape_used_mm += static_cast<double>(tape_length_dots) * 25.4 / 360.0;
  strlcpy(this->state_.last_print_result, "Completed", sizeof(this->state_.last_print_result));
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
}

void PtouchBridge::count_failed_print_(const char *detail) {
  portENTER_CRITICAL(&this->state_lock_);
  this->state_.failed_prints++;
  strlcpy(this->state_.last_print_result, detail, sizeof(this->state_.last_print_result));
  this->state_.revision++;
  portEXIT_CRITICAL(&this->state_lock_);
}

void PtouchBridge::publish_state_() {
  State snapshot;
  portENTER_CRITICAL(&this->state_lock_);
  if (this->state_.revision == this->published_revision_) {
    portEXIT_CRITICAL(&this->state_lock_);
    return;
  }
  std::memcpy(&snapshot, &this->state_, sizeof(snapshot));
  portEXIT_CRITICAL(&this->state_lock_);

  const float numeric_values[] = {
      static_cast<float>(snapshot.labels_printed),
      static_cast<float>(snapshot.tape_used_mm),
      static_cast<float>(snapshot.failed_prints),
      static_cast<float>(snapshot.bluetooth_connections),
      static_cast<float>(snapshot.connection_attempts),
      static_cast<float>(snapshot.bluetooth_recoveries),
  };
  for (size_t i = 0; i < this->numeric_sensors_.size(); i++)
    if (this->numeric_sensors_[i] != nullptr)
      this->numeric_sensors_[i]->publish_state(numeric_values[i]);

  const char *text_values[] = {
      snapshot.phase,
      snapshot.detail,
      snapshot.current_cartridge[0] ? snapshot.current_cartridge : "Unknown",
      snapshot.last_cartridge[0] ? snapshot.last_cartridge : "Unknown",
      snapshot.last_print_result,
      this->printer_address_text_.c_str(),
      snapshot.last_recovery_reason[0] ? snapshot.last_recovery_reason : "None",
  };
  for (size_t i = 0; i < this->text_sensors_.size(); i++)
    if (this->text_sensors_[i] != nullptr)
      this->text_sensors_[i]->publish_state(text_values[i]);

  const bool binary_values[] = {snapshot.connected, snapshot.media_loaded};
  for (size_t i = 0; i < this->binary_sensors_.size(); i++)
    if (this->binary_sensors_[i] != nullptr)
      this->binary_sensors_[i]->publish_state(binary_values[i]);

  this->published_revision_ = snapshot.revision;
}

bool PtouchBridge::authorized_(httpd_req_t *request) const {
  if (this->http_token_.empty())
    return true;
  size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
  if (length == 0 || length > 256)
    return false;
  char value[257];
  if (httpd_req_get_hdr_value_str(request, "Authorization", value, sizeof(value)) != ESP_OK)
    return false;
  return std::string(value) == "Bearer " + this->http_token_;
}

esp_err_t PtouchBridge::send_information_(httpd_req_t *request, const char *status, const char *error) {
  State snapshot;
  portENTER_CRITICAL(&this->state_lock_);
  std::memcpy(&snapshot, &this->state_, sizeof(snapshot));
  portEXIT_CRITICAL(&this->state_lock_);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 1);
  cJSON_AddStringToObject(root, "hostname", App.get_name().c_str());
  cJSON_AddNumberToObject(root, "uptimeMs", static_cast<double>(esp_timer_get_time() / 1000));

  cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
  wifi_ap_record_t access_point{};
  bool wifi_connected = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
  cJSON_AddBoolToObject(wifi, "connected", wifi_connected);
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip{};
  char address[16] = "";
  if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK)
    esp_ip4addr_ntoa(&ip.ip, address, sizeof(address));
  cJSON_AddStringToObject(wifi, "address", address);
  if (wifi_connected)
    cJSON_AddNumberToObject(wifi, "rssi", access_point.rssi);

  cJSON *bluetooth = cJSON_AddObjectToObject(root, "bluetooth");
  cJSON_AddStringToObject(bluetooth, "address", this->printer_address_text_.c_str());
  cJSON_AddStringToObject(bluetooth, "name", snapshot.name);
  cJSON_AddStringToObject(bluetooth, "phase", snapshot.phase);
  cJSON_AddStringToObject(bluetooth, "detail", snapshot.detail);
  cJSON_AddBoolToObject(bluetooth, "paired", snapshot.paired);
  cJSON_AddBoolToObject(bluetooth, "connected", snapshot.connected);
  if (snapshot.has_rssi)
    cJSON_AddNumberToObject(bluetooth, "rssi", snapshot.rssi);
  if (error != nullptr)
    cJSON_AddStringToObject(root, "error", error);

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (body == nullptr)
    return httpd_resp_send_500(request);
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_sendstr(request, body);
  free(body);
  return result;
}

esp_err_t PtouchBridge::require_authorization_(httpd_req_t *request) {
  if (instance_->authorized_(request))
    return ESP_OK;
  instance_->send_information_(request, "401 Unauthorized", "unauthorized");
  return ESP_ERR_INVALID_STATE;
}

esp_err_t PtouchBridge::info_handler_(httpd_req_t *request) {
  if (require_authorization_(request) != ESP_OK)
    return ESP_OK;
  return instance_->send_information_(request, "200 OK");
}

esp_err_t PtouchBridge::send_status_frame_(httpd_req_t *request, const uint8_t frame[STATUS_BYTES]) {
  httpd_resp_set_type(request, "application/octet-stream");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, reinterpret_cast<const char *>(frame), STATUS_BYTES);
}

esp_err_t PtouchBridge::status_handler_(httpd_req_t *request) {
  if (require_authorization_(request) != ESP_OK)
    return ESP_OK;
  auto *self = instance_;
  esp_err_t error = self->begin_transaction_(15000);
  if (error != ESP_OK)
    return self->send_information_(request, "503 Service Unavailable", self->state_.detail);

  const uint8_t zeros[100] = {};
  static const uint8_t command[] = {0x1B, '@', 0x1B, 'i', 'S'};
  error = self->send_(zeros, sizeof(zeros), 10000);
  if (error == ESP_OK)
    error = self->send_(command, sizeof(command), 10000);
  uint8_t frame[STATUS_BYTES];
  if (error == ESP_OK)
    error = self->receive_(frame, sizeof(frame), 5000);
  if (error != ESP_OK) {
    self->abort_transaction_();
    return self->send_information_(request, "503 Service Unavailable", esp_err_to_name(error));
  }
  self->update_status_(frame);
  self->end_transaction_();
  return send_status_frame_(request, frame);
}

uint32_t PtouchBridge::tape_length_header_(httpd_req_t *request) {
  size_t length = httpd_req_get_hdr_value_len(request, "X-Ptouch-Tape-Length-Dots");
  if (length == 0 || length > 15)
    return 0;
  char value[16];
  if (httpd_req_get_hdr_value_str(request, "X-Ptouch-Tape-Length-Dots", value, sizeof(value)) != ESP_OK)
    return 0;
  char *end = nullptr;
  unsigned long dots = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0' || dots > UINT32_MAX)
    return 0;
  return static_cast<uint32_t>(dots);
}

esp_err_t PtouchBridge::page_handler_(httpd_req_t *request) {
  if (require_authorization_(request) != ESP_OK)
    return ESP_OK;
  auto *self = instance_;
  if (request->content_len <= 0 || request->content_len > MAX_PAGE_BYTES)
    return self->send_information_(request, "400 Bad Request", "invalid page length");
  uint32_t tape_length_dots = tape_length_header_(request);

  esp_err_t error = self->begin_transaction_(15000);
  if (error != ESP_OK) {
    self->count_failed_print_(self->state_.detail);
    return self->send_information_(request, "503 Service Unavailable", self->state_.detail);
  }

  uint8_t buffer[1024];
  int remaining = request->content_len;
  bool submitted = false;
  while (remaining > 0) {
    int wanted = std::min(remaining, static_cast<int>(sizeof(buffer)));
    int count = httpd_req_recv(request, reinterpret_cast<char *>(buffer), wanted);
    if (count == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (count <= 0) {
      error = ESP_FAIL;
      break;
    }
    submitted = true;
    error = self->send_(buffer, count, 10000);
    if (error != ESP_OK)
      break;
    remaining -= count;
  }

  uint8_t frame[STATUS_BYTES];
  if (error == ESP_OK) {
    int64_t deadline = esp_timer_get_time() + 120 * 1000000LL;
    do {
      int64_t left = deadline - esp_timer_get_time();
      if (left <= 0) {
        error = ESP_ERR_TIMEOUT;
        break;
      }
      error = self->receive_(frame, sizeof(frame), static_cast<uint32_t>(left / 1000));
      if (error != ESP_OK)
        break;
    } while (!terminal_status_(frame));
  }

  if (error != ESP_OK) {
    ESP_LOGE(TAG, "Page transaction failed after submission=%d: %s", submitted, esp_err_to_name(error));
    char detail[96];
    std::snprintf(detail, sizeof(detail), "%s%s", submitted ? "Outcome unknown: " : "",
                  esp_err_to_name(error));
    self->count_failed_print_(detail);
    self->abort_transaction_();
    return self->send_information_(request, "503 Service Unavailable", detail);
  }

  self->update_status_(frame);
  self->end_transaction_();
  if (frame[8] == 0 && frame[9] == 0 && frame[18] == 1)
    self->count_print_(tape_length_dots);
  else
    self->count_failed_print_("Printer reported an error");
  return send_status_frame_(request, frame);
}

esp_err_t PtouchBridge::disconnect_handler_(httpd_req_t *request) {
  if (require_authorization_(request) != ESP_OK)
    return ESP_OK;
  instance_->disconnect_();
  return instance_->send_information_(request, "200 OK");
}

esp_err_t PtouchBridge::start_http_() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;
  esp_err_t error = httpd_start(&this->server_, &config);
  if (error != ESP_OK)
    return error;

  const httpd_uri_t routes[] = {
      {.uri = "/api/v1/bridge/info", .method = HTTP_GET, .handler = info_handler_, .user_ctx = nullptr},
      {.uri = "/api/v1/bridge/status", .method = HTTP_POST, .handler = status_handler_, .user_ctx = nullptr},
      {.uri = "/api/v1/bridge/page", .method = HTTP_POST, .handler = page_handler_, .user_ctx = nullptr},
      {.uri = "/api/v1/bridge/disconnect", .method = HTTP_POST, .handler = disconnect_handler_, .user_ctx = nullptr},
  };
  for (const auto &route : routes) {
    error = httpd_register_uri_handler(this->server_, &route);
    if (error != ESP_OK)
      return error;
  }
  ESP_LOGI(TAG, "Bridge HTTP API listening on port 80");
  return ESP_OK;
}

}  // namespace ptouch_bridge
}  // namespace esphome
