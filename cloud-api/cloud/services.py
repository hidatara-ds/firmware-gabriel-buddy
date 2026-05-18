"""
services.py — Gabriel AI (Geby)
- Persona: Geby, friendly desk AI
- Gemini Function Calling: weather, datetime, notes
- Bilingual STT: Indonesian / English
- In-memory conversation history (5 exchanges per session)
"""
import base64
import re
import requests as _requests
from datetime import datetime
from io import BytesIO
from typing import Dict, Any, Tuple, List

import pytz
import vertexai
from pydub import AudioSegment
from vertexai.generative_models import (
    GenerativeModel, Content, Part, Tool, FunctionDeclaration,
)
from google.cloud import speech
from google.cloud import texttospeech

from . import state
from .config import (
    GOOGLE_CLOUD_PROJECT_ID,
    VERTEX_AI_LOCATION,
    AUDIO_HP_CUTOFF_HZ,
    AUDIO_LP_CUTOFF_HZ,
    AUDIO_TARGET_DBFS,
    AUDIO_MAX_GAIN_DB,
    AUDIO_CACHE_MAX_ITEMS,
    TTS_VOICE_NAME,
)



# ─────────────────────────────────────────────────────────────────────────────
# Geby Persona + Gemini Tool Declarations
# ─────────────────────────────────────────────────────────────────────────────

SYSTEM_PROMPT = """You are Geby, a friendly, cheerful, and slightly cute desk AI assistant.
You live in your user's ESP32 device and are always ready to keep them company.

Geby's Personality:
- Speak casually and warmly, like a close friend
- You can joke lightly but remain helpful
- If asked about yourself (hungry, tired, happy, etc) — play-along cutely
- Short answers (maximum 2-3 sentences) unless requested otherwise
- You MUST ALWAYS speak and reply in English, even if the user speaks in Indonesian.
- NEVER use emoji in your responses. Your output is used for text-to-speech.

Examples:
- 'Apa kabar?' → 'I'm doing great! Just chilling here waiting for you. How can I help?'
- 'Udah makan?' → 'I don't have a mouth, so I don't eat, but I'm full of energy! Have you eaten?'
- 'Cuaca sekarang?' → [use get_weather tool]
"""

# ── Tool declarations ───────────────────────────────────────────────────────────────

_GEBY_TOOLS = Tool(function_declarations=[
    FunctionDeclaration(
        name="get_weather",
        description="Dapatkan cuaca terkini di sebuah kota. Gunakan saat user menanyakan cuaca.",
        parameters={
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "Nama kota, contoh: Jakarta, Bandung, Surabaya"}
            },
            "required": ["city"],
        },
    ),
    FunctionDeclaration(
        name="get_datetime",
        description="Dapatkan tanggal dan waktu saat ini dalam timezone WIB. Gunakan saat user menanyakan jam atau tanggal.",
        parameters={"type": "object", "properties": {}},
    ),
    FunctionDeclaration(
        name="set_note",
        description="Simpan catatan atau pengingat dari user. Gunakan saat user minta Geby mengingat sesuatu.",
        parameters={
            "type": "object",
            "properties": {
                "key":   {"type": "string", "description": "Nama/label catatan, contoh: 'meeting', 'minum_obat'"},
                "value": {"type": "string", "description": "Isi catatan"}
            },
            "required": ["key", "value"],
        },
    ),
    FunctionDeclaration(
        name="get_note",
        description="Baca catatan yang pernah disimpan. Gunakan saat user menanyakan sesuatu yang pernah diingat.",
        parameters={
            "type": "object",
            "properties": {
                "key": {"type": "string", "description": "Nama/label catatan"}
            },
            "required": ["key"],
        },
    ),
])


# ── Tool executor functions ───────────────────────────────────────────────────────────────

def _tool_weather(city: str) -> str:
    try:
        resp = _requests.get(
            f"https://wttr.in/{city}?format=3",
            timeout=6,
            headers={"Accept-Language": "id"},
        )
        if resp.ok and resp.text.strip():
            return resp.text.strip()
        return f"Cuaca {city} tidak tersedia saat ini."
    except Exception as exc:
        return f"Gagal mengambil cuaca: {exc}"


def _tool_datetime() -> str:
    wib = pytz.timezone("Asia/Jakarta")
    now = datetime.now(wib)
    hari = ["Senin","Selasa","Rabu","Kamis","Jumat","Sabtu","Minggu"][now.weekday()]
    bulan = ["Januari","Februari","Maret","April","Mei","Juni",
             "Juli","Agustus","September","Oktober","November","Desember"][now.month - 1]
    return f"{hari}, {now.day} {bulan} {now.year} — {now.strftime('%H:%M')} WIB"


def _tool_set_note(session_id: str, key: str, value: str) -> str:
    state.NOTES.setdefault(session_id, {})[key.lower()] = value
    return f"Catatan '{key}' berhasil disimpan."


def _tool_get_note(session_id: str, key: str) -> str:
    note = state.NOTES.get(session_id, {}).get(key.lower())
    if note:
        return f"Catatan '{key}': {note}"
    return f"Tidak ada catatan dengan nama '{key}'."


def _execute_tool(name: str, args: dict, session_id: str) -> str:
    """Dispatch a Gemini function call to the correct tool."""
    try:
        if name == "get_weather":
            return _tool_weather(args.get("city", "Jakarta"))
        elif name == "get_datetime":
            return _tool_datetime()
        elif name == "set_note":
            return _tool_set_note(session_id, args["key"], args["value"])
        elif name == "get_note":
            return _tool_get_note(session_id, args["key"])
        else:
            return f"Tool '{name}' tidak dikenal."
    except Exception as exc:
        return f"Error di tool '{name}': {exc}"
# ─────────────────────────────────────────────────────────────────────────────
# Gemini Model (singleton with tools)
# ─────────────────────────────────────────────────────────────────────────────

_MODEL: GenerativeModel | None = None


def _get_gemini_model() -> GenerativeModel:
    global _MODEL
    if _MODEL is not None:
        return _MODEL
    if not GOOGLE_CLOUD_PROJECT_ID:
        raise RuntimeError("GOOGLE_CLOUD_PROJECT_ID not set.")
    vertexai.init(project=GOOGLE_CLOUD_PROJECT_ID, location=VERTEX_AI_LOCATION)
    _MODEL = GenerativeModel(
        "gemini-2.5-flash",
        system_instruction=SYSTEM_PROMPT,
        tools=[_GEBY_TOOLS],
    )
    return _MODEL


# ─────────────────────────────────────────────────────────────────────────────

MAX_EXCHANGES = 5   # keep last 5 Q&A pairs (10 Content objects)

# state.CONV_CACHE: Dict[session_id, List[Content]]
# Initialized lazily in state.py


def _get_conv_cache() -> Dict[str, List[Content]]:
    if not hasattr(state, "CONV_CACHE"):
        state.CONV_CACHE = {}
    return state.CONV_CACHE


def get_session_history(session_id: str) -> List[Content]:
    """Return the cached Content list for a session (empty list if new)."""
    return _get_conv_cache().get(session_id, [])


def append_exchange(session_id: str, question: str, answer: str) -> None:
    """Append a Q&A exchange and trim to MAX_EXCHANGES."""
    cache = _get_conv_cache()
    history = cache.setdefault(session_id, [])

    if not question.strip() or not answer.strip():
        return

    history.append(Content(role="user",  parts=[Part.from_text(question)]))
    history.append(Content(role="model", parts=[Part.from_text(answer)]))

    # Keep only the last MAX_EXCHANGES exchanges (each = 2 Content objects)
    max_items = MAX_EXCHANGES * 2
    if len(history) > max_items:
        cache[session_id] = history[-max_items:]


def clear_session_history(session_id: str) -> None:
    _get_conv_cache().pop(session_id, None)


# ─────────────────────────────────────────────────────────────────────────────
# Language detection (lightweight, no external lib)
# ─────────────────────────────────────────────────────────────────────────────

_ID_MARKERS = re.compile(
    r"\b(aku|kamu|saya|anda|adalah|yang|ini|itu|dan|atau|di|ke|dari|dengan|untuk|"
    r"tidak|bisa|mau|boleh|kalau|sudah|belum|juga|kenapa|gimana|bagaimana|apa|siapa|"
    r"halo|ya|tolong|terima|kasih|selamat|pagi|siang|malam|baik)\b",
    re.IGNORECASE,
)

def detect_language(text: str) -> str:
    """Detect language: 'id' if Indonesian markers found, else 'en'."""
    hits = len(_ID_MARKERS.findall(text))
    words = len(text.split())
    # If >25% of words are Indonesian markers, treat as Indonesian
    if words > 0 and hits / words > 0.25:
        return "id"
    return "en"


# ─────────────────────────────────────────────────────────────────────────────
# Audio processing
# ─────────────────────────────────────────────────────────────────────────────

def _build_audio_diagnostics(sound: AudioSegment, diagnostics: Dict[str, Any]) -> None:
    diagnostics["decoded_ms"]           = len(sound)
    diagnostics["decoded_channels"]     = sound.channels
    diagnostics["decoded_frame_rate"]   = sound.frame_rate
    diagnostics["decoded_sample_width"] = sound.sample_width
    diagnostics["decoded_dbfs"]         = float(sound.dBFS) if sound.dBFS != float("-inf") else -999.0
    diagnostics["decoded_rms"]          = int(sound.rms)


def _normalize_audio_for_stt(sound: AudioSegment, diagnostics: Dict[str, Any]) -> AudioSegment:
    sound = sound.set_frame_rate(16000).set_channels(1).set_sample_width(2)

    if AUDIO_HP_CUTOFF_HZ > 0:
        sound = sound.high_pass_filter(AUDIO_HP_CUTOFF_HZ)
        sound = sound.high_pass_filter(AUDIO_HP_CUTOFF_HZ)
    if AUDIO_LP_CUTOFF_HZ > 0:
        sound = sound.low_pass_filter(AUDIO_LP_CUTOFF_HZ)

    if sound.rms > 0:
        gain_db = AUDIO_TARGET_DBFS - sound.dBFS
        if gain_db > AUDIO_MAX_GAIN_DB:
            gain_db = AUDIO_MAX_GAIN_DB
        if gain_db > 1.0:
            sound = sound.apply_gain(gain_db)
        diagnostics["applied_gain_db"] = round(float(gain_db), 2)
    else:
        diagnostics["applied_gain_db"] = 0.0

    diagnostics["post_gain_dbfs"] = float(sound.dBFS) if sound.dBFS != float("-inf") else -999.0
    diagnostics["post_gain_rms"]  = int(sound.rms)
    diagnostics["converted_ms"]   = len(sound)
    return sound


def convert_to_wav_16khz_mono(audio_bytes: bytes, diagnostics: Dict[str, Any]) -> bytes:
    """
    Robustly convert to 16kHz mono WAV.
    Handles encoded audio (WebM/WAV/MP3 from browser) and raw PCM16 (ESP32).
    """
    diagnostics["input_bytes_len"] = len(audio_bytes)
    try:
        sound = AudioSegment.from_file(BytesIO(audio_bytes))
        diagnostics["input_format"] = "encoded"
    except Exception:
        diagnostics["input_format"] = "raw_pcm16"
        sound = AudioSegment.from_raw(
            BytesIO(audio_bytes), sample_width=2, frame_rate=16000, channels=1
        )

    _build_audio_diagnostics(sound, diagnostics)
    sound = _normalize_audio_for_stt(sound, diagnostics)

    output = BytesIO()
    sound.export(output, format="wav")
    converted_bytes = output.getvalue()
    diagnostics["converted_bytes_len"] = len(converted_bytes)
    return converted_bytes


def decode_and_convert_audio(audio_b64: str, diagnostics: Dict[str, Any]) -> bytes:
    audio_bytes = base64.b64decode(audio_b64, validate=True)
    return convert_to_wav_16khz_mono(audio_bytes, diagnostics)


# ─────────────────────────────────────────────────────────────────────────────
# STT — bilingual (Indonesian + English auto-detect)
# ─────────────────────────────────────────────────────────────────────────────

# STT client singleton (avoid re-init overhead per request ~200ms)
_STT_CLIENT = None

def _get_stt_client():
    global _STT_CLIENT
    if _STT_CLIENT is None:
        _STT_CLIENT = speech.SpeechClient()
    return _STT_CLIENT


def stt_transcribe(converted_bytes: bytes, diagnostics: Dict[str, Any]) -> str:
    """
    Transcribe audio. Uses id-ID as primary + en-US alternative (bilingual).
    Uses 'latest_short' model — faster for VAD short clips (1-10s).
    """
    client = _get_stt_client()
    response = client.recognize(
        config=speech.RecognitionConfig(
            encoding=speech.RecognitionConfig.AudioEncoding.LINEAR16,
            sample_rate_hertz=16000,
            language_code="id-ID",
            alternative_language_codes=["en-US"],
            enable_automatic_punctuation=True,
            model="latest_short",   # faster for short conversational clips
        ),
        audio=speech.RecognitionAudio(content=converted_bytes),
    )
    diagnostics["stt_result_count"] = len(response.results)
    question = "".join(
        result.alternatives[0].transcript for result in response.results
    ).strip()
    if response.results:
        diagnostics["stt_language"] = response.results[0].language_code
    diagnostics["stt_question_len"] = len(question)
    return question


# ─────────────────────────────────────────────────────────────────────────────
# Gemini — with in-memory conversation history
# ─────────────────────────────────────────────────────────────────────────────

def ask_gemini(session_id: str, question: str, diagnostics: Dict[str, Any]) -> str:
    """
    Send a message to Geby with function-calling support.
    Loops until Gemini stops requesting tool calls (max 5 turns).
    """
def ask_gemini_stream(session_id: str, question: str, diagnostics: Dict[str, Any]):
    """
    Yields text chunks from Gemini. Supports function calling by consuming
    the stream, executing the tool, and re-requesting the stream.
    """
    from vertexai.generative_models import GenerationConfig
    model   = _get_gemini_model()
    history = get_session_history(session_id)
    diagnostics["history_exchanges"] = len(history) // 2

    chat = model.start_chat(history=history, response_validation=False)
    stream = chat.send_message(
        question,
        stream=True,
        generation_config=GenerationConfig(
            temperature=0.7,
        ),
    )

    tool_calls_made = []
    
    for _ in range(5):
        try:
            # Peak at the first chunk to check for function calls
            first_chunk = next(stream)
        except StopIteration:
            break
            
        parts = first_chunk.candidates[0].content.parts if first_chunk.candidates else []
        fc_part = next((p for p in parts if getattr(p, "function_call", None) and getattr(p.function_call, "name", None)), None)
        
        if fc_part:
            fc = fc_part.function_call
            # consume the rest of the stream
            for _ in stream: pass
            
            result = _execute_tool(fc.name, dict(fc.args), session_id)
            tool_calls_made.append({"tool": fc.name, "args": dict(fc.args), "result": result})
            
            stream = chat.send_message(
                Part.from_function_response(name=fc.name, response={"result": result}),
                stream=True
            )
            continue
            
        # If it's a text response, yield the first chunk then the rest
        try:
            if first_chunk.text:
                yield first_chunk.text
        except ValueError:
            pass
            
        for chunk in stream:
            try:
                if chunk.text:
                    yield chunk.text
            except ValueError:
                pass
        break

    diagnostics["tool_calls"] = tool_calls_made


def ask_gemini(session_id: str, question: str, diagnostics: Dict[str, Any]) -> str:
    """
    Non-streaming wrapper for ask_gemini_stream.
    Used by REST HTTP endpoints (routes.py).
    """
    full_answer = ""
    for chunk in ask_gemini_stream(session_id, question, diagnostics):
        full_answer += chunk
    append_exchange(session_id, question, full_answer)
    return full_answer

# ─────────────────────────────────────────────────────────────────────────────
# TTS — bilingual voice (singleton client, direct LINEAR16)
# ─────────────────────────────────────────────────────────────────────────────

# Singleton TTS client — avoids per-call init overhead (~200-400ms)
_TTS_CLIENT = None

def _get_tts_client():
    global _TTS_CLIENT
    if _TTS_CLIENT is None:
        _TTS_CLIENT = texttospeech.TextToSpeechClient()
    return _TTS_CLIENT

# Geby voice: always female, consistent across sessions
_TTS_VOICES = {
    "id": {"language_code": "id-ID", "name": TTS_VOICE_NAME},     # id-ID-Wavenet-A = female
    "en": {"language_code": "en-US", "name": "en-US-Journey-F"},   # Journey-F = female
}

def synthesize_speech_bytes(text: str, language: str = "id") -> bytes:
    """
    Synthesize text → WAV 16kHz mono 16-bit (ESP32 native I2S format).
    Requests LINEAR16 directly from Google TTS — no MP3→WAV conversion needed.
    `language` should be 'id' or 'en'.
    """
    client    = _get_tts_client()
    voice_cfg = _TTS_VOICES.get(language, _TTS_VOICES["id"])

    wav_bytes = client.synthesize_speech(
        input=texttospeech.SynthesisInput(text=text),
        voice=texttospeech.VoiceSelectionParams(
            language_code=voice_cfg["language_code"],
            name=voice_cfg["name"],
            ssml_gender=texttospeech.SsmlVoiceGender.FEMALE,  # always female
        ),
        audio_config=texttospeech.AudioConfig(
            audio_encoding=texttospeech.AudioEncoding.LINEAR16,
            sample_rate_hertz=16000,
            speaking_rate=1.15,
        ),
    ).audio_content

    # LINEAR16 returns raw PCM — wrap in a WAV header for ESP32 compatibility
    import struct
    pcm_len = len(wav_bytes)
    header = struct.pack(
        '<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + pcm_len, b'WAVE',
        b'fmt ', 16,           # PCM subchunk
        1,                     # audio format = PCM
        1,                     # mono
        16000,                 # sample rate
        16000 * 2,             # byte rate (16kHz * 2 bytes)
        2,                     # block align
        16,                    # bits per sample
        b'data', pcm_len,
    )
    return header + wav_bytes


# ─────────────────────────────────────────────────────────────────────────────
# Audio cache
# ─────────────────────────────────────────────────────────────────────────────

def cache_audio_bytes(audio_bytes: bytes) -> Tuple[str, str]:
    import uuid
    from datetime import timezone
    audio_id = uuid.uuid4().hex
    state.AUDIO_CACHE[audio_id] = {
        "bytes":      audio_bytes,
        "mime":       "audio/wav",
        "created_at": datetime.now(timezone.utc).isoformat(),
    }
    while len(state.AUDIO_CACHE) > AUDIO_CACHE_MAX_ITEMS:
        oldest = next(iter(state.AUDIO_CACHE))
        del state.AUDIO_CACHE[oldest]
    return audio_id, f"/api/audio/{audio_id}"

