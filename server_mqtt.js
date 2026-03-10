// server_mqtt.js - ESP32 MQTT 데이터 수신 서버
const mqtt = require("mqtt");

// ---------------------------
// MQTT 설정
// ---------------------------
const MQTT_BROKER = "mqtt://192.168.0.20:1883"; // Mosquitto 브로커 주소
const MQTT_TOPIC = "test"; // ESP32가 전송하는 토픽 (mqtt.c의 MQTT_TOPIC과 동일)

// ---------------------------
// 상태 관리
// ---------------------------
let mqttClient = null;
let mqttConnected = false;
let messageCount = 0;

// ---------------------------
// MQTT 클라이언트 초기화
// ---------------------------
function initMQTT() {
  console.log("========================================");
  console.log("ESP32 MQTT 데이터 수신 서버 시작");
  console.log("========================================");
  console.log(`🔌 MQTT 브로커: ${MQTT_BROKER}`);
  console.log(`📡 구독 토픽: ${MQTT_TOPIC}`);
  console.log("========================================\n");
  
  mqttClient = mqtt.connect(MQTT_BROKER, {
    clientId: `nodejs_mqtt_client_${Date.now()}`,
    reconnectPeriod: 1000,
    connectTimeout: 5000
  });

  mqttClient.on("connect", () => {
    mqttConnected = true;
    console.log("✅ MQTT 브로커 연결 성공!\n");
    console.log(`📡 토픽 구독 시도: "${MQTT_TOPIC}"`);
    
    mqttClient.subscribe(MQTT_TOPIC, { qos: 1 }, (err, granted) => {
      if (err) {
        console.error("❌ MQTT 토픽 구독 실패:", err);
      } else {
        console.log("✅ 토픽 구독 성공:", granted);
        console.log("\n📨 ESP32에서 보내는 데이터를 기다리는 중...\n");
        console.log("========================================\n");
      }
    });
  });

  mqttClient.on("message", (topic, message) => {
    messageCount++;
    console.log(`\n[메시지 #${messageCount}] ========================================`);
    console.log(`📨 MQTT 메시지 수신`);
    console.log(`  토픽: ${topic}`);
    console.log(`  크기: ${message.length} bytes`);
    console.log(`  시간: ${new Date().toLocaleString('ko-KR')}`);
    console.log("----------------------------------------");
    
    try {
      const data = JSON.parse(message.toString());
      console.log("📋 파싱된 JSON 데이터:");
      console.log(JSON.stringify(data, null, 2));
      
      // 주요 필드 출력
      if (data.address) console.log(`  - 주소: ${data.address}`);
      if (data.hr !== undefined) console.log(`  - 심박수: ${data.hr}`);
      if (data.spo2 !== undefined) console.log(`  - 산소포화도: ${data.spo2}`);
      if (data.temp !== undefined) console.log(`  - 온도: ${data.temp}`);
      if (data.bat !== undefined) console.log(`  - 배터리: ${data.bat}%`);
      if (data.ip) console.log(`  - IP 주소: ${data.ip}`);
      if (data.raw && Array.isArray(data.raw)) {
        console.log(`  - Raw 데이터 개수: ${data.raw.length}개`);
      }
      
    } catch (error) {
      // JSON이 아닌 경우 문자열로 처리
      console.log("📝 텍스트 데이터:");
      console.log(message.toString());
    }
    
    console.log("========================================\n");
  });

  mqttClient.on("error", (error) => {
    console.error("❌ MQTT 오류:", error.message);
    mqttConnected = false;
  });

  mqttClient.on("close", () => {
    console.log("⚠️ MQTT 연결 종료");
    mqttConnected = false;
  });

  mqttClient.on("offline", () => {
    console.log("⚠️ MQTT 오프라인");
    mqttConnected = false;
  });

  mqttClient.on("reconnect", () => {
    console.log("🔄 MQTT 재연결 시도 중...");
  });
}

// ---------------------------
// 종료 처리
// ---------------------------
process.on("SIGINT", () => {
  console.log("\n\n서버 종료 중...");
  if (mqttClient) {
    mqttClient.end();
  }
  console.log(`총 ${messageCount}개의 메시지를 수신했습니다.`);
  process.exit(0);
});

// ---------------------------
// 서버 시작
// ---------------------------
initMQTT();

