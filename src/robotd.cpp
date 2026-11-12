// ILrDOS 2.2 — daemon do robô: console autenticado, visão/rostos, voz, movimento, IoT, frota de outros robôs e autonomia.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <deque>
#include <iomanip>
#include <unordered_map>

#include <ilrdos/autonomy.hpp>
#include <ilrdos/conversation.hpp>
#include <ilrdos/fleet.hpp>
#include <ilrdos/impact.hpp>
#include <ilrdos/iot.hpp>
#include <ilrdos/motors.hpp>
#include <ilrdos/structure.hpp>

using namespace ilrdos;

namespace {

constexpr char kName[] = "ILrDOS";
constexpr char kVersion[] = "2.3.0";
constexpr int kDefaultPort = 4317;
constexpr int kMaxLine = 512;
constexpr int kClientTimeoutSeconds = 20;

volatile std::sig_atomic_t g_running = 1;
volatile std::sig_atomic_t g_listenFd = -1;

void handleSignal(int) {
    g_running = 0;
    if (g_listenFd >= 0) {
        close(static_cast<int>(g_listenFd));
        g_listenFd = -1;
    }
}

std::string toHex(const unsigned char* bytes, std::size_t length) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i) {
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

bool constantTimeEqual(const std::string& left, const std::string& right) {
    const std::size_t count = std::max(left.size(), right.size());
    unsigned char difference = static_cast<unsigned char>(left.size() ^ right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
        const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
        difference |= static_cast<unsigned char>(a ^ b);
    }
    return difference == 0;
}

std::string hmacSha256Hex(const std::string& secret, const std::string& message) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(),
             digest.data(), &digestLength) == nullptr) {
        throw std::runtime_error("Falha ao calcular a resposta de autenticação.");
    }
    return toHex(digest.data(), digestLength);
}

std::string randomNonce() {
    std::array<unsigned char, 32> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Não foi possível gerar o desafio seguro.");
        offset += static_cast<std::size_t>(count);
    }
    return toHex(bytes.data(), bytes.size());
}

std::string loadToken(const std::string& path) {
    struct stat details {};
    if (stat(path.c_str(), &details) != 0) {
        throw std::runtime_error("Arquivo de token não encontrado: " + path);
    }
    if ((details.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw std::runtime_error("Permissões inseguras no token. Execute: chmod 600 " + path);
    }

    std::ifstream input(path);
    if (!input) throw std::runtime_error("Não foi possível ler o arquivo de token.");
    std::string token;
    std::getline(input, token);
    token = trim(token);
    if (token.size() < 24 || token.size() > 256) {
        throw std::runtime_error("O token precisa ter entre 24 e 256 caracteres.");
    }
    return token;
}

bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::optional<std::string> readLine(int fd, std::size_t maxLength = kMaxLine) {
    std::string result;
    result.reserve(64);
    char byte = 0;
    while (result.size() < maxLength) {
        const ssize_t count = recv(fd, &byte, 1, 0);
        if (count <= 0) return std::nullopt;
        if (byte == '\n') return result;
        if (byte != '\r') result.push_back(byte);
    }
    return std::nullopt;
}

class FailureTracker {
public:
    bool blocked(const std::string& address) {
        const auto now = std::chrono::steady_clock::now();
        auto& failures = failures_[address];
        while (!failures.empty() && now - failures.front() > std::chrono::minutes(1)) {
            failures.pop_front();
        }
        return failures.size() >= 5;
    }

    void record(const std::string& address) {
        failures_[address].push_back(std::chrono::steady_clock::now());
    }

private:
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> failures_;
};

void setClientTimeout(int clientFd) {
    timeval timeout{};
    timeout.tv_sec = kClientTimeoutSeconds;
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}


struct Services {
    RobotController& robot;
    VisionSystem& vision;
    Autonomy& autonomy;
    DriveSystem& drive;
    ImpactSensor& impact;
    IotHub& iot;
    FleetHub& fleet;
    FaceStore& faces;
    Conversation& conversation;
    RobotStructure& structure;
};

std::string oneLine(std::string text) {
    for (auto& c : text) if (c == '\n' || c == '\r') c = ' ';
    text = trim(text);
    return text + "\n";
}

DriveSystem::Dir parseDir(const std::string& word, bool& ok) {
    ok = true;
    if (word == "forward") return DriveSystem::Dir::Forward;
    if (word == "back") return DriveSystem::Dir::Back;
    if (word == "left") return DriveSystem::Dir::Left;
    if (word == "right") return DriveSystem::Dir::Right;
    ok = false;
    return DriveSystem::Dir::Forward;
}

int toInt(const std::vector<std::string>& words, std::size_t index, int fallback) {
    if (index >= words.size()) return fallback;
    try { return std::stoi(words[index]); } catch (...) { return fallback; }
}

std::string faceCommand(const std::vector<std::string>& w, Services& s) {
    if (w.size() >= 2 && w[1] == "list") {
        const auto names = s.faces.names();
        if (names.empty()) return "Nenhum rosto cadastrado.";
        std::string out = "Rostos cadastrados:";
        for (const auto& n : names) out += " " + n;
        return out;
    }
    if (w.size() >= 2 && w[1] == "who") { s.vision.scan(); return describePeople(s.vision.last()); }
    if (w.size() >= 2 && w[1] == "cancel") { s.vision.cancelEnroll(); return "Cadastro cancelado."; }
    if (w.size() == 3 && w[1] == "enroll") {
        if (!s.autonomy.enabled()) return "Ligue a autonomia (autonomy on): ela faz as varreduras do cadastro.";
        const std::string result = s.vision.startEnroll(w[2]);
        if (result.rfind("Cadastro", 0) == 0) s.conversation.announce("Vou cadastrar o seu rosto. Olhe para a câmera e fique parado.");
        return result;
    }
    if (w.size() == 3 && w[1] == "forget") {
        std::string target = w[2];
        if (target != "all") {
            for (const auto& n : s.faces.names()) if (foldPt(n) == foldPt(target)) target = n;
        }
        const int result = s.faces.forget(target);
        if (result == -1) return "Não conheço esse rosto. Use: face list.";
        if (result == -2) return "Removi da memória, mas falhei ao gravar o arquivo.";
        return target == "all" ? "Todos os rostos foram esquecidos." : "Rosto esquecido: " + target + ".";
    }
    return "Uso: face list | who | enroll <nome> | forget <nome|all> | cancel";
}

std::string commandReply(const std::string& raw, Services& s) {
    const std::string command = trim(raw);
    const auto w = splitWords(command);
    if (command == "help") {
        return "Comandos: help | status | eye idle|listen|think|speak|alert|off | vision status|scan | "
               "face list|who|enroll <nome>|forget <nome|all>|cancel | move forward|back|left|right [ms] [vel] | "
               "turn left|right [graus] | untwist [zero] | stop | iot list|read <nome>|<dispositivo> on|off|toggle | "
               "fleet list|status [nome]|stop all|<robô|grupo> start|stop|pause|dock | say <texto> | hear <texto> | "
               "voice status | structure [map|power|brain|datalink|casing|visor] | impact [test] | autonomy on|off|status|motion off|track|roam | exit. "
               "O terminal controla somente funções autorizadas do robô.";
    }
    if (command == "status") {
        std::ostringstream reply;
        reply << kName << " " << kVersion << " | olho=" << s.robot.state()
              << " | GPIO=" << (s.robot.simulated() ? "simulado" : "ativo")
              << " | autonomia=" << (s.autonomy.enabled() ? "on" : "off")
              << " | " << s.drive.status()
              << " | frota=" << s.fleet.count()
              << " | rostos=" << s.faces.names().size()
              << " | " << s.structure.summary()
              << " | ligado=" << s.robot.uptimeSeconds() << "s";
        return reply.str();
    }
    if (command == "vision status") return s.vision.status();
    if (command == "vision scan") { s.vision.scan(); return s.vision.status(); }
    if (command == "voice status") return s.conversation.status();
    if (!w.empty() && w[0] == "structure") {
        if (w.size() == 1) return s.structure.report();
        if (w.size() == 2 && w[1] == "map") return RobotStructure::map();
        if (w.size() == 2) {
            const std::string part = s.structure.describe(w[1]);
            if (!part.empty()) return part;
        }
        return "Use: structure [map|power|brain|datalink|casing|visor]";
    }
    if (!w.empty() && w[0] == "impact") {
        if (w.size() == 2 && w[1] == "test") {
            s.impact.triggerAlert();
            return "Alerta de impacto disparado (teste).";
        }
        return s.impact.status();
    }
    if (command == "autonomy status") return s.autonomy.status();
    if (command == "autonomy on") { s.autonomy.setEnabled(true); return "Autonomia ligada."; }
    if (command == "autonomy off") { s.autonomy.setEnabled(false); return "Autonomia desligada."; }
    if (w.size() == 3 && w[0] == "autonomy" && w[1] == "motion") {
        if (w[2] == "off") s.autonomy.setMotion(MotionMode::Off);
        else if (w[2] == "track") s.autonomy.setMotion(MotionMode::Track);
        else if (w[2] == "roam") s.autonomy.setMotion(MotionMode::Roam);
        else return "Use: autonomy motion off|track|roam";
        return std::string("Movimento autônomo: ") + w[2] + ".";
    }
    if (!w.empty() && w[0] == "face") return faceCommand(w, s);
    if (command == "stop") { s.drive.stop("terminal"); return "Parado."; }
    if (w.size() >= 2 && w[0] == "move") {
        bool ok = false;
        const auto dir = parseDir(w[1], ok);
        if (!ok) return "Use: move forward|back|left|right [ms] [velocidade%]";
        s.autonomy.noteExternalMotion();
        return s.drive.move(dir, toInt(w, 3, 45), toInt(w, 2, 1000));
    }
    if (!w.empty() && w[0] == "untwist") {
        if (w.size() == 2 && w[1] == "zero") return s.drive.resetTwist();
        s.autonomy.noteExternalMotion();
        return s.drive.untwist();
    }
    if (w.size() >= 2 && w[0] == "turn" && (w[1] == "left" || w[1] == "right")) {
        s.autonomy.noteExternalMotion();
        return s.drive.turnDegrees(w[1] == "left", toInt(w, 2, 90));
    }
    if (!w.empty() && w[0] == "fleet") {
        if (w.size() == 2 && w[1] == "list") return s.fleet.list();
        if (w.size() == 2 && w[1] == "status") return s.fleet.list();
        if (w.size() == 3 && w[1] == "status") return s.fleet.status(w[2]);
        if (w.size() == 3 && w[1] == "stop" && w[2] == "all") return s.fleet.stopAll(Origin::Terminal);
        if (w.size() == 3) return s.fleet.command(w[1], w[2], Origin::Terminal);
        return "Uso: fleet list | status [nome] | stop all | <robô|grupo> start|stop|pause|dock";
    }
    if (command == "iot list") return s.iot.list();
    if (w.size() == 3 && w[0] == "iot" && w[1] == "read") return s.iot.read(w[2]);
    if (w.size() == 3 && w[0] == "iot") return s.iot.command(w[1], w[2], Origin::Terminal);
    if (w.size() >= 2 && w[0] == "say") { s.conversation.announce(command.substr(4).substr(0, 300)); return "Falando."; }
    if (w.size() >= 2 && w[0] == "hear") return "Resposta: " + s.conversation.hear(command.substr(5).substr(0, 300));
    if (command.rfind("eye ", 0) == 0) {
        const std::string state = trim(command.substr(4));
        if (!s.robot.setState(state)) return "Estado inválido. Use: idle, listen, think, speak, alert ou off.";
        return "Olho definido para: " + state;
    }
    if (command == "exit" || command == "quit") return "BYE";
    if (command.empty()) return {};
    return "Comando desconhecido. Digite help.";
}

void serveClient(int clientFd, const std::string& address, const std::string& token, Services& services,
                 FailureTracker& failures) {
    setClientTimeout(clientFd);
    if (failures.blocked(address)) {
        sendAll(clientFd, "ILrDOS/1 DENIED\n");
        return;
    }

    const std::string nonce = randomNonce();
    if (!sendAll(clientFd, "ILrDOS/1 CHALLENGE " + nonce + "\n")) return;
    const auto answer = readLine(clientFd, 128);
    if (!answer || !constantTimeEqual(trim(*answer), hmacSha256Hex(token, nonce))) {
        failures.record(address);
        sendAll(clientFd, "ILrDOS/1 DENIED\n");
        std::cerr << "[segurança] Autenticação recusada para " << address << '\n';
        return;
    }

    if (!sendAll(clientFd, "ILrDOS/1 OK\n"
                           "ILrDOS conectado. Digite help para listar comandos.\n")) return;
    std::cout << "[rede] Cliente autenticado: " << address << '\n';

    while (g_running) {
        const auto line = readLine(clientFd);
        if (!line) break;
        const std::string reply = oneLine(commandReply(*line, services));
        if (reply != "\n" && !sendAll(clientFd, reply)) break;
        if (trim(*line) == "exit" || trim(*line) == "quit") break;
    }
}

struct Options {
    std::string bindAddress{"0.0.0.0"};
    std::string tokenFile{"/etc/ilrdos/token"};
    std::string configFile{"/boot/ilrdos-robot.txt"};
    std::string gpioChip{"/dev/gpiochip0"};
    std::string visionModel{"/usr/share/ilrdos/models/face_detection_yunet_2023mar.onnx"};
    int port{kDefaultPort};
    int cameraIndex{0};
    unsigned int eyePin{18};
    unsigned int alertPin{23};
    bool simulate{false};
    bool noVoice{false};
    std::optional<bool> autonomy;   // padrão: ligada, exceto em --simulate
    int scanInterval{4};
};

int pinSetting(const Settings& cfg, const char* key) { return cfg.integer(key, -1, -1, 27); }

int runServer(const Options& o) {
    Settings cfg;
    cfg.load(o.configFile);
    const std::string token = loadToken(o.tokenFile);

    RobotEye eye(o.gpioChip, o.eyePin, o.alertPin, o.simulate);
    RobotController robot(eye);

    // rostos
    const bool osImage = access("/boot/ilrdos-token", F_OK) == 0;
    const std::string faceDb = cfg.str("FACE_DB", o.simulate ? "/tmp/ilrdos-sim-faces.db"
                                                  : (osImage ? "/boot/ilrdos-faces.db" : "/var/lib/ilrdos/faces.db"));
    FaceStore faces(faceDb, cfg.real("FACE_THRESHOLD", 0.40, 0.25, 0.85));
    std::cout << "[rostos] " << faces.load() << '\n';
    VisionSystem vision(o.visionModel,
                        cfg.str("SFACE_MODEL", "/usr/share/ilrdos/models/face_recognition_sface_2021dec.onnx"),
                        o.cameraIndex, o.simulate, faces);

    // movimento
    DriveSystem::Config dc;
    dc.leftFwd = pinSetting(cfg, "MOTOR_LEFT_FWD");
    dc.leftRev = pinSetting(cfg, "MOTOR_LEFT_REV");
    dc.rightFwd = pinSetting(cfg, "MOTOR_RIGHT_FWD");
    dc.rightRev = pinSetting(cfg, "MOTOR_RIGHT_REV");
    for (const auto& p : splitChar(cfg.str("MOTOR_ENABLE_PINS"), ',')) { try { dc.enablePins.push_back(std::stoi(p)); } catch (...) {} }
    dc.trig = pinSetting(cfg, "SONAR_TRIG");
    dc.echo = pinSetting(cfg, "SONAR_ECHO");
    dc.maxSpeed = cfg.integer("MOTOR_MAX_SPEED", 60, 20, 100);
    dc.sonarStopCm = cfg.integer("SONAR_STOP_CM", 20, 10, 100);
    dc.turnMsPer90 = cfg.integer("TURN_MS_PER_90", 700, 100, 5000);
    // Envelope de movimento da estrutura (cabos, rampa, inversão, impacto) — ver motors.hpp
    dc.maxTwistDeg = cfg.integer("MAX_TWIST_DEG", 270, 45, 720);
    dc.rampMs = cfg.integer("MOTOR_RAMP_MS", 300, 0, 1000);
    dc.reverseSettleMs = cfg.integer("REVERSE_SETTLE_MS", 250, 0, 1000);
    dc.impactLockoutMs = cfg.integer("IMPACT_LOCKOUT_MS", 4000, 0, 30000);
    if (!o.simulate) dc.twistFile = cfg.str("TWIST_FILE", osImage ? "/boot/ilrdos-twist.txt" : "/var/lib/ilrdos/twist.txt");
    if (o.simulate && std::getenv("ILRDOS_SIM_MOTORS") != nullptr) {   // pinos fictícios para testar no PC
        dc.leftFwd = 1; dc.leftRev = 2; dc.rightFwd = 3; dc.rightRev = 4; dc.trig = 5; dc.echo = 6;
    }
    DriveSystem drive(o.gpioChip, dc, o.simulate);

    // Sensor de impacto do casco protetor
    ImpactSensor::Config ic;
    ic.sensorPin   = pinSetting(cfg, "IMPACT_SENSOR_PIN");
    ic.buzzerPin   = pinSetting(cfg, "IMPACT_BUZZER_PIN");
    ic.buzzerFreqHz = cfg.integer("IMPACT_BUZZER_FREQ_HZ", 900, 200, 3000);
    ImpactSensor impact(o.gpioChip, ic, o.simulate);
    // Queda detectada → para os motores imediatamente, antes de bipar.
    impact.onImpact = [&] { drive.stop("impacto"); };

    // IoT
    IotHub iot(cfg.str("IOT_CONFIG", "/boot/ilrdos-iot.txt"), o.gpioChip, o.simulate);
    std::cout << "[iot] " << iot.load() << '\n';

    // Frota: outros robôs (aspiradores, máquinas de casa e de indústria) coordenados pelo ILrDOS
    FleetHub fleet(cfg.str("FLEET_CONFIG", "/boot/ilrdos-fleet.txt"), o.simulate);
    std::cout << "[frota] " << fleet.load() << '\n';

    // voz e conversa
    const std::string pc = cfg.str("PC_IP");
    const std::string ttsUrl = cfg.str("TTS_URL", pc.empty() ? "" : "http://" + pc + ":5050/v1/audio/speech");
    Speaker speaker(cfg.str("AUDIO_OUT", "auto"), ttsUrl, cfg.str("TTS_VOICE", "glados"));
    Listener listener(cfg.str("VOSK_MODEL", "/usr/share/ilrdos/models/vosk-pt"), cfg.str("AUDIO_IN", "auto"));
    LlmClient llm(pc, cfg.str("OLLAMA_MODEL", "qwen2.5:7b-instruct"));

    Autonomy* autonomyPtr = nullptr;   // criada depois da conversa; as ações abaixo só rodam com ela pronta
    RobotStructure* structurePtr = nullptr;
    Actuators act;
    act.move = [&](const std::string& dir, int speed, int ms) {
        bool ok = false;
        const auto d = parseDir(dir, ok);
        if (!ok) return std::string("Direção inválida.");
        if (autonomyPtr) autonomyPtr->noteExternalMotion();
        return drive.move(d, speed, ms);
    };
    act.turn = [&](bool left, int deg) { if (autonomyPtr) autonomyPtr->noteExternalMotion(); return drive.turnDegrees(left, deg); };
    act.stop = [&] { drive.stop("voz"); };
    act.iot = [&](const std::string& d, const std::string& a) { return iot.command(d, a, Origin::Voice); };
    act.iotModel = [&](const std::string& d, const std::string& a) { return iot.command(d, a, Origin::Model); };
    // "eu confirmo": primeiro a frota (se havia um robô esperando), depois os dispositivos IoT.
    act.iotConfirm = [&] {
        const std::string reply = fleet.confirmPending();
        return reply.empty() ? iot.confirmPending() : reply;
    };
    act.fleetInfos = [&] { return fleet.infos(); };
    act.fleet = [&](const std::string& r, const std::string& a) { return fleet.command(r, a, Origin::Voice); };
    act.fleetModel = [&](const std::string& r, const std::string& a) { return fleet.command(r, a, Origin::Model); };
    act.fleetStatus = [&](const std::string& r) { return fleet.status(r); };
    act.fleetStopAll = [&] { return fleet.stopAll(Origin::Voice); };
    act.iotRead = [&](const std::string& d) { return iot.read(d); };
    act.devices = [&] { return iot.infos(); };
    act.look = [&] { return vision.recent(1500); };
    act.peek = [&] { return vision.last(); };
    act.knownNames = [&] { return faces.names(); };
    act.startEnroll = [&](const std::string& n) { return vision.startEnroll(n); };
    act.forget = [&](const std::string& n) { return faces.forget(n); };
    act.selfStatus = [&] {
        if (!autonomyPtr) return std::string("Estou iniciando.");
        return autonomyPtr->selfStatus() + (structurePtr ? " " + structurePtr->speech() : std::string());
    };
    act.selfAwareness = [&] {
        return structurePtr ? structurePtr->selfAwareness() : std::string("Ainda estou me montando.");
    };
    act.distanceCm = [&] { return drive.distanceCm(); };
    act.canMove = [&] { return drive.available(); };
    act.setEye = [&](const std::string& st) { robot.setAutoState(st); };
    act.setSpeaking = [&](bool v) { robot.setSpeaking(v); };
    act.setThinking = [&](bool v) { robot.setThinking(v); };
    act.setListening = [&](bool v) { robot.setListening(v); };

    Conversation::Options co;
    if (const auto words = splitChar(cfg.str("WAKE_WORDS"), ','); !words.empty()) co.wakeWords = words;
    co.sendNamesToLlm = cfg.flag("SEND_NAMES_TO_LLM", true);
    Conversation conversation(speaker, listener, llm, act, co);
    conversation.start(!o.noVoice && !o.simulate);

    vision.setEnrollCallback([&](bool ok, const std::string& name, const std::string& message) {
        conversation.announce(ok ? "Pronto! Guardei o rosto de " + FaceStore::displayName(name) + ". Agora eu te reconheço."
                                 : message);
    });

    AutonomyHooks hooks;
    hooks.say = [&](const std::string& t) { conversation.announce(t); };
    hooks.fireRule = [&](const std::string& e, const std::string& s, const std::string& who) { iot.fire(e, s, who); fleet.fire(e, s, who); };
    hooks.pulseTurn = [&](bool left, int ms, int speed) { drive.pulseTurn(left, ms, speed); };
    hooks.move = [&](const std::string& dir, int speed, int ms) { bool ok = false; const auto d = parseDir(dir, ok); return ok ? drive.move(d, speed, ms) : std::string(); };
    hooks.turn = [&](bool left, int deg) { return drive.turnDegrees(left, deg); };
    hooks.canMove = [&] { return drive.available(); };
    hooks.hasSonar = [&] { return drive.hasSonar(); };
    hooks.distanceCm = [&] { return drive.distanceCm(); };
    hooks.moving = [&] { return drive.moving(); };
    hooks.stop = [&] { drive.stop("autonomia"); };

    MotionMode motion = MotionMode::Off;
    const std::string wanted = foldPt(cfg.str("MOTION_AUTONOMY", "auto"));
    if (wanted == "roam") motion = MotionMode::Roam;
    else if (wanted == "track") motion = MotionMode::Track;
    else if (wanted == "auto") motion = drive.available() ? (drive.hasSonar() ? MotionMode::Roam : MotionMode::Track) : MotionMode::Off;
    if (motion == MotionMode::Roam && !drive.hasSonar()) motion = MotionMode::Track;   // passear exige sonar
    const bool autonomyOn = o.autonomy.value_or(!o.simulate);
    Autonomy autonomy(robot, vision, hooks, autonomyOn, o.scanInterval, motion, cfg.flag("TRACK_INVERT", false));
    autonomyPtr = &autonomy;

    // Estrutura física (desenho do projeto): energia, cérebro, cabos de dados, casco e visor.
    RobotStructure::Config sc;
    sc.cameraIndex = o.cameraIndex;
    sc.simulate = o.simulate;
#ifdef ILRDOS_HAVE_OPENCV
    sc.visionCompiled = true;
#endif
    sc.throttledFile = cfg.str("THROTTLED_FILE");
    StructureProbes sp;
    sp.health = [&] { return autonomy.health(); };
    sp.uptimeSeconds = [&] { return robot.uptimeSeconds(); };
    sp.lastFaces = [&] { return vision.lastFaces(); };
    sp.recognitionAvailable = [&] { return vision.recognitionAvailable(); };
    sp.motorsActive = [&] { return drive.available(); };
    if (drive.available()) {
        sp.cableTwistDeg = [&] { return drive.twistDeg(); };
        sp.cableTwistLimitDeg = [&] { return drive.maxTwistDeg(); };
    }
    if (impact.available()) {
        sp.recentImpact       = [&] { return impact.recentImpact(); };
        sp.secondsSinceImpact = [&] { return impact.secondsSinceImpact(); };
        sp.impactCount        = [&] { return impact.impactCount(); };
    }
    RobotStructure structure(sc, sp);
    structurePtr = &structure;
    drive.setGuard([&] { return structure.motionBlockReason(); });   // tensão baixa / calor crítico = não anda
    std::cout << "[estrutura] " << structure.summary() << " | " << RobotStructure::map() << '\n';

    Services services{robot, vision, autonomy, drive, impact, iot, fleet, faces, conversation, structure};
    FailureTracker failures;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(o.port);
    const char* host = o.bindAddress == "0.0.0.0" ? nullptr : o.bindAddress.c_str();
    if (getaddrinfo(host, portText.c_str(), &hints, &result) != 0) throw std::runtime_error("Endereço de rede inválido: " + o.bindAddress);

    int serverFd = -1;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        serverFd = socket(item->ai_family, item->ai_socktype | SOCK_CLOEXEC, item->ai_protocol);
        if (serverFd < 0) continue;
        int reuse = 1;
        setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(serverFd, item->ai_addr, item->ai_addrlen) == 0 && listen(serverFd, 8) == 0) break;
        close(serverFd);
        serverFd = -1;
    }
    freeaddrinfo(result);
    if (serverFd < 0) throw std::runtime_error("Não foi possível abrir a porta " + portText + ".");

    std::cout << kName << " " << kVersion << " pronto em " << o.bindAddress << ":" << o.port
              << " | autonomia " << (autonomyOn ? "ligada" : "desligada") << " (movimento " << motionName(motion) << ")"
              << " | GPIO " << (o.simulate ? "simulado" : o.gpioChip) << " | " << conversation.status()
              << " | token carregado.\n";
    std::cout.flush();
    g_listenFd = serverFd;
    while (g_running) {
        sockaddr_in peer{};
        socklen_t peerLength = sizeof(peer);
        const int clientFd = accept4(serverFd, reinterpret_cast<sockaddr*>(&peer), &peerLength, SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            if (!g_running) break;
            std::cerr << "[rede] Falha ao aceitar conexão.\n";
            continue;
        }
        char addressBuffer[INET_ADDRSTRLEN]{};
        const char* address = inet_ntop(AF_INET, &peer.sin_addr, addressBuffer, sizeof(addressBuffer));
        try {
            serveClient(clientFd, address ? address : "desconhecido", token, services, failures);
        } catch (const std::exception& error) {
            std::cerr << "[rede] Sessão encerrada: " << error.what() << '\n';
        }
        close(clientFd);
    }
    g_listenFd = -1;
    close(serverFd);
    drive.stop("encerrando");
    std::cout << "\n" << kName << " encerrado com segurança.\n";
    return 0;
}

int parseNumber(const char* value, const std::string& name, int low, int high) {
    try {
        const int number = std::stoi(value);
        if (number < low || number > high) throw std::out_of_range("range");
        return number;
    } catch (...) {
        throw std::runtime_error("Valor inválido para " + name + ".");
    }
}

Options parseArguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next = [&](const std::string& option) -> const char* {
            if (++i >= argc) throw std::runtime_error("Falta valor para " + option + ".");
            return argv[i];
        };
        if (argument == "--bind") options.bindAddress = next(argument);
        else if (argument == "--port") options.port = parseNumber(next(argument), argument, 1024, 65535);
        else if (argument == "--token-file") options.tokenFile = next(argument);
        else if (argument == "--config") options.configFile = next(argument);
        else if (argument == "--gpio-chip") options.gpioChip = next(argument);
        else if (argument == "--vision-model") options.visionModel = next(argument);
        else if (argument == "--camera-index") options.cameraIndex = parseNumber(next(argument), argument, 0, 32);
        else if (argument == "--eye-pin") options.eyePin = parseNumber(next(argument), argument, 0, 127);
        else if (argument == "--alert-pin") options.alertPin = parseNumber(next(argument), argument, 0, 127);
        else if (argument == "--simulate") options.simulate = true;
        else if (argument == "--no-voice") options.noVoice = true;
        else if (argument == "--autonomy") options.autonomy = true;
        else if (argument == "--no-autonomy") options.autonomy = false;
        else if (argument == "--scan-interval") options.scanInterval = parseNumber(next(argument), argument, 1, 600);
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Uso: ilrdosd [--bind IP] [--port PORT] [--token-file ARQUIVO] [--config ARQUIVO]\n"
                         "             [--gpio-chip CAMINHO] [--eye-pin BCM] [--alert-pin BCM]\n"
                         "             [--vision-model ONNX] [--camera-index N] [--simulate] [--no-voice]\n"
                         "             [--autonomy|--no-autonomy] [--scan-interval SEGUNDOS]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Opção desconhecida: " + argument);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        return runServer(parseArguments(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "[ILrDOS] ERRO: " << error.what() << '\n';
        return 1;
    }
}
