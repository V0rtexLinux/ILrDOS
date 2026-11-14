// ILrDOS — Câmara Central de IA (estilo Aperture Science): o ILrDOS supervisiona e coordena a "fábrica" inteira,
// como a GLaDOS faz no Portal 1 e 2: painéis que se movem, anel de olhos/câmeras, anúncios (PA), ventilação,
// câmaras de teste (cenas), núcleos de personalidade e bloqueio geral.
//
// Tudo é feito pelos dispositivos declarados em ilrdos-iot.txt e pelos robôs de ilrdos-fleet.txt: o módulo não
// abre nenhuma porta nova para o mundo físico. NÃO existe neurotoxina nem torreta armada: o equivalente seguro da
// "toxina" é a ventilação/evacuação de fumaça, e o equivalente das "torretas" é o anel de olhos (luz + câmera).
// Dispositivos sensitive=1 continuam só no terminal autenticado, como no resto do ILrDOS.
// Só funciona com o perfil de corpo industrial (BODY_PROFILE=industrial, ver examples/diy-robot/body/body-industrial.json);
// em qualquer outro perfil todos os comandos facility respondem que o recurso está indisponível.
#pragma once

#include <functional>
#include <mutex>
#include <sstream>

#include "common.hpp"
#include "fleet.hpp"
#include "iot.hpp"

namespace ilrdos {

class FacilityCore {
public:
    struct Scene {
        std::string name;
        bool sensitive{false};                 // sensitive=1: só o terminal autenticado dispara
        std::vector<std::string> actions;      // "iot <dev> <on|off>", "fleet <robô> <ação>", "ring <estado>", "say <texto>"
    };

    // Callbacks fornecidos pelo daemon.
    std::function<void(const std::string&)> say;          // fala (TTS)
    std::function<void(const std::string&)> setEye;       // idle|alert|... no olho do robô

    FacilityCore(IotHub& iot, FleetHub& fleet, std::string configPath)
        : iot_(iot), fleet_(fleet), path_(std::move(configPath)) {}

    // Formato (uma linha por item, # comenta):
    //   zone   <nome>                      câmara/setor da fábrica (só para o mapa e o status)
    //   ring   <dispositivo>               unidade do anel de olhos ciano (ligar = olhos acesos)
    //   panel  <dispositivo>               painel/placa da parede que abre e fecha (on = aberto)
    //   lights <dispositivo>               iluminação geral
    //   vent   <dispositivo>               ventilação/exaustão (evacuação de fumaça)
    //   scene  <nome> [sensitive=1] : ação ; ação ; ...     (câmara de teste = cena)
    // Liga o módulo só para o perfil industrial. Fora dele nada é carregado nem executado.
    void setProfile(const std::string& profile) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = (foldPt(profile) == "industrial");
    }
    bool enabled() const { std::lock_guard<std::mutex> lock(mutex_); return enabled_; }

    std::string load() {
        if (!enabled()) return "desativada (disponível só no perfil industrial: BODY_PROFILE=industrial)";
        std::lock_guard<std::mutex> lock(mutex_);
        zones_.clear(); ring_.clear(); panels_.clear(); lights_.clear(); vents_.clear(); scenes_.clear();
        std::ifstream in(path_);
        if (!in) return "sem " + path_ + " (câmara central em modo vazio)";
        std::string line;
        int count = 0;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const auto colon = line.find(':');
            if (line.rfind("scene ", 0) == 0 && colon != std::string::npos) {
                const auto head = splitWords(line.substr(6, colon - 6));
                if (head.empty()) continue;
                Scene scene;
                scene.name = foldPt(head[0]);
                for (std::size_t i = 1; i < head.size(); ++i) if (head[i] == "sensitive=1") scene.sensitive = true;
                std::stringstream body(line.substr(colon + 1));
                std::string action;
                while (std::getline(body, action, ';')) {
                    action = trim(action);
                    if (!action.empty()) scene.actions.push_back(action);
                }
                scenes_.push_back(scene);
                ++count;
                continue;
            }
            const auto w = splitWords(line);
            if (w.size() < 2) continue;
            if (w[0] == "zone") zones_.push_back(w[1]);
            else if (w[0] == "ring") ring_.push_back(w[1]);
            else if (w[0] == "panel") panels_.push_back(w[1]);
            else if (w[0] == "lights") lights_.push_back(w[1]);
            else if (w[0] == "vent") vents_.push_back(w[1]);
            else continue;
            ++count;
        }
        return std::to_string(count) + " itens (anel=" + std::to_string(ring_.size()) +
               ", paineis=" + std::to_string(panels_.size()) + ", cenas=" + std::to_string(scenes_.size()) + ")";
    }

    std::string status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream out;
        out << "câmara central: núcleo=" << coreName() << " | bloqueio=" << (lockdown_ ? "SIM" : "não")
            << " | anel=" << ring_.size() << " | paineis=" << panels_.size() << " | cenas=" << scenes_.size()
            << " | setores=" << zones_.size();
        if (!lastAction_.empty()) out << " | última ação: " << lastAction_;
        return out.str();
    }

    std::string scenes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scenes_.empty()) return "Nenhuma câmara de teste (cena) configurada.";
        std::string out = "Câmaras de teste:";
        for (const auto& s : scenes_) out += " " + s.name + (s.sensitive ? "(terminal)" : "");
        return out;
    }

    // Ponto único de entrada: terminal, voz e modelo de linguagem passam por aqui.
    std::string command(const std::vector<std::string>& w, Origin origin) {
        if (!enabled()) return "A câmara central só funciona no perfil industrial (BODY_PROFILE=industrial).";
        if (w.size() < 2) return status();
        const std::string& verb = w[1];
        if (verb == "status") return status();
        if (verb == "scenes") return scenes();
        if (verb == "zones") return zonesText();
        if (verb == "core") return coreCommand(w, origin);

        if (verb == "release") {
            if (origin != Origin::Terminal) return "Só o terminal autenticado libera o bloqueio.";
            std::lock_guard<std::mutex> lock(mutex_);
            lockdown_ = false;
            lastAction_ = "bloqueio liberado";
            if (setEye) setEye("idle");
            return "Bloqueio liberado. Retomando testes.";
        }
        if (verb == "lockdown") return lockdown(origin);

        // Daqui para baixo: ações que mexem na fábrica. Respeitam bloqueio e o núcleo de moralidade.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lockdown_) return "Instalação em bloqueio. Só 'facility release' pelo terminal.";
            if (morality_ && origin != Origin::Terminal) return "O núcleo de moralidade está ativo: só monitoramento por voz.";
        }

        if (verb == "ring" && w.size() >= 3) {
            const std::string s = foldPt(w[2]);
            if (s != "scan" && s != "idle" && s != "alert" && s != "off") return "Use: facility ring scan|idle|alert|off";
            return ringState(s, origin);
        }
        if (verb == "panels" && w.size() >= 3) {
            if (w[2] != "open" && w[2] != "close") return "Use: facility panels open|close";
            return apply(panelsCopy(), w[2] == "open" ? "on" : "off", origin, "paineis " + w[2]);
        }
        if (verb == "lights" && w.size() >= 3 && (w[2] == "on" || w[2] == "off"))
            return apply(lightsCopy(), w[2], origin, "luzes " + w[2]);
        if (verb == "vent" && w.size() >= 3 && (w[2] == "on" || w[2] == "off")) {
            const std::string r = apply(ventsCopy(), w[2], origin, "ventilação " + w[2]);
            if (w[2] == "on") speak("Ventilação ativada. Por favor, deixe a área com calma.");
            return r;
        }
        if (verb == "announce" && w.size() >= 3) {
            std::string text;
            for (std::size_t i = 2; i < w.size(); ++i) text += (i > 2 ? " " : "") + w[i];
            speak(text.substr(0, 300));
            return "Anúncio feito em toda a instalação.";
        }
        if (verb == "scene" && w.size() >= 3) return runScene(foldPt(w[2]), origin);
        return "Uso: facility status|zones|scenes|ring scan|idle|alert|off|panels open|close|lights on|off|"
               "vent on|off|scene <nome>|announce <texto>|core normal|moralidade|lockdown|release";
    }

private:
    IotHub& iot_;
    FleetHub& fleet_;
    std::string path_;
    mutable std::mutex mutex_;
    std::vector<std::string> zones_, ring_, panels_, lights_, vents_;
    std::vector<Scene> scenes_;
    bool enabled_{false};
    bool lockdown_{false};
    bool morality_{false};
    std::string lastAction_;

    const char* coreName() const { return morality_ ? "moralidade" : "normal"; }
    std::string zonesText() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (zones_.empty()) return "Nenhum setor configurado.";
        std::string out = "Setores:";
        for (const auto& z : zones_) out += " " + z;
        return out;
    }
    std::vector<std::string> panelsCopy() const { std::lock_guard<std::mutex> l(mutex_); return panels_; }
    std::vector<std::string> lightsCopy() const { std::lock_guard<std::mutex> l(mutex_); return lights_; }
    std::vector<std::string> ventsCopy() const { std::lock_guard<std::mutex> l(mutex_); return vents_; }
    void speak(const std::string& text) { if (say) say(text); }

    // Aplica on/off em vários dispositivos pela MESMA API do IoT (mantém confirm=1, sensitive=1 e limites).
    std::string apply(const std::vector<std::string>& devices, const std::string& action, Origin origin, const std::string& label) {
        if (devices.empty()) return "Nenhum dispositivo configurado para isso.";
        std::string out;
        for (const auto& d : devices) out += iot_.command(d, action, origin) + " ";
        std::lock_guard<std::mutex> lock(mutex_);
        lastAction_ = label;
        return trim(out);
    }

    std::string ringState(const std::string& state, Origin origin) {
        std::vector<std::string> ring;
        { std::lock_guard<std::mutex> l(mutex_); ring = ring_; }
        const std::string action = (state == "off") ? "off" : "on";
        std::string out;
        for (const auto& d : ring) out += iot_.command(d, action, origin) + " ";
        if (setEye) setEye(state == "alert" ? "alert" : "idle");
        std::lock_guard<std::mutex> lock(mutex_);
        lastAction_ = "anel " + state;
        return "Anel de olhos: " + state + ". " + trim(out);
    }

    // Bloqueio: para toda a frota, apaga painéis móveis e alerta. Qualquer origem pode acionar (é a ação segura).
    std::string lockdown(Origin origin) {
        const std::string stopped = fleet_.stopAll(origin);
        for (const auto& d : panelsCopy()) iot_.command(d, "off", origin);
        if (setEye) setEye("alert");
        speak("Bloqueio da instalação ativado. Todos os equipamentos foram parados.");
        std::lock_guard<std::mutex> lock(mutex_);
        lockdown_ = true;
        lastAction_ = "bloqueio";
        return "Bloqueio ativado. " + stopped;
    }

    std::string runScene(const std::string& name, Origin origin) {
        Scene scene;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& s : scenes_) if (s.name == name) { scene = s; found = true; break; }
        }
        if (!found) return "Não conheço essa câmara de teste. " + scenes();
        if (scene.sensitive && origin != Origin::Terminal) return "Essa câmara só pode ser iniciada pelo terminal autenticado.";
        std::string out = "Câmara " + scene.name + ":";
        for (const auto& action : scene.actions) {
            const auto a = splitWords(action);
            if (a.size() >= 3 && a[0] == "iot") out += " " + iot_.command(a[1], a[2], origin);
            else if (a.size() >= 3 && a[0] == "fleet") out += " " + fleet_.command(a[1], a[2], origin);
            else if (a.size() >= 2 && a[0] == "ring") out += " " + ringState(foldPt(a[1]), origin);
            else if (a.size() >= 2 && a[0] == "say") speak(action.substr(4).substr(0, 300));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        lastAction_ = "câmara " + scene.name;
        return out;
    }

    // Núcleos de personalidade: "moralidade" tira o controle por voz/IA (só monitora), como no Portal 1.
    std::string coreCommand(const std::vector<std::string>& w, Origin origin) {
        if (w.size() < 3) return std::string("Núcleo atual: ") + coreName() + ". Use: facility core normal|moralidade";
        const std::string c = foldPt(w[2]);
        if (c == "moralidade" || c == "morality") {
            std::lock_guard<std::mutex> lock(mutex_);
            morality_ = true;
            return "Núcleo de moralidade instalado. Controle por voz e IA suspenso; só monitoramento.";
        }
        if (c == "normal") {
            if (origin != Origin::Terminal) return "Só o terminal autenticado remove o núcleo de moralidade.";
            std::lock_guard<std::mutex> lock(mutex_);
            morality_ = false;
            return "Núcleo normal. Controle total restaurado.";
        }
        return "Use: facility core normal|moralidade";
    }
};

}  // namespace ilrdos
