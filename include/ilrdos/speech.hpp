// ILrDOS — entrada e saída de voz: microfone (ALSA), reconhecimento (Vosk), TTS remoto/espeak e LLM (Ollama).
// ALSA e Vosk são opcionais em tempo de compilação (ILRDOS_HAVE_ALSA / ILRDOS_HAVE_VOSK); sem eles o
// robô ainda conversa por texto (comando `hear`) e a fala vira log, o que permite testar tudo no PC.
#pragma once

#ifdef ILRDOS_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif
#ifdef ILRDOS_HAVE_VOSK
#include <vosk_api.h>
#endif

#include "http.hpp"

namespace ilrdos {

// "auto" escolhe a primeira placa USB de áudio (no Pi Zero 2 W o HDMI é a placa 0, sem microfone).
inline std::string resolveAudioDevice(const std::string& configured) {
    if (configured != "auto") return configured;
    std::ifstream cards("/proc/asound/cards");
    std::string line;
    while (std::getline(cards, line)) {
        if (line.find("USB-Audio") == std::string::npos) continue;
        try { return "plughw:" + std::to_string(std::stoi(line)) + ",0"; } catch (...) {}
    }
    return "default";
}

#ifdef ILRDOS_HAVE_ALSA
class AlsaCapture {
public:
    ~AlsaCapture() { close(); }
    bool open(const std::string& device) {
        if (snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) { pcm_ = nullptr; return false; }
        if (snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1, 200000) < 0) { close(); return false; }
        return true;
    }
    void close() { if (pcm_) { snd_pcm_close(pcm_); pcm_ = nullptr; } }
    bool isOpen() const { return pcm_ != nullptr; }
    int read(std::int16_t* buffer, int frames) {
        const snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buffer, static_cast<snd_pcm_uframes_t>(frames));
        if (n < 0) { snd_pcm_recover(pcm_, static_cast<int>(n), 1); return 0; }
        return static_cast<int>(n);
    }
    void flush() { if (pcm_) { snd_pcm_drop(pcm_); snd_pcm_prepare(pcm_); } }
private:
    snd_pcm_t* pcm_{nullptr};
};

inline bool playPcm16(const std::string& device, const std::vector<std::int16_t>& samples, int channels, int rate) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0) return false;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, static_cast<unsigned>(channels),
                           static_cast<unsigned>(rate), 1, 300000) < 0) { snd_pcm_close(pcm); return false; }
    const std::size_t frames = samples.size() / static_cast<std::size_t>(channels);
    std::size_t done = 0;
    while (done < frames) {
        const snd_pcm_sframes_t n = snd_pcm_writei(pcm, samples.data() + done * static_cast<std::size_t>(channels),
                                                   std::min<std::size_t>(frames - done, 4096));
        if (n < 0) { if (snd_pcm_recover(pcm, static_cast<int>(n), 1) < 0) break; continue; }
        done += static_cast<std::size_t>(n);
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return done >= frames;
}
#endif

// WAV PCM 16 bits -> amostras. Devolve false para qualquer outro formato.
inline bool parseWav(const std::string& wav, std::vector<std::int16_t>& samples, int& channels, int& rate) {
    if (wav.size() < 44 || wav.compare(0, 4, "RIFF") != 0 || wav.compare(8, 4, "WAVE") != 0) return false;
    auto u16 = [&](std::size_t p) { return static_cast<unsigned>(static_cast<unsigned char>(wav[p]) | (static_cast<unsigned char>(wav[p + 1]) << 8)); };
    auto u32 = [&](std::size_t p) { return u16(p) | (u16(p + 2) << 16); };
    std::size_t pos = 12;
    bool haveFmt = false;
    while (pos + 8 <= wav.size()) {
        const std::string id = wav.substr(pos, 4);
        const std::size_t size = u32(pos + 4);
        if (id == "fmt " && pos + 8 + 16 <= wav.size()) {
            if (u16(pos + 8) != 1 || u16(pos + 22) != 16) return false;
            channels = static_cast<int>(u16(pos + 10));
            rate = static_cast<int>(u32(pos + 12));
            haveFmt = true;
        } else if (id == "data" && haveFmt) {
            const std::size_t available = std::min(size, wav.size() - pos - 8);
            samples.resize(available / 2);
            std::memcpy(samples.data(), wav.data() + pos + 8, samples.size() * 2);
            return channels >= 1 && channels <= 2 && rate >= 8000 && rate <= 48000;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

class Speaker {
public:
    Speaker(std::string outDevice, std::string ttsUrl, std::string ttsVoice)
        : device_(resolveAudioDevice(outDevice)), ttsUrl_(std::move(ttsUrl)), voice_(std::move(ttsVoice)) {}

    bool audioAvailable() const {
#ifdef ILRDOS_HAVE_ALSA
        return true;
#else
        return false;
#endif
    }

    // Fala o texto (bloqueia até terminar). Primeiro o TTS do PC; se falhar, espeak local.
    void say(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        logLine("fala", text);
        if (std::getenv("ILRDOS_SIM_SILENT") != nullptr) return;
        std::string wav;
        if (!ttsUrl_.empty()) wav = remoteTts(text.substr(0, 400));
        std::vector<std::int16_t> samples;
        int channels = 1, rate = 22050;
        if (wav.empty() || !parseWav(wav, samples, channels, rate)) {
            samples.clear();
            if (!localTts(text.substr(0, 400), samples, channels, rate)) return;
        }
        play(samples, channels, rate);
    }

private:
    std::string remoteTts(const std::string& text) {
        std::string host, path; int port = 0;
        if (!parseHttpUrl(ttsUrl_, host, port, path)) return {};
        const std::string body = "{\"input\":\"" + jsonEscape(text) + "\",\"voice\":\"" + jsonEscape(voice_) +
                                 "\",\"response_format\":\"wav\"}";
        const auto response = httpRequest("POST", host, port, path, {"Content-Type: application/json"}, body, 20000, 8u << 20);
        return response.ok() ? response.body : std::string{};
    }

    bool localTts(const std::string& text, std::vector<std::int16_t>& samples, int& channels, int& rate) {
        for (const char* program : {"/usr/bin/espeak", "/usr/bin/espeak-ng", "/usr/local/bin/espeak"}) {
            if (access(program, X_OK) != 0) continue;
            const std::string file = "/tmp/ilrdos-tts-" + std::to_string(::getpid()) + ".wav";
            if (!runProgram({program, "-v", "pt", "-s", "145", "--stdin", "-w", file}, text, 20)) continue;
            std::ifstream in(file, std::ios::binary);
            std::string wav((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::remove(file.c_str());
            if (parseWav(wav, samples, channels, rate)) return true;
        }
        return false;
    }

    void play(const std::vector<std::int16_t>& samples, int channels, int rate) {
#ifdef ILRDOS_HAVE_ALSA
        if (!playPcm16(device_, samples, channels, rate)) logLine("fala", "falha ao tocar no dispositivo " + device_);
#else
        (void)samples; (void)channels; (void)rate;
#endif
    }

    std::mutex mutex_;
    std::string device_, ttsUrl_, voice_;
};

// Reconhecimento contínuo (Vosk). Sem Vosk/ALSA, available() é falso e só `hear` injeta falas.
class Listener {
public:
    Listener(const std::string& modelPath, const std::string& inDevice)
        : modelPath_(modelPath), device_(resolveAudioDevice(inDevice)) {}

    ~Listener() {
#ifdef ILRDOS_HAVE_VOSK
        if (recognizer_) vosk_recognizer_free(recognizer_);
        if (model_) vosk_model_free(model_);
#endif
    }

    // Prepara modelo e microfone. Retorna texto do problema, ou vazio se pronto.
    std::string open() {
#if defined(ILRDOS_HAVE_VOSK) && defined(ILRDOS_HAVE_ALSA)
        if (access((modelPath_ + "/am/final.mdl").c_str(), R_OK) != 0) return "modelo Vosk não encontrado em " + modelPath_;
        vosk_set_log_level(-1);
        model_ = vosk_model_new(modelPath_.c_str());
        if (!model_) return "falha ao carregar o modelo Vosk";
        recognizer_ = vosk_recognizer_new(model_, 16000.0F);
        if (!recognizer_) return "falha ao criar o reconhecedor Vosk";
        if (!capture_.open(device_)) return "não consegui abrir o microfone (" + device_ + ")";
        return {};
#else
        return "compilado sem Vosk/ALSA (a conversa só funciona por texto)";
#endif
    }

    // Lê ~250 ms de áudio; devolve uma frase quando o Vosk detecta fim de fala.
    std::optional<std::string> poll() {
#if defined(ILRDOS_HAVE_VOSK) && defined(ILRDOS_HAVE_ALSA)
        std::int16_t buffer[4000];
        const int frames = capture_.read(buffer, 4000);
        if (frames <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); return std::nullopt; }
        if (vosk_recognizer_accept_waveform_s(recognizer_, buffer, frames) == 1) {
            const std::string text = trim(jsonString(vosk_recognizer_result(recognizer_), "text"));
            if (!text.empty()) return text;
        }
#endif
        return std::nullopt;
    }

    // Descarta o que foi captado (usado depois que o robô fala, para não ouvir a si mesmo).
    void resetAudio() {
#if defined(ILRDOS_HAVE_VOSK) && defined(ILRDOS_HAVE_ALSA)
        capture_.flush();
        if (recognizer_) vosk_recognizer_reset(recognizer_);
#endif
    }

private:
    std::string modelPath_, device_;
#if defined(ILRDOS_HAVE_VOSK) && defined(ILRDOS_HAVE_ALSA)
    AlsaCapture capture_;
    VoskModel* model_{nullptr};
    VoskRecognizer* recognizer_{nullptr};
#endif
};

// Cliente do Ollama (no PC da rede local).
class LlmClient {
public:
    LlmClient(std::string host, std::string model) : host_(std::move(host)), model_(std::move(model)) {}
    bool enabled() const { return !host_.empty(); }

    // messages: pares (papel, texto). Devolve resposta ou string vazia + erro.
    std::string chat(const std::vector<std::pair<std::string, std::string>>& messages, std::string& error) const {
        std::string body = "{\"model\":\"" + jsonEscape(model_) + "\",\"stream\":false,\"messages\":[";
        for (std::size_t i = 0; i < messages.size(); ++i) {
            body += std::string(i ? "," : "") + "{\"role\":\"" + messages[i].first + "\",\"content\":\"" +
                    jsonEscape(messages[i].second) + "\"}";
        }
        body += "],\"options\":{\"temperature\":0.35,\"num_predict\":180}}";
        const auto response = httpRequest("POST", host_, 11434, "/api/chat", {"Content-Type: application/json"}, body, 45000, 1u << 20);
        if (!response.ok()) {
            error = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error;
            return {};
        }
        const std::string content = trim(jsonString(response.body.substr(response.body.find("\"message\"") == std::string::npos ? 0 : response.body.find("\"message\"")), "content"));
        if (content.empty()) error = "resposta vazia do modelo";
        return content;
    }

private:
    std::string host_, model_;
};

}  // namespace ilrdos
