// server.js - HTTP 방식으로 변경
const express = require("express");
const axios = require("axios");

const app = express();
app.use(express.json());

// ---------------------------
// 환경 설정
// ---------------------------
const PORT = 3000; // Express 서버 포트
const ESP_HTTP_URL = "http://192.168.0.4:8080"; // ESP32 HTTP 서버 주소
const POLL_INTERVAL = 2000; // 폴링 간격(ms)

// ---------------------------
// 상태 관리
// ---------------------------
let connected = false;
let lastMessage = null;
let pollTimer = null;

// SSE(서버센트이벤트) 클라이언트 저장용
const sseClients = new Set();

function broadcastToSSE(data) {
  const payload = `data: ${JSON.stringify(data)}\n\n`;
  for (const res of sseClients) {
    try {
      res.write(payload);
    } catch (_) {}
  }
}

// ---------------------------
// ESP32 HTTP 통신 로직
// ---------------------------
async function pollESP32() {
  try {
    const response = await axios.get(`${ESP_HTTP_URL}/ws`, {
      timeout: 5000
    });
    
    if (!connected) {
      connected = true;
      console.log("✅ Connected to ESP32 HTTP server");
      broadcastToSSE({ type: "connected", time: new Date().toISOString() });
    }
    
    const message = {
      time: new Date().toISOString(),
      payload: response.data
    };
    
    lastMessage = message;
    console.log("[ESP -> Server]", message);
    broadcastToSSE(message);
    
  } catch (error) {
    if (connected) {
      connected = false;
      console.warn("⚠️ ESP32 connection lost:", error.message);
      broadcastToSSE({ type: "disconnected", time: new Date().toISOString() });
    }
  }
}

async function sendToESP32(data) {
  try {
    const response = await axios.post(`${ESP_HTTP_URL}/api/send`, data, {
      timeout: 5000
    });
    console.log("[Server -> ESP]", response.data);
    return { success: true, response: response.data };
  } catch (error) {
    console.error("❌ Send to ESP32 failed:", error.message);
    return { success: false, error: error.message };
  }
}

// ---------------------------
// 폴링 시작
// ---------------------------
function startPolling() {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(pollESP32, POLL_INTERVAL);
  pollESP32(); // 즉시 한 번 실행
}

// ---------------------------
// Express HTTP API
// ---------------------------

// 상태 확인
app.get("/status", (req, res) => {
  res.json({
    connected,
    lastMessage,
    espUrl: ESP_HTTP_URL,
  });
});

// ESP로 데이터 전송 (POST)
app.post("/send", async (req, res) => {
  const payload = req.body;
  const result = await sendToESP32(payload);
  
  if (result.success) {
    res.json({ ok: true, sent: payload, response: result.response });
  } else {
    res.status(500).json({ ok: false, error: result.error });
  }
});

// SSE 이벤트 스트림
app.get("/events", (req, res) => {
  res.set({
    "Content-Type": "text/event-stream",
    "Cache-Control": "no-cache",
    Connection: "keep-alive",
    "Access-Control-Allow-Origin": "*",
  });
  res.flushHeaders();

  res.write(
    `data: ${JSON.stringify({ type: "connected", time: new Date() })}\n\n`
  );

  sseClients.add(res);
  console.log("SSE client connected:", sseClients.size);

  req.on("close", () => {
    sseClients.delete(res);
    console.log("SSE client disconnected:", sseClients.size);
  });
});

// 간단한 테스트 페이지
app.get("/", (req, res) => {
  res.type("html").send(`
    <html>
      <head>
        <title>ESP32 HTTP Client</title>
        <style>
          body { font-family: Arial, sans-serif; margin: 20px; }
          #status { padding: 10px; background: #f0f0f0; border-radius: 5px; }
          #events { max-height: 400px; overflow-y: auto; }
          li { margin: 5px 0; padding: 5px; background: #f9f9f9; }
          .send-form { margin: 20px 0; }
          input, button { padding: 8px; margin: 5px; }
        </style>
      </head>
      <body>
        <h2>ESP32 HTTP Client (Express)</h2>
        <div id="status">Connecting...</div>
        
        <div class="send-form">
          <h3>Send Message to ESP32:</h3>
          <input type="text" id="messageInput" placeholder="Enter message" />
          <button onclick="sendMessage()">Send</button>
        </div>
        
        <h3>Received Messages:</h3>
        <ul id="events"></ul>
        
        <script>
          const evt = new EventSource('/events');
          const statusDiv = document.getElementById('status');
          const list = document.getElementById('events');
          const messageInput = document.getElementById('messageInput');

          evt.onmessage = (e) => {
            const d = JSON.parse(e.data);
            const li = document.createElement('li');
            li.textContent = JSON.stringify(d, null, 2);
            list.prepend(li);
            
            if (d.type === 'connected') {
              statusDiv.textContent = 'Connected to ESP32 (HTTP)';
              statusDiv.style.background = '#d4edda';
            } else if (d.type === 'disconnected') {
              statusDiv.textContent = 'Disconnected from ESP32';
              statusDiv.style.background = '#f8d7da';
            }
          };
          
          evt.onerror = () => {
            statusDiv.textContent = 'SSE error/disconnected';
            statusDiv.style.background = '#f8d7da';
          };
          
          async function sendMessage() {
            const message = messageInput.value.trim();
            if (!message) return;
            
            try {
              const response = await fetch('/send', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ message: message, time: new Date().toISOString() })
              });
              
              const result = await response.json();
              if (result.ok) {
                messageInput.value = '';
                console.log('Message sent:', result);
              } else {
                alert('Send failed: ' + result.error);
              }
            } catch (error) {
              alert('Send error: ' + error.message);
            }
          }
          
          messageInput.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') sendMessage();
          });
        </script>
      </body>
    </html>
  `);
});

// ---------------------------
// 서버 시작
// ---------------------------
app.listen(PORT, () => {
  console.log(`🚀 Express server running at http://localhost:${PORT}`);
  console.log(`📡 ESP32 HTTP URL: ${ESP_HTTP_URL}`);
  startPolling(); // 폴링 시작
});
