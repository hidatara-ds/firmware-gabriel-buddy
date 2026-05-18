"""
WebSocket handlers — Gabriel AI
Threading model: heavy work runs in native Python threads,
fully compatible with Google Cloud gRPC (STT / Gemini / TTS).
"""
import json
import threading
import uuid
import re
from datetime import datetime, timezone


from flask import request as flask_request
from flask_socketio import emit

from . import state
from .services import (
    stt_transcribe,
    ask_gemini_stream,
    append_exchange,
    synthesize_speech_bytes,
    cache_audio_bytes,
    convert_to_wav_16khz_mono,
    detect_language,
)


_socketio = None   # set on register


# ─────────────────────────────────────────────────────────────────────────────
# Audio accumulator
# ─────────────────────────────────────────────────────────────────────────────

class AudioAccumulator:
    """Thread-safe audio chunk accumulator with overflow guard."""

    MAX_BYTES = 320_000   # ~10 sec @ 16kHz/16bit/mono

    def __init__(self):
        self.buffer = bytearray()
        self._lock = threading.Lock()

    def add_chunk(self, chunk: bytes) -> bool:
        with self._lock:
            if len(self.buffer) + len(chunk) > self.MAX_BYTES:
                return False
            self.buffer.extend(chunk)
            return True

    def take(self) -> bytes:
        """Atomically read and clear the buffer."""
        with self._lock:
            data = bytes(self.buffer)
            self.buffer.clear()
            return data

    def __len__(self):
        with self._lock:
            return len(self.buffer)


# ─────────────────────────────────────────────────────────────────────────────
# Helper: emit from any thread
# ─────────────────────────────────────────────────────────────────────────────

def _emit(sid: str, payload: dict):
    if _socketio:
        _socketio.emit("message", json.dumps(payload), to=sid)


# ─────────────────────────────────────────────────────────────────────────────
# Registration
# ─────────────────────────────────────────────────────────────────────────────

def register_websocket_handlers(socketio):
    global _socketio
    _socketio = socketio

    @socketio.on("connect")
    def handle_connect():
        sid = flask_request.sid
        session_id = str(uuid.uuid4())
        state.SESSIONS[session_id] = {
            "session_id": session_id,
            "audio_accumulator": AudioAccumulator(),
            "connected_at": datetime.now(timezone.utc),
        }
        state.SID_TO_SESSION[sid] = session_id
        emit("message", json.dumps({
            "type": "connected",
            "session_id": session_id,
        }))

    @socketio.on("disconnect")
    def handle_disconnect():
        sid = flask_request.sid
        session_id = state.SID_TO_SESSION.pop(sid, None)
        if session_id:
            state.SESSIONS.pop(session_id, None)

    @socketio.on("message")
    def handle_message(data):
        sid = flask_request.sid
        session_id = state.SID_TO_SESSION.get(sid)
        if not session_id:
            emit("message", json.dumps({"type": "error", "message": "Session not found"}))
            return

        session = state.SESSIONS.get(session_id)
        if not session:
            emit("message", json.dumps({"type": "error", "message": "Session expired"}))
            return

        # ── Binary frame = raw audio chunk ──────────────────────────────────
        if isinstance(data, bytes):
            acc = session["audio_accumulator"]
            if not acc.add_chunk(data):
                emit("message", json.dumps({
                    "type": "error",
                    "message": "Audio terlalu panjang (max 10 detik)"
                }))
                acc.take()   # clear overflow
            return

        # ── Text frame = JSON control ────────────────────────────────────────
        try:
            msg = json.loads(data) if isinstance(data, str) else data
        except (json.JSONDecodeError, TypeError):
            emit("message", json.dumps({"type": "error", "message": "Invalid JSON"}))
            return

        msg_type = msg.get("type")

        if msg_type == "ping":
            emit("message", json.dumps({"type": "pong"}))

        elif msg_type == "audio_chunk":
            # Base64-encoded PCM chunk from ESP32 firmware
            # (raw binary sendBIN is not routable by Flask-SocketIO as message event)
            import base64 as _b64
            b64_data = msg.get("data", "")
            if not b64_data:
                emit("message", json.dumps({"type": "error", "message": "Empty audio_chunk"}))
                return
            try:
                chunk_bytes = _b64.b64decode(b64_data)
            except Exception:
                emit("message", json.dumps({"type": "error", "message": "Invalid base64 in audio_chunk"}))
                return
            acc = session["audio_accumulator"]
            if not acc.add_chunk(chunk_bytes):
                emit("message", json.dumps({
                    "type": "error",
                    "message": "Audio terlalu panjang (max 10 detik)"
                }))
                acc.take()

        elif msg_type == "audio_end":
            acc = session["audio_accumulator"]
            if len(acc) == 0:
                emit("message", json.dumps({
                    "type": "error",
                    "message": "Tidak ada audio yang diterima"
                }))
                return
            # Acknowledge immediately, then process in a native thread
            emit("message", json.dumps({"type": "processing"}))
            audio_bytes = acc.take()
            t = threading.Thread(
                target=_process_audio,
                args=(sid, session_id, audio_bytes),
                daemon=True,
            )
            t.start()

        else:
            emit("message", json.dumps({
                "type": "error",
                "message": f"Unknown message type: {msg_type}"
            }))


# ─────────────────────────────────────────────────────────────────────────────
# Heavy processing — native Python thread (gRPC safe)
# ─────────────────────────────────────────────────────────────────────────────

def _process_audio(sid: str, session_id: str, audio_bytes: bytes):
    """
    Runs in a dedicated thread. Uses _emit() to push results back to the client.
    Native threads are fully gRPC-compatible — no blocking of the SocketIO loop.
    """
    diagnostics = {"transport": "ws_thread", "session_id": session_id}
    try:
        import time
        start_time = time.time()
        
        # Step 1 — Convert (WebM from browser or raw PCM from ESP32)
        wav_bytes = convert_to_wav_16khz_mono(audio_bytes, diagnostics)
        convert_time = time.time()
        print(f"[{session_id}] Audio convert took: {convert_time - start_time:.2f}s")

        state.LATEST_INPUT_WAV = wav_bytes
        state.LATEST_INPUT_META = {
            "session_id": session_id,
            "bytes": len(wav_bytes),
            "input_format": diagnostics.get("input_format"),
            "timestamp": datetime.now(timezone.utc).isoformat(),
        }

        # Step 2 — STT
        question = stt_transcribe(wav_bytes, diagnostics)
        stt_time = time.time()
        print(f"[{session_id}] STT took: {stt_time - convert_time:.2f}s | Result: {question}")
        
        if not question:
            _emit(sid, {
                "type": "error",
                "message": "Suara tidak terdeteksi. Coba bicara lebih dekat ke mikrofon."
            })
            return

        # Immediately show what was heard
        _emit(sid, {"type": "stt_result", "text": question})

        # Step 3 — Ask Gemini
        lang   = "en"  # default, re-detected on answer below

        full_answer  = ""
        for chunk_text in ask_gemini_stream(session_id, question, diagnostics):
            full_answer  += chunk_text
            
        gemini_time = time.time()
        print(f"[{session_id}] Gemini took: {gemini_time - stt_time:.2f}s | Result: {full_answer}")

        if not full_answer.strip():
            full_answer = "Maaf."

        # Detect language on the ANSWER (what gets TTS'd, not the question)
        lang = detect_language(full_answer)
        diagnostics["detected_lang"] = lang
        print(f"[{session_id}] Detected lang: {lang}")

        # Step 4 — Sentence-level TTS streaming
        # Split into sentences, TTS each one, emit audio_ready immediately.
        # ESP32 starts playing sentence 1 while sentences 2-N are still being TTS'd.
        raw_sentences = re.split(r'(?<=[.!?])\s+', full_answer)
        raw_sentences = [s.strip() for s in raw_sentences if s.strip()]
        
        # Filter out emoji-only fragments and merge short bits with previous sentence
        sentences = []
        for s in raw_sentences:
            # Skip if only emoji/whitespace/punctuation (no actual words)
            if not re.search(r'[a-zA-Z0-9]', s):
                if sentences:
                    sentences[-1] += " " + s  # append emoji to previous
                continue
            # Merge very short fragments with previous to avoid tiny TTS calls
            if len(s) < 10 and sentences:
                sentences[-1] += " " + s
            else:
                sentences.append(s)
        
        if not sentences:
            sentences = [full_answer]

        tts_start = time.time()
        for i, sentence in enumerate(sentences):
            print(f"[{session_id}] TTS sentence {i+1}/{len(sentences)}: \"{sentence[:60]}\"")
            sent_audio = synthesize_speech_bytes(sentence, language=lang)
            tts_elapsed = time.time() - tts_start
            print(f"[{session_id}] TTS sentence {i+1} done in {tts_elapsed:.2f}s ({len(sent_audio)} bytes)")

            audio_id, audio_url = cache_audio_bytes(sent_audio)
            _emit(sid, {
                "type":      "audio_ready",
                "text":      sentence,
                "audio_url": audio_url,
                "audio_id":  audio_id,
            })

        append_exchange(session_id, question, full_answer)

        # Step 5 — Send done
        total_time = time.time() - start_time
        print(f"[{session_id}] Total websocket pipeline took: {total_time:.2f}s")
        _emit(sid, {
            "type": "done",
            "full_answer": full_answer,
            "session_id": session_id,
            "stage": "ok",
        })


    except Exception as exc:
        _emit(sid, {
            "type": "error",
            "message": f"Processing error: {exc}",
            "stage": "internal_error",
        })
