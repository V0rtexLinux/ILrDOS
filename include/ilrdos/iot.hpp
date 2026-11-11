// ILrDOS — controle de dispositivos de casa e de indústria: Wi-Fi (HTTP local e MQTT), relés em GPIO,
// Modbus por cabo (TCP e RS-485/RTU, para CLPs, contatores e sensores) e atuadores Bluetooth LE.
//
// Regras de segurança:
//  - NUNCA controla celulares, roteadores/modems nem computadores (privacidade): são recusados na leitura da
//    configuração, pelo nome, apelido ou `class=`.
//  - `confirm=1` (máquinas): por voz só liga/desliga depois de "eu confirmo" (15 s); nunca por regra automática
//    nem por marca do modelo de linguagem. Equipamentos de Modbus/RTU têm intervalo mínimo entre comandos
//    (protege contator/relé) e o estado é conferido de volta no equipamento.
//  - O ILrDOS NÃO é circuito de segurança: parada de emergência, cortinas de luz e intertravamentos ficam em hardware.
//  - Só existem os dispositivos declarados em ilrdos-iot.txt (partição BOOT). Nada de URL vinda de fala/rede.
//  - Endereços precisam ser da rede local (http.hpp).
//  - `sensitive=1` (fechaduras, portões, fogão...) só responde ao terminal autenticado: nunca por voz,
//    pelo modelo de linguagem, por regras automáticas ou por reconhecimento facial.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <iomanip>

#include "fieldbus.hpp"
#include "http.hpp"

namespace ilrdos {

enum class Origin { Terminal, Voice, Auto, Model };   // Model = marca [[iot ...]] do modelo de linguagem

struct IotDevice {
    enum class Kind { Http, Mqtt, Gpio, Modbus, Rtu, Ble, W1 };
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
    std::unique_ptr<GpioInput> input;    // sensor digital em cabo (contato de porta/janela, presença)
    std::string w1Id;                    // sensor de temperatura 1-Wire (DS18B20): 28-xxxxxxxxxxxx
    std::string onText{"ativo"}, offText{"inativo"};
    // modbus (TCP ou RTU por cabo)
    ModbusLink link;
    int coil{-1};                        // saída liga/desliga
    int reg{-1};                         // leitura (sensor)
    std::string regType{"holding"};      // coil | discrete | holding | input
    double scale{1.0}, offset{0.0};
    bool regSigned{false};
    std::string label;                   // unidade mostrada na leitura (C, bar, rpm...)
    // bluetooth LE
    std::string mac, handle, onHex, offHex, addrType{"public"};
    // segurança
    bool readOnly{false};                // sensor
    bool confirm{false};                 // máquina: voz só com "eu confirmo"
    int minIntervalMs{0};
    std::string devClass;
    Clock::time_point lastCmd{};
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
    bool readOnly{false};
    bool confirm{false};
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
            if (words[0] == "device" || words[0] == "sensor") problem = parseDevice(words);
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
        for (const auto& device : devices_) out.push_back({device->name, device->aliases, device->sensitive, device->state, device->readOnly, device->confirm});
        return out;
    }

    std::string list() const {
        const auto all = infos();
        if (all.empty()) return "Nenhum dispositivo IoT configurado (edite ilrdos-iot.txt na partição BOOT).";
        std::string out = "Dispositivos:";
        for (const auto& info : all) {
            out += " " + info.name + "[" + info.state + "]";
            if (info.sensitive) out += "(sensível: só terminal)";
            if (info.readOnly) out += "(sensor)";
            if (info.confirm) out += "(pede confirmação)";
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
        return commandImpl(name, action, origin, false);
    }

    // "eu confirmo": executa o comando de máquina que ficou esperando (até 15 s).
    std::string confirmPending() {
        std::string name, action;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingName_.empty() || Clock::now() > pendingUntil_) {
                pendingName_.clear();
                return "Não há nenhum comando esperando confirmação.";
            }
            name = pendingName_;
            action = pendingAction_;
            pendingName_.clear();
        }
        return commandImpl(name, action, Origin::Voice, true);
    }

    // Lê o estado real (coil) ou o valor de um sensor. Equipamentos sem retorno mostram o último comando.
    std::string read(const std::string& name) {
        IotDevice* device = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& item : devices_) if (item->name == name) device = item.get();
        }
        if (device == nullptr) return "Dispositivo desconhecido: " + name + ". Use: iot list.";
        if (simulate_) return name + ": " + (device->readOnly ? std::string("valor simulado") : device->state) + " (simulado).";
        const bool bus = device->kind == IotDevice::Kind::Modbus || device->kind == IotDevice::Kind::Rtu;
        std::string error;
        if (device->readOnly && device->kind == IotDevice::Kind::Gpio) {
            const auto level = device->input ? device->input->get() : std::nullopt;
            if (!level) return "Falha ao ler " + name + ": entrada GPIO indisponível.";
            return name + ": " + ((*level == device->activeHigh) ? device->onText : device->offText) + ".";
        }
        if (device->readOnly && device->kind == IotDevice::Kind::W1) {
            const auto celsius = readW1(device->w1Id);
            if (!celsius) return "Falha ao ler " + name + ": sensor 1-Wire sem resposta.";
            return name + ": " + formatNumber(*celsius * device->scale + device->offset) + " " + (device->label.empty() ? "C" : device->label) + ".";
        }
        if (bus && device->readOnly) {
            if (device->regType == "coil" || device->regType == "discrete") {
                const auto bit = modbusReadBit(device->link, device->regType == "coil" ? 1 : 2, static_cast<unsigned>(device->reg), error);
                if (!bit) return "Falha ao ler " + name + ": " + error + ".";
                return name + ": " + (*bit ? "ativo" : "inativo") + ".";
            }
            const auto raw = modbusReadRegister(device->link, device->regType == "input" ? 4 : 3, static_cast<unsigned>(device->reg),
                                                device->regSigned, error);
            if (!raw) return "Falha ao ler " + name + ": " + error + ".";
            return name + ": " + formatNumber(*raw * device->scale + device->offset) + (device->label.empty() ? "" : " " + device->label) + ".";
        }
        if (bus && device->coil >= 0) {
            const auto bit = modbusReadBit(device->link, 1, static_cast<unsigned>(device->coil), error);
            if (!bit) return "Falha ao ler " + name + ": " + error + ".";
            std::lock_guard<std::mutex> lock(mutex_);
            device->state = *bit ? "on" : "off";
            return name + ": " + device->state + " (lido no equipamento).";
        }
        return name + ": " + device->state + " (último comando; este equipamento não informa o estado).";
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
    static std::string formatNumber(double value) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << value;
        std::string text = out.str();
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
        return text.empty() ? "0" : text;
    }

    static bool validW1Id(const std::string& id) {
        if (id.size() != 15 || id[2] != '-') return false;
        return std::all_of(id.begin(), id.end(), [](char c) { return c == '-' || std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    }

    // Temperatura de um sensor 1-Wire (DS18B20) em graus Celsius.
    static std::optional<double> readW1(const std::string& id) {
        const std::string root = envOr("ILRDOS_W1_ROOT", "/sys/bus/w1/devices");
        if (const auto raw = readFirstLine(root + "/" + id + "/temperature")) {
            try { return std::stol(*raw) / 1000.0; } catch (...) {}
        }
        std::ifstream slave(root + "/" + id + "/w1_slave");
        std::string line, all;
        while (std::getline(slave, line)) all += line + "\n";
        const auto crc = all.find("YES");
        const auto t = all.find("t=");
        if (crc == std::string::npos || t == std::string::npos) return std::nullopt;
        try { return std::stol(all.substr(t + 2)) / 1000.0; } catch (...) { return std::nullopt; }
    }

    // Celulares, roteadores/modems e computadores nunca entram (podem ter dados pessoais).
    static std::string blockedReason(const IotDevice& device) {
        static const char* terms[] = {"celular", "telefone", "smartphone", "iphone", "android", "tablet", "roteador", "router", "modem",
                                      "access point", "computador", "pc", "notebook", "laptop", "desktop", "servidor", "macbook"};
        std::string text = foldPt(device.name) + " " + foldPt(device.devClass);
        for (const auto& alias : device.aliases) text += " " + alias;
        std::replace(text.begin(), text.end(), '_', ' ');
        for (const char* term : terms) {
            if (hasPhrase(text, term)) {
                return std::string("dispositivo recusado ('") + term + "'): o ILrDOS não controla celulares, roteadores nem computadores (privacidade)";
            }
        }
        return {};
    }

    std::string commandImpl(const std::string& name, const std::string& action, Origin origin, bool confirmed) {
        IotDevice* device = nullptr;
        std::string act = action;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& item : devices_) if (item->name == name) device = item.get();
            if (device == nullptr) return "Dispositivo desconhecido: " + name + ". Use: iot list.";
            if (device->readOnly) return name + " é um sensor (só leitura). Use: iot read " + name + ".";
            if (device->sensitive && origin != Origin::Terminal) {
                logLine("iot", "recusado (sensível, origem não é o terminal): " + name);
                return "Esse dispositivo é sensível: só o terminal autenticado pode controlá-lo.";
            }
            if (act != "on" && act != "off" && act != "toggle") return "Ação inválida. Use on, off ou toggle.";
            if (device->confirm && !confirmed && origin != Origin::Terminal) {
                if (origin != Origin::Voice) {
                    logLine("iot", "recusado (máquina pede confirmação falada): " + name);
                    return "Essa máquina só liga por pedido falado com confirmação ou pelo terminal.";
                }
                pendingName_ = name;
                pendingAction_ = act;
                pendingUntil_ = Clock::now() + std::chrono::seconds(15);
                logLine("iot", "aguardando confirmação: " + name + " -> " + act);
                return "Isso controla uma máquina. Para " + std::string(act == "off" ? "desligar " : act == "on" ? "ligar " : "alternar ") +
                       name + ", diga: eu confirmo (em até 15 segundos).";
            }
            const auto now = Clock::now();
            if (device->minIntervalMs > 0 && now - device->lastCmd < std::chrono::milliseconds(device->minIntervalMs)) {
                return "Aguarde um instante antes de comandar " + name + " de novo (proteção do equipamento).";
            }
            device->lastCmd = now;
        }
        if (act == "toggle") {
            const bool bus = device->kind == IotDevice::Kind::Modbus || device->kind == IotDevice::Kind::Rtu;
            if (bus && !simulate_ && device->coil >= 0) {
                std::string readError;
                if (const auto bit = modbusReadBit(device->link, 1, static_cast<unsigned>(device->coil), readError)) device->state = *bit ? "on" : "off";
            }
            act = (device->state == "on") ? "off" : "on";
        }
        std::string error;
        const bool ok = execute(*device, act == "on", error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok) device->state = act;
        }
        logLine("iot", name + " -> " + act + (ok ? " ok" : " FALHOU: " + error));
        return ok ? name + ": " + act + "." : "Falha ao controlar " + name + ": " + error + ".";
    }

    static bool validName(const std::string& name) {
        if (name.empty() || name.size() > 24) return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        });
    }

    std::string parseDevice(const std::vector<std::string>& words) {
        if (words.size() < 3) return "uso: device <nome> <http|mqtt|gpio|modbus|rtu|ble> chave=valor ...  |  sensor <nome> <modbus|rtu> reg=..";
        auto device = std::make_unique<IotDevice>();
        const bool isSensor = words[0] == "sensor";
        bool portSet = false;
        device->readOnly = isSensor;
        device->name = words[1];
        if (!validName(device->name)) return "nome inválido (use a-z, 0-9 e _; até 24 caracteres)";
        for (const auto& existing : devices_) if (existing->name == device->name) return "nome repetido";
        if (words[2] == "http") device->kind = IotDevice::Kind::Http;
        else if (words[2] == "mqtt") device->kind = IotDevice::Kind::Mqtt;
        else if (words[2] == "gpio") device->kind = IotDevice::Kind::Gpio;
        else if (words[2] == "modbus") device->kind = IotDevice::Kind::Modbus;
        else if (words[2] == "rtu") device->kind = IotDevice::Kind::Rtu;
        else if (words[2] == "ble") device->kind = IotDevice::Kind::Ble;
        else if (words[2] == "w1") device->kind = IotDevice::Kind::W1;
        else return "tipo desconhecido: " + words[2];
        if (isSensor && device->kind != IotDevice::Kind::Modbus && device->kind != IotDevice::Kind::Rtu &&
            device->kind != IotDevice::Kind::Gpio && device->kind != IotDevice::Kind::W1) return "sensor só existe em modbus, rtu, gpio ou w1";
        if (!isSensor && device->kind == IotDevice::Kind::W1) return "w1 (1-Wire) é só para sensores";
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
            else if (key == "port") { try { device->port = std::stoi(value); portSet = true; } catch (...) { return "porta inválida"; } }
            else if (key == "unit") { try { device->link.unit = std::stoi(value); } catch (...) { return "unit inválido"; } }
            else if (key == "coil") { try { device->coil = std::stoi(value); } catch (...) { return "coil inválido"; } }
            else if (key == "reg") { try { device->reg = std::stoi(value); } catch (...) { return "reg inválido"; } }
            else if (key == "type") device->regType = value;
            else if (key == "scale") { try { device->scale = std::stod(value); } catch (...) { return "scale inválido"; } }
            else if (key == "offset") { try { device->offset = std::stod(value); } catch (...) { return "offset inválido"; } }
            else if (key == "signed") device->regSigned = value == "1";
            else if (key == "label") device->label = percentDecode(value);
            else if (key == "serial") device->link.tty = value;
            else if (key == "baud") { try { device->link.baud = std::stoi(value); } catch (...) { return "baud inválido"; } }
            else if (key == "parity") device->link.parity = value.empty() ? 'N' : static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
            else if (key == "mac") device->mac = value;
            else if (key == "handle") device->handle = value;
            else if (key == "on_hex") device->onHex = value;
            else if (key == "off_hex") device->offHex = value;
            else if (key == "addr_type") device->addrType = value;
            else if (key == "confirm") device->confirm = value == "1" || value == "true";
            else if (key == "interval") { try { device->minIntervalMs = std::clamp(std::stoi(value), 0, 60000); } catch (...) { return "interval inválido"; } }
            else if (key == "class") device->devClass = value;
            else if (key == "id") device->w1Id = value;
            else if (key == "on_text") device->onText = percentDecode(value);
            else if (key == "off_text") device->offText = percentDecode(value);
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
        if (const std::string blocked = blockedReason(*device); !blocked.empty()) return blocked;
        if (device->kind == IotDevice::Kind::Modbus || device->kind == IotDevice::Kind::Rtu) {
            ModbusLink& link = device->link;
            link.serial = device->kind == IotDevice::Kind::Rtu;
            link.host = device->host;
            link.port = portSet ? device->port : 502;
            if (link.unit < 1 || link.unit > 247) return "unit precisa estar entre 1 e 247";
            if (link.serial) {
                if (!validSerialPath(link.tty)) return "serial= precisa ser /dev/ttyUSB0, /dev/ttyAMA0 ou /dev/serial/...";
                if (baudConstant(link.baud) == B0) return "baud não suportado";
                if (link.parity != 'N' && link.parity != 'E' && link.parity != 'O') return "parity precisa ser N, E ou O";
            } else if (link.host.empty()) {
                return "modbus precisa de host=";
            }
            if (device->readOnly) {
                if (device->reg < 0 || device->reg > 65535) return "sensor precisa de reg=0..65535";
                if (device->regType != "coil" && device->regType != "discrete" && device->regType != "holding" && device->regType != "input") {
                    return "type precisa ser coil, discrete, holding ou input";
                }
            } else {
                if (device->coil < 0 || device->coil > 65535) return "precisa de coil=0..65535";
                if (device->minIntervalMs == 0) device->minIntervalMs = 1000;   // protege contator/relé
            }
            device->state = "?";
        } else if (device->kind == IotDevice::Kind::Ble) {
            if (!validMac(device->mac)) return "ble precisa de mac=AA:BB:CC:DD:EE:FF";
            if (!validBleHandle(device->handle)) return "ble precisa de handle=0x0012";
            if (!validHexBytes(device->onHex) || !validHexBytes(device->offHex)) return "ble precisa de on_hex= e off_hex= (bytes em hexadecimal)";
            if (device->addrType != "public" && device->addrType != "random") return "addr_type precisa ser public ou random";
        } else if (device->kind == IotDevice::Kind::Http) {
            std::string host, path; int port = 0;
            for (const auto& url : {device->onUrl, device->offUrl}) {
                if (!parseHttpUrl(url, host, port, path)) return "on=/off= precisam ser URLs http:// completas";
            }
        } else if (device->kind == IotDevice::Kind::Mqtt) {
            if (device->host.empty() || device->topic.empty()) return "mqtt precisa de host= e topic=";
        } else if (device->kind == IotDevice::Kind::Gpio) {
            if (device->pin < 0 || device->pin > 27) return "gpio precisa de pin=0..27";
            if (!simulate_) {
                try {
                    if (chipFd_ < 0) chipFd_ = openGpioChip(chipPath_);
                    if (isSensor) {
                        device->input = std::make_unique<GpioInput>(chipFd_, static_cast<unsigned>(device->pin), device->name.c_str());
                    } else {
                        device->line = std::make_unique<GpioOutput>(chipFd_, static_cast<unsigned>(device->pin), device->name.c_str());
                        device->line->set(!device->activeHigh);
                    }
                } catch (const std::exception& error) { return error.what(); }
            }
            device->state = isSensor ? "?" : "off";
        } else if (device->kind == IotDevice::Kind::W1) {
            if (!validW1Id(device->w1Id)) return "w1 precisa de id=28-xxxxxxxxxxxx (veja /sys/bus/w1/devices)";
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
            if (device->sensitive || device->confirm) return "regras automáticas não podem controlar dispositivos sensíveis nem máquinas (confirm=1)";
            if (device->readOnly) return "sensores são só leitura";
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
            case IotDevice::Kind::Modbus:
            case IotDevice::Kind::Rtu: {
                if (!modbusWriteCoil(device.link, static_cast<unsigned>(device.coil), on, error)) return false;
                std::string readError;
                const auto back = modbusReadBit(device.link, 1, static_cast<unsigned>(device.coil), readError);
                if (back && *back != on) { error = "o equipamento não confirmou a mudança"; return false; }
                return true;
            }
            case IotDevice::Kind::W1:
                error = "sensor é só leitura";
                return false;
            case IotDevice::Kind::Ble:
                return bleWrite(device.mac, device.addrType, device.handle, on ? device.onHex : device.offHex, error);
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
    std::string pendingName_, pendingAction_;
    Clock::time_point pendingUntil_{};
    std::atomic<bool> running_{true};
    std::thread worker_;
};

}  // namespace ilrdos
