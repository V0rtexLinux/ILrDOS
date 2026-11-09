// ILrDOS — locomoção (ponte H com PWM por software) e sonar HC-SR04.
//
// Segurança, em camadas:
//  1. Todo movimento tem prazo (máx. 3 s). Se o daemon travar ou a rede cair, o robô para sozinho.
//  2. Velocidade limitada por MOTOR_MAX_SPEED (padrão 60%); ré mais curta e mais lenta (sem sensor traseiro).
//  3. Para a frente: o sonar para o robô a < SONAR_STOP_CM. Sonar configurado mas sem leitura = não anda.
//  4. `stop` (terminal) e "pare" (voz) sempre vencem qualquer movimento.
//  5. Ao encerrar o daemon os pinos voltam a nível baixo (GpioOutput).
//
// Envelope de movimento da ESTRUTURA (desenho do robô: cabos de energia -/+ e cabos de dados do visor correm
// pelo corpo até o cérebro/computador principal, protegido pelo casco). Para o robô não travar nem bugar:
//  6. Orçamento de torção dos cabos: o giro acumulado (esq/dir) é contado e limitado a ±MAX_TWIST_DEG.
//     No limite o robô só gira para o lado de volta; `untwist` desfaz a torção sozinho.
//  7. Partida e parada suaves (rampa): sem tranco no visor, nos cabos nem no computador principal.
//  8. Inversão de sentido só depois de uma pausa (sem "chicote" nas rodas/cabos nem curto na ponte H).
//  9. Após queda/batida no casco, bloqueia novos movimentos por alguns segundos.
// 10. Energia dos cabos baixa ou cérebro em calor crítico (guarda da estrutura) = não anda.
#pragma once

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <functional>

#include "common.hpp"

namespace ilrdos {

class DriveSystem {
public:
    struct Config {
        int leftFwd{-1}, leftRev{-1}, rightFwd{-1}, rightRev{-1};
        std::vector<int> enablePins;      // ENA/ENB mantidos em nível alto (opcional)
        int trig{-1}, echo{-1};
        int maxSpeed{60};
        int sonarStopCm{20};
        int turnMsPer90{700};             // calibre com o seu robô (sem encoder: giro por tempo)
        int maxTwistDeg{270};             // torção máxima dos cabos (energia -/+ e dados do visor) para cada lado
        int rampMs{300};                  // tempo para acelerar/frear suavemente (0 = sem rampa)
        int reverseSettleMs{250};         // pausa com rodas paradas ao inverter o sentido de uma roda
        int impactLockoutMs{4000};        // bloqueio de movimento depois de queda/batida no casco
        std::string twistFile;            // guarda a torção entre reinícios (vazio = não guarda)
    };
    struct MoveResult {
        bool ok{false};
        std::string text;
        int speed{0};
        int durationMs{0};
    };
    enum class Dir { Forward, Back, Left, Right };

    DriveSystem(const std::string& chipPath, Config config, bool simulate)
        : cfg_(std::move(config)), simulate_(simulate) {
        cfg_.maxSpeed = std::clamp(cfg_.maxSpeed, 20, 100);
        cfg_.maxTwistDeg = std::clamp(cfg_.maxTwistDeg, 45, 720);
        cfg_.rampMs = std::clamp(cfg_.rampMs, 0, 1000);
        cfg_.reverseSettleMs = std::clamp(cfg_.reverseSettleMs, 0, 1000);
        cfg_.impactLockoutMs = std::clamp(cfg_.impactLockoutMs, 0, 30000);
        if (!cfg_.twistFile.empty()) {
            std::ifstream saved(cfg_.twistFile);
            double value = 0.0;
            if (saved >> value && std::isfinite(value)) twistDeg_ = std::clamp(value, -720.0, 720.0);
        }
        motorsConfigured_ = cfg_.leftFwd >= 0 && cfg_.leftRev >= 0 && cfg_.rightFwd >= 0 && cfg_.rightRev >= 0;
        sonarConfigured_ = cfg_.trig >= 0 && cfg_.echo >= 0;
        if (!simulate_ && (motorsConfigured_ || sonarConfigured_)) {
            chipFd_ = openGpioChip(chipPath);
            try {
                if (motorsConfigured_) {
                    lines_[0] = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(cfg_.leftFwd), "mlf");
                    lines_[1] = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(cfg_.leftRev), "mlr");
                    lines_[2] = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(cfg_.rightFwd), "mrf");
                    lines_[3] = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(cfg_.rightRev), "mrr");
                    for (const int pin : cfg_.enablePins) {
                        enables_.push_back(std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(pin), "men"));
                        enables_.back()->set(true);
                    }
                }
                if (sonarConfigured_) {
                    trig_ = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(cfg_.trig), "trig");
                    echo_ = std::make_unique<GpioEdgeInput>(chipFd_, static_cast<unsigned>(cfg_.echo), "echo");
                }
            } catch (...) {
                releaseGpio();
                throw;
            }
        }
        if (motorsConfigured_) pwm_ = std::thread([this] { pwmLoop(); });
        if (sonarConfigured_) sonar_ = std::thread([this] { sonarLoop(); });
    }

    ~DriveSystem() {
        running_.store(false);
        cv_.notify_all();
        if (pwm_.joinable()) pwm_.join();
        if (sonar_.joinable()) sonar_.join();
        releaseGpio();
    }
    DriveSystem(const DriveSystem&) = delete;
    DriveSystem& operator=(const DriveSystem&) = delete;

    bool available() const { return motorsConfigured_; }
    bool hasSonar() const { return sonarConfigured_; }
    int maxSpeed() const { return cfg_.maxSpeed; }

    // Distância (cm) da leitura mais recente, se tiver menos de 1 s.
    std::optional<double> distanceCm() const {
        if (!sonarConfigured_) return std::nullopt;
        const long age = nowMs() - lastReadingMs_.load();
        const double value = lastCm_.load();
        if (value < 0 || age > 1000) return std::nullopt;
        return value;
    }

    bool moving() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

    // Quando devolve texto não vazio, o movimento é recusado (ex.: tensão baixa nos cabos de energia).
    void setGuard(std::function<std::string()> guard) { guard_ = std::move(guard); }

    int maxTwistDeg() const { return cfg_.maxTwistDeg; }

    // Torção estimada dos cabos em graus (+ = direita/horário, − = esquerda). Estimativa por tempo, sem encoder.
    double twistDeg() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return twistDeg_ + turnDeltaLocked(Clock::now());
    }

    // Declara que os cabos estão fisicamente retos (depois de conferir com a mão).
    std::string resetTwist() {
        std::lock_guard<std::mutex> lock(mutex_);
        twistDeg_ = 0.0;
        saveTwistLocked();
        return "Torção dos cabos zerada (assumi que os cabos estão retos).";
    }

    // Gira de volta até os cabos ficarem retos.
    std::string untwist() {
        if (!motorsConfigured_) return "Motores não configurados (veja MOTOR_* em ilrdos-robot.txt).";
        const double twist = twistDeg();
        if (std::abs(twist) < 5.0) return "Cabos já estão alinhados (torção ~0°).";
        const int speed = 40;
        const int ms = std::clamp(static_cast<int>(std::abs(twist) / 90.0 * cfg_.turnMsPer90 * 50.0 / speed) +
                                  cfg_.rampMs, 100, 3000);
        const bool turnLeft = twist > 0;
        const auto result = moveEx(turnLeft ? Dir::Left : Dir::Right, speed, ms);
        if (!result.ok) return result.text;
        return std::string("Desfazendo a torção dos cabos: girando ~") + std::to_string(static_cast<int>(std::abs(twist))) +
               "° para a " + (turnLeft ? "esquerda." : "direita.") +
               (ms >= 3000 ? " (repita `untwist` se ainda restar torção)" : "");
    }

    std::string move(Dir dir, int speed, int durationMs) { return moveEx(dir, speed, durationMs).text; }

    MoveResult moveEx(Dir dir, int speed, int durationMs, int rampMs = -1) {
        if (!motorsConfigured_) return {false, "Motores não configurados (veja MOTOR_* em ilrdos-robot.txt).", 0, 0};
        if (const auto why = blockReason()) return {false, *why, 0, 0};
        speed = std::clamp(speed, 20, cfg_.maxSpeed);
        durationMs = std::clamp(durationMs, 100, 3000);
        if (dir == Dir::Back) { speed = std::min(speed, 40); durationMs = std::min(durationMs, 1500); }
        if (dir == Dir::Forward) {
            if (sonarConfigured_) {
                const auto distance = distanceCm();
                if (!distance) return {false, "Não ando para a frente: o sonar está sem leitura.", 0, 0};
                if (*distance <= cfg_.sonarStopCm + 5) {
                    return {false, "Obstáculo a " + std::to_string(static_cast<int>(*distance)) + " cm; não ando para a frente.", 0, 0};
                }
            }
        }
        std::string clipped;
        if (dir == Dir::Left || dir == Dir::Right) {
            // Orçamento de torção: quanto ainda dá para girar para este lado sem esticar os cabos.
            const int sign = dir == Dir::Right ? 1 : -1;
            const double room = cfg_.maxTwistDeg - sign * twistDeg();
            if (room < 10.0) {
                return {false, "Cabos já torcidos " + std::to_string(static_cast<int>(std::abs(twistDeg()))) + "° para a " +
                               (sign > 0 ? "direita" : "esquerda") + " (limite " + std::to_string(cfg_.maxTwistDeg) +
                               "°); gire para o outro lado ou use `untwist`.", 0, 0};
            }
            const int roomMs = static_cast<int>(room / 90.0 * cfg_.turnMsPer90 * 50.0 / speed) + cfg_.rampMs;
            if (roomMs < durationMs) {
                durationMs = std::max(100, roomMs);
                clipped = " (reduzido: limite de torção dos cabos)";
            }
        }
        int left = 0, right = 0;
        switch (dir) {
            case Dir::Forward: left = speed; right = speed; break;
            case Dir::Back: left = -speed; right = -speed; break;
            case Dir::Left: left = -speed; right = speed; break;
            case Dir::Right: left = speed; right = -speed; break;
        }
        start(left, right, durationMs, rampMs < 0 ? cfg_.rampMs : rampMs);
        static const char* names[] = {"frente", "trás", "esquerda", "direita"};
        return {true, std::string("Movendo para ") + names[static_cast<int>(dir)] + " a " + std::to_string(speed) +
                      "% por " + std::to_string(durationMs) + " ms." + clipped, speed, durationMs};
    }

    std::string turnDegrees(bool left, int degrees) {
        degrees = std::clamp(degrees, 10, 180);
        // A rampa suave "come" um pouco do giro; compensa para o ângulo continuar calibrado.
        const int base = std::max(100, cfg_.turnMsPer90 * degrees / 90);
        const int ms = std::min(base + std::min(cfg_.rampMs, base / 2), 3000);
        const auto result = moveEx(left ? Dir::Left : Dir::Right, 50, ms);
        if (!result.ok) return result.text;
        const int done = std::min(degrees, static_cast<int>(std::lround((result.durationMs - std::min(cfg_.rampMs, base / 2)) *
                                                                         90.0 / cfg_.turnMsPer90)));
        const bool shortened = done < degrees - 5;
        return std::string("Girando ~") + std::to_string(shortened ? std::max(done, 10) : degrees) + "° para a " +
               (left ? "esquerda." : "direita.") + (shortened ? " (reduzido: limite de torção dos cabos)" : "");
    }

    // Pequeno giro usado pelo rastreamento de rostos (sem mensagem; rampa curta porque o pulso já é curto).
    void pulseTurn(bool left, int ms, int speed) {
        if (!motorsConfigured_ || blockReason()) return;
        speed = std::clamp(speed, 20, std::min(cfg_.maxSpeed, 50));
        ms = std::clamp(ms, 50, 400);
        const int sign = left ? -1 : 1;
        if (cfg_.maxTwistDeg - sign * twistDeg() < 10.0) return;   // cabos no limite: não gira mais para este lado
        start(left ? -speed : speed, left ? speed : -speed, ms, std::min(cfg_.rampMs, 40));
    }

    // Parada de emergência: imediata (sem rampa). Impacto no casco também bloqueia novos movimentos por um tempo.
    void stop(const char* why = "comando") {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::string(why) == "impacto") lockoutUntil_ = Clock::now() + std::chrono::milliseconds(cfg_.impactLockoutMs);
        stopLocked(why);
    }

    std::string status() const {
        std::ostringstream out;
        out << "motores=" << (!motorsConfigured_ ? "nao-configurados" : simulate_ ? "simulados" : "ok")
            << " | velocidade-max=" << cfg_.maxSpeed << "% | sonar=";
        if (!sonarConfigured_) out << "nao-configurado";
        else if (const auto d = distanceCm()) out << static_cast<int>(*d) << "cm";
        else out << "sem-leitura";
        if (motorsConfigured_) out << " | torcao-cabos=" << std::showpos << static_cast<int>(std::lround(twistDeg()))
                                   << std::noshowpos << "/" << cfg_.maxTwistDeg << "deg";
        return out.str();
    }

private:
    static long nowMs() {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
    }

    // Motivo para recusar qualquer movimento agora (vazio = liberado).
    std::optional<std::string> blockReason() const {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = Clock::now();
            if (now < lockoutUntil_) {
                const long left = std::chrono::duration_cast<std::chrono::seconds>(lockoutUntil_ - now).count() + 1;
                return "O casco levou uma batida agora há pouco; espero " + std::to_string(left) +
                       " s antes de mover (cabos e computador principal precisam assentar).";
            }
        }
        if (guard_) {
            const std::string reason = guard_();
            if (!reason.empty()) return reason;
        }
        return std::nullopt;
    }

    // Graus girados (com sinal) pelo movimento em curso, ainda não somados a twistDeg_.
    // Considera a rampa: o tempo "perdido" acelerando/freando não gira o robô.
    double turnDeltaLocked(Clock::time_point now) const {
        if (!active_ || left_ == 0 || left_ != -right_) return 0.0;
        const auto end = std::min(now, until_);
        if (end <= startedAt_) return 0.0;
        const double elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - startedAt_).count() / 1000.0;
        const double total = std::chrono::duration_cast<std::chrono::microseconds>(until_ - startedAt_).count() / 1000.0;
        const double ramp = rampEffMs_;
        double effective = elapsed;
        if (ramp > 0.0) {
            effective -= elapsed < ramp ? elapsed - elapsed * elapsed / (2.0 * ramp) : ramp / 2.0;
            const double x = elapsed - (total - ramp);
            if (x > 0.0) effective -= x * x / (2.0 * ramp);
        }
        const double degrees = std::max(0.0, effective) / cfg_.turnMsPer90 * 90.0 * std::abs(left_) / 50.0;
        return left_ > 0 ? degrees : -degrees;   // esquerda>0, direita<0 = giro para a direita
    }

    void saveTwistLocked() {
        if (cfg_.twistFile.empty()) return;
        std::ofstream out(cfg_.twistFile, std::ios::trunc);
        if (out) out << twistDeg_ << '\n';
    }

    // Encerra o movimento em curso (soma a torção, lembra o sentido e põe os pinos em zero).
    void haltLocked() {
        if (active_) {
            const double delta = turnDeltaLocked(Clock::now());
            if (delta != 0.0) {
                twistDeg_ = std::clamp(twistDeg_ + delta, -720.0, 720.0);
                saveTwistLocked();
            }
            lastLeft_ = left_;
            lastRight_ = right_;
            lastEndAt_ = Clock::now();
            active_ = false;
        }
        left_ = right_ = 0;
        applyPins(0, 0);
    }

    void start(int left, int right, int durationMs, int rampMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_) haltLocked();   // comando novo interrompe o anterior: rodas param antes de seguir
        const auto now = Clock::now();
        // Inversão de sentido de qualquer roda: espera um pouco com as rodas paradas.
        std::chrono::milliseconds settle{0};
        if (lastEndAt_ != Clock::time_point{} && ((lastLeft_ * left < 0) || (lastRight_ * right < 0))) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEndAt_);
            settle = std::max(std::chrono::milliseconds(0), std::chrono::milliseconds(cfg_.reverseSettleMs) - since);
        }
        left_ = left;
        right_ = right;
        startedAt_ = now + settle;
        until_ = startedAt_ + std::chrono::milliseconds(durationMs);
        rampEffMs_ = std::clamp(rampMs, 0, durationMs / 3);
        active_ = true;
        simAnnounced_ = false;
        cv_.notify_all();
    }

    void stopLocked(const char* why) {
        if (active_ && simulate_) logLine("motor simulado", std::string("parado (") + why + ")");
        haltLocked();
    }

    void applyPins(int left, int right) {
        if (left == appliedLeft_ && right == appliedRight_) return;
        appliedLeft_ = left;
        appliedRight_ = right;
        if (simulate_ || !lines_[0]) return;
        lines_[0]->set(left > 0);
        lines_[1]->set(left < 0);
        lines_[2]->set(right > 0);
        lines_[3]->set(right < 0);
    }

    // PWM por software: período de 10 ms, resolução de 5%.
    void pwmLoop() {
        while (running_.load()) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!active_) {
                cv_.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }
            const auto now = Clock::now();
            if (now >= until_) { stopLocked("fim do tempo"); continue; }
            if (now < startedAt_) {   // pausa de inversão de sentido: rodas paradas
                applyPins(0, 0);
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (left_ != 0 && left_ == -right_) {   // giro: não passa do limite de torção dos cabos
                const double twist = twistDeg_ + turnDeltaLocked(now);
                if (std::abs(twist) >= cfg_.maxTwistDeg && ((twist > 0) == (left_ > 0))) {
                    stopLocked("limite de torção dos cabos");
                    continue;
                }
            }
            if (left_ > 0 && right_ > 0 && sonarConfigured_) {
                const auto distance = distanceCm();
                if (!distance) {
                    if (now - startedAt_ > std::chrono::milliseconds(600)) { stopLocked("sonar sem leitura"); continue; }
                } else if (*distance < cfg_.sonarStopCm) {
                    stopLocked("obstáculo");
                    continue;
                }
            }
            if (simulate_ && !simAnnounced_) {
                simAnnounced_ = true;
                logLine("motor simulado", "esq=" + std::to_string(left_) + "% dir=" + std::to_string(right_) + "%");
            }
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - startedAt_).count() % 10000;
            const int phase = static_cast<int>(micros / 100);   // 0..99
            // Rampa: velocidade sobe no começo e desce no fim (mínimo de 20% para o motor não engasgar).
            double scale = 1.0;
            if (rampEffMs_ > 0) {
                const double sinceStart = std::chrono::duration_cast<std::chrono::microseconds>(now - startedAt_).count() / 1000.0;
                const double remaining = std::chrono::duration_cast<std::chrono::microseconds>(until_ - now).count() / 1000.0;
                scale = std::clamp(std::min(sinceStart, remaining) / rampEffMs_, 0.0, 1.0);
            }
            const auto duty = [&](int speed) {
                const double target = std::abs(speed) * scale;
                return target <= 0.0 ? 0.0 : std::min<double>(std::abs(speed), std::max(20.0, target));
            };
            applyPins(phase < duty(left_) ? (left_ > 0 ? 100 : -100) : 0,
                      phase < duty(right_) ? (right_ > 0 ? 100 : -100) : 0);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(400));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        stopLocked("encerrando");
    }

    std::optional<double> measureOnce() {
        if (simulate_) {
            double value = 200.0;
            std::ifstream input(envOr("ILRDOS_SIM_SONAR_FILE", "/nonexistent"));
            if (input) input >> value;
            return value;
        }
        echo_->drain();
        trig_->set(true);
        std::this_thread::sleep_for(std::chrono::microseconds(15));
        trig_->set(false);
        const auto rise = echo_->wait(30);
        if (!rise || rise->first != GPIOEVENT_EVENT_RISING_EDGE) return std::nullopt;
        const auto fall = echo_->wait(30);
        if (!fall || fall->first != GPIOEVENT_EVENT_FALLING_EDGE || fall->second <= rise->second) return std::nullopt;
        const double micros = static_cast<double>(fall->second - rise->second) / 1000.0;
        const double cm = micros / 58.0;
        if (cm < 2.0 || cm > 400.0) return std::nullopt;
        return cm;
    }

    void sonarLoop() {
        while (running_.load()) {
            if (const auto cm = measureOnce()) {
                lastCm_.store(*cm);
                lastReadingMs_.store(nowMs());
            }
            bool busy;
            { std::lock_guard<std::mutex> lock(mutex_); busy = active_; }
            std::this_thread::sleep_for(std::chrono::milliseconds(busy ? 60 : 250));
        }
    }

    void releaseGpio() {
        for (auto& line : lines_) line.reset();
        enables_.clear();
        trig_.reset();
        echo_.reset();
        if (chipFd_ >= 0) { close(chipFd_); chipFd_ = -1; }
    }

    Config cfg_;
    bool simulate_{false};
    bool motorsConfigured_{false};
    bool sonarConfigured_{false};
    int chipFd_{-1};
    std::array<std::unique_ptr<GpioOutput>, 4> lines_;
    std::vector<std::unique_ptr<GpioOutput>> enables_;
    std::unique_ptr<GpioOutput> trig_;
    std::unique_ptr<GpioEdgeInput> echo_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool active_{false};
    bool simAnnounced_{false};
    int left_{0}, right_{0};
    int appliedLeft_{0}, appliedRight_{0};
    Clock::time_point startedAt_{}, until_{};
    double rampEffMs_{0.0};
    double twistDeg_{0.0};                 // torção acumulada dos cabos (+ direita, − esquerda)
    int lastLeft_{0}, lastRight_{0};       // sentido das rodas no último movimento
    Clock::time_point lastEndAt_{};
    Clock::time_point lockoutUntil_{};     // bloqueio após impacto no casco
    std::function<std::string()> guard_;

    std::atomic<double> lastCm_{-1.0};
    std::atomic<long> lastReadingMs_{0};
    std::atomic<bool> running_{true};
    std::thread pwm_, sonar_;
};

}  // namespace ilrdos
