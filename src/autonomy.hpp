// ILrDOS — "cérebro" autônomo: decide sozinho o que fazer, sem ninguém pedir.
//
//  Pessoas:   varre a câmera (devagar sem ninguém, rápido com alguém); identifica quem é conhecido;
//             cumprimenta pelo nome (ou educadamente, se desconhecido); dispara regras de IoT.
//  Atenção:   sozinho por muito tempo, faz um pulso "think" para chamar atenção.
//  Movimento: nível "track" gira para encarar quem está na frente; "roam" também se aproxima (parando longe)
//             e passeia devagar quando está sozinho, sempre com o sonar de olho.
//  Autocuidado: calor (reduz e pausa a visão), Wi-Fi (reinicia) e câmera (tenta de novo).
// Um comando manual tem prioridade; `autonomy off` desliga tudo.
#pragma once

#include <atomic>
#include <functional>
#include <random>
#include <set>

#include "robot.hpp"
#include "vision.hpp"

namespace ilrdos {

enum class MotionMode { Off, Track, Roam };

inline const char* motionName(MotionMode mode) {
    return mode == MotionMode::Roam ? "roam" : mode == MotionMode::Track ? "track" : "off";
}

struct AutonomyHooks {
    std::function<void(const std::string& text)> say;
    std::function<void(const std::string& event, const std::string& subject, const std::string& who)> fireRule;
    std::function<void(bool left, int ms, int speed)> pulseTurn;
    std::function<std::string(const std::string& dir, int speed, int ms)> move;
    std::function<std::string(bool left, int degrees)> turn;
    std::function<bool()> canMove;
    std::function<bool()> hasSonar;
    std::function<std::optional<double>()> distanceCm;
    std::function<bool()> moving;
    std::function<void()> stop;
};

class Autonomy {
public:
    Autonomy(RobotController& robot, VisionSystem& vision, AutonomyHooks hooks, bool enabled, int scanSeconds,
             MotionMode motion, bool trackInvert)
        : robot_(robot), vision_(vision), hooks_(std::move(hooks)), enabled_(enabled), scanInterval_(scanSeconds),
          motion_(motion), trackInvert_(trackInvert), started_(Clock::now()), rng_(std::random_device{}()),
          thermalPath_(envOr("ILRDOS_THERMAL_FILE", "/sys/class/thermal/thermal_zone0/temp")),
          wifiStatePath_(envOr("ILRDOS_WIFI_STATE_FILE", "/sys/class/net/wlan0/operstate")),
          wifiRecovery_(envOr("ILRDOS_WIFI_RECOVERY", "/etc/init.d/S35ilrdos-wifi")),
          wifiGrace_(envSeconds("ILRDOS_WIFI_GRACE_SECONDS", 60)),
          attentionPeriod_(std::max(10, envSeconds("ILRDOS_ATTENTION_SECONDS", 300))),
          roamPause_(std::max(5, envSeconds("ILRDOS_ROAM_SECONDS", 25))) {
        resetLocked(started_);
        worker_ = std::thread([this] { run(); });
    }

    ~Autonomy() {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
    }

    void setEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled && !enabled_) resetLocked(Clock::now());
        enabled_ = enabled;
        log(enabled ? "ligada" : "desligada");
    }

    bool enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    }

    void setMotion(MotionMode mode) {
        std::lock_guard<std::mutex> lock(mutex_);
        motion_ = mode;
        log(std::string("movimento autônomo: ") + motionName(mode));
    }

    MotionMode motion() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return motion_;
    }

    // Comando manual/voz de movimento: a autonomia dá um tempo antes de voltar a mexer o robô.
    void noteExternalMotion() {
        std::lock_guard<std::mutex> lock(mutex_);
        externalMotionAt_ = Clock::now();
    }

    std::string status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        out << "Autonomia: " << (enabled_ ? "ligada" : "desligada")
            << " | movimento=" << motionName(motion_)
            << " | pessoa=" << (present_ ? "sim" : "nao")
            << " | conhecidos=" << knownPresent_.size()
            << " | chegadas=" << arrivals_ << " | varreduras=" << scans_
            << " | camera=" << (cameraFailing() ? "falhando" : "ok")
            << " | wifi=" << (wifiDown_ ? "caido" : "ok") << " | temp=";
        if (haveTemp_) out << std::fixed << std::setprecision(1) << tempC_ << "C";
        else out << "n/d";
        out << " (" << thermalName() << ")\n";
        return out.str();
    }

    // Texto curto sobre a saúde do robô, usado na conversa.
    std::string selfStatus() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        if (haveTemp_) out << "Minha CPU está a " << static_cast<int>(tempC_) << " graus (" << thermalName() << ")";
        else out << "Não consigo ler minha temperatura";
        out << "; câmera " << (cameraFailing() ? "com problema" : "ok") << "; Wi-Fi " << (wifiDown_ ? "caído" : "ok") << ".";
        return out.str();
    }

private:
    enum class Thermal { Normal, Hot, Critical };
    static constexpr int kCameraFailLimit = 5;   // varreduras seguidas com erro
    static constexpr int kSeenToArrive = 2;      // varreduras com rosto para "chegou"
    static constexpr int kMissedToLeave = 3;     // sem rosto para "saiu" (ausência não é prova)
    static constexpr int kNameSeenToConfirm = 2; // varreduras seguidas para confirmar um nome
    static constexpr int kNameMissedToLose = 6;

    using Deferred = std::vector<std::function<void()>>;

    static void log(const std::string& message) { logLine("autonomia", message); }

    bool cameraFailing() const { return cameraFailures_ >= kCameraFailLimit; }

    const char* thermalName() const {
        switch (thermal_) {
            case Thermal::Hot: return "quente";
            case Thermal::Critical: return "critico";
            default: return "normal";
        }
    }

    void resetLocked(Clock::time_point now) {
        present_ = false;
        seen_ = missed_ = cameraFailures_ = 0;
        idleSince_ = lastAttention_ = now;
        attentionUntil_ = now;
        nextScan_ = now + std::chrono::seconds(scanInterval_);
        nextHealth_ = now;
        nextRoam_ = now + std::chrono::seconds(roamPause_);
        knownPresent_.clear();
        nameSeen_.clear();
        nameMissed_.clear();
    }

    void run() {
        while (running_.load()) {
            bool scanNow = false;
            bool recoverWifi = false;
            bool active = false;
            Deferred deferred;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active = enabled_;
                const auto now = Clock::now();
                if (active) {
                    if (now >= nextHealth_) {
                        recoverWifi = healthLocked(now, deferred);
                        nextHealth_ = now + std::chrono::seconds(10);
                    }
                    if (now >= nextScan_) {
                        if (thermal_ == Thermal::Critical) {
                            nextScan_ = now + std::chrono::seconds(10);   // visão pausada p/ esfriar
                        } else {
                            scanNow = true;
                        }
                    }
                }
            }
            if (recoverWifi && access(wifiRecovery_.c_str(), X_OK) == 0) {
                log("reiniciando o Wi-Fi");
                runProgram({wifiRecovery_, "restart"});
            }
            if (scanNow) {
                const Observation obs = vision_.scan();
                std::lock_guard<std::mutex> lock(mutex_);
                const auto now = Clock::now();
                recordScanLocked(obs, now, deferred);
                motionLocked(obs, now, deferred);
                nextScan_ = now + currentIntervalLocked();
            }
            if (active) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (enabled_) robot_.setAutoState(decideLocked(Clock::now()));
            }
            for (auto& action : deferred) action();   // fala, regras e motores: fora do mutex
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::chrono::milliseconds currentIntervalLocked() const {
        const bool hot = thermal_ == Thermal::Hot;
        if (cameraFailing()) return std::chrono::seconds(30);          // tenta de novo (hotplug)
        if (vision_.enrolling()) return std::chrono::milliseconds(hot ? 800 : 400);
        if (present_ && motion_ != MotionMode::Off) return std::chrono::milliseconds(hot ? 2800 : 700);
        if (present_) return std::chrono::seconds(hot ? scanInterval_ * 4 : std::max(1, scanInterval_ / 2));
        return std::chrono::seconds(hot ? scanInterval_ * 4 : scanInterval_);
    }

    void say(Deferred& deferred, const std::string& text) {
        if (hooks_.say) deferred.push_back([this, text] { hooks_.say(text); });
    }

    void fire(Deferred& deferred, const std::string& event, const std::string& subject = "any", const std::string& who = {}) {
        if (hooks_.fireRule) deferred.push_back([this, event, subject, who] { hooks_.fireRule(event, subject, who); });
    }

    std::string greeting(const std::string& name) {
        static const char* forms[] = {"Olá, %s! Que bom te ver.", "Oi, %s! Em que posso ajudar?", "Olá, %s! Estou por aqui se precisar."};
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), forms[greetIndex_++ % 3], FaceStore::displayName(name).c_str());
        return buffer;
    }

    void recordScanLocked(const Observation& obs, Clock::time_point now, Deferred& deferred) {
        ++scans_;
        if (obs.faces < 0) {
            if (++cameraFailures_ == kCameraFailLimit) {
                log("camera indisponivel");
                if (now - lastSelfSpeech_ > std::chrono::minutes(5)) { lastSelfSpeech_ = now; say(deferred, "Minha câmera parou de responder."); }
            }
            seen_ = missed_ = 0;
            return;
        }
        if (cameraFailing()) log("camera voltou");
        cameraFailures_ = 0;

        if (obs.faces > 0) {
            seen_++;
            missed_ = 0;
            if (!present_ && seen_ >= kSeenToArrive) {
                present_ = true;
                ++arrivals_;
                arrivedAt_ = now;
                identifyPending_ = true;
                log("pessoa detectada");
                fire(deferred, "arrive", "any");
            }
        } else {
            missed_++;
            seen_ = 0;
            if (present_ && missed_ >= kMissedToLeave) {
                present_ = false;
                idleSince_ = lastAttention_ = now;
                nextRoam_ = now + std::chrono::seconds(roamPause_);
                identifyPending_ = false;
                knownPresent_.clear();
                nameSeen_.clear();
                nameMissed_.clear();
                log("ninguem por perto");
                fire(deferred, "empty", "any");
            }
        }

        // identidades: confirma um nome só depois de 2 varreduras seguidas (evita "trocar" pessoas no susto)
        const auto names = obs.knownNames();
        for (const auto& name : names) {
            nameMissed_[name] = 0;
            if (++nameSeen_[name] >= kNameSeenToConfirm && knownPresent_.insert(name).second) {
                log("reconheci " + name);
                const auto last = lastGreeted_.find(name);
                if (last == lastGreeted_.end() || now - last->second > std::chrono::minutes(10)) {
                    lastGreeted_[name] = now;
                    say(deferred, greeting(name));
                }
                fire(deferred, "arrive", "known", name);
            }
        }
        for (auto it = knownPresent_.begin(); it != knownPresent_.end();) {
            if (std::find(names.begin(), names.end(), *it) != names.end()) { ++it; continue; }
            if (++nameMissed_[*it] >= kNameMissedToLose) {
                log(*it + " saiu de vista");
                nameSeen_.erase(*it);
                it = knownPresent_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& [name, count] : nameSeen_) {
            if (std::find(names.begin(), names.end(), name) == names.end() && knownPresent_.count(name) == 0) count = 0;
        }

        // desconhecido: depois de uns segundos sem conseguir identificar ninguém, cumprimenta com educação
        if (present_ && identifyPending_ && now - arrivedAt_ >= std::chrono::seconds(4)) {
            identifyPending_ = false;
            if (knownPresent_.empty() && obs.unknownCount() > 0 && now - lastUnknownGreet_ > std::chrono::seconds(90)) {
                lastUnknownGreet_ = now;
                say(deferred, "Olá! Eu sou o ILrDOS. Para falar comigo, diga ILrDOS antes do pedido.");
                fire(deferred, "arrive", "unknown");
            }
        }
    }

    // Movimento autônomo. Todos os limites reais (tempo, velocidade, sonar) ficam no DriveSystem.
    void motionLocked(const Observation& obs, Clock::time_point now, Deferred& deferred) {
        if (motion_ == MotionMode::Off || !hooks_.canMove || !hooks_.canMove()) return;
        if (thermal_ != Thermal::Normal || vision_.enrolling()) return;
        if (hooks_.moving && hooks_.moving()) return;
        const bool recentExternal = now - externalMotionAt_ < (present_ ? std::chrono::seconds(6) : std::chrono::seconds(30));
        if (recentExternal) return;

        if (present_ && !obs.people.empty()) {
            const Person* target = &obs.people.front();
            for (const auto& p : obs.people) if (p.w > target->w) target = &p;
            const double error = target->cx - 0.5;
            if (std::abs(error) > 0.15 && now >= nextTrack_) {
                nextTrack_ = now + std::chrono::milliseconds(600);
                const bool toLeft = (error < 0) != trackInvert_;
                const int ms = std::clamp(static_cast<int>(80 + 400 * std::abs(error)), 80, 250);
                if (hooks_.pulseTurn) deferred.push_back([this, toLeft, ms] { hooks_.pulseTurn(toLeft, ms, 40); });
            } else if (motion_ == MotionMode::Roam && std::abs(error) <= 0.2 && target->w < 0.18 && now >= nextApproach_ &&
                       hooks_.hasSonar && hooks_.hasSonar()) {
                const auto distance = hooks_.distanceCm ? hooks_.distanceCm() : std::nullopt;
                if (distance && *distance > 70.0) {            // chega perto, mas nunca colado na pessoa
                    nextApproach_ = now + std::chrono::seconds(2);
                    if (hooks_.move) deferred.push_back([this] { hooks_.move("forward", 35, 400); });
                }
            }
            return;
        }

        if (!present_ && motion_ == MotionMode::Roam && now >= nextRoam_ && hooks_.hasSonar && hooks_.hasSonar()) {
            const auto distance = hooks_.distanceCm ? hooks_.distanceCm() : std::nullopt;
            nextRoam_ = now + std::chrono::seconds(20 + static_cast<int>(rng_() % 21));
            if (!distance) return;
            if (*distance > 60.0) {
                const int ms = 900 + static_cast<int>(rng_() % 700);
                if (hooks_.move) deferred.push_back([this, ms] { hooks_.move("forward", 35, ms); });
            } else {
                const bool left = rng_() % 2 == 0;
                const int degrees = 40 + static_cast<int>(rng_() % 100);
                if (hooks_.turn) deferred.push_back([this, left, degrees] { hooks_.turn(left, degrees); });
            }
        }
    }

    // Retorna true se o Wi-Fi deve ser reiniciado (feito fora do mutex).
    bool healthLocked(Clock::time_point now, Deferred& deferred) {
        if (const auto raw = readFirstLine(thermalPath_)) {
            try {
                tempC_ = std::stol(*raw) / 1000.0;
                haveTemp_ = true;
                Thermal next = thermal_;
                switch (thermal_) {
                    case Thermal::Normal:
                        if (tempC_ >= 85.0) next = Thermal::Critical;
                        else if (tempC_ >= 80.0) next = Thermal::Hot;
                        break;
                    case Thermal::Hot:
                        if (tempC_ >= 85.0) next = Thermal::Critical;
                        else if (tempC_ < 75.0) next = Thermal::Normal;
                        break;
                    case Thermal::Critical:
                        if (tempC_ < 75.0) next = Thermal::Normal;
                        else if (tempC_ < 80.0) next = Thermal::Hot;
                        break;
                }
                if (next != thermal_) {
                    const Thermal previous = thermal_;
                    thermal_ = next;
                    log(std::string("temperatura ") + thermalName());
                    if (hooks_.stop) hooks_.stop();   // esquentou: para as rodas
                    if (next == Thermal::Critical) {
                        fire(deferred, "hot", "any");
                        if (now - lastSelfSpeech_ > std::chrono::minutes(5)) { lastSelfSpeech_ = now; say(deferred, "Estou muito quente. Vou pausar a visão até esfriar."); }
                    } else if (previous == Thermal::Critical || (previous == Thermal::Hot && next == Thermal::Normal)) {
                        fire(deferred, "cool", "any");
                    }
                }
            } catch (...) {
                haveTemp_ = false;
            }
        } else {
            haveTemp_ = false;
        }

        bool recover = false;
        const auto grace = std::chrono::seconds(wifiGrace_);
        if (const auto state = readFirstLine(wifiStatePath_)) {
            if (*state == "up") {
                if (wifiDown_) log("wifi voltou");
                wifiDown_ = false;
                wifiDownSince_.reset();
            } else {
                if (!wifiDownSince_) wifiDownSince_ = now;
                if (now - started_ >= grace && now - *wifiDownSince_ >= grace) {
                    if (!wifiDown_) log("wifi caiu");
                    wifiDown_ = true;
                    if (now - lastRecovery_ >= std::chrono::seconds(120)) {
                        lastRecovery_ = now;
                        recover = true;
                    }
                }
            }
        } else {
            wifiDown_ = false;          // sem interface wlan0: nada a monitorar
            wifiDownSince_.reset();
        }
        return recover;
    }

    // Prioridade: superaquecimento > falha (pisca alerta 5 s por minuto) > pessoa > atenção > repouso.
    std::string decideLocked(Clock::time_point now) {
        if (thermal_ == Thermal::Critical) return "alert";
        const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_).count();
        if ((cameraFailing() || wifiDown_) && (uptime % 60) < 5) return "alert";
        if (present_) return "listen";
        if (!cameraFailing()) {
            const auto period = std::chrono::seconds(attentionPeriod_);
            if (now < attentionUntil_) return "think";
            if (now - idleSince_ >= period && now - lastAttention_ >= period) {
                attentionUntil_ = now + std::chrono::seconds(4);
                lastAttention_ = now;
                return "think";
            }
        }
        return "idle";
    }

    RobotController& robot_;
    VisionSystem& vision_;
    AutonomyHooks hooks_;
    mutable std::mutex mutex_;
    bool enabled_{true};
    int scanInterval_{4};
    MotionMode motion_{MotionMode::Off};
    bool trackInvert_{false};
    Clock::time_point started_;
    std::mt19937 rng_;
    std::string thermalPath_, wifiStatePath_, wifiRecovery_;
    int wifiGrace_{60};
    int attentionPeriod_{300};
    int roamPause_{25};

    bool present_{false};
    bool identifyPending_{false};
    int seen_{0}, missed_{0}, cameraFailures_{0};
    long arrivals_{0}, scans_{0};
    unsigned greetIndex_{0};
    std::set<std::string> knownPresent_;
    std::map<std::string, int> nameSeen_, nameMissed_;
    std::map<std::string, Clock::time_point> lastGreeted_;
    Clock::time_point lastUnknownGreet_{}, arrivedAt_{}, lastSelfSpeech_{}, externalMotionAt_{};
    Clock::time_point nextTrack_{}, nextApproach_{}, nextRoam_{};
    Thermal thermal_{Thermal::Normal};
    double tempC_{0.0};
    bool haveTemp_{false};
    bool wifiDown_{false};
    std::optional<Clock::time_point> wifiDownSince_;
    Clock::time_point lastRecovery_{};
    Clock::time_point idleSince_{}, lastAttention_{}, attentionUntil_{}, nextScan_{}, nextHealth_{};

    std::atomic<bool> running_{true};
    std::thread worker_;
};

}  // namespace ilrdos
