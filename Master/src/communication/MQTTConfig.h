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

// =========================================
// MQTT TOPICS — Liveness perangkat (publish)
// =========================================
// Satu-satunya sumber kebenaran "ESP hidup atau mati" untuk web.
//
// Sebelum ini firmware tidak pernah mengirim sinyal liveness apa pun, sehingga
// web terpaksa menebak dari kesegaran topik sensor. Di RELAY_ONLY_MODE topik
// sensor memang tidak pernah dipublish (malah dihapus oleh
// clearStaleRetainedTopics), jadi tebakan itu selalu berbunyi "ESP mati"
// meskipun perangkatnya sehat.
//
// Sekarang: dipublish retained "online" tepat setelah CONNACK, dan "offline"
// dipasang sebagai Last Will pada saat connect — broker yang akan menerbitkannya
// otomatis bila ESP hilang tanpa sempat pamit (WiFi drop, brownout, listrik
// mati). Web cukup membaca satu topik ini; tidak perlu menghitung timeout.
#define TOPIC_DEVICE_STATUS "greenhouse/status"

// Payload liveness dirakit saat kompilasi supaya Last Will bisa memakai pointer
// string statis — PubSubClient menyimpan pointer will, bukan salinannya, jadi
// buffer runtime tidak aman dipakai di sini.
#define PAYLOAD_STATUS_ONLINE  "{\"device_id\":\"" MQTT_CLIENT_ID "\",\"status\":\"online\"}"
#define PAYLOAD_STATUS_OFFLINE "{\"device_id\":\"" MQTT_CLIENT_ID "\",\"status\":\"offline\"}"

#define STATUS_WILL_QOS    1
#define STATUS_WILL_RETAIN true

// Interval publish (ms)
#define MQTT_PUBLISH_INTERVAL 1000UL

// =========================================
// Pemulihan koneksi & failsafe
// =========================================
// Jeda antar percobaan sambung ulang. WiFi.begin() dan PubSubClient::connect()
// keduanya blocking, jadi keduanya di-throttle: tanpa ini, broker yang tidak
// bisa dihubungi membuat tiap iterasi loop() menggantung sampai timeout socket
// (15 detik) dan penjadwal irigasi ikut kelaparan.
#define WIFI_RETRY_INTERVAL_MS 10000UL
#define MQTT_RETRY_INTERVAL_MS 5000UL

// Berapa lama link boleh putus sebelum firmware bertindak sendiri.
// Nilai 0 mematikan masing-masing perilaku.
//
// LINK_FAILSAFE_MS: matikan SEMUA relay. Tanpa ini, pompa dan solenoid yang
// terlanjur menyala akan terus jalan tanpa ada yang bisa mematikan dari jauh —
// persis kejadian relay 4/5/8 terkunci ON di lapangan.
//
// LINK_RESTART_MS: boot ulang board. Boot memanggil RelayManager::allOff() lalu
// mengulang seluruh urutan koneksi dari nol, termasuk hal-hal yang tidak bisa
// diperbaiki dari dalam loop (stack WiFi yang wedged).
#define LINK_FAILSAFE_MS 300000UL   // 5 menit
#define LINK_RESTART_MS  900000UL   // 15 menit

// Watchdog perangkat keras: reboot bila loop() macet total (deadlock, driver
// hang) — keadaan yang tidak mungkin ditangani oleh kode di dalam loop itu
// sendiri. Harus lebih besar dari operasi blocking terlama di loop, yaitu
// PubSubClient::connect() dengan socket timeout 15 detik.
#define HW_WATCHDOG_TIMEOUT_S 60

// Jika 0, firmware tidak akan membuka captive portal/blocking WiFi saat boot.
// FSM tetap jalan offline; MQTT publish dicoba hanya saat WiFi sudah terhubung.
#define WIFI_BLOCKING_PORTAL_ENABLED 1

#endif
