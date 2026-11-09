// ILrDOS — controle de dispositivos IoT (HTTP local, MQTT e relés em GPIO).
//
// Regras de segurança:
//  - Só existem os dispositivos declarados em ilrdos-iot.txt (partição BOOT). Nada de URL vinda de fala/rede.
//  - Endereços precisam ser da rede local (http.hpp).
//  - `sensitive=1` (fechaduras, portões, fogão...) só responde ao terminal autenticado: nunca por voz,
//    pelo modelo de linguagem, por regras automáticas ou por reconhecimento facial.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>

#include "http.hpp"

namespace ilrdos {

enum class Origin { Terminal, Voice, Auto };

struct IotDevice {
    enum class Kind { Http, Mqtt, Gpio };
    std::string name;
    std::vector<std::string> aliases;   // normalizados com foldPt
    Kind kind{Kind::Http};
    // http
    std::string onUrl, offUrl, method{"GET"}, onBody, offBody;
    std::vector<std::string> headers;
    // mqtt
    std::string host, topic, onPayload{"ON"}, offPayload{"OFF"}, user, pass;
    int port{1883};
    bool retain{false};
    // gpio
    int pin{-1};
    bool activeHigh{true};
    bool sensitive{false};
    std::string state{"?"};
    std::unique_ptr<GpioOutput> line;
};

struct IotRule {
    std::string event, filter, device, action;
    Clock::time_point last{};
};

struct IotInfo {
    std::string name;
    std::vector<std::string> aliases;
    bool sensitive;
    std::string state;
};

inline bool mqttPublish(const std::string& host, int port, const std::string& user, const std::string& pass,
                        const std::string& topic, const std::string& payload, bool retain, std::string& error) {
    auto lengthBytes = [](std::size_t length) {
        std::string out;
        do {
            unsigned char digit = length % 128;
            length /= 128;
            if (length > 0) digit |= 0x80;
            out.push_back(static_cast<char>(digit));
        } while (length > 0);
        return out;
    };
    auto text = [](const std::string& value) {
        std::string out;
        out.push_back(static_cast<char>(value.size() >> 8));
        out.push_back(static_cast<char>(value.size() & 0xFF));
        return out + value;
    };
    const auto address = resolveLan(host, port, error);
    if (!address) return false;
    const int fd = connectWithTimeout(*address, 2000, error);
    if (fd < 0) return false;

    unsigned char flags = 0x02;   // clean session
    if (!user.empty()) flags |= 0x80;
    if (!user.empty() && !pass.empty()) flags |= 0x40;
    std::string connect = text("MQTT");
    connect.push_back(4);
    connect.push_back(static_cast<char>(flags));
    connect.push_back(0);
    connect.push_back(30);
    connect += text("ilrdos-" + std::to_string(::getpid()));
    if (!user.empty()) connect += text(user);
    if (!user.empty() && !pass.empty()) connect += text(pass);
    std::string packet(1, static_cast<char>(0x10));
    packet += lengthBytes(connect.size()) + connect;

    bool ok = sendAllTimeout(fd, packet, 2000);
    if (ok) {
        unsigned char ack[4];
        std::size_t got = 0;
        pollfd p{fd, POLLIN, 0};
        while (got < sizeof(ack) && poll(&p, 1, 2000) > 0) {
            const ssize_t count = recv(fd, ack + got, sizeof(ack) - got, 0);
            if (count <= 0) break;
            got += static_cast<std::size_t>(count);
        }
        ok = got == 4 && ack[0] == 0x20 && ack[3] == 0;
        if (!ok) error = "broker recusou a conexão";
    } else {
        error = "falha ao enviar ao broker";
    }
    if (ok) {
        const std::string body = text(topic) + payload;
        std::string publish(1, static_cast<char>(0x30 | (retain ? 1 : 0)));
        publish += lengthBytes(body.size()) + body;
        publish += std::string("\xE0\x00", 2);
        ok = sendAllTimeout(fd, publish, 2000);
        if (!ok) error = "falha ao publicar";
    }
    close(fd);
    return ok;
}

class IotHub {
public:
    IotHub(std::string configPath, std::string chipPath, bool simulate)
        : path_(std::move(configPath)), chipPath_(std::move(chipPath)), simulate_(simulate) {
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~IotHub() {
        running_.store(false);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        devices_.clear();
        if (chipFd_ >= 0) close(chipFd_);
    }

    // Lê o arquivo de configuração. Linhas inválidas são registradas e ignoradas.
    std::string load() {
        std::ifstream input(path_);
        if (!input) return "IoT: sem arquivo " + path_ + " (nenhum dispositivo).";
        std::string line;
        int lineNumber = 0, bad = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        devices_.clear();
        rules_.clear();
        while (std::getline(input, line)) {
            ++lineNumber;
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::string problem;
            const auto words = splitWords(line);
            if (words[0] == "device") problem = parseDevice(words);
            else if (words[0] == "rule") problem = parseRule(words);
            else problem = "linha desconhecida";
            if (!problem.empty()) {
                ++bad;
                logLine("iot", "linha " + std::to_string(lineNumber) + " ignorada: " + problem);
            }
        }
        return "IoT: " + std::to_string(devices_.size()) + " dispositivo(s), " + std::to_string(rules_.size()) +
               " regra(s)" + (bad ? ", " + std::to_string(bad) + " linha(s) inválida(s)" : "") + ".";
    }

    std::vector<IotInfo> infos() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<IotInfo> out;
        for (const auto& device : devices_) out.push_back({device->name, device->aliases, device->sensitive, device->state});
        return out;
    }

    std::string list() const {
        const auto all = infos();
        if (all.empty()) return "Nenhum dispositivo IoT configurado (edite ilrdos-iot.txt na partição BOOT).";
        std::string out = "Dispositivos:";
        for (const auto& info : all) {
            out += " " + info.name + "[" + info.state + "]";
            if (info.sensitive) out += "(sensível: só terminal)";
        }
        return out;
    }

    // Procura um dispositivo citado numa fala normalizada (por nome ou apelido). Mais longo vence.
    std::optional<std::string> findByPhrase(const std::string& folded, bool& ambiguous) const {
        ambiguous = false;
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t bestLength = 0;
        std::string best;
        for (const auto& device : devices_) {
            std::vector<std::string> candidates = device->aliases;
            candidates.push_back(foldPt(device->name));
            for (const auto& candidate : candidates) {
                if (!hasPhrase(folded, candidate)) continue;
                if (candidate.size() > bestLength) { bestLength = candidate.size(); best = device->name; ambiguous = false; }
                else if (candidate.size() == bestLength && best != device->name) ambiguous = true;
            }
        }
        if (best.empty()) return std::nullopt;
        return best;
    }

    std::string command(const std::string& name, const std::string& action, Origin origin) {
        IotDevice* device = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& item : devices_) if (item->name == name) device = item.get();
            if (device == nullptr) return "Dispositivo desconhecido: " + name + ". Use: iot list.";
            if (device->sensitive && origin != Origin::Terminal) {
                logLine("iot", "recusado (sensível, origem não é o terminal): " + name);
                return "Esse dispositivo é sensível: só o terminal autenticado pode controlá-lo.";
            }
        }
        std::string act = action;
        if (act == "toggle") act = (device->state == "on") ? "off" : "on";
        if (act != "on" && act != "off") return "Ação inválida. Use on, off ou toggle.";
        std::string error;
        const bool ok = execute(*device, act == "on", error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok) device->state = act;
        }
        logLine("iot", name + " -> " + act + (ok ? " ok" : " FALHOU: " + error));
        return ok ? name + ": " + act + "." : "Falha ao controlar " + name + ": " + error + ".";
    }

    // Dispara regras. subject: "any", "known" (com `who`), "unknown". Executa em segundo plano.
    void fire(const std::string& event, const std::string& subject = "any", const std::string& who = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = Clock::now();
        for (auto& rule : rules_) {
            if (rule.event != event) continue;
            if (event == "arrive") {
                bool match = false;
                if (rule.filter == "any") match = subject == "any";
                else if (rule.filter == "known") match = subject == "known";
                else if (rule.filter == "unknown") match = subject == "unknown";
                else if (rule.filter.rfind("known:", 0) == 0) {
                    match = subject == "known" && foldPt(rule.filter.substr(6)) == foldPt(who);
                }
                if (!match) continue;
            }
            if (now - rule.last < std::chrono::seconds(3)) continue;
            rule.last = now;
            if (queue_.size() < 16) queue_.push_back({rule.device, rule.action});
        }
        cv_.notify_all();
    }

private:
    static bool validName(const std::string& name) {
        if (name.empty() || name.size() > 24) return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    }

    std::string parseDevice(const std::vector<std::string>& words) {
        if (words.size() < 3) return "uso: device <nome> <http|mqtt|gpio> chave=valor ...";
        auto device = std::make_unique<IotDevice>();
        device->name = words[1];
        if (!validName(device->name)) return "nome inválido (use a-z, 0-9 e _; até 24 caracteres)";
        for (const auto& existing : devices_) if (existing->name == device->name) return "nome repetido";
        if (words[2] == "http") device->kind = IotDevice::Kind::Http;
        else if (words[2] == "mqtt") device->kind = IotDevice::Kind::Mqtt;
        else if (words[2] == "gpio") device->kind = IotDevice::Kind::Gpio;
        else return "tipo desconhecido: " + words[2];
        for (std::size_t i = 3; i < words.size(); ++i) {
            const auto eq = words[i].find('=');
            if (eq == std::string::npos) return "esperava chave=valor em '" + words[i] + "'";
            const std::string key = words[i].substr(0, eq), value = words[i].substr(eq + 1);
            if (key == "alias") { for (const auto& a : splitChar(value, ',')) device->aliases.push_back(foldPt(a)); }
            else if (key == "sensitive") device->sensitive = value == "1" || value == "true";
            else if (key == "on") device->onUrl = value;
            else if (key == "off") device->offUrl = value;
            else if (key == "method") device->method = value == "POST" ? "POST" : "GET";
            else if (key == "on_body") device->onBody = percentDecode(value);
            else if (key == "off_body") device->offBody = percentDecode(value);
            else if (key == "header") device->headers.push_back(percentDecode(value));
            else if (key == "host") device->host = value;
            else if (key == "port") { try { device->port = std::stoi(value); } catch (...) { return "porta inválida"; } }
            else if (key == "topic") device->topic = value;
            else if (key == "on_payload") device->onPayload = percentDecode(value);
            else if (key == "off_payload") device->offPayload = percentDecode(value);
            else if (key == "user") device->user = value;
            else if (key == "pass") device->pass = percentDecode(value);
            else if (key == "retain") device->retain = value == "1";
            else if (key == "pin") { try { device->pin = std::stoi(value); } catch (...) { return "pino inválido"; } }
            else if (key == "active") device->activeHigh = value != "low";
            else return "opção desconhecida: " + key;
        }
        if (device->kind == IotDevice::Kind::Http) {
            std::string host, path; int port = 0;
            for (const auto& url : {device->onUrl, device->offUrl}) {
                if (!parseHttpUrl(url, host, port, path)) return "on=/off= precisam ser URLs http:// completas";
            }
        } else if (device->kind == IotDevice::Kind::Mqtt) {
            if (device->host.empty() || device->topic.empty()) return "mqtt precisa de host= e topic=";
        } else {
            if (device->pin < 0 || device->pin > 27) return "gpio precisa de pin=0..27";
            if (!simulate_) {
                try {
                    if (chipFd_ < 0) chipFd_ = openGpioChip(chipPath_);
                    device->line = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(device->pin), device->name.c_str());
                    device->line->set(!device->activeHigh);
                } catch (const std::exception& error) { return error.what(); }
            }
            device->state = "off";
        }
        devices_.push_back(std::move(device));
        return {};
    }

    std::string parseRule(const std::vector<std::string>& words) {
        // rule <evento> [filtro] -> <dispositivo> <on|off|toggle>
        const auto arrow = std::find(words.begin(), words.end(), "->");
        if (words.size() < 5 || arrow == words.end() || arrow - words.begin() < 2 || words.end() - arrow != 3) {
            return "uso: rule <arrive|empty|hot|cool> [any|known|known:Nome|unknown] -> <dispositivo> <on|off|toggle>";
        }
        IotRule rule;
        rule.event = words[1];
        rule.filter = (arrow - words.begin() > 2) ? words[2] : "any";
        rule.device = *(arrow + 1);
        rule.action = *(arrow + 2);
        if (rule.event != "arrive" && rule.event != "empty" && rule.event != "hot" && rule.event != "cool") return "evento desconhecido";
        if (rule.action != "on" && rule.action != "off" && rule.action != "toggle") return "ação inválida";
        for (const auto& device : devices_) {
            if (device->name != rule.device) continue;
            if (device->sensitive) return "regras automáticas não podem controlar dispositivos sensíveis";
            rules_.push_back(rule);
            return {};
        }
        return "dispositivo '" + rule.device + "' não foi declarado antes da regra";
    }

    bool execute(IotDevice& device, bool on, std::string& error) {
        if (simulate_) { logLine("iot simulado", device.name + (on ? " ligado" : " desligado")); return true; }
        switch (device.kind) {
            case IotDevice::Kind::Http: {
                std::string host, path; int port = 0;
                if (!parseHttpUrl(on ? device.onUrl : device.offUrl, host, port, path)) { error = "URL inválida"; return false; }
                const auto response = httpRequest(device.method, host, port, path, device.headers,
                                                  on ? device.onBody : device.offBody, 3000, 64 * 1024);
                if (!response.ok()) { error = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error; return false; }
                return true;
            }
            case IotDevice::Kind::Mqtt:
                return mqttPublish(device.host, device.port, device.user, device.pass, device.topic,
                                   on ? device.onPayload : device.offPayload, device.retain, error);
            case IotDevice::Kind::Gpio:
                if (!device.line) { error = "GPIO indisponível"; return false; }
                device.line->set(on == device.activeHigh);
                return true;
        }
        return false;
    }

    void workerLoop() {
        while (running_.load()) {
            std::pair<std::string, std::string> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return !queue_.empty() || !running_.load(); });
                if (queue_.empty()) continue;
                job = queue_.front();
                queue_.pop_front();
            }
            command(job.first, job.second, Origin::Auto);
        }
    }

    std::string path_, chipPath_;
    bool simulate_{false};
    int chipFd_{-1};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<IotDevice>> devices_;
    std::vector<IotRule> rules_;
    std::deque<std::pair<std::string, std::string>> queue_;
    std::atomic<bool> running_{true};
    std::thread worker_;
};

}  // namespace ilrdos
