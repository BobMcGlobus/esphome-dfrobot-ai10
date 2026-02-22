# ESPHome DFRobot SEN0677 AI Vision Sensor Component

An ESPHome external component for the [DFRobot SEN0677](https://www.dfrobot.com/product-2762.html) AI Binocular Vision Sensor, enabling **face recognition**, **palm vein recognition**, and **QR code scanning** over UART.

The UART protocol was reverse-engineered from the [DFRobot_AI10 Arduino library](https://github.com/DFRobot/DFRobot_AI10) source code (MIT License).

## Features

- **Face recognition** — enroll and recognize faces with UID + username
- **Palm vein recognition** — enroll and recognize palm veins (same API, sensor auto-detects)
- **QR code scanning** — read QR codes with configurable timeout
- **User management** — enroll, delete single users, delete all users, list user IDs
- **Continuous recognition** — stream recognition results in real-time
- **ESPHome automations** — native `on_recognized` and `on_qr_scanned` triggers
- **Non-blocking** — async state machine, no `delay()` calls in the main loop
- **Home Assistant integration** — buttons, sensors, and binary sensors out of the box
- **Pure ESPHome** — no Arduino library dependency, works with ESP-IDF framework

## Hardware

| Pin | Connection |
|-----|-----------|
| SEN0677 VCC | **5V** (not 3.3V!) |
| SEN0677 GND | GND |
| SEN0677 TX | ESP32 RX pin |
| SEN0677 RX | ESP32 TX pin |

> **Important:** The sensor requires 5V power. UART logic levels are 3.3V compatible — no level shifter needed for data lines.

> **ESP32-S3 PSRAM note:** If your board has PSRAM (ESP32-S3R8), GPIO33–37 may conflict with the PSRAM interface. Either avoid these pins, or disable PSRAM in your config:
> ```yaml
> esp32:
>   framework:
>     type: esp-idf
>     sdkconfig_options:
>       CONFIG_SPIRAM: "n"
> ```

## Installation

### Option 1: Local (recommended for development)

Copy the `components/dfrobot_ai10/` folder into your ESPHome config directory:

```
esphome/
├── your-device.yaml
└── components/
    └── dfrobot_ai10/
        ├── __init__.py
        ├── dfrobot_ai10.h
        └── dfrobot_ai10.cpp
```

Then reference it in your YAML:

```yaml
external_components:
  - source:
      type: local
      path: components
```

### Option 2: GitHub (recommended for users)

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/BobMcGlobus/esphome-dfrobot-ai10
      ref: main
    components: [dfrobot_ai10]
```

## Configuration

### Minimal setup

```yaml
uart:
  id: uart_ai10
  tx_pin: GPIO33
  rx_pin: GPIO34
  baud_rate: 115200

dfrobot_ai10:
  id: ai10_sensor
  uart_id: uart_ai10
```

### Automation triggers (recommended)

The component provides native ESPHome triggers for recognition events — no polling required.

#### `on_recognized` — face or palm vein match

Fires when the sensor successfully identifies a registered user. Provides two variables:

- `name` (`std::string`) — the enrolled username
- `uid` (`uint16_t`) — the user ID (≤1000 = face, >1000 = palm)

```yaml
dfrobot_ai10:
  id: ai10_sensor
  uart_id: uart_ai10

  on_recognized:
    then:
      - logger.log:
          format: "Recognized: %s (UID %d)"
          args: [ 'name.c_str()', 'uid' ]
      - homeassistant.event:
          event: esphome.ai_recognized
          data:
            name: !lambda 'return name;'
            uid: !lambda 'return to_string(uid);'
```

#### `on_qr_scanned` — QR code read

Fires when a QR code is successfully decoded. Provides one variable:

- `qr_data` (`std::string`) — the decoded QR code content

```yaml
dfrobot_ai10:
  id: ai10_sensor
  uart_id: uart_ai10

  on_qr_scanned:
    then:
      - logger.log:
          format: "QR Code: %s"
          args: [ 'qr_data.c_str()' ]
      - text_sensor.template.publish:
          id: last_qr_code
          state: !lambda 'return qr_data;'
```

### Buttons (appear in Home Assistant)

```yaml
button:
  - platform: template
    name: "Start Recognition"
    on_press:
      - lambda: id(ai10_sensor)->start_recognition(30, true);

  - platform: template
    name: "Stop Recognition"
    on_press:
      - lambda: id(ai10_sensor)->send_reset();

  - platform: template
    name: "Scan QR Code"
    on_press:
      - lambda: id(ai10_sensor)->scan_qr_code(15);
```

### Services (for calls with parameters)

```yaml
api:
  services:
    - service: enroll_user
      variables:
        user_name: string
        is_admin: bool
      then:
        - lambda: |-
            uint8_t admin = is_admin ? 1 : 0;
            id(ai10_sensor)->enroll_user(admin, user_name.c_str(), 10);

    - service: delete_user
      variables:
        uid: int
      then:
        - lambda: id(ai10_sensor)->delete_user(uid);

    - service: start_recognition
      variables:
        timeout: int
        continuous: bool
      then:
        - lambda: id(ai10_sensor)->start_recognition(timeout, continuous);
```

### Template sensors (optional, for HA dashboards)

The `on_recognized` trigger is the recommended way to react to events. If you additionally want persistent state in Home Assistant (e.g. for dashboard cards), you can use template sensors — see [example.yaml](example.yaml) for a complete working configuration.

## API Reference

All methods can be called from ESPHome lambdas via `id(ai10_sensor)->method()`.

All commands use a **non-blocking state machine**: they send a RESET first, then execute the actual command after the sensor confirms the reset. This means no `delay()` calls block the main loop.

| Method | Description |
|--------|-------------|
| `start_recognition(timeout, continuous)` | Start face/palm recognition. Set `continuous=true` for continuous scanning. |
| `send_reset()` | Stop any running operation (recognition, enrollment, QR scan). |
| `enroll_user(admin, name, timeout)` | Enroll a new face or palm. Hold face/palm in front of sensor. `admin`: 0=normal, 1=admin. |
| `delete_user(uid)` | Delete a specific user by UID. |
| `delete_all_users()` | Delete all enrolled users. |
| `get_all_user_ids()` | Query number of enrolled users and their UIDs (logged + stored). |
| `scan_qr_code(timeout)` | Start QR code scanning with timeout in seconds. |

### Getters (for use in template sensors)

| Getter | Returns | Description |
|--------|---------|-------------|
| `get_last_uid()` | `uint16_t` | UID of last recognized user |
| `get_last_user_name()` | `std::string` | Name of last recognized user |
| `get_last_type()` | `RecognitionType` | `TYPE_FACE`, `TYPE_PALM`, or `TYPE_QR` |
| `is_recognized()` | `bool` | `true` after a successful recognition |
| `is_face_detected()` | `bool` | `true` while a face/palm is in front of sensor |
| `get_last_qr_data()` | `std::string` | Content of last scanned QR code |
| `get_user_count()` | `uint8_t` | Number of enrolled users (after `get_all_user_ids()`) |

### Triggers

| Trigger | Variables | Description |
|---------|-----------|-------------|
| `on_recognized` | `name` (string), `uid` (uint16) | Fires on successful face/palm recognition |
| `on_qr_scanned` | `qr_data` (string) | Fires when a QR code is successfully read |

## How It Works

### Face vs. Palm Vein

The sensor automatically detects whether it sees a face or a palm — there is no separate command. The same `enroll_user()` and `start_recognition()` calls work for both. The difference is indicated by the **UID range** returned:

- **UID ≤ 1000** → Face
- **UID > 1000** → Palm vein

### User Names

The sensor uses the **username as a unique key**. You cannot enroll the same name twice — even for different biometric types. To enroll both face and palm for the same person, use slightly different names (e.g., `"Jonas"` for face, `"Jonas-Palm"` for palm vein).

### Non-Blocking Command Flow

Every command follows this async pattern:

1. Public method stores parameters and sets `pending_action_`
2. A RESET command is sent to abort any running operation
3. When the RESET reply arrives, `execute_pending_action_()` sends the actual command
4. The reply is processed and triggers are fired

This ensures the ESP32 main loop is never blocked, keeping WiFi, other components, and the watchdog timer running smoothly.

### Protocol

The sensor communicates at 115200 baud using a binary protocol:

```
TX: [0xEF][0xAA][CMD][LenH][LenL][payload...][XOR]
RX: [0xEF][0xAA][MsgID][LenH][LenL][payload...][XOR]
```

- **Sync word:** `0xEF 0xAA`
- **Checksum:** XOR of all bytes between sync and checksum
- **MsgID 0x00 (Reply):** Response to a command — contains `cmd_echo + result + data`
- **MsgID 0x01 (Note):** Asynchronous notification — face position, bounding box, state

## Tested Hardware

- **Sensor:** DFRobot SEN0677 AI Binocular Vision Sensor
- **MCU:** ESP32-S3 (Waveshare ESP32-S3-ETH, W5500 Ethernet)
- **Framework:** ESP-IDF (ESPHome 2026.1.5)
- **Baud rate:** 115200 (fixed, cannot be changed)

## License

MIT License — protocol details derived from the [DFRobot_AI10](https://github.com/DFRobot/DFRobot_AI10) library (also MIT).

## Credits

- Protocol reverse-engineered from [DFRobot_AI10 Arduino library](https://github.com/DFRobot/DFRobot_AI10) source code
- Built for ESPHome using the [external components](https://esphome.io/components/external_components.html) framework