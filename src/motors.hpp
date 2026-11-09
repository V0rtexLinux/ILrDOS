// ILrDOS — locomoção (ponte H com PWM por software) e sonar HC-SR04.
//
// Segurança, em camadas:
//  1. Todo movimento tem prazo (máx. 3 s). Se o daemon travar ou a rede cair, o robô para sozinho.
//  2. Velocidade limitada por MOTOR_MAX_SPEED (padrão 60%); ré mais curta e mais lenta (sem sensor traseiro).
//  3. Para a frente: o sonar para o robô a < SONAR_STOP_CM. Sonar configurado mas sem leitura = não anda.
//  4. `stop` (terminal) e "pare" (voz) sempre vencem qualquer movimento.
//  5. Ao encerrar o daemon os pinos voltam a nível baixo (GpioOutput).
#pragma once

#include <atomic>
#include <condition_variable>

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
    };
    enum class Dir { Forward, Back, Left, Right };

    DriveSystem(const std::string& chipPath, Config config, bool simulate)
        : cfg_(std::move(config)), simulate_(simulate) {
        cfg_.maxSpeed = std::clamp(cfg_.maxSpeed, 20, 100);
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

    std::string move(Dir dir, int speed, int durationMs) {
        if (!motorsConfigured_) return "Motores não configurados (veja MOTOR_* em ilrdos-robot.txt).";
        speed = std::clamp(speed, 20, cfg_.maxSpeed);
        durationMs = std::clamp(durationMs, 100, 3000);
        if (dir == Dir::Back) { speed = std::min(speed, 40); durationMs = std::min(durationMs, 1500); }
        if (dir == Dir::Forward) {
            if (sonarConfigured_) {
                const auto distance = distanceCm();
                if (!distance) return "Não ando para a frente: o sonar está sem leitura.";
                if (*distance <= cfg_.sonarStopCm + 5) {
                    return "Obstáculo a " + std::to_string(static_cast<int>(*distance)) + " cm; não ando para a frente.";
                }
            }
        }
        int left = 0, right = 0;
        switch (dir) {
            case Dir::Forward: left = speed; right = speed; break;
            case Dir::Back: left = -speed; right = -speed; break;
            case Dir::Left: left = -speed; right = speed; break;
            case Dir::Right: left = speed; right = -speed; break;
        }
        start(left, right, durationMs);
        static const char* names[] = {"frente", "trás", "esquerda", "direita"};
        return std::string("Movendo para ") + names[static_cast<int>(dir)] + " a " + std::to_string(speed) +
               "% por " + std::to_string(durationMs) + " ms.";
    }

    std::string turnDegrees(bool left, int degrees) {
        degrees = std::clamp(degrees, 10, 180);
        const int ms = std::max(100, cfg_.turnMsPer90 * degrees / 90);
        std::string text = move(left ? Dir::Left : Dir::Right, 50, std::min(ms, 3000));
        if (motorsConfigured_) text = std::string("Girando ~") + std::to_string(degrees) + "° para a " +
                                      (left ? "esquerda." : "direita.");
        return text;
    }

    // Pequeno giro usado pelo rastreamento de rostos (sem mensagem).
    void pulseTurn(bool left, int ms, int speed) {
        if (!motorsConfigured_) return;
        speed = std::clamp(speed, 20, std::min(cfg_.maxSpeed, 50));
        start(left ? -speed : speed, left ? speed : -speed, std::clamp(ms, 50, 400));
    }

    void stop(const char* why = "comando") {
        std::lock_guard<std::mutex> lock(mutex_);
        stopLocked(why);
    }

    std::string status() const {
        std::ostringstream out;
        out << "motores=" << (!motorsConfigured_ ? "nao-configurados" : simulate_ ? "simulados" : "ok")
            << " | velocidade-max=" << cfg_.maxSpeed << "% | sonar=";
        if (!sonarConfigured_) out << "nao-configurado";
        else if (const auto d = distanceCm()) out << static_cast<int>(*d) << "cm";
        else out << "sem-leitura";
        return out.str();
    }

private:
    static long nowMs() {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
    }

    void start(int left, int right, int durationMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        left_ = left;
        right_ = right;
        startedAt_ = Clock::now();
        until_ = startedAt_ + std::chrono::milliseconds(durationMs);
        active_ = true;
        simAnnounced_ = false;
        cv_.notify_all();
    }

    void stopLocked(const char* why) {
        if (active_) {
            active_ = false;
            if (simulate_) logLine("motor simulado", std::string("parado (") + why + ")");
        }
        left_ = right_ = 0;
        applyPins(0, 0);
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
            applyPins(phase < std::abs(left_) ? (left_ > 0 ? 100 : -100) : 0,
                      phase < std::abs(right_) ? (right_ > 0 ? 100 : -100) : 0);
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

    std::atomic<double> lastCm_{-1.0};
    std::atomic<long> lastReadingMs_{0};
    std::atomic<bool> running_{true};
    std::thread pwm_, sonar_;
};

}  // namespace ilrdos
