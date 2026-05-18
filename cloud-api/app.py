#app.py
# 
# Copyright © 2025. All Rights Reserved.
# 
# PROPRIETARY AND CONFIDENTIAL
# This software is the proprietary information of the copyright holder.
# Unauthorized copying, distribution, or use is strictly prohibited.
# See LICENSE file in the root directory for terms and conditions.
#

import os
import base64
import json
from flask import Flask, request, jsonify, Response
import vertexai
from vertexai.preview.generative_models import GenerativeModel
# NOTE: sounddevice removed - not needed in Cloud Run (no audio hardware)
# import sounddevice as sd
import numpy as np
from google.cloud import speech
import requests
import tempfile
import wave
from google.cloud import texttospeech
import uuid
import sqlite3
from datetime import datetime
from flask_socketio import SocketIO, emit

app = Flask(__name__)
# Initialize SocketIO with threading mode (safer for Cloud Run with mixed libraries)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='threading', logger=True, engineio_logger=True)
LATEST_INPUT_WAV = None
LATEST_INPUT_META = {}
AUDIO_CACHE = {}
AUDIO_CACHE_MAX_ITEMS = 20

# Speech cleanup tuning (override via Cloud Run env vars if needed)
AUDIO_HP_CUTOFF_HZ = int(os.environ.get("AUDIO_HP_CUTOFF_HZ", "80"))   # lower HP keeps more voice bass
AUDIO_LP_CUTOFF_HZ = int(os.environ.get("AUDIO_LP_CUTOFF_HZ", "3400"))
AUDIO_TARGET_DBFS = float(os.environ.get("AUDIO_TARGET_DBFS", "-18.0"))  # was -24
AUDIO_MAX_GAIN_DB = float(os.environ.get("AUDIO_MAX_GAIN_DB", "24.0"))   # was 10

# Inisialisasi Vertex AI
#key_path = os.path.join(os.path.dirname(__file__), "key.json")
#os.environ["GOOGLE_APPLICATION_CREDENTIALS"] = key_path

# Get project ID and location from environment variables for security
YOUR_PROJECT_ID = os.environ.get("GOOGLE_CLOUD_PROJECT_ID", os.environ.get("GCP_PROJECT_ID"))
YOUR_VERTEX_AI_LOCATION = os.environ.get("VERTEX_AI_LOCATION", "us-central1")

if not YOUR_PROJECT_ID:
    raise ValueError("GOOGLE_CLOUD_PROJECT_ID or GCP_PROJECT_ID environment variable must be set")

vertexai.init(project=YOUR_PROJECT_ID, location=YOUR_VERTEX_AI_LOCATION)
model = GenerativeModel("gemini-2.0-flash-001")

# ── Singleton API clients (avoid per-request init overhead ~200-500ms) ────
speech_client = speech.SpeechClient()
tts_client = texttospeech.TextToSpeechClient()

# In-memory session store.
# WARNING: This is not suitable for production on Cloud Run with multiple
# instances, as each instance will have its own memory. Use a shared
# database like Redis or Firestore for session management in production.
SESSIONS = {}
# Map Flask-SocketIO connection sid to session_id
SID_TO_SESSION = {}

class AudioAccumulator:
    """Accumulates incoming audio chunks with overflow protection."""
    def __init__(self, max_bytes=320000):
        self.buffer = bytearray()
        self.max_bytes = max_bytes
    
    def add_chunk(self, chunk: bytes) -> bool:
        """Add audio chunk to buffer. Returns False if buffer would overflow."""
        if len(self.buffer) + len(chunk) > self.max_bytes:
            return False  # Reject chunk, buffer full
        self.buffer.extend(chunk)
        return True
    
    def get_audio(self) -> bytes:
        """Get accumulated audio as bytes."""
        return bytes(self.buffer)
    
    def clear(self):
        """Clear the audio buffer."""
        self.buffer.clear()

DB_PATH = os.path.join(os.path.dirname(__file__), 'session_history.db')

def init_db():
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('''
        CREATE TABLE IF NOT EXISTS chat_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id TEXT,
            role TEXT,
            message TEXT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    ''')
    conn.commit()
    conn.close()

init_db()

def save_message(session_id, role, message):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('INSERT INTO chat_history (session_id, role, message, timestamp) VALUES (?, ?, ?, ?)',
              (session_id, role, message, datetime.now()))
    conn.commit()
    conn.close()

def get_history(session_id):
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    c.execute('SELECT role, message FROM chat_history WHERE session_id = ? ORDER BY id ASC', (session_id,))
    rows = c.fetchall()
    conn.close()
    # Format sesuai dengan yang diharapkan VertexAI: [{'role': 'user', 'parts': [msg]}, ...]
    return [{'role': row[0], 'parts': [row[1]]} for row in rows]

def load_history(session_id):
    """Load conversation history from database for a session."""
    return get_history(session_id)

def persist_session(session_id):
    """Persist session data to database on disconnect."""
    # Session messages are already saved incrementally via save_message()
    # This function is called on disconnect for any final cleanup
    if session_id in SESSIONS:
        print(f'[WebSocket] Session {session_id} disconnected and persisted')

# WebSocket event handlers
@socketio.on('connect')
def handle_connect():
    """Handle WebSocket connection - create session and send acknowledgment."""
    from flask import request as flask_request
    sid = flask_request.sid
    
    session_id = str(uuid.uuid4())
    SESSIONS[session_id] = {
        'audio_buffer': bytearray(),
        'history': load_history(session_id),
        'connected_at': datetime.utcnow()
    }
    SID_TO_SESSION[sid] = session_id
    
    print(f'[WebSocket] Client connected, sid: {sid}, session_id: {session_id}')
    emit('message', json.dumps({'type': 'connected', 'session_id': session_id}))

@socketio.on('disconnect')
def handle_disconnect():
    """Handle WebSocket disconnection - persist session to database."""
    from flask import request as flask_request
    sid = flask_request.sid
    
    # Find session_id for this connection
    session_id = SID_TO_SESSION.get(sid)
    
    if session_id:
        persist_session(session_id)
        if session_id in SESSIONS:
            del SESSIONS[session_id]
        del SID_TO_SESSION[sid]
        print(f'[WebSocket] Client disconnected, sid: {sid}, session_id: {session_id}')
    else:
        print(f'[WebSocket] Client disconnected, sid: {sid}, no session found')

@socketio.on('message')
def handle_message(data):
    """Handle WebSocket messages - route binary audio chunks and JSON control messages."""
    from flask import request as flask_request
    sid = flask_request.sid
    
    # Get session for this connection
    session_id = SID_TO_SESSION.get(sid)
    if not session_id:
        print(f'[WebSocket] Message received but no session found for sid: {sid}')
        emit('message', json.dumps({'type': 'error', 'message': 'Session not found'}))
        return
    
    session = SESSIONS.get(session_id)
    if not session:
        print(f'[WebSocket] Message received but session data not found: {session_id}')
        emit('message', json.dumps({'type': 'error', 'message': 'Session data not found'}))
        return
    
    # Route based on message type: binary (audio chunk) or JSON (control message)
    if isinstance(data, bytes):
        # Raw binary audio chunk
        handle_audio_chunk(session_id, session, data)
    else:
        # JSON control message or base64 audio_chunk from ESP32
        try:
            msg = json.loads(data) if isinstance(data, str) else data
            msg_type = msg.get('type')
            
            if msg_type == 'audio_chunk':
                # ESP32 sends base64-encoded PCM as JSON: {"type":"audio_chunk","data":"<b64>"}
                b64_data = msg.get('data', '')
                if b64_data:
                    try:
                        pcm_bytes = base64.b64decode(b64_data)
                        handle_audio_chunk(session_id, session, pcm_bytes)
                    except Exception as decode_err:
                        print(f'[WebSocket] base64 decode error: {decode_err}')
                        emit('message', json.dumps({'type': 'error', 'message': 'Invalid audio_chunk base64'}))
            elif msg_type == 'audio_end':
                handle_audio_end(session_id, session)
            elif msg_type == 'ping':
                handle_ping(session_id)
            else:
                print(f'[WebSocket] Unknown message type: {msg_type}')
                emit('message', json.dumps({'type': 'error', 'message': f'Unknown message type: {msg_type}'}))
        except json.JSONDecodeError as e:
            print(f'[WebSocket] JSON parse error: {e}')
            emit('message', json.dumps({'type': 'error', 'message': 'Invalid JSON message'}))
        except Exception as e:
            print(f'[WebSocket] Error handling message: {e}')
            emit('message', json.dumps({'type': 'error', 'message': f'Message handling error: {str(e)}'}))

def handle_audio_chunk(session_id: str, session: dict, chunk: bytes):
    """Handle incoming binary audio chunk or base64-encoded JSON chunk.
    
    The ESP32 firmware sends audio as base64-encoded JSON:
      {"type":"audio_chunk","data":"<base64-pcm>"}
    This arrives as a text message (already routed through handle_message),
    so this function receives raw PCM bytes after decoding.
    """
    if 'audio_accumulator' not in session:
        session['audio_accumulator'] = AudioAccumulator(max_bytes=640000)  # ~20s of 16kHz mono
    
    accumulator = session['audio_accumulator']
    if not accumulator.add_chunk(chunk):
        print(f'[WebSocket] Audio buffer overflow for session {session_id}')
        emit('message', json.dumps({'type': 'error', 'message': 'Audio too long (max 20 seconds)'}))
        accumulator.clear()
        return
    
    print(f'[WebSocket] Audio chunk: {len(chunk)} bytes, total: {len(accumulator.buffer)} bytes')

def handle_audio_end(session_id: str, session: dict):
    """Handle audio_end — run STT → Gemini → sentence-level TTS streaming."""
    import re
    print(f'[WebSocket] Audio end for session {session_id}')
    
    accumulator = session.get('audio_accumulator')
    if not accumulator or len(accumulator.buffer) == 0:
        emit('message', json.dumps({'type': 'error', 'message': 'No audio data received'}))
        return
    
    raw_pcm = accumulator.get_audio()
    accumulator.clear()
    print(f'[WebSocket] Processing {len(raw_pcm)} bytes of PCM audio')
    
    emit('message', json.dumps({'type': 'processing'}))
    
    try:
        # ── Build a WAV from the raw PCM ────────────────────────────────────
        import io
        wav_io = io.BytesIO()
        with wave.open(wav_io, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)   # 16-bit
            wf.setframerate(16000)
            wf.writeframes(raw_pcm)
        wav_bytes = wav_io.getvalue()
        
        # ── STT (uses global singleton client) ────────────────────────────────
        audio_obj = speech.RecognitionAudio(content=wav_bytes)
        config = speech.RecognitionConfig(
            encoding=speech.RecognitionConfig.AudioEncoding.LINEAR16,
            sample_rate_hertz=16000,
            language_code="id-ID",
            enable_automatic_punctuation=True
        )
        stt_response = speech_client.recognize(config=config, audio=audio_obj)
        question = " ".join(
            r.alternatives[0].transcript for r in stt_response.results
        ).strip()
        print(f'[WebSocket] STT: "{question}"')
        
        if not question:
            emit('message', json.dumps({'type': 'error', 'message': 'Suara tidak terdeteksi. Coba bicara lebih dekat ke mikrofon.'}))
            return
        
        emit('message', json.dumps({'type': 'stt_result', 'text': question}))
        
        # ── Gemini (max_output_tokens limits response length for speed) ─────
        from vertexai.generative_models import GenerationConfig
        history = load_history(session_id)
        chat = model.start_chat(history=history)
        save_message(session_id, 'user', question)
        
        prompt = u"Jawab singkat max 2 kalimat: %s" % question
        gemini_response = chat.send_message(
            prompt,
            generation_config=GenerationConfig(
                max_output_tokens=100,
                temperature=0.7,
            )
        )
        answer = gemini_response.text.strip()
        save_message(session_id, 'assistant', answer)
        print(f'[WebSocket] Gemini: "{answer[:120]}"')
        
        # ── Sentence-level TTS streaming ─────────────────────────────────────
        # Split answer into sentences, TTS each one, emit audio_ready immediately.
        # ESP32 starts playing sentence 1 while sentences 2-N are still being TTS'd.
        sentences = re.split(r'(?<=[.!?])\s+', answer)
        sentences = [s.strip() for s in sentences if s.strip()]
        
        # If splitting produced nothing useful, treat entire answer as one sentence
        if not sentences:
            sentences = [answer]
        
        voice_params = texttospeech.VoiceSelectionParams(
            language_code="id-ID",
            name="id-ID-Wavenet-A",
            ssml_gender=texttospeech.SsmlVoiceGender.NEUTRAL
        )
        audio_cfg = texttospeech.AudioConfig(
            audio_encoding=texttospeech.AudioEncoding.LINEAR16,
            sample_rate_hertz=16000,
            speaking_rate=1.15
        )
        
        for i, sentence in enumerate(sentences):
            print(f'[WebSocket] TTS sentence {i+1}/{len(sentences)}: "{sentence[:60]}"')
            tts_input = texttospeech.SynthesisInput(text=sentence)
            tts_response = tts_client.synthesize_speech(
                input=tts_input, voice=voice_params, audio_config=audio_cfg
            )
            tts_wav_bytes = tts_response.audio_content
            
            audio_id = uuid.uuid4().hex
            AUDIO_CACHE[audio_id] = {
                'bytes': tts_wav_bytes,
                'mime': 'audio/wav',
                'created_at': datetime.utcnow().isoformat() + 'Z',
            }
            while len(AUDIO_CACHE) > AUDIO_CACHE_MAX_ITEMS:
                del AUDIO_CACHE[next(iter(AUDIO_CACHE))]
            
            audio_url = f"/api/audio/{audio_id}"
            print(f'[WebSocket] Sentence {i+1} cached: {audio_url} ({len(tts_wav_bytes)} bytes)')
            
            # Emit immediately — ESP32 starts playing while next sentences are TTS'd
            emit('message', json.dumps({
                'type': 'audio_ready',
                'text': sentence,
                'audio_url': audio_url,
                'audio_mime': 'audio/wav'
            }))
        
        # Signal completion
        emit('message', json.dumps({
            'type': 'done',
            'full_answer': answer
        }))
    
    except Exception as e:
        print(f'[WebSocket] Pipeline error: {e}')
        emit('message', json.dumps({'type': 'error', 'message': str(e)}))

def handle_ping(session_id: str):
    """Handle ping message - respond with pong."""
    print(f'[WebSocket] Ping received from session {session_id}')
    emit('message', json.dumps({'type': 'pong'}))

@app.route('/')
def index():
    return '''
    <!DOCTYPE html>
    <html>
    <head>
        <title>Gabriel WebSocket Test</title>
        <meta charset="UTF-8">
        <style>
            * { margin: 0; padding: 0; box-sizing: border-box; }
            body { 
                font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
                background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
                min-height: 100vh;
                display: flex;
                justify-content: center;
                align-items: center;
                padding: 20px;
            }
            .container {
                background: white;
                border-radius: 20px;
                box-shadow: 0 20px 60px rgba(0,0,0,0.3);
                padding: 40px;
                max-width: 800px;
                width: 100%;
            }
            h1 {
                color: #667eea;
                margin-bottom: 10px;
                font-size: 32px;
            }
            .subtitle {
                color: #666;
                margin-bottom: 30px;
                font-size: 14px;
            }
            .status {
                padding: 15px;
                border-radius: 10px;
                margin-bottom: 20px;
                font-weight: 500;
            }
            .status.disconnected {
                background: #fee;
                color: #c33;
                border: 2px solid #fcc;
            }
            .status.connected {
                background: #efe;
                color: #3c3;
                border: 2px solid #cfc;
            }
            .status.connecting {
                background: #ffc;
                color: #cc6;
                border: 2px solid #fec;
            }
            .info {
                background: #f5f5f5;
                padding: 15px;
                border-radius: 10px;
                margin-bottom: 20px;
                font-size: 13px;
                color: #666;
            }
            .info strong {
                color: #333;
            }
            .buttons {
                display: flex;
                gap: 10px;
                margin-bottom: 20px;
                flex-wrap: wrap;
            }
            button {
                padding: 12px 24px;
                border: none;
                border-radius: 8px;
                font-size: 14px;
                font-weight: 600;
                cursor: pointer;
                transition: all 0.3s;
                flex: 1;
                min-width: 120px;
            }
            button:hover {
                transform: translateY(-2px);
                box-shadow: 0 4px 12px rgba(0,0,0,0.2);
            }
            button:active {
                transform: translateY(0);
            }
            .btn-connect {
                background: #667eea;
                color: white;
            }
            .btn-disconnect {
                background: #f56565;
                color: white;
            }
            .btn-ping {
                background: #48bb78;
                color: white;
            }
            .btn-audio {
                background: #ed8936;
                color: white;
            }
            button:disabled {
                opacity: 0.5;
                cursor: not-allowed;
            }
            .log {
                background: #1a202c;
                color: #e2e8f0;
                padding: 20px;
                border-radius: 10px;
                max-height: 400px;
                overflow-y: auto;
                font-family: 'Courier New', monospace;
                font-size: 12px;
                line-height: 1.6;
            }
            .log-entry {
                margin-bottom: 8px;
                padding: 4px 0;
                border-bottom: 1px solid #2d3748;
            }
            .log-time {
                color: #718096;
                margin-right: 10px;
            }
            .log-type {
                font-weight: bold;
                margin-right: 10px;
            }
            .log-send { color: #4299e1; }
            .log-recv { color: #48bb78; }
            .log-error { color: #f56565; }
            .log-info { color: #ed8936; }
        </style>
    </head>
    <body>
        <div class="container">
            <h1>🚀 Gabriel WebSocket Test</h1>
            <div class="subtitle">Real-time WebSocket Connection Testing</div>
            
            <div id="status" class="status disconnected">
                ⚫ Disconnected
            </div>
            
            <div class="info">
                <strong>Session ID:</strong> <span id="sessionId">Not connected</span><br>
                <strong>WebSocket URL:</strong> <span id="wsUrl">-</span><br>
                <strong>Messages Sent:</strong> <span id="msgSent">0</span> | 
                <strong>Received:</strong> <span id="msgRecv">0</span>
            </div>
            
            <div class="buttons">
                <button class="btn-connect" onclick="connect()" id="btnConnect">Connect</button>
                <button class="btn-disconnect" onclick="disconnect()" id="btnDisconnect" disabled>Disconnect</button>
                <button class="btn-ping" onclick="sendPing()" id="btnPing" disabled>Send Ping</button>
                <button class="btn-audio" onclick="sendTestAudio()" id="btnAudio" disabled>Send Test Audio</button>
            </div>
            
            <div class="log" id="log">
                <div class="log-entry">
                    <span class="log-time">[Ready]</span>
                    <span class="log-type log-info">INFO</span>
                    Click "Connect" to start WebSocket connection
                </div>
            </div>
        </div>
        
        <script src="https://cdn.socket.io/4.5.4/socket.io.min.js"></script>
        <script>
            let socket = null;
            let sessionId = null;
            let msgSent = 0;
            let msgRecv = 0;
            
            function updateStatus(status, text) {
                const statusEl = document.getElementById('status');
                statusEl.className = 'status ' + status;
                statusEl.innerHTML = text;
            }
            
            function log(type, message) {
                const logEl = document.getElementById('log');
                const time = new Date().toLocaleTimeString();
                const entry = document.createElement('div');
                entry.className = 'log-entry';
                entry.innerHTML = `
                    <span class="log-time">[${time}]</span>
                    <span class="log-type log-${type}">${type.toUpperCase()}</span>
                    ${message}
                `;
                logEl.appendChild(entry);
                logEl.scrollTop = logEl.scrollHeight;
            }
            
            function updateButtons(connected) {
                document.getElementById('btnConnect').disabled = connected;
                document.getElementById('btnDisconnect').disabled = !connected;
                document.getElementById('btnPing').disabled = !connected;
                document.getElementById('btnAudio').disabled = !connected;
            }
            
            function connect() {
                if (socket && socket.connected) {
                    log('info', 'Already connected');
                    return;
                }
                
                updateStatus('connecting', '🟡 Connecting...');
                log('info', 'Connecting to WebSocket...');
                
                const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
                const wsUrl = protocol + '//' + window.location.host;
                document.getElementById('wsUrl').textContent = wsUrl;
                
                socket = io(wsUrl, {
                    transports: ['websocket', 'polling']
                });
                
                socket.on('connect', () => {
                    updateStatus('connected', '🟢 Connected');
                    log('info', 'WebSocket connected!');
                    updateButtons(true);
                });
                
                socket.on('disconnect', () => {
                    updateStatus('disconnected', '⚫ Disconnected');
                    log('info', 'WebSocket disconnected');
                    updateButtons(false);
                    sessionId = null;
                    document.getElementById('sessionId').textContent = 'Not connected';
                });
                
                socket.on('message', (data) => {
                    msgRecv++;
                    document.getElementById('msgRecv').textContent = msgRecv;
                    
                    try {
                        const msg = typeof data === 'string' ? JSON.parse(data) : data;
                        log('recv', 'Received: ' + JSON.stringify(msg, null, 2));
                        
                        if (msg.type === 'connected') {
                            sessionId = msg.session_id;
                            document.getElementById('sessionId').textContent = sessionId;
                            log('info', 'Session established: ' + sessionId);
                        }
                    } catch (e) {
                        log('error', 'Failed to parse message: ' + e.message);
                    }
                });
                
                socket.on('connect_error', (error) => {
                    log('error', 'Connection error: ' + error.message);
                    updateStatus('disconnected', '⚫ Connection Failed');
                    updateButtons(false);
                });
            }
            
            function disconnect() {
                if (socket) {
                    log('info', 'Disconnecting...');
                    socket.disconnect();
                }
            }
            
            function sendPing() {
                if (!socket || !socket.connected) {
                    log('error', 'Not connected');
                    return;
                }
                
                const msg = {type: 'ping'};
                socket.emit('message', JSON.stringify(msg));
                msgSent++;
                document.getElementById('msgSent').textContent = msgSent;
                log('send', 'Sent ping');
            }
            
            function sendTestAudio() {
                if (!socket || !socket.connected) {
                    log('error', 'Not connected');
                    return;
                }
                
                log('info', 'Sending test audio chunks...');
                
                // Send 3 chunks of 5120 bytes each
                for (let i = 0; i < 3; i++) {
                    const chunk = new Uint8Array(5120);
                    for (let j = 0; j < 5120; j++) {
                        chunk[j] = (i * 10 + j) % 256;
                    }
                    socket.emit('message', chunk.buffer);
                    msgSent++;
                    log('send', `Sent audio chunk ${i+1}/3 (5120 bytes)`);
                }
                
                // Send audio_end signal
                setTimeout(() => {
                    const msg = {type: 'audio_end'};
                    socket.emit('message', JSON.stringify(msg));
                    msgSent++;
                    document.getElementById('msgSent').textContent = msgSent;
                    log('send', 'Sent audio_end signal');
                }, 500);
            }
            
            // Auto-connect on page load
            window.addEventListener('load', () => {
                log('info', 'Page loaded. Ready to connect.');
            });
        </script>
    </body>
    </html>
    '''

# Endpoint untuk menerima audio dari ESP32
from vertexai.generative_models import Part as GeminiPart

@app.route('/api/process-audio', methods=['POST'])
def api_process_audio():
    """
    Receives base64-encoded WAV from ESP32.
    Uses Gemini multimodal (audio → text) — no separate STT step.
    Returns: {answer, audio_url} where audio_url points to a LINEAR16 WAV.
    """
    try:
        if not request.is_json:
            return jsonify({'error': 'Content-Type must be application/json'}), 400

        data = request.get_json()
        audio_b64  = data.get('audio')
        session_id = data.get('session_id') or uuid.uuid4().hex

        if not audio_b64:
            return jsonify({'error': 'No audio data', 'stage': 'validate'}), 400

        try:
            audio_bytes = base64.b64decode(audio_b64, validate=True)
        except Exception as e:
            return jsonify({'error': f'Invalid base64: {e}', 'stage': 'decode'}), 400

        print(f'[API] Audio: {len(audio_bytes)} bytes, session: {session_id}')

        # ── Gemini multimodal: audio → transcript + answer ──────────────────
        audio_part = GeminiPart.from_data(data=audio_bytes, mime_type='audio/wav')

        history = get_history(session_id)
        history_ctx = ""
        if history:
            history_ctx = "Konteks percakapan:\n"
            history_ctx += "\n".join(
                f"{h['role']}: {h['parts'][0]}" for h in history[-6:]
            ) + "\n\n"

        prompt = (
            f"{history_ctx}"
            "Kamu adalah Gabriel, asisten AI suara. "
            "Dengarkan audio dan balas dalam format TEPAT ini:\n"
            "TRANSCRIPT: <apa yang diucapkan>\n"
            "ANSWER: <jawaban singkat max 2 kalimat, bahasa Indonesia>\n\n"
            "Jika audio tidak jelas:\n"
            "TRANSCRIPT: [tidak jelas]\n"
            "ANSWER: Maaf, saya tidak mendengar. Bisa ulangi?"
        )

        gemini_resp = model.generate_content([audio_part, prompt])
        raw = gemini_resp.text.strip()
        print(f'[API] Gemini: {raw[:300]}')

        question = "[audio]"
        answer   = raw
        for line in raw.splitlines():
            up = line.upper()
            if up.startswith("TRANSCRIPT:"):
                question = line[len("TRANSCRIPT:"):].strip()
            elif up.startswith("ANSWER:"):
                answer = line[len("ANSWER:"):].strip()

        if not answer:
            answer = raw

        print(f'[API] Q: "{question}" | A: "{answer}"')
        save_message(session_id, 'user', question)
        save_message(session_id, 'assistant', answer)

        # ── TTS → WAV @ 16kHz for ESP32 (direct LINEAR16, no MP3 conversion) ──
        tts_wav = tts_client.synthesize_speech(
            input=texttospeech.SynthesisInput(text=answer),
            voice=texttospeech.VoiceSelectionParams(
                language_code="id-ID",
                name="id-ID-Wavenet-A",
                ssml_gender=texttospeech.SsmlVoiceGender.FEMALE
            ),
            audio_config=texttospeech.AudioConfig(
                audio_encoding=texttospeech.AudioEncoding.LINEAR16,
                sample_rate_hertz=16000,
                speaking_rate=1.15
            )
        ).audio_content

        audio_id = uuid.uuid4().hex
        AUDIO_CACHE[audio_id] = {
            'bytes': tts_wav,
            'mime':  'audio/wav',
            'created_at': datetime.utcnow().isoformat() + 'Z',
        }
        while len(AUDIO_CACHE) > AUDIO_CACHE_MAX_ITEMS:
            del AUDIO_CACHE[next(iter(AUDIO_CACHE))]

        audio_url = f'/api/audio/{audio_id}'
        print(f'[API] Done. WAV: {len(tts_wav)} bytes → {audio_url}')

        return jsonify({
            'question':   question,
            'answer':     answer,
            'session_id': session_id,
            'stage':      'ok',
            'audio_url':  audio_url,
            'audio_mime': 'audio/wav',
        })

    except Exception as e:
        print(f'[API] ERROR: {e}')
        import traceback; traceback.print_exc()
        return jsonify({'error': str(e), 'stage': 'internal_error'}), 500


@app.route('/api/audio/<audio_id>', methods=['GET'])
def get_cached_audio(audio_id):
    audio_item = AUDIO_CACHE.get(audio_id)
    if not audio_item:
        return jsonify({'error': 'Audio tidak ditemukan atau kadaluarsa'}), 404
    return Response(
        audio_item['bytes'],
        mimetype=audio_item.get('mime', 'audio/mpeg'),
        headers={'Content-Disposition': f'inline; filename={audio_id}.mp3'}
    )

@app.route('/api/debug/latest-input-meta', methods=['GET'])
def debug_latest_input_meta():
    if not LATEST_INPUT_META:
        return jsonify({'error': 'Belum ada audio input tersimpan'}), 404
    return jsonify({
        'status': 'ok',
        'meta': LATEST_INPUT_META
    })

@app.route('/api/debug/latest-input-wav', methods=['GET'])
def debug_latest_input_wav():
    if not LATEST_INPUT_WAV:
        return jsonify({'error': 'Belum ada audio input tersimpan'}), 404
    return Response(
        LATEST_INPUT_WAV,
        mimetype='audio/wav',
        headers={
            'Content-Disposition': 'inline; filename=latest-input.wav'
        }
    )

# Endpoint untuk testing API
@app.route('/api/test', methods=['POST'])
def test_api():
    try:
        test_text = "Halo, ini adalah tes API."
        # Generate response dari Gemini
        gemini_response = model.generate_content(test_text)
        answer = gemini_response.text

        # Convert ke audio
        synthesize_speech(answer, "temp_test_response.mp3")

        with open("temp_test_response.mp3", "rb") as audio_file:
            audio_base64 = base64.b64encode(audio_file.read()).decode('utf-8')

        os.remove("temp_test_response.mp3")

        return jsonify({
            'status': 'success',
            'data': {
                'test_text': test_text,
                'answer': answer,
                'audio': audio_base64
            }
        })

    except Exception as e:
        return jsonify({
            'status': 'error',
            'error': str(e)
        }), 500

# Fungsi untuk merekam audio dari mic (format: WAV)
# NOTE: Only used in local mode (RUN_LOCAL=true), not in Cloud Run
def record_audio(duration=5, fs=16000):
    try:
        import sounddevice as sd
        print(f"Rekam suara selama {duration} detik...")
        audio = sd.rec(int(duration * fs), samplerate=fs, channels=1, dtype='int16')
        sd.wait()
        return audio, fs
    except ImportError:
        print("sounddevice not available (Cloud Run mode)")
        return None, None

# Simpan audio ke file WAV sementara
def save_wav(audio, fs):
    temp = tempfile.NamedTemporaryFile(delete=False, suffix='.wav')
    with wave.open(temp.name, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(fs)
        wf.writeframes(audio.tobytes())
    return temp.name

# Konversi file audio ke base64
def audio_to_base64(filepath):
    with open(filepath, 'rb') as f:
        return base64.b64encode(f.read()).decode('utf-8')

# Kirim audio ke server dan terima jawaban
def send_audio_to_server(audio_base64, server_url):
    payload = {'audio': audio_base64}
    r = requests.post(server_url, json=payload)
    r.raise_for_status()
    return r.json()

# Main loop
def main():
    SERVER_URL = 'http://localhost:5000/api/process-audio'  # Ganti jika server beda
    while True:
        input("Tekan ENTER untuk mulai rekam (atau Ctrl+C untuk keluar)...")
        audio, fs = record_audio(duration=5)
        wav_path = save_wav(audio, fs)
        audio_b64 = audio_to_base64(wav_path)
        print("Mengirim audio ke server...")
        try:
            response = send_audio_to_server(audio_b64, SERVER_URL)
            print("Jawaban dari AI:", response.get('answer'))
            # Jika ingin Pepper mengucapkan:
            # pepper_say(response.get('answer'))
            # Jika ingin play audio TTS:
            if response.get('audio_base64'):
                tts_path = tempfile.NamedTemporaryFile(delete=False, suffix='.mp3').name
                with open(tts_path, 'wb') as f:
                    f.write(base64.b64decode(response['audio_base64']))
                print(f"Audio jawaban disimpan di: {tts_path}")
                # Play audio jika ingin (gunakan mpg123/vlc atau library lain)
        except Exception as e:
            print("Gagal:", e)

def synthesize_speech(text, output_path):
    client = texttospeech.TextToSpeechClient()
    input_text = texttospeech.SynthesisInput(text=text)
    # Pilih voice yang natural
    voice = texttospeech.VoiceSelectionParams(
        language_code="id-ID",
        name="id-ID-Wavenet-A",  # Coba juga id-ID-Wavenet-B, dst
        ssml_gender=texttospeech.SsmlVoiceGender.NEUTRAL
    )
    audio_config = texttospeech.AudioConfig(
        audio_encoding=texttospeech.AudioEncoding.MP3,
        speaking_rate=1.0
    )
    response = client.synthesize_speech(
        input=input_text, voice=voice, audio_config=audio_config
    )
    with open(output_path, "wb") as out:
        out.write(response.audio_content)

if __name__ == '__main__':
    if os.environ.get("RUN_LOCAL", "false").lower() == "true":
        main()  # jalankan hanya saat lokal
    else:
        socketio.run(app, host='0.0.0.0', port=int(os.environ.get('PORT', 8080)))
