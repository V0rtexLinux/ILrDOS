// ILrDOS — banco de rostos conhecidos.
//
// Privacidade (dado biométrico!):
//  - Guarda só vetores numéricos (embeddings de 128 números), nunca fotos.
//  - Só cadastra com consentimento explícito: comando autenticado no terminal, ou voz + confirmação falada.
//  - Nunca cadastra rostos desconhecidos sozinho.
//  - O arquivo fica no próprio cartão SD (permissão 0600); apagar o arquivo apaga tudo.
//  - Identificação serve para cumprimentar e personalizar a conversa. Não é autenticação:
//    dispositivos sensíveis ignoram o reconhecimento facial (uma foto poderia enganar).
#pragma once

#include <cmath>
#include <sys/stat.h>

#include "common.hpp"

namespace ilrdos {

struct FaceMatch {
    std::string name;      // vazio = desconhecido
    double score{0.0};     // similaridade de cosseno com a melhor amostra
};

class FaceStore {
public:
    static constexpr std::size_t kDim = 128;
    static constexpr std::size_t kMaxSamplesPerPerson = 8;
    static constexpr std::size_t kMaxPeople = 64;

    explicit FaceStore(std::string path, double threshold = 0.40)
        : path_(std::move(path)), threshold_(std::clamp(threshold, 0.25, 0.85)) {}

    static bool validName(const std::string& name) {
        if (name.empty() || name.size() > 32) return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            const unsigned char u = static_cast<unsigned char>(c);
            return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
                   c == '_' || c == '-' || u >= 0x80;   // acentos (UTF-8) são permitidos
        });
    }

    static std::string displayName(const std::string& name) {
        std::string out = name;
        std::replace(out.begin(), out.end(), '_', ' ');
        return out;
    }

    static std::vector<float> normalized(std::vector<float> v) {
        double norm = 0.0;
        for (const float x : v) norm += static_cast<double>(x) * x;
        norm = std::sqrt(norm);
        if (norm > 1e-9) for (auto& x : v) x = static_cast<float>(x / norm);
        return v;
    }

    double threshold() const { return threshold_; }

    std::string load() {
        std::lock_guard<std::mutex> lock(mutex_);
        people_.clear();
        std::ifstream input(path_);
        if (!input) return "Rostos: banco vazio (" + path_ + " ainda não existe).";
        std::string line;
        std::getline(input, line);
        if (trim(line) != "ILRDOS-FACES 1") return "Rostos: formato desconhecido em " + path_ + " (ignorado).";
        while (std::getline(input, line)) {
            std::istringstream in(line);
            std::string name;
            in >> name;
            if (!validName(name)) continue;
            std::vector<float> sample;
            float value;
            while (in >> value) sample.push_back(value);
            if (sample.size() != kDim) continue;
            auto& samples = people_[name];
            if (samples.size() < kMaxSamplesPerPerson) samples.push_back(normalized(sample));
        }
        return "Rostos: " + std::to_string(people_.size()) + " pessoa(s) conhecida(s).";
    }

    std::vector<std::string> names() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        for (const auto& [name, samples] : people_) out.push_back(name);
        return out;
    }

    bool has(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return people_.count(name) != 0;
    }

    // Melhor pessoa acima do limiar, desde que a segunda melhor fique claramente atrás.
    FaceMatch match(const std::vector<float>& feature) const {
        if (feature.size() != kDim) return {};
        const auto query = normalized(feature);
        std::lock_guard<std::mutex> lock(mutex_);
        double best = -1.0, second = -1.0;
        std::string bestName;
        for (const auto& [name, samples] : people_) {
            double score = -1.0;
            for (const auto& sample : samples) {
                double dot = 0.0;
                for (std::size_t i = 0; i < kDim; ++i) dot += static_cast<double>(sample[i]) * query[i];
                score = std::max(score, dot);
            }
            if (score > best) { second = best; best = score; bestName = name; }
            else if (score > second) second = score;
        }
        if (bestName.empty() || best < threshold_ || (second > 0 && best - second < 0.05)) return {"", std::max(best, 0.0)};
        return {bestName, best};
    }

    // Acrescenta amostras (consentimento já obtido por quem chama). Devolve texto de erro ou vazio.
    std::string add(const std::string& name, const std::vector<std::vector<float>>& features) {
        if (!validName(name)) return "Nome inválido (use letras, números, _ ou -; até 32 caracteres).";
        std::lock_guard<std::mutex> lock(mutex_);
        if (people_.count(name) == 0 && people_.size() >= kMaxPeople) return "Banco de rostos cheio.";
        auto& samples = people_[name];
        for (const auto& feature : features) {
            if (feature.size() != kDim) continue;
            samples.push_back(normalized(feature));
            if (samples.size() > kMaxSamplesPerPerson) samples.erase(samples.begin());
        }
        return saveLocked();
    }

    // Esquece uma pessoa (ou "all"). Devolve quantas foram removidas, -1 se não encontrou, -2 se falhou ao gravar.
    int forget(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        int removed = 0;
        if (name == "all") { removed = static_cast<int>(people_.size()); people_.clear(); }
        else if (people_.erase(name) > 0) removed = 1;
        else return -1;
        return saveLocked().empty() ? removed : -2;
    }

private:
    std::string saveLocked() {
        WritableScope writable(path_);
        const std::string temp = path_ + ".tmp";
        {
            const mode_t old = umask(077);
            std::ofstream out(temp, std::ios::trunc);
            umask(old);
            if (!out) return "Não consegui gravar " + path_ + ".";
            out << "ILRDOS-FACES 1\n";
            for (const auto& [name, samples] : people_) {
                for (const auto& sample : samples) {
                    out << name;
                    char buffer[32];
                    for (const float x : sample) { std::snprintf(buffer, sizeof(buffer), " %.6g", static_cast<double>(x)); out << buffer; }
                    out << '\n';
                }
            }
            out.flush();
            if (!out) return "Falha ao escrever " + path_ + " (cartão cheio?).";
        }
        if (std::rename(temp.c_str(), path_.c_str()) != 0) return "Não consegui substituir " + path_ + ".";
        return {};
    }

    std::string path_;
    double threshold_;
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<std::vector<float>>> people_;
};

// Vetor determinístico por nome, usado só no modo --simulate (mesmo nome => rostos "parecidos").
inline std::vector<float> simulatedFeature(const std::string& label, unsigned jitterSeed) {
    std::uint32_t seed = 2166136261u;
    for (const unsigned char c : label) { seed ^= c; seed *= 16777619u; }
    std::vector<float> v(FaceStore::kDim);
    auto next = [](std::uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    std::uint32_t jitter = jitterSeed * 2654435761u + 12345u;
    for (auto& x : v) {
        x = static_cast<float>(static_cast<int>(next(seed) % 2001) - 1000) / 1000.0F;
        x += static_cast<float>(static_cast<int>(next(jitter) % 201) - 100) / 4000.0F;   // ruído ±2,5%
    }
    return FaceStore::normalized(v);
}

}  // namespace ilrdos
