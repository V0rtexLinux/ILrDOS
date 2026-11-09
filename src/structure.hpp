// ILrDOS — estrutura física do robô, conforme o desenho do projeto.
//
//   [cabos de eletricidade principais  − / +] ──► [cérebro / computador principal]
//   [visor / câmera] ══ cabos de transmissão de dados ══► [cérebro / computador principal]
//   [casco protetor] envolve o cérebro (e esquenta junto com ele)
//
// Cada peça do desenho vira uma `Part` com estado (ok / atenção / falha / sem sensor) e um texto curto.
// O OS usa essa lista para `structure` (terminal), `status`, a fala "como você está?" e o banner.
// Só lê sensores (nada aqui mexe em GPIO ou em motores); o que não tem sensor aparece como "sem sensor"
// em vez de fingir que está ok.
#pragma once

#include <filesystem>
#include <functional>
#include <iomanip>

#include "common.hpp"

namespace ilrdos {

enum class PartState { Ok, Warn, Fail, Unknown };

inline const char* partStateName(PartState state) {
    switch (state) {
        case PartState::Ok: return "ok";
        case PartState::Warn: return "atencao";
        case PartState::Fail: return "falha";
        default: return "sem-sensor";
    }
}

// Identificadores fixos das peças do desenho (ordem do desenho: energia → cérebro → dados → casco → visor).
enum class PartId { Power, Brain, DataLink, Casing, Visor };

struct Part {
    PartId id;
    const char* key;      // nome curto usado no terminal: power, brain, datalink, casing, visor
    const char* label;    // nome da peça, como escrito no desenho
    PartState state;
    std::string detail;
};

// Leitura de saúde vinda da autonomia (temperatura, câmera) — o resto a estrutura mede sozinha.
struct HealthSnapshot {
    bool haveTemp{false};
    double tempC{0.0};
    std::string thermal{"normal"};   // normal | quente | critico
    bool cameraFailing{false};
};

struct StructureProbes {
    std::function<HealthSnapshot()> health;
    std::function<long()> uptimeSeconds;
    std::function<int()> lastFaces;             // rostos na última varredura (-1 = câmera sem imagem)
    std::function<bool()> recognitionAvailable;
    std::function<bool()> motorsActive;         // rodas configuradas (para dizer o que o casco protege)
    // Sensor de impacto do casco protetor (opcional; nullptr = sem sensor).
    std::function<bool()> recentImpact;         // queda/batida detectada recentemente
    std::function<long()> secondsSinceImpact;   // segundos desde a última queda (-1 = nunca)
    std::function<int()>  impactCount;          // total de quedas nesta sessão
};

class RobotStructure {
public:
    struct Config {
        int cameraIndex{0};
        bool simulate{false};
        bool visionCompiled{false};
        // Override para testes e para outras placas. Vazio = procura sensores padrão do Raspberry Pi.
        std::string throttledFile;
        std::string hwmonRoot{"/sys/class/hwmon"};
        std::string videoDevRoot{"/dev"};
        std::string loadAvgFile{"/proc/loadavg"};
    };

    RobotStructure(Config config, StructureProbes probes) : cfg_(std::move(config)), probes_(std::move(probes)) {
        if (cfg_.throttledFile.empty()) {
            cfg_.throttledFile = envOr("ILRDOS_THROTTLED_FILE", "/sys/devices/platform/soc/soc:firmware/get_throttled");
        }
    }

    // As cinco peças do desenho, sempre nesta ordem.
    std::vector<Part> parts() const {
        const HealthSnapshot health = probes_.health ? probes_.health() : HealthSnapshot{};
        return {power(), brain(health), dataLink(health), casing(health), visor(health)};
    }

    // Pior estado entre as peças (Unknown não conta como problema).
    PartState overall() const {
        PartState worst = PartState::Ok;
        for (const auto& part : parts()) {
            if (part.state == PartState::Fail) return PartState::Fail;
            if (part.state == PartState::Warn) worst = PartState::Warn;
        }
        return worst;
    }

    // Uma linha (o terminal junta tudo em uma linha de resposta).
    std::string report() const {
        std::string out = "Estrutura ILrDOS";
        for (const auto& part : parts()) {
            out += std::string(" | ") + part.label + " [" + partStateName(part.state) + "]: " + part.detail;
        }
        return out;
    }

    // `structure <peça>`: detalhe de uma peça só. Devolve vazio se a chave não existe.
    std::string describe(const std::string& key) const {
        for (const auto& part : parts()) {
            if (key == part.key) {
                return std::string(part.label) + " [" + partStateName(part.state) + "]: " + part.detail;
            }
        }
        return {};
    }

    // Resumo curto de uma linha para o `status`.
    std::string summary() const {
        const auto list = parts();
        std::string out = "estrutura=";
        out += overallName(list);
        for (const auto& part : list) {
            if (part.state == PartState::Warn || part.state == PartState::Fail) {
                out += std::string(" (") + part.key + ":" + partStateName(part.state) + ")";
            }
        }
        return out;
    }

    // Frase falada (conversa): só cita o que não está bem.
    std::string speech() const {
        std::string problems;
        for (const auto& part : parts()) {
            if (part.state != PartState::Warn && part.state != PartState::Fail) continue;
            problems += (problems.empty() ? "" : "; ") + std::string(spokenName(part.id)) + " " +
                        (part.state == PartState::Fail ? "com falha" : "pede atenção");
        }
        return problems.empty() ? "Minha estrutura está toda ok." : "Na minha estrutura: " + problems + ".";
    }

    // Desenho em texto, para o banner e para `structure map`.
    static const char* map() {
        return "[cabos de energia -/+] -> [cerebro/computador principal] <- [cabos de dados] <- [visor/camera]; "
               "[casco protetor] envolve o cerebro";
    }

private:
    static const char* spokenName(PartId id) {
        switch (id) {
            case PartId::Power: return "os cabos de energia";
            case PartId::Brain: return "o computador principal";
            case PartId::DataLink: return "os cabos de dados da câmera";
            case PartId::Casing: return "o casco";
            default: return "o visor";
        }
    }

    static std::string overallName(const std::vector<Part>& list) {
        PartState worst = PartState::Ok;
        for (const auto& part : list) {
            if (part.state == PartState::Fail) return "falha";
            if (part.state == PartState::Warn) worst = PartState::Warn;
        }
        return worst == PartState::Warn ? "atencao" : "ok";
    }

    // ---------------------------------------------------------------- cabos de eletricidade principais (− e +)
    // Raspberry Pi: o firmware avisa quando a tensão do 5 V cai (cabo fino, fonte fraca, bateria baixa).
    // Duas fontes aceitas: get_throttled (bits 0 e 16) ou o hwmon "rpi_volt" (in0_lcrit_alarm).
    Part power() const {
        Part part{PartId::Power, "power", "cabos de eletricidade principais (-/+)", PartState::Unknown, {}};
        if (cfg_.simulate && !std::filesystem::exists(cfg_.throttledFile)) {
            part.detail = "simulado; sem leitura de tensão";
            return part;
        }
        if (const auto raw = readFirstLine(cfg_.throttledFile)) {
            try {
                const unsigned long bits = std::stoul(*raw, nullptr, 0);
                const bool underNow = (bits & 0x1UL) != 0;
                const bool underPast = (bits & 0x10000UL) != 0;
                const bool throttled = (bits & 0x4UL) != 0;
                if (underNow) {
                    part.state = PartState::Fail;
                    part.detail = "tensão baixa agora; confira cabos +/-, fonte ou bateria";
                } else if (throttled || underPast) {
                    part.state = PartState::Warn;
                    part.detail = "tensão já caiu desde que liguei (CPU limitada); revise a alimentação";
                } else {
                    part.state = PartState::Ok;
                    part.detail = "tensão estável";
                }
                return part;
            } catch (...) {}
        }
        if (const auto alarm = hwmonVoltageAlarm()) {
            part.state = *alarm ? PartState::Fail : PartState::Ok;
            part.detail = *alarm ? "tensão baixa agora; confira cabos +/-, fonte ou bateria" : "tensão estável";
            return part;
        }
        part.detail = "sem sensor de tensão nesta placa";
        return part;
    }

    std::optional<bool> hwmonVoltageAlarm() const {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cfg_.hwmonRoot, ec)) {
            const auto name = readFirstLine((entry.path() / "name").string());
            if (!name || *name != "rpi_volt") continue;
            if (const auto value = readFirstLine((entry.path() / "in0_lcrit_alarm").string())) return *value == "1";
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------- cérebro / computador principal
    Part brain(const HealthSnapshot& health) const {
        Part part{PartId::Brain, "brain", "cerebro/computador principal", PartState::Ok, {}};
        std::ostringstream out;
        if (health.haveTemp) out << "CPU " << std::fixed << std::setprecision(1) << health.tempC << "C";
        else out << "CPU sem leitura de temperatura";
        if (const auto load = loadAverage()) {
            out << ", carga " << std::fixed << std::setprecision(2) << *load;
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            if (*load > 1.5 * cores) part.state = PartState::Warn;
        }
        if (probes_.uptimeSeconds) out << ", no ar " << probes_.uptimeSeconds() << "s";
        if (health.thermal == "critico") part.state = PartState::Fail;
        else if (health.thermal == "quente" && part.state == PartState::Ok) part.state = PartState::Warn;
        if (!health.haveTemp && part.state == PartState::Ok) part.state = PartState::Unknown;
        part.detail = out.str();
        return part;
    }

    std::optional<double> loadAverage() const {
        if (const auto line = readFirstLine(cfg_.loadAvgFile)) {
            try { return std::stod(*line); } catch (...) {}
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------- cabos de transmissão de dados: câmera/visor → computador principal
    Part dataLink(const HealthSnapshot& health) const {
        Part part{PartId::DataLink, "datalink", "cabos de transmissao de dados (visor -> computador)", PartState::Ok, {}};
        const std::string node = cfg_.videoDevRoot + "/video" + std::to_string(cfg_.cameraIndex);
        if (cfg_.simulate) {
            part.detail = "simulado (video" + std::to_string(cfg_.cameraIndex) + ")";
            return part;
        }
        if (!cfg_.visionCompiled) {
            part.state = PartState::Unknown;
            part.detail = "visão não compilada; sem leitura do cabo";
            return part;
        }
        if (access(node.c_str(), F_OK) != 0) {
            part.state = PartState::Fail;
            part.detail = node + " não existe; cabo USB da câmera solto ou câmera desligada";
        } else if (health.cameraFailing) {
            part.state = PartState::Fail;
            part.detail = node + " existe, mas não chega imagem ao computador";
        } else {
            part.detail = node + " transmitindo";
        }
        return part;
    }

    // ---------------------------------------------------------------- casco protetor
    // O casco protege o cérebro fisicamente (queda/batida) e termicamente (temperatura interna).
    // Sensor de impacto: detecta quando o robô cai e emite bip suave de alerta.
    // Sensor térmico: mesma lógica de antes (temperatura do cérebro aquece o casco junto).
    Part casing(const HealthSnapshot& health) const {
        Part part{PartId::Casing, "casing", "casco protetor", PartState::Ok, {}};

        // 1. Prioridade: impacto detectado pelo sensor do casco.
        if (probes_.recentImpact && probes_.recentImpact()) {
            part.state = PartState::Warn;
            const long secs = probes_.secondsSinceImpact ? probes_.secondsSinceImpact() : -1;
            const int count = probes_.impactCount ? probes_.impactCount() : 0;
            part.detail = "queda detectada ha " + std::to_string(secs) + "s (total nesta sessao: " +
                           std::to_string(count) + ")";
            // Temperatura pode agravar a situação.
            if (health.thermal == "critico") {
                part.state = PartState::Fail;
                part.detail += "; interior muito quente";
            }
            return part;
        }

        // 2. Sem impacto recente: avalia temperatura como antes.
        if (!health.haveTemp) {
            part.state = PartState::Unknown;
            part.detail = "sem leitura da temperatura interna";
        } else if (health.thermal == "critico") {
            part.state = PartState::Fail;
            part.detail = "interior muito quente: rodas paradas e visao pausada ate esfriar";
        } else if (health.thermal == "quente") {
            part.state = PartState::Warn;
            part.detail = "interior quente: rodas paradas e visao mais lenta";
        } else {
            part.detail = "interior em temperatura normal";
            // Informa se o sensor de impacto está monitorando o casco.
            if (probes_.recentImpact) part.detail += " (sensor de queda ativo)";
        }
        if (probes_.motorsActive && probes_.motorsActive() && part.state == PartState::Ok) part.detail += " (rodas protegidas)";
        return part;
    }

    // ---------------------------------------------------------------- visor / câmera
    Part visor(const HealthSnapshot& health) const {
        Part part{PartId::Visor, "visor", "visor/camera", PartState::Ok, {}};
        std::ostringstream out;
        if (cfg_.simulate) {
            part.detail = "simulado";
            return part;
        }
        if (!cfg_.visionCompiled) {
            part.state = PartState::Unknown;
            part.detail = "visão não compilada (instale OpenCV 4 e recompile)";
            return part;
        }
        if (health.cameraFailing) {
            part.state = PartState::Fail;
            part.detail = "sem imagem";
            return part;
        }
        const int faces = probes_.lastFaces ? probes_.lastFaces() : 0;
        out << "câmera " << cfg_.cameraIndex << " ok";
        if (faces >= 0) out << ", rostos na última varredura: " << faces;
        out << (probes_.recognitionAvailable && probes_.recognitionAvailable() ? ", reconhecimento ativo" : ", só detecção");
        part.detail = out.str();
        return part;
    }

    Config cfg_;
    StructureProbes probes_;
};

}  // namespace ilrdos
