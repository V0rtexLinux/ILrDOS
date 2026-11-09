// ILrDOS — utilidades compartilhadas (texto, ambiente, GPIO, processos).
#pragma once

#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ilrdos {

using Clock = std::chrono::steady_clock;

inline std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first);
}

inline std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string word;
    while (in >> word) out.push_back(word);
    return out;
}

inline std::vector<std::string> splitChar(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string part;
    std::istringstream in(text);
    while (std::getline(in, part, separator)) {
        part = trim(part);
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

// Minúsculas, sem acentos do português e sem pontuação: "Acenda a LUZ, João!" -> "acenda a luz joao".
// Usado para comparar falas transcritas, apelidos de dispositivos e palavras de ativação.
inline std::string foldPt(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    auto push = [&](char c) {
        if (c == ' ') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        } else {
            out.push_back(c);
        }
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z') push(static_cast<char>(c - 'A' + 'a'));
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) push(static_cast<char>(c));
            else push(' ');
        } else if (c == 0xC3 && i + 1 < in.size()) {
            const unsigned char d = static_cast<unsigned char>(in[++i]);
            char mapped = ' ';
            if ((d >= 0x80 && d <= 0x85) || (d >= 0xA0 && d <= 0xA5)) mapped = 'a';
            else if (d == 0x87 || d == 0xA7) mapped = 'c';
            else if ((d >= 0x88 && d <= 0x8B) || (d >= 0xA8 && d <= 0xAB)) mapped = 'e';
            else if ((d >= 0x8C && d <= 0x8F) || (d >= 0xAC && d <= 0xAF)) mapped = 'i';
            else if (d == 0x91 || d == 0xB1) mapped = 'n';
            else if ((d >= 0x92 && d <= 0x96) || (d >= 0xB2 && d <= 0xB6)) mapped = 'o';
            else if ((d >= 0x99 && d <= 0x9C) || (d >= 0xB9 && d <= 0xBC)) mapped = 'u';
            push(mapped);
        } else {
            push(' ');
        }
    }
    return trim(out);
}

// Procura uma palavra/frase inteira (já normalizada com foldPt) dentro de um texto normalizado.
inline bool hasPhrase(const std::string& folded, const std::string& phrase) {
    if (phrase.empty()) return false;
    std::size_t pos = 0;
    while ((pos = folded.find(phrase, pos)) != std::string::npos) {
        const bool leftOk = pos == 0 || folded[pos - 1] == ' ';
        const std::size_t end = pos + phrase.size();
        const bool rightOk = end == folded.size() || folded[end] == ' ';
        if (leftOk && rightOk) return true;
        ++pos;
    }
    return false;
}

inline bool hasAnyPhrase(const std::string& folded, std::initializer_list<const char*> phrases) {
    for (const char* phrase : phrases) {
        if (hasPhrase(folded, phrase)) return true;
    }
    return false;
}

inline std::string envOr(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : fallback;
}

inline int envSeconds(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    try { return std::clamp(std::stoi(value), 1, 86400); } catch (...) { return fallback; }
}

inline std::optional<std::string> readFirstLine(const std::string& path) {
    std::ifstream input(path);
    std::string line;
    if (!input || !std::getline(input, line)) return std::nullopt;
    return trim(line);
}

inline void logLine(const char* tag, const std::string& message) {
    std::cout << '[' << tag << "] " << message << '\n';
    std::cout.flush();
}

// Capitaliza a primeira letra ASCII ("maria" -> "Maria"); nomes UTF-8 passam como vieram.
inline std::string capitalize(std::string value) {
    if (!value.empty() && value[0] >= 'a' && value[0] <= 'z') value[0] = static_cast<char>(value[0] - 'a' + 'A');
    return value;
}

// ---------------------------------------------------------------- processos

// Executa um programa fixo, sem shell. Argumentos nunca vêm da rede sem validação.
// `input` (opcional) vai para o stdin do programa. Retorna true se saiu com código 0.
inline bool runProgram(const std::vector<std::string>& argv, const std::string& input = {},
                       int timeoutSeconds = 20) {
    if (argv.empty()) return false;
    int inPipe[2] = {-1, -1};
    if (!input.empty() && pipe(inPipe) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) {
        if (inPipe[0] >= 0) { close(inPipe[0]); close(inPipe[1]); }
        return false;
    }
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDWR);
        if (inPipe[0] >= 0) { dup2(inPipe[0], 0); close(inPipe[0]); close(inPipe[1]); }
        else if (devnull >= 0) dup2(devnull, 0);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        std::vector<char*> args;
        for (const auto& item : argv) args.push_back(const_cast<char*>(item.c_str()));
        args.push_back(nullptr);
        execv(args[0], args.data());
        _exit(127);
    }
    if (inPipe[0] >= 0) {
        close(inPipe[0]);
        std::signal(SIGPIPE, SIG_IGN);
        std::size_t written = 0;
        while (written < input.size()) {
            const ssize_t count = write(inPipe[1], input.data() + written, input.size() - written);
            if (count <= 0) break;
            written += static_cast<std::size_t>(count);
        }
        close(inPipe[1]);
    }
    const auto deadline = Clock::now() + std::chrono::seconds(timeoutSeconds);
    int status = 0;
    while (true) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0) return false;
        if (Clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// A partição BOOT fica somente leitura; para gravar (ex.: rostos) remonta rw só durante a escrita.
class WritableScope {
public:
    explicit WritableScope(const std::string& path) {
        if (path.rfind("/boot/", 0) == 0) {
            remounted_ = runProgram({"/bin/mount", "-o", "remount,rw", "/boot"}, {}, 10);
        }
    }
    ~WritableScope() {
        if (remounted_) {
            ::sync();
            runProgram({"/bin/mount", "-o", "remount,ro", "/boot"}, {}, 10);
        }
    }
    WritableScope(const WritableScope&) = delete;
    WritableScope& operator=(const WritableScope&) = delete;

private:
    bool remounted_{false};
};

// ---------------------------------------------------------------- configuração

// Chaves CHAVE=valor (partição BOOT). Ordem de prioridade: variável ILRDOS_<CHAVE> > arquivo > padrão.
class Settings {
public:
    void load(const std::string& path) {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            values_[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    std::string str(const std::string& key, const std::string& fallback = {}) const {
        const std::string envName = "ILRDOS_" + key;
        if (const char* env = std::getenv(envName.c_str()); env != nullptr && *env != '\0') return env;
        const auto it = values_.find(key);
        return (it != values_.end() && !it->second.empty()) ? it->second : fallback;
    }
    int integer(const std::string& key, int fallback, int low, int high) const {
        const std::string text = str(key);
        if (text.empty()) return fallback;
        try { return std::clamp(std::stoi(text), low, high); } catch (...) { return fallback; }
    }
    double real(const std::string& key, double fallback, double low, double high) const {
        const std::string text = str(key);
        if (text.empty()) return fallback;
        try { return std::clamp(std::stod(text), low, high); } catch (...) { return fallback; }
    }
    bool flag(const std::string& key, bool fallback) const {
        const std::string text = foldPt(str(key));
        if (text.empty()) return fallback;
        return text == "1" || text == "sim" || text == "true" || text == "on" || text == "yes";
    }

private:
    std::map<std::string, std::string> values_;
};

// ---------------------------------------------------------------- GPIO (uAPI v1)

inline int openGpioChip(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("Não foi possível abrir " + path +
                                 ". Use --simulate no computador ou conceda acesso ao GPIO no Pi.");
    }
    return fd;
}

class GpioOutput {
public:
    GpioOutput(int chipFd, unsigned int offset, const char* label) : label_(label) {
        gpiohandle_request request{};
        request.lineoffsets[0] = offset;
        request.flags = GPIOHANDLE_REQUEST_OUTPUT;
        request.default_values[0] = 0;
        request.lines = 1;
        std::snprintf(request.consumer_label, sizeof(request.consumer_label), "ilrdos-%s", label);
        if (ioctl(chipFd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
            throw std::runtime_error("Não foi possível reservar GPIO " + std::to_string(offset) +
                                     " (" + label + "). Verifique o chip, os pinos e as permissões.");
        }
        fd_ = request.fd;
    }

    ~GpioOutput() {
        if (fd_ >= 0) {
            set(false);
            close(fd_);
        }
    }
    GpioOutput(const GpioOutput&) = delete;
    GpioOutput& operator=(const GpioOutput&) = delete;

    void set(bool high) const {
        gpiohandle_data values{};
        values.values[0] = high ? 1 : 0;
        if (ioctl(fd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &values) < 0) {
            std::cerr << "[ILrDOS] Aviso: falha ao atualizar " << label_ << " GPIO.\n";
        }
    }

private:
    int fd_{-1};
    std::string label_;
};

// Entrada com eventos de borda e timestamp do kernel (usada pelo sonar).
class GpioEdgeInput {
public:
    GpioEdgeInput(int chipFd, unsigned int offset, const char* label) {
        gpioevent_request request{};
        request.lineoffset = offset;
        request.handleflags = GPIOHANDLE_REQUEST_INPUT;
        request.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES;
        std::snprintf(request.consumer_label, sizeof(request.consumer_label), "ilrdos-%s", label);
        if (ioctl(chipFd, GPIO_GET_LINEEVENT_IOCTL, &request) < 0) {
            throw std::runtime_error("Não foi possível reservar GPIO " + std::to_string(offset) +
                                     " (" + label + ") como entrada.");
        }
        fd_ = request.fd;
    }
    ~GpioEdgeInput() { if (fd_ >= 0) close(fd_); }
    GpioEdgeInput(const GpioEdgeInput&) = delete;
    GpioEdgeInput& operator=(const GpioEdgeInput&) = delete;

    void drain() const {
        pollfd p{fd_, POLLIN, 0};
        gpioevent_data event{};
        while (poll(&p, 1, 0) > 0 && read(fd_, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event))) {}
    }

    // Espera uma borda; devolve (id, timestamp ns) ou nullopt no timeout.
    std::optional<std::pair<unsigned, std::uint64_t>> wait(int timeoutMs) const {
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, timeoutMs) <= 0) return std::nullopt;
        gpioevent_data event{};
        if (read(fd_, &event, sizeof(event)) != static_cast<ssize_t>(sizeof(event))) return std::nullopt;
        return std::make_pair(static_cast<unsigned>(event.id), static_cast<std::uint64_t>(event.timestamp));
    }

private:
    int fd_{-1};
};

}  // namespace ilrdos
