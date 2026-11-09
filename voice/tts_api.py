"""Small OpenAI-compatible TTS endpoint for the GLaDOS and Kokoro ONNX models."""

from dataclasses import dataclass
from functools import lru_cache
import io
from typing import Literal

import soundfile as sf
from glados.TTS import SpeechSynthesizerProtocol, get_speech_synthesizer
from litestar import Litestar, post
from litestar.exceptions import HTTPException
from litestar.response import Stream

SUPPORTED_VOICES = {"glados", "af_alloy"}


@dataclass
class SpeechRequest:
    input: str
    voice: str = "af_alloy"
    response_format: Literal["wav"] = "wav"


@lru_cache(maxsize=1)
def _get_synthesizer(voice: str) -> SpeechSynthesizerProtocol:
    if voice not in SUPPORTED_VOICES:
        raise ValueError(f"Unsupported voice: {voice}")
    return get_speech_synthesizer(voice)


@post("/v1/audio/speech")
async def create_speech(data: SpeechRequest) -> Stream:
    text = data.input.strip()
    if not text or len(text) > 1000:
        raise HTTPException(status_code=400, detail="Text must contain 1 to 1000 characters.")
    if data.voice not in SUPPORTED_VOICES:
        raise HTTPException(status_code=400, detail="Choose voice 'glados' or 'af_alloy'.")

    synthesizer = _get_synthesizer(data.voice)
    audio = synthesizer.generate_speech_audio(text)
    buffer = io.BytesIO()
    sf.write(buffer, audio, synthesizer.sample_rate, format="WAV")
    buffer.seek(0)
    return Stream(
        buffer,
        headers={
            "content-type": "audio/wav",
            "content-disposition": 'attachment; filename="speech.wav"',
        },
    )


app = Litestar([create_speech])
