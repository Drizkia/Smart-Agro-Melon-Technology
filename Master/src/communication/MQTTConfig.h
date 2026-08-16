#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

// Konfigurasi broker + daftar topik, dipakai bersama oleh MQTTManager
// (mode sistem penuh) dan RelayOnlyMQTT (mode RELAY_ONLY_MODE).
// WiFi dikonfigurasi via captive portal (WiFiManager) — tidak ada SSID hardcoded.

#define MQTT_BROKER    "aead004bf5144152b88233f1a1949418.s1.eu.hivemq.cloud"
#define MQTT_PORT      8883
#define MQTT_CLIENT_ID "greenhouse-master-01"
#define MQTT_USER      "greenhouse_esp32"
#define MQTT_PASS      "Kebonagungpanenmelon1"

// =========================================
// MQTT TOPICS — Telemetry (publish)
// =========================================
#define TOPIC_SENSORS       "greenhouse/sensors"
#define TOPIC_FSM_STATE     "greenhouse/fsm/state"
#define TOPIC_RELAY_STATUS  "greenhouse/actuators/status"
#define TOPIC_CONFIG_ACK    "greenhouse/config/ack"
#define TOPIC_SOIL_HEALTH       "greenhouse/soil/health"
#define TOPIC_ALERT_TANK_LOW    "greenhouse/alert/tank_low"

// Status penjadwal timer — hanya dipublish saat RELAY_ONLY_MODE.
#define TOPIC_TIMER_STATUS  "greenhouse/timer/status"

// Pembacaan flow meter nutrisi A & B — hanya dipublish saat RELAY_ONLY_MODE.
// Dipakai operator untuk tahu berapa liter yang sudah didosis manual.
#define TOPIC_FLOW_STATUS   "greenhouse/flow/status"

// =========================================
// MQTT TOPICS — Commands (subscribe)
// =========================================
#define TOPIC_CMD           "greenhouse/actuators/cmd"
#define TOPIC_CFG_PPM       "greenhouse/config/ppm"
#define TOPIC_CFG_PH        "greenhouse/config/ph"
#define TOPIC_CFG_RECIPE    "greenhouse/config/recipe"
#define TOPIC_CFG_RECIPE_DELETE "greenhouse/config/recipe/delete"
#define TOPIC_CFG_IRRIG     "greenhouse/config/irrigation"
#define TOPIC_CFG_SYSTEM    "greenhouse/config/system"
#define TOPIC_CFG_SCHEDULE  "greenhouse/config/schedule"
#define TOPIC_CFG_TIMER_IRRIG   "greenhouse/config/timer_irrigation"
#define TOPIC_CFG_MIX_EXT       "greenhouse/config/mix_schedule_ext"
#define TOPIC_CMD_SOIL_RESET    "greenhouse/soil/reset_mode"

// Remote control — restart ESP32 dari web tanpa menyentuh perangkat fisik.
// Payload bebas (misal "restart" atau JSON {"action":"restart"}). Terima pesan
// apa pun di topic ini → ESP.restart() setelah publish ACK.
#define TOPIC_CMD_RESET         "greenhouse/control/reset"
#define TOPIC_CMD_RESET_ACK     "greenhouse/control/reset/ack"

// Interval publish (ms)
#define MQTT_PUBLISH_INTERVAL 1000UL

// Jika 0, firmware tidak akan membuka captive portal/blocking WiFi saat boot.
// FSM tetap jalan offline; MQTT publish dicoba hanya saat WiFi sudah terhubung.
#define WIFI_BLOCKING_PORTAL_ENABLED 1

#endif
