// ILrDOS — olho (LED amarelo) e alerta (LED vermelho) com padrões de piscar.
#pragma once

#include <atomic>

#include "common.hpp"

namespace ilrdos {

constexpr std::array<const char*, 6> kEyeStates = {"idle", "listen", "think", "speak", "alert", "off"};

class RobotEye {
public:
    RobotEye(const std::string& chipPath, unsigned int eyePin, unsigned int alertPin, bool simulate)
        : simulate_(simulate) {
        if (simulate_) return;
        chipFd_ = openGpioChip(chipPath);
        try {
            eye_ = std::make_unique<GpioOutput>(chipFd_, eyePin, "eye");
            alert_ = std::make_unique<GpioOutput>(chipFd_, alertPin, "alert");
        } catch (...) {
            close(chipFd_);
            chipFd_ = -1;
            throw;
        }
    }

    ~RobotEye() {
        eye_.reset();
        alert_.reset();
        if (chipFd_ >= 0) close(chipFd_);
    }

    void update(const std::string& state, Clock::time_point started) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        bool yellow = false;
        bool red = false;

        if (state == "listen") yellow = true;
        else if (state == "idle") yellow = (elapsed % 2400) < 1700;
        else if (state == "think") yellow = (elapsed % 300) < 150;
        else if (state == "speak") yellow = (elapsed % 900) < 600;
        else if (state == "alert") red = (elapsed % 300) < 150;

        if (eye_) eye_->set(yellow);
        if (alert_) alert_->set(red);
        if (simulate_) {
            const std::string display = state + (yellow ? " (olho aceso)" : "") + (red ? " (alerta vermelho)" : "");
            if (display != lastSimulation_) {
                std::cout << "[GPIO simulado] " << display << '\n';
                lastSimulation_ = display;
            }
        }
    }

    bool simulated() const { return simulate_; }

private:
    bool simulate_{false};
    int chipFd_{-1};
    std::unique_ptr<GpioOutput> eye_;
    std::unique_ptr<GpioOutput> alert_;
    std::string lastSimulation_;
};

class RobotController {
public:
    explicit RobotController(RobotEye& eye) : eye_(eye), started_(Clock::now()) {
        animation_ = std::thread([this] {
            while (running_.load()) {
                eye_.update(effectiveState(), started_);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            eye_.update("off", started_);
        });
    }

    ~RobotController() {
        running_.store(false);
        if (animation_.joinable()) animation_.join();
    }

    // Estado base (comando manual ou autonomia) sem as sobreposições de fala/pensamento.
    std::string state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    // O que o olho mostra de fato: alerta > falando > pensando > ouvindo > estado base.
    std::string effectiveState() const {
        const std::string base = state();
        if (base == "alert" || base == "off") return base;
        if (speaking_.load()) return "speak";
        if (thinking_.load()) return "think";
        if (listening_.load()) return "listen";
        return base;
    }

    bool setState(const std::string& state) {
        if (std::find_if(kEyeStates.begin(), kEyeStates.end(),
                         [&](const char* valid) { return state == valid; }) == kEyeStates.end()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
        manualUntil_ = Clock::now() + kManualHold;
        return true;
    }

    // Usado pela autonomia: não sobrescreve um comando manual recente.
    void setAutoState(const std::string& state) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Clock::now() < manualUntil_) return;
        state_ = state;
    }

    bool manualHold() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return Clock::now() < manualUntil_;
    }

    void setSpeaking(bool value) { speaking_.store(value); }
    void setThinking(bool value) { thinking_.store(value); }
    void setListening(bool value) { listening_.store(value); }
    bool speaking() const { return speaking_.load(); }

    long uptimeSeconds() const {
        return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_).count();
    }

    bool simulated() const { return eye_.simulated(); }

private:
    RobotEye& eye_;
    Clock::time_point started_;
    mutable std::mutex mutex_;
    std::string state_{"idle"};
    Clock::time_point manualUntil_{};
    static constexpr std::chrono::seconds kManualHold{60};
    std::atomic<bool> running_{true};
    std::atomic<bool> speaking_{false}, thinking_{false}, listening_{false};
    std::thread animation_;
};

}  // namespace ilrdos
