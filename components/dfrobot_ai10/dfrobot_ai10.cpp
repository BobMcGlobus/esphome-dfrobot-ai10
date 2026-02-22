#include "dfrobot_ai10.h"

namespace esphome {
namespace dfrobot_ai10 {

static const char *const TAG = "dfrobot_ai10";

void DFRobotAI10Component::setup() {
  ESP_LOGI(TAG, "SEN0677 AI Vision Sensor — initializing...");
  // Boot-up delay (datasheet requirement: 900ms–2.5s)
  // This is in setup(), so blocking here is acceptable and safer for init sequence.
  delay(3000);

  // Send initial reset (mirrors Arduino library begin())
  this->send_reset();
  this->setup_done_ = true;
  ESP_LOGI(TAG, "SEN0677 reset sent, waiting for reply...");
}

void DFRobotAI10Component::dump_config() {
  ESP_LOGCONFIG(TAG, "DFRobot AI10 (SEN0677):");
  ESP_LOGCONFIG(TAG, "  Protocol: SyncWord=0xEF 0xAA, XOR checksum");
  this->check_uart_settings(115200);
}

void DFRobotAI10Component::loop() {
  // Read bytes one-by-one and feed into state machine parser
  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);

    switch (this->parser_state_) {
      case STATE_WAIT_SYNC_H:
        if (byte == SYNC_H) {
          this->parser_state_ = STATE_WAIT_SYNC_L;
        }
        break;

      case STATE_WAIT_SYNC_L:
        if (byte == SYNC_L) {
          this->parser_state_ = STATE_READ_HEADER;
          this->header_pos_ = 0;
        } else if (byte == SYNC_H) {
          // Another 0xEF — stay in WAIT_SYNC_L
        } else {
          this->parser_state_ = STATE_WAIT_SYNC_H;
        }
        break;

      case STATE_READ_HEADER:
        this->header_buf_[this->header_pos_++] = byte;
        if (this->header_pos_ >= 3) {
          // Header complete: msg_id(1) + lenH(1) + lenL(1)
          this->payload_len_ = (this->header_buf_[1] << 8) | this->header_buf_[2];
          this->payload_pos_ = 0;

          if (this->payload_len_ > 280) {
            ESP_LOGW(TAG, "Payload too large: %d bytes, resetting parser", this->payload_len_);
            this->parser_state_ = STATE_WAIT_SYNC_H;
          } else {
            this->parser_state_ = STATE_READ_PAYLOAD;
          }
        }
        break;

      case STATE_READ_PAYLOAD:
        this->payload_buf_[this->payload_pos_++] = byte;
        if (this->payload_pos_ >= this->payload_len_ + 1) {
          // Packet complete — verify XOR checksum
          uint8_t xor_calc = 0;
          for (uint8_t i = 0; i < 3; i++) xor_calc ^= this->header_buf_[i];
          for (uint16_t i = 0; i < this->payload_len_; i++) xor_calc ^= this->payload_buf_[i];

          uint8_t xor_recv = this->payload_buf_[this->payload_len_];

          if (xor_calc == xor_recv) {
            uint8_t msg_id = this->header_buf_[0];
            // ESP_LOGV(TAG, "Packet received: MsgID=0x%02X, Len=%d, XOR OK", msg_id, this->payload_len_);
            this->process_packet_(msg_id, this->payload_buf_, this->payload_len_);
          } else {
            ESP_LOGW(TAG, "XOR checksum mismatch! calc=0x%02X recv=0x%02X", xor_calc, xor_recv);
          }
          this->parser_state_ = STATE_WAIT_SYNC_H;
        }
        break;
    }
  }

  // Status log every 10 seconds
  uint32_t now = millis();
  if (this->setup_done_ && now - this->last_status_log_ > 10000) {
    this->last_status_log_ = now;
    ESP_LOGD(TAG, "Parser state: %s, last result: %s",
             this->parser_state_ == STATE_WAIT_SYNC_H ? "IDLE" : "RECEIVING",
             result_to_str_(this->last_result_));
  }
}

// ===== Public API (Non-blocking State Machine) =====

void DFRobotAI10Component::send_reset() {
  // Directly send reset packet
  uint8_t data[] = {CMD_RESET, 0x00, 0x00};
  this->send_packet_(data, sizeof(data));
  ESP_LOGI(TAG, "CMD: RESET (0x10)");
}

void DFRobotAI10Component::start_recognition(uint8_t timeout, bool continuous) {
  if (this->pending_action_ != PENDING_NONE) return;
  // Store intent and parameters, then trigger reset to clear previous state.
  // The actual verify command will be sent in handle_reply_() after reset confirmation.
  this->pending_action_ = PENDING_VERIFY;
  this->stored_timeout_ = timeout;
  this->stored_continuous_ = continuous;
  this->send_reset();
}

void DFRobotAI10Component::enroll_user(uint8_t admin, const char *name, uint8_t timeout) {
  if (this->pending_action_ != PENDING_NONE) return;
  this->pending_action_ = PENDING_ENROLL;
  this->stored_admin_ = admin;
  this->stored_name_ = name;
  this->stored_timeout_ = timeout;
  this->send_reset();
}

void DFRobotAI10Component::delete_user(uint16_t uid) {
  if (this->pending_action_ != PENDING_NONE) return;
  this->pending_action_ = PENDING_DELETE_USER;
  this->stored_uid_ = uid;
  this->send_reset();
}

void DFRobotAI10Component::delete_all_users() {
  if (this->pending_action_ != PENDING_NONE) return;
  this->pending_action_ = PENDING_DELETE_ALL;
  this->send_reset();
}

void DFRobotAI10Component::get_all_user_ids() {
  if (this->pending_action_ != PENDING_NONE) return;
  this->pending_action_ = PENDING_GET_USERS;
  this->send_reset();
}

void DFRobotAI10Component::scan_qr_code(uint8_t timeout) {
  if (this->pending_action_ != PENDING_NONE) return;
  this->pending_action_ = PENDING_SCAN_QR;
  this->stored_timeout_ = timeout;
  this->send_reset();
}

// ===== Internal Helper: Execute Pending Action =====

void DFRobotAI10Component::execute_pending_action_() {
  if (this->pending_action_ == PENDING_NONE) return;

  switch (this->pending_action_) {
    case PENDING_VERIFY: {
      uint8_t data[] = {
        CMD_VERIFY,
        0x00, 0x02,
        (uint8_t)(this->stored_continuous_ ? 0x01 : 0x00),
        this->stored_timeout_
      };
      this->send_packet_(data, sizeof(data));
      this->pending_cmd_ = CMD_VERIFY;
      this->recognized_ = false; // Reset auth state on new verify start
      ESP_LOGI(TAG, "CMD: VERIFY (0x12) continuous=%d timeout=%ds", this->stored_continuous_, this->stored_timeout_);
      break;
    }

    case PENDING_ENROLL: {
      ESP_LOGI(TAG, "CMD: ENROLL '%s' admin=%d timeout=%ds", 
               this->stored_name_.c_str(), this->stored_admin_, this->stored_timeout_);
      
      EnrollData ed;
      memset(&ed, 0, sizeof(ed));
      ed.admin = this->stored_admin_;
      strncpy((char *)ed.user_name, this->stored_name_.c_str(), 31);
      ed.face_dir = 0x00;
      ed.timeout = this->stored_timeout_;

      uint16_t struct_size = sizeof(EnrollData);
      std::vector<uint8_t> data;
      data.push_back(CMD_ENROLL_SINGLE);
      data.push_back((struct_size >> 8) & 0xFF);
      data.push_back(struct_size & 0xFF);
      for (uint16_t i = 0; i < struct_size; i++) {
        data.push_back(((uint8_t *)&ed)[i]);
      }
      this->send_packet_(data.data(), data.size());
      this->pending_cmd_ = CMD_ENROLL_SINGLE;
      break;
    }

    case PENDING_DELETE_USER: {
      uint8_t data[] = {
        CMD_DEL_USER,
        0x00, 0x02,
        (uint8_t)((this->stored_uid_ >> 8) & 0xFF),
        (uint8_t)(this->stored_uid_ & 0xFF)
      };
      this->send_packet_(data, sizeof(data));
      this->pending_cmd_ = CMD_DEL_USER;
      ESP_LOGI(TAG, "CMD: DELETE USER uid=%d", this->stored_uid_);
      break;
    }

    case PENDING_DELETE_ALL: {
      uint8_t data[] = {CMD_DEL_ALL_USER, 0x00, 0x00};
      this->send_packet_(data, sizeof(data));
      this->pending_cmd_ = CMD_DEL_ALL_USER;
      ESP_LOGI(TAG, "CMD: DELETE ALL USERS");
      break;
    }

    case PENDING_GET_USERS: {
      uint8_t data[] = {CMD_GET_ALL_USERID, 0x00, 0x01, 0x00};
      this->send_packet_(data, sizeof(data));
      this->pending_cmd_ = CMD_GET_ALL_USERID;
      ESP_LOGI(TAG, "CMD: GET ALL USER IDs");
      break;
    }

    case PENDING_SCAN_QR: {
      uint8_t data[] = {CMD_SCAN_QR_CODE, 0x00, 0x01, this->stored_timeout_};
      this->send_packet_(data, sizeof(data));
      this->pending_cmd_ = CMD_SCAN_QR_CODE;
      ESP_LOGI(TAG, "CMD: SCAN QR CODE timeout=%ds", this->stored_timeout_);
      break;
    }
    
    default:
      break;
  }

  // Clear pending action after execution
  this->pending_action_ = PENDING_NONE;
}

// ===== Protocol Implementation =====

void DFRobotAI10Component::send_packet_(const uint8_t *data, size_t len) {
  uint8_t xor_check = xor_checksum_(data, len);

  // Wire format: [SYNC_H][SYNC_L][data...][XOR]
  uint8_t sync[2] = {SYNC_H, SYNC_L};
  this->write_array(sync, 2);
  this->write_array(data, len);
  this->write_byte(xor_check);
  this->flush();

  // Debug log (hex string construction omitted for brevity/performance in standard log level)
  if (len <= 8) {
     // Only full hex dump for short packets
     char buf[32] = {0};
     for(size_t i=0; i<len; i++) sprintf(buf + i*3, "%02X ", data[i]);
     ESP_LOGD(TAG, "TX: EF AA %s%02X", buf, xor_check);
  } else {
     ESP_LOGD(TAG, "TX: EF AA [Payload %d bytes] %02X", len, xor_check);
  }
}

uint8_t DFRobotAI10Component::xor_checksum_(const uint8_t *data, size_t len) {
  uint8_t xor_val = 0;
  for (size_t i = 0; i < len; i++) {
    xor_val ^= data[i];
  }
  return xor_val;
}

void DFRobotAI10Component::process_packet_(uint8_t msg_id, const uint8_t *payload, uint16_t len) {
  if (msg_id == MID_REPLY) {
    this->handle_reply_(payload, len);
  } else if (msg_id == MID_NOTE) {
    this->handle_note_(payload, len);
  } else {
    ESP_LOGW(TAG, "Unknown MsgID: 0x%02X", msg_id);
    this->log_hex_("  Data: ", payload, len);
  }
}

void DFRobotAI10Component::handle_reply_(const uint8_t *payload, uint16_t len) {
  if (len < 2) {
    ESP_LOGW(TAG, "Reply too short: %d bytes", len);
    return;
  }

  // Reply format: [cmd_echo][result][data...]
  uint8_t cmd_echo = payload[0];
  uint8_t result = payload[1];
  this->last_result_ = result;

  ESP_LOGI(TAG, "REPLY: cmd=0x%02X result=%d (%s)", cmd_echo, result, result_to_str_(result));

  // Security Check: Invalidate auth state on failure (e.g. liveness check failed)
  if (result != RES_SUCCESS) {
    ESP_LOGW(TAG, "  Command failed: %s", result_to_str_(result));
    
    if (cmd_echo == CMD_VERIFY) {
       this->recognized_ = false;
       this->last_uid_ = 0;
       this->last_user_name_ = "";
       this->last_type_ = TYPE_NONE;
    }
    
    // Clear pending action if Reset failed, to prevent stuck state
    if (cmd_echo == CMD_RESET) {
        this->pending_action_ = PENDING_NONE;
    }
    
    return;
  }

  // Process successful response data
  const uint8_t *data = payload + 2;
  uint16_t data_len = len - 2;

  switch (cmd_echo) {
    case CMD_RESET:
      ESP_LOGD(TAG, "  Reset successful. Executing pending actions...");
      this->execute_pending_action_();
      break;

    case CMD_VERIFY:
      // Response: UID(2) + userName(32) + admin(1) = 35 bytes
      if (data_len >= 35) {
        uint16_t uid = (data[0] << 8) | data[1];
        char name[33] = {0};
        memcpy(name, &data[2], 32);
        name[32] = '\0';
        uint8_t admin = data[34];

        this->last_uid_ = uid;
        this->last_user_name_ = name;
        this->last_type_ = (uid > 1000) ? TYPE_PALM : TYPE_FACE;
        this->recognized_ = true;

        ESP_LOGI(TAG, "  RECOGNIZED: uid=%d name='%s' admin=%d type=%s",
                 uid, name, admin,
                 this->last_type_ == TYPE_FACE ? "face" : "palm");
        
        // Trigger event in ESPHome
        this->on_recognized_callback_.call(this->last_user_name_, this->last_uid_);
      }
      break;

    case CMD_SCAN_QR_CODE:
      if (data_len > 0) {
        this->last_qr_data_ = std::string((char *)data, data_len);
        this->last_type_ = TYPE_QR;
        this->recognized_ = true;
        
        ESP_LOGI(TAG, "  QR CODE: '%s'", this->last_qr_data_.c_str());
        
        // Trigger QR event in ESPHome
        this->on_qr_scanned_callback_.call(this->last_qr_data_);
      }
      break;

    case CMD_ENROLL_SINGLE:
      if (data_len >= 2) {
        uint16_t uid = (data[0] << 8) | data[1];
        this->last_uid_ = uid;
        ESP_LOGI(TAG, "  Enrollment successful! uid=%d", uid);
      }
      break;

    case CMD_DEL_USER:
      ESP_LOGI(TAG, "  User deleted");
      break;

    case CMD_DEL_ALL_USER:
      ESP_LOGI(TAG, "  All users deleted");
      this->user_count_ = 0;
      break;

    case CMD_GET_ALL_USERID:
      if (data_len >= 1) {
        uint8_t user_count = data[0];
        this->user_count_ = user_count;
        ESP_LOGI(TAG, "  Registered users: %d", user_count);
        for (uint8_t i = 0; i < user_count && (1 + i * 2 + 1) < data_len; i++) {
          uint16_t uid = (data[1 + i * 2] << 8) | data[2 + i * 2];
          ESP_LOGI(TAG, "    UID[%d] = %d", i, uid);
        }
      }
      break;

    default:
      ESP_LOGD(TAG, "  Unhandled reply for cmd 0x%02X", cmd_echo);
      this->log_hex_("  Data: ", data, data_len);
      break;
  }
}

void DFRobotAI10Component::handle_note_(const uint8_t *payload, uint16_t len) {
  // Full note (sNoteData_t): nid(1) + state(2) + left(2) + top(2) + right(2) + bottom(2) + yaw(2) + pitch(2) + roll(2) = 17 bytes
  if (len >= 17) {
    uint8_t nid = payload[0];
    int16_t state = (payload[1] << 8) | payload[2];
    int16_t left = (payload[3] << 8) | payload[4];
    int16_t top = (payload[5] << 8) | payload[6];
    int16_t right = (payload[7] << 8) | payload[8];
    int16_t bottom = (payload[9] << 8) | payload[10];

    const char *state_str = "unknown";
    if (state == 0x00) { state_str = "face"; this->face_detected_ = true; }
    else if (state == (int16_t)0x80) { state_str = "palm"; this->face_detected_ = true; }
    else { this->face_detected_ = false; }

    ESP_LOGD(TAG, "NOTE: nid=%d state=%s rect=[%d,%d,%d,%d]",
             nid, state_str, left, top, right, bottom);
  } else if (len >= 2) {
    // Short note — state transition updates (e.g. face exit)
    uint8_t note_type = payload[0];
    uint8_t note_val = payload[1];
    
    // 0x0A 0x00 = Face/Object left the frame
    if (note_type == 0x0A && note_val == 0x00) {
      this->face_detected_ = false;
      this->recognized_ = false; // Reset auth state when user leaves
    }
    ESP_LOGD(TAG, "NOTE: %d bytes (type=0x%02X val=0x%02X)", len, note_type, note_val);
  } else {
    ESP_LOGD(TAG, "NOTE: %d bytes", len);
    this->log_hex_("  ", payload, len);
  }
}

void DFRobotAI10Component::log_hex_(const char *prefix, const uint8_t *data, size_t len) {
  if (len == 0) return;
  std::string hex;
  hex.reserve(len * 3);
  for (size_t i = 0; i < len && i < 100; i++) {
    char tmp[4];
    sprintf(tmp, "%02X ", data[i]);
    hex += tmp;
  }
  if (len > 100) hex += "...";
  ESP_LOGD(TAG, "%s%s", prefix, hex.c_str());
}

const char *DFRobotAI10Component::result_to_str_(uint8_t result) {
  switch (result) {
    case RES_SUCCESS:              return "success";
    case RES_REJECTED:             return "rejected";
    case RES_ABORTED:              return "aborted";
    case RES_FAILED_CAMERA:        return "camera error";
    case RES_FAILED_UNKNOWN:       return "unknown error";
    case RES_FAILED_INVALID_PARAM: return "invalid parameter";
    case RES_FAILED_NO_MEMORY:     return "no memory";
    case RES_FAILED_UNKNOWN_USER:  return "unknown user";
    case RES_FAILED_MAX_USER:      return "max users reached";
    case RES_FAILED_FACE_ENROLLED: return "face already enrolled";
    case RES_FAILED_LIVE_CHECK:    return "liveness check failed";
    case RES_FAILED_TIMEOUT:       return "timeout";
    default:                       return "unknown";
  }
}

}  // namespace dfrobot_ai10
}  // namespace esphome