// ILrDOS — frota: o robô coordena OUTROS robôs (aspiradores, esfregões, cortadores de grama, robôs de piscina,
// AGVs, esteiras, braços e outras máquinas de casa ou de indústria). O ILrDOS não pilota esses robôs: ele manda
// comandos de alto nível (start, stop, pause, dock) pelo canal que cada um já oferece (HTTP/Home Assistant/Valetudo,
// MQTT, Modbus TCP/RTU ou Bluetooth LE) e coordena quem trabalha onde e quando.
//
// Regras de segurança (as mesmas do resto do ILrDOS, mais as da frota):
//  - STOP vence tudo: `stop` é sempre aceito, de qualquer origem, sem confirmação nem intervalo mínimo.
//    `pause` e `dock` (voltar para a base) também são sempre aceitos: são ações na direção segura.
//  - `start` de máquina pesada (industrial: AGV, braço, esteira, empilhadeira...; ou cortador de grama, com lâmina)
//    e de qualquer robô com `confirm=1` só acontece por voz com "eu confirmo", ou pelo terminal autenticado;
//    nunca por regra automática nem por marca do modelo de linguagem.
//  - `sensitive=1`: só o terminal autenticado inicia o robô.
//  - Zonas: dois robôs da mesma `zone=` não trabalham juntos (aspirador e esfregão na mesma sala, dois AGVs no
//    mesmo corredor). O segundo só inicia quando o primeiro parar ou passar do tempo máximo (`max_run`).
//  - Celulares, roteadores e computadores nunca entram. Endereços só da rede local (http.hpp).
//  - Só existem os robôs declarados em ilrdos-fleet.txt. Nada de URL vinda de fala ou da rede.
//  - O ILrDOS NÃO é circuito de segurança: parada de emergência, cortinas de luz e intertravamentos ficam em
//    hardware. O `stop` daqui é um comando de rede e pode falhar se o robô estiver fora do ar.
#pragma once

#include <condition_variable>
#include <deque>
#include <future>

#include "iot.hpp"

namespace ilrdos {

enum class FleetAction { Start = 0, Stop = 1, Pause = 2, Dock = 3 };

inline const char* fleetActionName(FleetAction action) {
    switch (action) {
        case FleetAction::Start: return "start";
        case FleetAction::Stop: return "stop";
        case FleetAction::Pause: return "pause";
        default: return "dock";
    }
}

inline std::optional<FleetAction> parseFleetAction(const std::string& word) {
    if (word == "start") return FleetAction::Start;
    if (word == "stop") return FleetAction::Stop;
    if (word == "pause") return FleetAction::Pause;
    if (word == "dock") return FleetAction::Dock;
    return std::nullopt;
}

struct FleetRobot {
    enum class Kind { Http, Mqtt, Modbus, Rtu, Ble, Diy };
    std::string name;
    std::vector<std::string> aliases;   // normalizados com foldPt
    Kind kind{Kind::Http};
    std::string devClass, zone;
    bool heavy{false};                  // industrial ou com lâmina: start sempre pede confirmação
    bool confirm{false};
    bool sensitive{false};
    int minIntervalMs{2000};
    int maxRunMin{120};                 // depois disso o estado "trabalhando" deixa de valer (robôs terminam sozinhos)
    // http
    std::array<std::string, 4> url;
    std::string method{"GET"}, body, statusUrl;
    std::vector<std::string> headers;
    // mqtt (padrões compatíveis com Valetudo: START / STOP / PAUSE / HOME)
    std::string host, topic, user, pass;
    int port{1883};
    bool retain{false};
    std::array<std::string, 4> payload{"START", "STOP", "PAUSE", "HOME"};
    // modbus (TCP ou RTU): cada ação aciona uma bobina (pulso de 300 ms, ou nível com pulse=0)
    ModbusLink link;
    std::array<int, 4> coil{-1, -1, -1, -1};
    int statusReg{-1};
    bool pulse{true};
    // bluetooth LE
    std::string mac, handle, addrType{"public"};
    std::array<std::string, 4> hex;
    // diy: robô construído pelo usuário (protocolo de linha por TCP ou serial; ver docs/DIY-ROBOTS.md)
    DiyLink diy;
    // estado conhecido (último comando; robôs não informam sozinhos, salvo status=/status_reg=)
    std::string state{"?"};
    Clock::time_point stateSince{}, lastCmd{};
};

struct FleetGroup {
    std::string name;
    std::vector<std::string> aliases;
    std::vector<std::string> members;
};

struct FleetRule {
    std::string event, filter, target;
    FleetAction action{FleetAction::Stop};
    Clock::time_point last{};
};

struct FleetInfo {
    std::string name;
    std::vector<std::string> aliases;
    std::string state, devClass, zone;
    bool isGroup{false};
    bool confirm{false};
    bool sensitive{false};
};

class FleetHub {
public:
    FleetHub(std::string configPath, bool simulate) : path_(std::move(configPath)), simulate_(simulate) {
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~FleetHub() {
        running_.store(false);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Lê o arquivo de configuração. Linhas inválidas são registradas e ignoradas.
    std::string load() {
        std::ifstream input(path_);
        if (!input) return "Frota: sem arquivo " + path_ + " (nenhum robô).";
        std::string line;
        int lineNumber = 0, bad = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        robots_.clear();
        groups_.clear();
        rules_.clear();
        while (std::getline(input, line)) {
            ++lineNumber;
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::string problem;
            const auto words = splitWords(line);
            if (words[0] == "robot") problem = parseRobot(words);
            else if (words[0] == "group") problem = parseGroup(words);
            else if (words[0] == "rule") problem = parseRule(words);
            else problem = "linha desconhecida";
            if (!problem.empty()) {
                ++bad;
                logLine("frota", "linha " + std::to_string(lineNumber) + " ignorada: " + problem);
            }
        }
        return "Frota: " + std::to_string(robots_.size()) + " robô(s), " + std::to_string(groups_.size()) + " grupo(s), " +
               std::to_string(rules_.size()) + " regra(s)" + (bad ? ", " + std::to_string(bad) + " linha(s) inválida(s)" : "") + ".";
    }

    std::size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return robots_.size();
    }

    // Robôs e grupos, para a conversa e para o terminal.
    std::vector<FleetInfo> infos() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<FleetInfo> out;
        const auto now = Clock::now();
        for (const auto& robot : robots_) {
            out.push_back({robot->name, robot->aliases, stateText(*robot, now), robot->devClass, robot->zone, false, robot->confirm, robot->sensitive});
        }
        for (const auto& group : groups_) {
            FleetInfo info{group.name, group.aliases, "grupo", {}, {}, true, false, false};
            for (const auto& member : group.members) {
                if (const FleetRobot* robot = findRobotConst(member)) {
                    info.confirm = info.confirm || robot->confirm;
                    info.sensitive = info.sensitive || robot->sensitive;
                }
            }
            out.push_back(info);
        }
        return out;
    }

    std::string list() const {
        const auto all = infos();
        if (all.empty()) return "Nenhum robô na frota (edite ilrdos-fleet.txt na partição BOOT).";
        std::string out = "Frota:";
        for (const auto& info : all) {
            out += " " + info.name + "[" + info.state + "]";
            if (!info.zone.empty()) out += "(zona " + info.zone + ")";
            if (info.sensitive) out += "(sensível: só terminal inicia)";
            if (info.confirm) out += "(pede confirmação)";
        }
        return out;
    }

    // Procura um robô ou grupo citado numa fala normalizada (por nome ou apelido). Mais longo vence.
    std::optional<std::string> findByPhrase(const std::string& folded, bool& ambiguous) const {
        ambiguous = false;
        std::size_t bestLength = 0;
        std::string best;
        for (const auto& info : infos()) {
            std::vector<std::string> candidates = info.aliases;
            candidates.push_back(foldPt(prettyName(info.name)));
            for (const auto& candidate : candidates) {
                if (!hasPhrase(folded, candidate)) continue;
                if (candidate.size() > bestLength) { bestLength = candidate.size(); best = info.name; ambiguous = false; }
                else if (candidate.size() == bestLength && best != info.name) ambiguous = true;
            }
        }
        if (best.empty()) return std::nullopt;
        return best;
    }

    // Comanda um robô ou um grupo. action: start | stop | pause | dock.
    std::string command(const std::string& name, const std::string& action, Origin origin) {
        const auto parsed = parseFleetAction(action);
        if (!parsed) return "Ação inválida. Use start, stop, pause ou dock.";
        std::vector<std::string> members;
        bool isGroup = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& group : groups_) if (group.name == name) { members = group.members; isGroup = true; }
        }
        if (!isGroup) return commandOne(name, *parsed, origin, false, false);
        return commandGroup(name, members, *parsed, origin);
    }

    // PARE TUDO: manda stop a todos os robôs ao mesmo tempo (um robô fora do ar não atrasa os outros).
    std::string stopAll(Origin origin) {
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingName_.clear();
            for (const auto& robot : robots_) names.push_back(robot->name);
        }
        if (names.empty()) return "Não há robôs na frota.";
        return summarize("frota", names, stopMany(names, origin), FleetAction::Stop);
    }

    // "eu confirmo": inicia o robô que ficou esperando (até 15 s). Devolve vazio se não havia pedido da frota,
    // para a confirmação seguir para os dispositivos IoT.
    std::string confirmPending() {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingName_.empty()) return {};
            const bool alive = Clock::now() <= pendingUntil_;
            name = pendingName_;
            pendingName_.clear();
            if (!alive) return {};
        }
        return commandOne(name, FleetAction::Start, Origin::Voice, true, false);
    }

    // Estado do robô: o que ele mesmo informa (status=/status_reg=) ou o último comando enviado.
    std::string status(const std::string& name) {
        FleetRobot* robot = nullptr;
        std::string statusUrl, method;
        std::vector<std::string> headers;
        ModbusLink link;
        int reg = -1;
        FleetRobot::Kind kind{};
        DiyLink diyLink;
        std::string last;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& group : groups_) {
                if (group.name != name) continue;
                std::string out = name + ":";
                for (const auto& member : group.members) {
                    if (const FleetRobot* item = findRobotConst(member)) out += " " + item->name + "=" + stateText(*item, Clock::now()) + ";";
                }
                return out;
            }
            robot = findRobot(name);
            if (robot == nullptr) return "Robô desconhecido: " + name + ". Use: fleet list.";
            statusUrl = robot->statusUrl;
            method = "GET";
            headers = robot->headers;
            link = robot->link;
            reg = robot->statusReg;
            kind = robot->kind;
            diyLink = robot->diy;
            last = stateText(*robot, Clock::now());
        }
        const std::string label = prettyName(name);
        if (simulate_) return label + ": " + last + " (simulado).";
        if (!statusUrl.empty()) {
            std::string host, path;
            int port = 0;
            if (parseHttpUrl(statusUrl, host, port, path)) {
                const auto response = httpRequest(method, host, port, path, headers, {}, 3000, 4096);
                if (response.ok()) return label + ": " + cleanText(response.body, 120) + " (informado pelo robô).";
                return label + ": não consegui consultar o robô (" + (response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error) + "). Último comando: " + last + ".";
            }
        }
        if (kind == FleetRobot::Kind::Diy) {
            std::string reply, error;
            if (diyExchange(diyLink, "STATUS", reply, error) && reply.rfind("OK", 0) == 0) {
                const std::string word = reply.size() > 3 ? trim(reply.substr(3)) : "";
                if (!word.empty()) return label + ": " + cleanText(stateName(word) == std::string("sem informação") ? word : stateName(word), 120) + " (informado pelo robô).";
            }
            return label + ": " + last + " (último comando; o robô não informou o estado" + (error.empty() ? "" : ": " + error) + ").";
        }
        if (reg >= 0 && (kind == FleetRobot::Kind::Modbus || kind == FleetRobot::Kind::Rtu)) {
            std::string error;
            const auto raw = modbusReadRegister(link, 3, static_cast<unsigned>(reg), false, error);
            if (raw) return label + ": registrador de estado = " + std::to_string(*raw) + " (informado pelo robô).";
            return label + ": não consegui ler o estado (" + error + "). Último comando: " + last + ".";
        }
        return label + ": " + last + " (último comando; este robô não informa o estado).";
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
                else if (rule.filter.rfind("known:", 0) == 0) match = subject == "known" && foldPt(rule.filter.substr(6)) == foldPt(who);
                if (!match) continue;
            }
            if (now - rule.last < std::chrono::seconds(3)) continue;
            rule.last = now;
            if (queue_.size() < 16) queue_.push_back({rule.target, rule.action});
        }
        cv_.notify_all();
    }

private:
    static std::string prettyName(std::string name) {
        std::replace(name.begin(), name.end(), '_', ' ');
        return name;
    }

    // Texto vindo de um robô é dado: só caracteres imprimíveis e tamanho limitado.
    static std::string cleanText(const std::string& in, std::size_t limit) {
        std::string out;
        for (const unsigned char c : in) {
            if (out.size() >= limit) break;
            out.push_back((c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c));
        }
        return trim(out);
    }

    static bool validName(const std::string& name) {
        if (name.empty() || name.size() > 24) return false;
        return std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
    }

    // Celulares, roteadores/modems e computadores nunca entram (podem ter dados pessoais).
    static std::string blockedReason(const FleetRobot& robot) {
        static const char* terms[] = {"celular", "telefone", "smartphone", "iphone", "android", "tablet", "roteador", "router", "modem",
                                      "access point", "computador", "pc", "notebook", "laptop", "desktop", "servidor", "macbook"};
        std::string text = foldPt(robot.name) + " " + foldPt(robot.devClass);
        for (const auto& alias : robot.aliases) text += " " + alias;
        std::replace(text.begin(), text.end(), '_', ' ');
        for (const char* term : terms) {
            if (hasPhrase(text, term)) {
                return std::string("robô recusado ('") + term + "'): o ILrDOS não controla celulares, roteadores nem computadores (privacidade)";
            }
        }
        return {};
    }

    // Máquinas industriais e cortadores (lâmina) sempre pedem confirmação para iniciar.
    static bool heavyClass(const std::string& devClass) {
        static const char* terms[] = {"industrial", "agv", "amr", "braco", "arm", "esteira", "conveyor", "empilhadeira", "forklift", "cnc",
                                      "maquina", "cortador", "mower", "grama", "podador", "serra", "prensa", "guindaste"};
        std::string text = foldPt(devClass);
        std::replace(text.begin(), text.end(), '_', ' ');
        for (const char* term : terms) if (hasPhrase(text, term)) return true;
        return false;
    }

    static const char* stateName(const std::string& state) {
        if (state == "working") return "trabalhando";
        if (state == "stopped") return "parado";
        if (state == "paused") return "pausado";
        if (state == "docking") return "voltando para a base";
        return "sem informação";
    }

    static bool workingNow(const FleetRobot& robot, Clock::time_point now) {
        return robot.state == "working" && now - robot.stateSince < std::chrono::minutes(robot.maxRunMin);
    }

    static std::string stateText(const FleetRobot& robot, Clock::time_point now) {
        if (robot.state == "working" && !workingNow(robot, now)) return "sem informação";
        return stateName(robot.state);
    }

    FleetRobot* findRobot(const std::string& name) {
        for (auto& robot : robots_) if (robot->name == name) return robot.get();
        return nullptr;
    }
    const FleetRobot* findRobotConst(const std::string& name) const {
        for (const auto& robot : robots_) if (robot->name == name) return robot.get();
        return nullptr;
    }
    bool nameTaken(const std::string& name) const {
        if (findRobotConst(name)) return true;
        for (const auto& group : groups_) if (group.name == name) return true;
        return false;
    }

    static bool hasAction(const FleetRobot& robot, FleetAction action) {
        const auto i = static_cast<std::size_t>(action);
        switch (robot.kind) {
            case FleetRobot::Kind::Http: return !robot.url[i].empty();
            case FleetRobot::Kind::Mqtt: return !robot.payload[i].empty();
            case FleetRobot::Kind::Modbus:
            case FleetRobot::Kind::Rtu: return robot.coil[i] >= 0;
            case FleetRobot::Kind::Ble: return !robot.hex[i].empty();
            case FleetRobot::Kind::Diy: return !robot.payload[i].empty();
        }
        return false;
    }

    static std::string doneText(const std::string& name, FleetAction action) {
        const std::string label = prettyName(name);
        switch (action) {
            case FleetAction::Start: return label + " iniciado.";
            case FleetAction::Stop: return label + " parado.";
            case FleetAction::Pause: return label + " pausado.";
            default: return label + " voltando para a base.";
        }
    }

    static const char* stateAfter(FleetAction action) {
        switch (action) {
            case FleetAction::Start: return "working";
            case FleetAction::Stop: return "stopped";
            case FleetAction::Pause: return "paused";
            default: return "docking";
        }
    }

    std::string commandOne(const std::string& name, FleetAction action, Origin origin, bool confirmed, bool inGroup) {
        FleetRobot* robot = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            robot = findRobot(name);
            if (robot == nullptr) return "Robô desconhecido: " + name + ". Use: fleet list.";
            if (!hasAction(*robot, action)) return prettyName(name) + " não tem a ação '" + fleetActionName(action) + "' configurada.";
            const bool isStart = action == FleetAction::Start;   // stop, pause e dock são sempre permitidos
            if (isStart && robot->sensitive && origin != Origin::Terminal) {
                logLine("frota", "recusado (sensível, origem não é o terminal): " + name);
                return "Esse robô é sensível: só o terminal autenticado pode iniciá-lo.";
            }
            if (isStart && robot->confirm && !confirmed && origin != Origin::Terminal) {
                if (origin != Origin::Voice || inGroup) {
                    logLine("frota", "recusado (máquina pede confirmação falada): " + name);
                    return prettyName(name) + " é uma máquina: só inicia por pedido falado com confirmação, individualmente, ou pelo terminal.";
                }
                pendingName_ = name;
                pendingUntil_ = Clock::now() + std::chrono::seconds(15);
                logLine("frota", "aguardando confirmação: " + name + " -> start");
                return "Isso inicia uma máquina. Para iniciar " + prettyName(name) + ", diga: eu confirmo (em até 15 segundos).";
            }
            const auto now = Clock::now();
            if (isStart && !robot->zone.empty()) {
                for (const auto& other : robots_) {
                    if (other.get() != robot && other->zone == robot->zone && workingNow(*other, now)) {
                        return "Não iniciei " + prettyName(name) + ": " + prettyName(other->name) + " já está trabalhando na mesma zona (" + robot->zone + ").";
                    }
                }
            }
            if (action != FleetAction::Stop && robot->minIntervalMs > 0 && now - robot->lastCmd < std::chrono::milliseconds(robot->minIntervalMs)) {
                return "Aguarde um instante antes de comandar " + prettyName(name) + " de novo (proteção do equipamento).";
            }
            robot->lastCmd = now;
        }
        std::string error;
        const bool ok = execute(*robot, action, error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ok) {
                robot->state = stateAfter(action);
                robot->stateSince = Clock::now();
            }
        }
        logLine("frota", name + " -> " + fleetActionName(action) + (ok ? " ok" : " FALHOU: " + error));
        return ok ? doneText(name, action) : "Falha ao comandar " + prettyName(name) + ": " + error + ".";
    }

    // Em grupo, os comandos de máquina que pedem confirmação são recusados (a confirmação é um robô por vez).
    std::string commandGroup(const std::string& group, const std::vector<std::string>& members, FleetAction action, Origin origin) {
        if (members.empty()) return "O grupo " + prettyName(group) + " está vazio.";
        if (action == FleetAction::Stop) return summarize(group, members, stopMany(members, origin), action);
        std::vector<std::string> results;
        for (const auto& member : members) results.push_back(commandOne(member, action, origin, false, true));
        return summarize(group, members, results, action);
    }

    std::vector<std::string> stopMany(const std::vector<std::string>& names, Origin origin) {
        std::vector<std::future<std::string>> jobs;
        for (const auto& name : names) {
            jobs.push_back(std::async(std::launch::async, [this, name, origin] { return commandOne(name, FleetAction::Stop, origin, false, true); }));
        }
        std::vector<std::string> results;
        for (auto& job : jobs) results.push_back(job.get());
        return results;
    }

    static std::string summarize(const std::string& group, const std::vector<std::string>& members,
                                 const std::vector<std::string>& results, FleetAction action) {
        int okCount = 0;
        std::string problems;
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (results[i] == doneText(members[i], action)) ++okCount;
            else problems += (problems.empty() ? "" : " ") + results[i];
        }
        std::string out = prettyName(group) + ": " + std::to_string(okCount) + " de " + std::to_string(results.size()) + " ";
        switch (action) {
            case FleetAction::Start: out += "iniciados."; break;
            case FleetAction::Stop: out += "parados."; break;
            case FleetAction::Pause: out += "pausados."; break;
            default: out += "voltando para a base."; break;
        }
        return problems.empty() ? out : out + " " + problems;
    }

    std::string parseRobot(const std::vector<std::string>& words) {
        if (words.size() < 3) return "uso: robot <nome> <http|mqtt|modbus|rtu|ble|diy> class=<tipo> chave=valor ...";
        auto robot = std::make_unique<FleetRobot>();
        bool portSet = false, intervalSet = false;
        robot->name = words[1];
        if (!validName(robot->name)) return "nome inválido (use a-z, 0-9 e _; até 24 caracteres)";
        if (nameTaken(robot->name)) return "nome repetido";
        if (words[2] == "http") robot->kind = FleetRobot::Kind::Http;
        else if (words[2] == "mqtt") robot->kind = FleetRobot::Kind::Mqtt;
        else if (words[2] == "modbus") robot->kind = FleetRobot::Kind::Modbus;
        else if (words[2] == "rtu") robot->kind = FleetRobot::Kind::Rtu;
        else if (words[2] == "ble") robot->kind = FleetRobot::Kind::Ble;
        else if (words[2] == "diy") robot->kind = FleetRobot::Kind::Diy;
        else return "tipo desconhecido: " + words[2];
        for (std::size_t i = 3; i < words.size(); ++i) {
            const auto eq = words[i].find('=');
            if (eq == std::string::npos) return "esperava chave=valor em '" + words[i] + "'";
            const std::string key = words[i].substr(0, eq), value = words[i].substr(eq + 1);
            auto number = [&](int& target, const char* label) -> std::string {
                try { target = std::stoi(value); } catch (...) { return std::string(label) + " inválido"; }
                return {};
            };
            std::string problem;
            if (key == "alias") { for (const auto& a : splitChar(value, ',')) robot->aliases.push_back(foldPt(a)); }
            else if (key == "class") robot->devClass = value;
            else if (key == "zone") { robot->zone = foldPt(value); if (robot->zone.empty() || robot->zone.size() > 24) return "zone inválida"; }
            else if (key == "sensitive") robot->sensitive = value == "1" || value == "true";
            else if (key == "confirm") robot->confirm = value == "1" || value == "true";
            else if (key == "interval") { problem = number(robot->minIntervalMs, "interval"); robot->minIntervalMs = std::clamp(robot->minIntervalMs, 0, 60000); intervalSet = true; }
            else if (key == "max_run") { problem = number(robot->maxRunMin, "max_run"); robot->maxRunMin = std::clamp(robot->maxRunMin, 1, 1440); }
            else if (key == "start" || key == "stop" || key == "pause" || key == "dock") robot->url[static_cast<std::size_t>(*parseFleetAction(key))] = value;
            else if (key == "status") robot->statusUrl = value;
            else if (key == "method") robot->method = value == "POST" ? "POST" : "GET";
            else if (key == "body") robot->body = percentDecode(value);
            else if (key == "header") robot->headers.push_back(percentDecode(value));
            else if (key == "host") robot->host = value;
            else if (key == "port") { problem = number(robot->port, "porta"); portSet = true; }
            else if (key == "topic") robot->topic = value;
            else if (key == "user") robot->user = value;
            else if (key == "pass") robot->pass = percentDecode(value);
            else if (key == "retain") robot->retain = value == "1";
            else if (key == "start_payload" || key == "stop_payload" || key == "pause_payload" || key == "dock_payload") {
                robot->payload[static_cast<std::size_t>(*parseFleetAction(key.substr(0, key.find('_'))))] = percentDecode(value);
            }
            else if (key == "unit") problem = number(robot->link.unit, "unit");
            else if (key == "serial") { robot->link.tty = value; robot->diy.tty = value; }
            else if (key == "baud") { problem = number(robot->link.baud, "baud"); robot->diy.baud = robot->link.baud; }
            else if (key == "wait") { problem = number(robot->diy.waitMs, "wait"); robot->diy.waitMs = std::clamp(robot->diy.waitMs, 0, 5000); }
            else if (key == "parity") robot->link.parity = value.empty() ? 'N' : static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
            else if (key == "coil_start" || key == "coil_stop" || key == "coil_pause" || key == "coil_dock") {
                problem = number(robot->coil[static_cast<std::size_t>(*parseFleetAction(key.substr(5)))], "coil");
            }
            else if (key == "status_reg") problem = number(robot->statusReg, "status_reg");
            else if (key == "pulse") robot->pulse = value != "0";
            else if (key == "mac") robot->mac = value;
            else if (key == "handle") robot->handle = value;
            else if (key == "addr_type") robot->addrType = value;
            else if (key == "start_hex" || key == "stop_hex" || key == "pause_hex" || key == "dock_hex") {
                robot->hex[static_cast<std::size_t>(*parseFleetAction(key.substr(0, key.find('_'))))] = value;
            }
            else return "opção desconhecida: " + key;
            if (!problem.empty()) return problem;
        }
        if (const std::string blocked = blockedReason(*robot); !blocked.empty()) return blocked;

        robot->heavy = heavyClass(robot->devClass);
        if (robot->heavy) robot->confirm = true;                      // máquina pesada: start só com "eu confirmo"
        if (robot->heavy && !intervalSet) robot->minIntervalMs = 3000;

        // Todo robô precisa poder ser parado e iniciado; pause e dock são opcionais.
        switch (robot->kind) {
            case FleetRobot::Kind::Http: {
                std::string host, path;
                int port = 0;
                for (const auto& url : robot->url) {
                    if (!url.empty() && !parseHttpUrl(url, host, port, path)) return "start=/stop=/pause=/dock= precisam ser URLs http:// completas";
                }
                if (!robot->statusUrl.empty() && !parseHttpUrl(robot->statusUrl, host, port, path)) return "status= precisa ser uma URL http:// completa";
                break;
            }
            case FleetRobot::Kind::Mqtt:
                if (robot->host.empty() || robot->topic.empty()) return "mqtt precisa de host= e topic=";
                break;
            case FleetRobot::Kind::Modbus:
            case FleetRobot::Kind::Rtu: {
                ModbusLink& link = robot->link;
                link.serial = robot->kind == FleetRobot::Kind::Rtu;
                link.host = robot->host;
                link.port = portSet ? robot->port : 502;
                if (link.unit < 1 || link.unit > 247) return "unit precisa estar entre 1 e 247";
                if (link.serial) {
                    if (!validSerialPath(link.tty)) return "serial= precisa ser /dev/ttyUSB0, /dev/ttyAMA0 ou /dev/serial/...";
                    if (baudConstant(link.baud) == B0) return "baud não suportado";
                    if (link.parity != 'N' && link.parity != 'E' && link.parity != 'O') return "parity precisa ser N, E ou O";
                } else if (link.host.empty()) {
                    return "modbus precisa de host=";
                }
                for (const int coil : robot->coil) if (coil > 65535) return "coil precisa estar entre 0 e 65535";
                if (robot->statusReg > 65535) return "status_reg precisa estar entre 0 e 65535";
                break;
            }
            case FleetRobot::Kind::Diy: {
                DiyLink& d = robot->diy;
                d.host = robot->host;
                d.port = portSet ? robot->port : 7777;
                d.serial = !d.tty.empty();
                if (d.serial) {
                    if (!validSerialPath(d.tty)) return "serial= precisa ser /dev/ttyUSB0, /dev/ttyAMA0 ou /dev/serial/...";
                    if (baudConstant(d.baud) == B0) return "baud não suportado";
                } else if (d.host.empty()) {
                    return "diy precisa de host= (Wi-Fi) ou serial= (USB)";
                }
                if (d.port < 1 || d.port > 65535) return "porta inválida";
                for (const auto& word : robot->payload) if (!word.empty() && !validDiyWord(word)) return "os comandos *_payload do diy usam só letras, números, _ - . :";
                break;
            }
            case FleetRobot::Kind::Ble:
                if (!validMac(robot->mac)) return "ble precisa de mac=AA:BB:CC:DD:EE:FF";
                if (!validBleHandle(robot->handle)) return "ble precisa de handle=0x0012";
                if (robot->addrType != "public" && robot->addrType != "random") return "addr_type precisa ser public ou random";
                for (const auto& hex : robot->hex) if (!hex.empty() && !validHexBytes(hex)) return "os comandos *_hex precisam ser bytes em hexadecimal";
                break;
        }
        if (!hasAction(*robot, FleetAction::Start) || !hasAction(*robot, FleetAction::Stop)) {
            return "todo robô precisa de uma ação start e uma stop (para sempre poder pará-lo)";
        }
        robots_.push_back(std::move(robot));
        return {};
    }

    std::string parseGroup(const std::vector<std::string>& words) {
        // group <nome> robo1,robo2,... [alias=casa,limpeza]
        if (words.size() < 3) return "uso: group <nome> <robo1,robo2,...> [alias=..]";
        FleetGroup group;
        group.name = words[1];
        if (!validName(group.name)) return "nome inválido (use a-z, 0-9 e _; até 24 caracteres)";
        if (nameTaken(group.name)) return "nome repetido";
        group.members = splitChar(words[2], ',');
        if (group.members.empty()) return "grupo sem membros";
        for (const auto& member : group.members) {
            if (findRobotConst(member) == nullptr) return "robô '" + member + "' não foi declarado antes do grupo";
        }
        for (std::size_t i = 3; i < words.size(); ++i) {
            if (words[i].rfind("alias=", 0) != 0) return "opção desconhecida: " + words[i];
            for (const auto& a : splitChar(words[i].substr(6), ',')) group.aliases.push_back(foldPt(a));
        }
        groups_.push_back(std::move(group));
        return {};
    }

    std::string parseRule(const std::vector<std::string>& words) {
        // rule <evento> [filtro] -> <robô|grupo> <start|stop|pause|dock>
        const auto arrow = std::find(words.begin(), words.end(), "->");
        if (words.size() < 5 || arrow == words.end() || arrow - words.begin() < 2 || words.end() - arrow != 3) {
            return "uso: rule <arrive|empty|hot|cool> [any|known|known:Nome|unknown] -> <robô|grupo> <start|stop|pause|dock>";
        }
        FleetRule rule;
        rule.event = words[1];
        rule.filter = (arrow - words.begin() > 2) ? words[2] : "any";
        rule.target = *(arrow + 1);
        const auto action = parseFleetAction(*(arrow + 2));
        if (rule.event != "arrive" && rule.event != "empty" && rule.event != "hot" && rule.event != "cool") return "evento desconhecido";
        if (!action) return "ação inválida";
        rule.action = *action;
        std::vector<std::string> members;
        for (const auto& group : groups_) if (group.name == rule.target) members = group.members;
        if (members.empty()) members.push_back(rule.target);
        for (const auto& member : members) {
            const FleetRobot* robot = findRobotConst(member);
            if (robot == nullptr) return "'" + rule.target + "' não foi declarado antes da regra";
            // Regras automáticas só INICIAM robôs comuns; parar, pausar e voltar para a base valem para todos.
            if (rule.action == FleetAction::Start && (robot->sensitive || robot->confirm)) {
                return "regras automáticas não podem iniciar máquinas (confirm=1, industrial, cortador) nem robôs sensíveis";
            }
            if (!hasAction(*robot, rule.action)) return "'" + member + "' não tem a ação '" + fleetActionName(rule.action) + "'";
        }
        rules_.push_back(rule);
        return {};
    }

    bool execute(FleetRobot& robot, FleetAction action, std::string& error) {
        if (simulate_) { logLine("frota simulada", robot.name + " <- " + fleetActionName(action)); return true; }
        const auto i = static_cast<std::size_t>(action);
        switch (robot.kind) {
            case FleetRobot::Kind::Http: {
                std::string host, path;
                int port = 0;
                if (!parseHttpUrl(robot.url[i], host, port, path)) { error = "URL inválida"; return false; }
                const auto response = httpRequest(robot.method, host, port, path, robot.headers, robot.body, 3000, 64 * 1024);
                if (!response.ok()) { error = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error; return false; }
                return true;
            }
            case FleetRobot::Kind::Mqtt:
                return mqttPublish(robot.host, robot.port, robot.user, robot.pass, robot.topic, robot.payload[i], robot.retain, error);
            case FleetRobot::Kind::Modbus:
            case FleetRobot::Kind::Rtu: {
                const auto coil = static_cast<unsigned>(robot.coil[i]);
                if (!modbusWriteCoil(robot.link, coil, true, error)) return false;
                if (!robot.pulse) {
                    std::string readError;
                    const auto back = modbusReadBit(robot.link, 1, coil, readError);
                    if (back && !*back) { error = "o equipamento não confirmou o comando"; return false; }
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::string releaseError;
                if (!modbusWriteCoil(robot.link, coil, false, releaseError)) { error = "falha ao soltar o pulso: " + releaseError; return false; }
                return true;
            }
            case FleetRobot::Kind::Ble:
                return bleWrite(robot.mac, robot.addrType, robot.handle, robot.hex[i], error);
            case FleetRobot::Kind::Diy: {
                std::string reply;
                if (!diyExchange(robot.diy, robot.payload[i], reply, error)) return false;
                if (reply.rfind("OK", 0) != 0) { error = "o robô recusou: " + (reply.size() > 4 ? reply.substr(4) : reply); return false; }
                return true;
            }
        }
        return false;
    }

    void workerLoop() {
        while (running_.load()) {
            std::pair<std::string, FleetAction> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return !queue_.empty() || !running_.load(); });
                if (queue_.empty()) continue;
                job = queue_.front();
                queue_.pop_front();
            }
            command(job.first, fleetActionName(job.second), Origin::Auto);
        }
    }

    std::string path_;
    bool simulate_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<FleetRobot>> robots_;
    std::vector<FleetGroup> groups_;
    std::vector<FleetRule> rules_;
    std::deque<std::pair<std::string, FleetAction>> queue_;
    std::string pendingName_;
    Clock::time_point pendingUntil_{};
    std::atomic<bool> running_{true};
    std::thread worker_;
};

}  // namespace ilrdos
