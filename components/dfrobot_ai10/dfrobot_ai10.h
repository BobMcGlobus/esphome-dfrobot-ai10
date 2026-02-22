#pragma once

/*
 * DFRobot SEN0677 AI Binocular Vision Sensor - ESPHome External Component
 *
 * Protocol reverse-engineered from DFRobot_AI10 Arduino library (MIT License)
 * https://github.com/DFRobot/DFRobot_AI10
 */

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/log.h"
#include "esphome/core/automation.h"

#include <vector>
#include <string>
#include <cstring>

namespace esphome {
namespace dfrobot_ai10 {

// ===== Protocol Constants =====
static const uint8_t SYNC_H = 0xEF;
static const uint8_t SYNC_L = 0xAA;

// Message IDs (response)
static const uint8_t MID_REPLY = 0x00;  // Reply to a command
static const uint8_t MID_NOTE  = 0x01;  // Notification (face position, state)

// Command IDs
static const uint8_t CMD_RESET                = 0x10;
static const uint8_t CMD_VERIFY               = 0x12;
static const uint8_t CMD_ENROLL_SINGLE        = 0x1D;
static const uint8_t CMD_DEL_USER             = 0x20;
static const uint8_t CMD_DEL_ALL_USER         = 0x21;
static const uint8_t CMD_GET_USER_INFO        = 0x22;
static const uint8_t CMD_GET_ALL_USERID       = 0x24;
static const uint8_t CMD_SCAN_QR_CODE         = 0x70;
static const uint8_t CMD_SET_FACE_DISPLAY     = 0xB5;

// Result codes
static const uint8_t RES_SUCCESS              = 0x00;
static const uint8_t RES_REJECTED             = 0x01;
static const uint8_t RES_ABORTED              = 0x02;
static const uint8_t RES_FAILED_CAMERA        = 0x04;
static const uint8_t RES_FAILED_UNKNOWN       = 0x05;
static const uint8_t RES_FAILED_INVALID_PARAM = 0x06;
static const uint8_t RES_FAILED_NO_MEMORY     = 0x07;
static const uint8_t RES_FAILED_UNKNOWN_USER  = 0x08;
static const uint8_t RES_FAILED_MAX_USER      = 0x09;
static const uint8_t RES_FAILED_FACE_ENROLLED = 0x0A;
static const uint8_t RES_FAILED_LIVE_CHECK    = 0x0C;
static const uint8_t RES_FAILED_TIMEOUT       = 0x0D;

// Enroll data structure (must match Arduino struct layout: 35 bytes)
struct __attribute__((packed)) EnrollData {
  uint8_t admin;
  uint8_t user_name[32];
  uint8_t face_dir;
  uint8_t timeout;
};

// Recognition types
enum RecognitionType : uint8_t {
  TYPE_NONE = 0,
  TYPE_FACE = 1,
  TYPE_PALM = 2,
  TYPE_QR   = 3,
};

// Parser states
enum ParserState : uint8_t {
  STATE_WAIT_SYNC_H,
  STATE_WAIT_SYNC_L,
  STATE_READ_HEADER,
  STATE_READ_PAYLOAD,
};

// State Machine for Pending Actions (Non-blocking logic)
enum PendingAction : uint8_t {
  PENDING_NONE,
  PENDING_VERIFY,
  PENDING_ENROLL,
  PENDING_DELETE_USER,
  PENDING_DELETE_ALL,
  PENDING_GET_USERS,
  PENDING_SCAN_QR
};

class DFRobotAI10Component : public Component, public uart::UARTDevice {
 public:
  // Register callbacks for triggers
  void add_on_recognized_callback(std::function<void(std::string, uint16_t)> &&callback) {
    this->on_recognized_callback_.add(std::move(callback));
  }

  void add_on_qr_scanned_callback(std::function<void(std::string)> &&callback) {
    this->on_qr_scanned_callback_.add(std::move(callback));
  }

  float get_setup_priority() const override { return setup_priority::DATA; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  // Public API — call these from HA services or buttons
  void enroll_user(uint8_t admin, const char *name, uint8_t timeout);
  void start_recognition(uint8_t timeout, bool continuous);
  void delete_user(uint16_t uid);
  void delete_all_users();
  void get_all_user_ids();
  void scan_qr_code(uint8_t timeout);
  void send_reset();

  // Getters for template sensors
  uint16_t get_last_uid() const { return this->last_uid_; }
  const std::string &get_last_user_name() const { return this->last_user_name_; }
  RecognitionType get_last_type() const { return this->last_type_; }
  uint8_t get_last_result() const { return this->last_result_; }
  const std::string &get_last_qr_data() const { return this->last_qr_data_; }
  bool is_recognized() const { return this->recognized_; }
  bool is_face_detected() const { return this->face_detected_; }
  uint8_t get_user_count() const { return this->user_count_; }

 protected:
  // CallbackManagers
  CallbackManager<void(std::string, uint16_t)> on_recognized_callback_;
  CallbackManager<void(std::string)> on_qr_scanned_callback_;

  // Protocol helpers
  void send_packet_(const uint8_t *data, size_t len);
  static uint8_t xor_checksum_(const uint8_t *data, size_t len);
  void process_packet_(uint8_t msg_id, const uint8_t *payload, uint16_t len);
  void handle_reply_(const uint8_t *payload, uint16_t len);
  void handle_note_(const uint8_t *payload, uint16_t len);
  void log_hex_(const char *prefix, const uint8_t *data, size_t len);
  static const char *result_to_str_(uint8_t result);

  // Helper for State Machine
  void execute_pending_action_();

  // Parser vars
  ParserState parser_state_{STATE_WAIT_SYNC_H};
  uint8_t header_buf_[3];
  uint8_t header_pos_{0};
  uint8_t payload_buf_[300];
  uint16_t payload_len_{0};
  uint16_t payload_pos_{0};

  // State vars
  uint16_t last_uid_{0};
  std::string last_user_name_;
  RecognitionType last_type_{TYPE_NONE};
  uint8_t last_result_{0xFF};
  std::string last_qr_data_;
  bool recognized_{false};
  bool face_detected_{false};
  uint8_t user_count_{0};

  // State Machine vars
  PendingAction pending_action_{PENDING_NONE};
  uint8_t pending_cmd_{0}; // To track current active command
  
  // Stored parameters for pending actions
  uint8_t stored_timeout_{0};
  bool stored_continuous_{false};
  uint8_t stored_admin_{0};
  std::string stored_name_;
  uint16_t stored_uid_{0};
  
  uint32_t last_status_log_{0};
  bool setup_done_{false};
};

// Trigger Classes for ESPHome Automation
class RecognizedTrigger : public Trigger<std::string, uint16_t> {
 public:
  explicit RecognizedTrigger(DFRobotAI10Component *parent) {
    parent->add_on_recognized_callback([this](std::string name, uint16_t uid) {
      this->trigger(name, uid);
    });
  }
};

class QRScannedTrigger : public Trigger<std::string> {
 public:
  explicit QRScannedTrigger(DFRobotAI10Component *parent) {
    parent->add_on_qr_scanned_callback([this](std::string data) {
      this->trigger(data);
    });
  }
};

}  // namespace dfrobot_ai10
}  // namespace esphome