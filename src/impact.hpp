// ILrDOS — sensor de impacto do casco protetor + buzzer de alerta suave.
//
// Quando o casco detecta queda ou batida forte, o sensor dispara (borda de subida).
// O buzzer emite 3 bips curtos (~900 Hz, 30 % duty cycle, 220 ms cada):
//   audível para alertar quem está por perto, mas sem ensurdecer nem irritar.
//
// Uso no desenho:
//   [casco protetor] ─ sensor de impacto ──► detecção de queda
//   [buzzer] ◄────────────────────────────── bip suave 3×
//
// Configuração em ilrdos-robot.txt:
//   IMPACT_SENSOR_PIN=<pino BCM do sensor de vibração, ex. 20>
//   IMPACT_BUZZER_PIN=<pino BCM do buzzer passivo,     ex. 21>
//   IMPACT_BUZZER_FREQ_HZ=900   (opcional; padrão 900 Hz)
#pragma once

#include <atomic>
#include <functional>

#include "common.hpp"

namespace ilrdos {

class ImpactSensor {
public:
    struct Config {
        int sensorPin{-1};          // pino de entrada do sensor de vibração/choque (SW-420 ou similar)
        int buzzerPin{-1};          // pino de saída para o buzzer passivo
        int buzzerFreqHz{900};      // frequência do bip (600–1 200 Hz: audível, não irritante)
        int beepCount{3};           // quantos bips por alerta
        int beepMs{220};            // duração de cada bip (ms)
        int pauseMs{150};           // silêncio entre bips (ms)
        int cooldownMs{5000};       // mínimo entre dois alertas (ms)
        int warnWindowMs{30000};    // janela após queda em que casing fica como "atenção" (ms)
    };

    // Chamado quando uma queda é detectada (antes dos bips, dentro da thread interna).
    // Use para parar os motores: `sensor.onImpact = [&]{ drive.stop("impacto"); };`
    std::function<void()> onImpact;

    ImpactSensor(const std::string& chipPath, Config config, bool simulate)
        : cfg_(std::move(config)), simulate_(simulate) {

        cfg_.buzzerFreqHz = std::clamp(cfg_.buzzerFreqHz, 200, 3000);
        sensorConfigured_ = cfg_.sensorPin >= 0;
        buzzerConfigured_ = cfg_.buzzerPin >= 0;

        if (!simulate_ && (sensorConfigured_ || buzzerConfigured_)) {
            chipFd_ = openGpioChip(chipPath);
            try {
                if (sensorConfigured_) {
                    // Sensores SW-420/KY-002 usam pull-up interno; borda de SUBIDA = impacto.
                    sensor_ = std::make_unique<GpioEdgeInput>(chipFd_, static_cast<unsigned>(cfg_.sensorPin), "impact");
                }
                if (buzzerConfigured_) {
                    buzzer_ = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(cfg_.buzzerPin), "buzz");
                    buzzer_->set(false);
                }
            } catch (...) {
                sensor_.reset();
                buzzer_.reset();
                if (chipFd_ >= 0) { close(chipFd_); chipFd_ = -1; }
                throw;
            }
        }

        if (sensorConfigured_) {
            monitor_ = std::thread([this] { monitorLoop(); });
        }
    }

    ~ImpactSensor() {
        running_.store(false);
        if (monitor_.joinable()) monitor_.join();
        if (buzzer_) buzzer_->set(false);
        sensor_.reset();
        buzzer_.reset();
        if (chipFd_ >= 0) { close(chipFd_); chipFd_ = -1; }
    }

    ImpactSensor(const ImpactSensor&) = delete;
    ImpactSensor& operator=(const ImpactSensor&) = delete;

    bool available() const { return sensorConfigured_; }

    // True se houve impacto nos últimos warnWindowMs.
    bool recentImpact() const {
        const long last = lastImpactMs_.load();
        if (last < 0) return false;
        return (nowMs() - last) < cfg_.warnWindowMs;
    }

    // Segundos desde o último impacto (-1 = nunca detectado).
    long secondsSinceImpact() const {
        const long last = lastImpactMs_.load();
        if (last < 0) return -1;
        return (nowMs() - last) / 1000;
    }

    // Número total de impactos detectados nesta sessão.
    int impactCount() const { return impactCount_.load(); }

    std::string status() const {
        if (!sensorConfigured_) return "sensor-de-impacto=nao-configurado";
        std::string out = "sensor-de-impacto=";
        if (simulate_) {
            out += "simulado";
        } else if (recentImpact()) {
            out += "queda-recente (ha " + std::to_string(secondsSinceImpact()) + "s)";
        } else {
            out += "ok";
        }
        out += " | total-quedas=" + std::to_string(impactCount_.load());
        return out;
    }

    // Disparar um alerta manualmente (para testes: `impact test` no terminal).
    void triggerAlert() { alert(); }

private:
    static long nowMs() {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now().time_since_epoch()).count());
    }

    // Bip suave: PWM de software com duty cycle de 30 % na frequência configurada.
    void beepOnce(int durationMs) {
        if (!buzzerConfigured_) return;
        const int periodUs = 1000000 / cfg_.buzzerFreqHz;      // ex. 1111 µs para 900 Hz
        const int onUs     = periodUs * 30 / 100;               // 30 % on  (333 µs)
        const int offUs    = periodUs - onUs;                   // 70 % off (778 µs)
        const auto end = Clock::now() + std::chrono::milliseconds(durationMs);
        if (simulate_) {
            logLine("buzzer simulado", std::to_string(cfg_.buzzerFreqHz) + "Hz por " + std::to_string(durationMs) + "ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
            return;
        }
        while (Clock::now() < end) {
            if (buzzer_) buzzer_->set(true);
            std::this_thread::sleep_for(std::chrono::microseconds(onUs));
            if (buzzer_) buzzer_->set(false);
            std::this_thread::sleep_for(std::chrono::microseconds(offUs));
        }
        if (buzzer_) buzzer_->set(false);
    }

    // Sequência completa: N bips suaves separados por pausas.
    void alert() {
        if (onImpact) onImpact();                   // para os motores antes de bipar
        lastImpactMs_.store(nowMs());
        impactCount_++;
        if (simulate_) logLine("impacto", "queda detectada (simulado)");
        else            logLine("impacto", "queda detectada pelo casco!");
        for (int i = 0; i < cfg_.beepCount; ++i) {
            if (!running_.load()) break;
            beepOnce(cfg_.beepMs);
            if (i < cfg_.beepCount - 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.pauseMs));
        }
    }

    void monitorLoop() {
        while (running_.load()) {
            if (simulate_) {
                // No modo simulado, lê variável de ambiente ILRDOS_SIM_IMPACT.
                const char* env = std::getenv("ILRDOS_SIM_IMPACT");
                if (env && std::string(env) == "1") {
                    setenv("ILRDOS_SIM_IMPACT", "0", 1);
                    const long now = nowMs();
                    const long last = lastImpactMs_.load();
                    if (last < 0 || (now - last) >= cfg_.cooldownMs) {
                        alert();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            if (!sensor_) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }

            // Aguarda borda com timeout de 500 ms para poder checar running_.
            const auto edge = sensor_->wait(500);
            if (!edge) continue;
            if (edge->first != GPIOEVENT_EVENT_RISING_EDGE) continue;

            // Cooldown: ignora impactos em cascata (robô trepidando no chão).
            const long now = nowMs();
            const long last = lastImpactMs_.load();
            if (last >= 0 && (now - last) < cfg_.cooldownMs) continue;

            alert();
        }
    }

    Config cfg_;
    bool simulate_{false};
    bool sensorConfigured_{false};
    bool buzzerConfigured_{false};
    int chipFd_{-1};
    std::unique_ptr<GpioEdgeInput> sensor_;
    std::unique_ptr<GpioOutput> buzzer_;

    std::atomic<long> lastImpactMs_{-1};
    std::atomic<int> impactCount_{0};
    std::atomic<bool> running_{true};
    std::thread monitor_;
};

}  // namespace ilrdos
