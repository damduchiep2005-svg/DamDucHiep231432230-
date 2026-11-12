
#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ==================================================
// 1. WIFI
// ==================================================
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ==================================================
// 2. MQTT
// ==================================================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* client_id = "esp32_benuoc_hiep_20261107";

// MQTT TOPICS
#define TOPIC_MUCNUOC   "benuoc/mucnuoc"
#define TOPIC_NHIETDO   "benuoc/nhietdo"
#define TOPIC_BOM_STATE "benuoc/bom/trang_thai"
#define TOPIC_VAN_STATE "benuoc/van/trang_thai"
#define TOPIC_BOM_CTRL  "benuoc/bom/dieu_khien"
#define TOPIC_VAN_CTRL  "benuoc/van/dieu_khien"

// ==================================================
// 3. GPIO
// ==================================================
#define TRIG_PIN 5
#define ECHO_PIN 18
#define TEMP_PIN 14

#define BOM_PIN 26
#define VAN_PIN 27
#define LED_XANH_PIN BOM_PIN  
#define LED_DO_PIN   VAN_PIN   
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// ==================================================
// 4. THONG SO BE NUOC
// ==================================================
// Chieu cao be mo phong: 2.2 met
const float CHIEU_CAO_BE = 2.2;

// Chu ky doc cam bien
const unsigned long READ_INTERVAL = 2000;

// ==================================================
// 5. DOI TUONG WIFI, MQTT, CAM BIEN
// ==================================================
WiFiClient espClient;
PubSubClient client(espClient);

OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);

// ==================================================
// 6. CAU TRUC DU LIEU
// ==================================================
struct SensorData {
  float mucNuoc;
  float nhietDo;
};

// ==================================================
// 7. QUEUE
// ==================================================
QueueHandle_t dataQueue;
QueueHandle_t dataAlert;

// ==================================================
// 8. BIEN HE THONG
// ==================================================
volatile bool bomState = false;
volatile bool vanState = false;

// Lenh dieu khien:
// 0: Khong co lenh
// 1: ON
// 2: OFF
// 3: AUTO
volatile int lenhBom = 0;
volatile int lenhVan = 0;

// Trang thai che do thu cong
bool manualBom = false;
bool manualVan = false;

// Du lieu cam bien
float mucNuoc = -1;
float nhietDo = -127;

// Thoi gian ket noi
unsigned long lastWiFiTry = 0;
unsigned long lastMQTTTry = 0;

// ==================================================
// 9. HAM DIEU KHIEN RELAY
// ==================================================
void datBom(bool on) {

  // Khong cho bom va van hoat dong cung luc
  if (on && vanState) {
    vanState = false;
    digitalWrite(VAN_PIN, RELAY_OFF);
  }

  bomState = on;
  digitalWrite(BOM_PIN, on ? RELAY_ON : RELAY_OFF);

  Serial.printf("[DIEU KHIEN] May bom: %s\n",
                on ? "ON" : "OFF");
}

void datVan(bool on) {

  // Khong cho bom va van hoat dong cung luc
  if (on && bomState) {
    bomState = false;
    digitalWrite(BOM_PIN, RELAY_OFF);
  }

  vanState = on;
  digitalWrite(VAN_PIN, on ? RELAY_ON : RELAY_OFF);

  Serial.printf("[DIEU KHIEN] Van xa: %s\n",
                on ? "ON" : "OFF");
}

// ==================================================
// 10. DOC CAM BIEN HC-SR04
// ==================================================
float docKhoangCach() {

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration =
      pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0) {
    return -1;
  }

  return duration * 0.0343f / 2.0f;
}

// ==================================================
// 11. TINH MUC NUOC (MET)
// ==================================================
float tinhMucNuoc(float distance) {

  if (distance < 0 ||
      distance > CHIEU_CAO_BE * 100.0) {
    return -1;
  }

  float level =
      CHIEU_CAO_BE - distance / 100.0;

  return level;
}

// ==================================================
// 12. CALLBACK NHAN LENH MQTT
// ==================================================
void callback(char* topic, byte* payload,
              unsigned int length) {

  String msg = "";

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  msg.trim();
  msg.toUpperCase();

  Serial.printf("[MQTT] %s: %s\n", topic, msg.c_str());

  if (strcmp(topic, TOPIC_BOM_CTRL) == 0) {

    if (msg == "ON") {
      lenhBom = 1;
    }
    else if (msg == "OFF") {
      lenhBom = 2;
    }
    else if (msg == "AUTO") {
      lenhBom = 3;
    }
  }

  else if (strcmp(topic, TOPIC_VAN_CTRL) == 0) {

    if (msg == "ON") {
      lenhVan = 1;
    }
    else if (msg == "OFF") {
      lenhVan = 2;
    }
    else if (msg == "AUTO") {
      lenhVan = 3;
    }
  }
}

// ==================================================
// 13. TASK 1: DOC DU LIEU CAM BIEN
// CORE 1
// ==================================================
void Task_ReadData(void *pvParameters) {

  TickType_t lastWakeTime = xTaskGetTickCount();

  while (1) {

    // Doc HC-SR04
    float distance = docKhoangCach();

    float level = tinhMucNuoc(distance);

    // Doc DS18B20
    sensors.requestTemperatures();

    float temp = sensors.getTempCByIndex(0);

    // Kiem tra du lieu
    bool validTemp =
        temp != DEVICE_DISCONNECTED_C &&
        temp >= -55 && temp <= 125;

    bool validLevel =
        level >= 0 && level <= CHIEU_CAO_BE;

    if (!validLevel) {
      Serial.println("[TASK 1] Loi cam bien HC-SR04");
    }

    if (!validTemp) {
      Serial.println("[TASK 1] Loi cam bien DS18B20");
    }

    // Hien thi Serial
    if (validLevel && validTemp) {

      Serial.println("-----------------------------");

      Serial.printf("[TASK 1] Khoang cach: %.2f cm\n",
                    distance);

      Serial.printf("[TASK 1] Muc nuoc: %.2f m\n",
                    level);

      Serial.printf("[TASK 1] Nhiet do: %.2f C\n",
                    temp);

      Serial.println("-----------------------------");
    }

    // Tao cau truc du lieu
    SensorData data;

    data.mucNuoc = validLevel ? level : -1;
    data.nhietDo = validTemp ? temp : -127;

    // Gui du lieu vao dataQueue
    if (xQueueSend(dataQueue, &data, 0) != pdPASS) {

      SensorData oldData;
      xQueueReceive(dataQueue, &oldData, 0);
      xQueueSend(dataQueue, &data, 0);

      Serial.println("[TASK 1] Queue day!");
    }

    // Gui muc nuoc vao dataAlert
    if (validLevel) {

      if (xQueueSend(dataAlert, &level, 0) != pdPASS) {

        float oldLevel;
        xQueueReceive(dataAlert, &oldLevel, 0);
        xQueueSend(dataAlert, &level, 0);
      }
    }

    vTaskDelayUntil(
        &lastWakeTime,
        pdMS_TO_TICKS(READ_INTERVAL)
    );
  }
}

// ==================================================
// 14. TASK 2: TRUYEN DU LIEU MQTT
// CORE 0
// ==================================================
void Task_TransmitData(void *pvParameters) {

  SensorData receivedData;

  while (1) {

    // Xu ly WiFi
    if (WiFi.status() != WL_CONNECTED) {

      if (millis() - lastWiFiTry >= 5000) {

        lastWiFiTry = millis();

        Serial.println("[TASK 2] Dang ket noi WiFi...");

        WiFi.begin(ssid, password);
      }
    }

    // Ket noi MQTT
    if (WiFi.status() == WL_CONNECTED &&
        !client.connected()) {

      if (millis() - lastMQTTTry >= 3000) {

        lastMQTTTry = millis();

        Serial.print("[TASK 2] Ket noi MQTT... ");

        if (client.connect(client_id)) {

          Serial.println("THANH CONG");

          client.subscribe(TOPIC_BOM_CTRL);
          client.subscribe(TOPIC_VAN_CTRL);

          client.publish(
              TOPIC_BOM_STATE,
              bomState ? "ON" : "OFF",
              true
          );

          client.publish(
              TOPIC_VAN_STATE,
              vanState ? "ON" : "OFF",
              true
          );
        }
        else {

          Serial.printf("THAT BAI rc=%d\n",
                        client.state());
        }
      }
    }

    // Duy tri MQTT
    if (client.connected()) {

      client.loop();

      // Nhan du lieu tu dataQueue
      if (xQueueReceive(
              dataQueue,
              &receivedData,
              pdMS_TO_TICKS(100)
          ) == pdPASS) {

        char buffer[32];

        // Gui muc nuoc
        if (receivedData.mucNuoc >= 0) {

          snprintf(buffer, sizeof(buffer),
                   "%.2f", receivedData.mucNuoc);

          client.publish(
              TOPIC_MUCNUOC,
              buffer,
              true
          );

          Serial.printf(
              "[TASK 2] Gui muc nuoc: %s m\n",
              buffer
          );
        }

        // Gui nhiet do
        if (receivedData.nhietDo != -127) {

          snprintf(buffer, sizeof(buffer),
                   "%.2f", receivedData.nhietDo);

          client.publish(
              TOPIC_NHIETDO,
              buffer,
              true
          );

          Serial.printf(
              "[TASK 2] Gui nhiet do: %s C\n",
              buffer
          );
        }

        // Cap nhat trang thai bom va van
        client.publish(
            TOPIC_BOM_STATE,
            bomState ? "ON" : "OFF",
            true
        );

        client.publish(
            TOPIC_VAN_STATE,
            vanState ? "ON" : "OFF",
            true
        );
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ==================================================
// 15. TASK 3: CANH BAO VA DIEU KHIEN
// CORE 1
// ==================================================
void Task_Alert(void *pvParameters) {

  float receivedLevel = -1;

  // Trang thai chu trinh tu dong
  int autoAction = 0;
  // 0: Khong hoat dong
  // 1: Bom 50%
  // 2: Bom 100%
  // 3: Van xa

  unsigned long actionStart = 0;
  unsigned long actionDuration = 0;

  bool done50 = false;
  bool done100 = false;
  bool doneDrain = false;

  while (1) {

    // Nhan muc nuoc tu dataAlert
    if (xQueueReceive(
            dataAlert,
            &receivedLevel,
            pdMS_TO_TICKS(100)
        ) == pdPASS) {

      mucNuoc = receivedLevel;

      Serial.printf(
          "[TASK 3] Nhan muc nuoc: %.2f m\n",
          receivedLevel
      );

      // Reset cac co khi muc nuoc roi khoi vung
      if (receivedLevel < 1.0 ||
          receivedLevel >= 1.5) {
        done50 = false;
      }

      if (receivedLevel < 1.5 ||
          receivedLevel >= 2.0) {
        done100 = false;
      }

      if (receivedLevel >= 0.5) {
        doneDrain = false;
      }
    }

    // ----------------------------------------------
    // NHAN LENH DIEU KHIEN THU CONG
    // ----------------------------------------------

    int cmdBom = lenhBom;
    int cmdVan = lenhVan;

    lenhBom = 0;
    lenhVan = 0;

    if (cmdBom == 1) {

      manualBom = true;
      datBom(true);
    }
    else if (cmdBom == 2) {

      manualBom = true;
      datBom(false);
    }
    else if (cmdBom == 3) {

      manualBom = false;
      autoAction = 0;

      Serial.println("[TASK 3] Bom chuyen AUTO");
    }

    if (cmdVan == 1) {

      manualVan = true;
      datVan(true);
    }
    else if (cmdVan == 2) {

      manualVan = true;
      datVan(false);
    }
    else if (cmdVan == 3) {

      manualVan = false;
      autoAction = 0;

      Serial.println("[TASK 3] Van chuyen AUTO");
    }

    // ----------------------------------------------
    // CHE DO TU DONG
    // ----------------------------------------------

    if (!manualBom && !manualVan) {

      // Neu chua co chu trinh nao dang chay
      if (autoAction == 0 && receivedLevel >= 0) {

        // Muc nuoc 1m - 1.5m
        if (receivedLevel >= 1.0 &&
            receivedLevel < 1.5 &&
            !done50) {

          autoAction = 1;
          actionStart = millis();
          actionDuration = 10000;

          done50 = true;

          datVan(false);

          Serial.println(
              "[TASK 3] Muc nuoc 1-1.5m: BOM 50% trong 10s"
          );
        }

        // Muc nuoc 1.5m - 2m
        else if (receivedLevel >= 1.5 &&
                 receivedLevel <= 2.0 &&
                 !done100) {

          autoAction = 2;
          actionStart = millis();
          actionDuration = 20000;

          done100 = true;

          datVan(false);
          datBom(true);

          Serial.println(
              "[TASK 3] Muc nuoc 1.5-2m: BOM 100% trong 20s"
          );
        }

        // Muc nuoc duoi 0.5m
        else if (receivedLevel < 0.5 &&
                 !doneDrain) {

          autoAction = 3;
          actionStart = millis();
          actionDuration = 10000;

          doneDrain = true;

          datBom(false);
          datVan(true);

          Serial.println(
              "[TASK 3] Muc nuoc <0.5m: MO VAN trong 10s"
          );
        }
      }

      // --------------------------------------------
      // THUC HIEN CHU TRINH BOM 50%
      // --------------------------------------------
      if (autoAction == 1) {

        unsigned long elapsed = millis() - actionStart;

        if (elapsed < actionDuration) {

          // Mo phong 50% duty cycle:
          // 1 giay ON, 1 giay OFF
          bool pumpOn = (elapsed % 2000) < 1000;

          datBom(pumpOn);
        }
        else {

          datBom(false);
          autoAction = 0;

          Serial.println(
              "[TASK 3] Ket thuc bom 50%"
          );
        }
      }

      // --------------------------------------------
      // THUC HIEN CHU TRINH BOM 100%
      // --------------------------------------------
      else if (autoAction == 2) {

        if (millis() - actionStart < actionDuration) {

          datBom(true);
        }
        else {

          datBom(false);
          autoAction = 0;

          Serial.println(
              "[TASK 3] Ket thuc bom 100%"
          );
        }
      }

      // --------------------------------------------
      // THUC HIEN CHU TRINH XA NUOC
      // --------------------------------------------
      else if (autoAction == 3) {

        if (millis() - actionStart < actionDuration) {

          datVan(true);
        }
        else {

          datVan(false);
          autoAction = 0;

          Serial.println(
              "[TASK 3] Ket thuc xa nuoc"
          );
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ==================================================
// 16. SETUP
// ==================================================
void setup() {

  Serial.begin(115200);

  // Cau hinh GPIO
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(BOM_PIN, OUTPUT);
  pinMode(VAN_PIN, OUTPUT);

  digitalWrite(BOM_PIN, RELAY_OFF);
  digitalWrite(VAN_PIN, RELAY_OFF);

  // Khoi dong DS18B20
  sensors.begin();

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(256);

  // Tao Queue
  dataQueue = xQueueCreate(5, sizeof(SensorData));
  dataAlert = xQueueCreate(5, sizeof(float));

  if (dataQueue == NULL || dataAlert == NULL) {

    Serial.println("LOI: KHONG TAO DUOC QUEUE!");

    while (1) {
      delay(1000);
    }
  }

  Serial.println("================================");
  Serial.println(" HE THONG GIAM SAT BE NUOI THUY SAN");
  Serial.println(" ESP32 + FREERTOS + MQTT");
  Serial.println("================================");

  // TASK 1: Doc du lieu - Core 1
  xTaskCreatePinnedToCore(
      Task_ReadData,
      "ReadData",
      4096,
      NULL,
      2,
      NULL,
      1
  );

  // TASK 2: Truyen du lieu - Core 0
  xTaskCreatePinnedToCore(
      Task_TransmitData,
      "TransmitData",
      8192,
      NULL,
      2,
      NULL,
      0
  );

  // TASK 3: Canh bao - Core 1
  xTaskCreatePinnedToCore(
      Task_Alert,
      "Alert",
      4096,
      NULL,
      3,
      NULL,
      1
  );

  Serial.println("Da khoi tao 3 Task FreeRTOS!");
}

// ==================================================
// 17. LOOP
// ==================================================
void loop() {

  // Tat loop chinh, cac Task FreeRTOS tu hoat dong
  vTaskDelay(pdMS_TO_TICKS(1000));
}
