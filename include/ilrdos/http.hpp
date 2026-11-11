// ILrDOS — cliente HTTP/TCP mínimo, somente para a rede local (sem TLS, sem redirecionamentos).
#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common.hpp"

namespace ilrdos {

struct HttpResponse {
    int status{0};
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

// Só falamos com endereços da rede local: evita que uma configuração errada (ou um texto malicioso)
// faça o robô acessar a internet ou serviços arbitrários.
inline bool isPrivateIPv4(std::uint32_t host) {
    const unsigned a = host >> 24, b = (host >> 16) & 0xFF;
    return a == 10 || a == 127 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) ||
           (a == 169 && b == 254);
}

inline std::optional<sockaddr_in> resolveLan(const std::string& host, int port, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
        error = "endereço não encontrado: " + host;
        return std::nullopt;
    }
    sockaddr_in address{};
    std::memcpy(&address, result->ai_addr, sizeof(address));
    freeaddrinfo(result);
    if (!isPrivateIPv4(ntohl(address.sin_addr.s_addr))) {
        error = "só são aceitos endereços da rede local (" + host + ")";
        return std::nullopt;
    }
    return address;
}

inline int connectWithTimeout(const sockaddr_in& address, int timeoutMs, std::string& error) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) { error = "socket"; return -1; }
    int rc = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (rc < 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeoutMs) > 0) {
            int soError = 0;
            socklen_t length = sizeof(soError);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length);
            rc = soError == 0 ? 0 : -1;
        } else {
            rc = -1;
        }
    }
    if (rc != 0) { close(fd); error = "sem conexão"; return -1; }
    return fd;
}

inline bool sendAllTimeout(int fd, const std::string& data, int timeoutMs) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeoutMs) <= 0) return false;
        const ssize_t count = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

inline bool parseHttpUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    const std::string prefix = "http://";
    if (url.rfind(prefix, 0) != 0) return false;   // https não é suportado (rede local)
    std::string rest = url.substr(prefix.size());
    const auto slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    port = 80;
    const auto colon = hostPort.find(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(hostPort.substr(colon + 1)); } catch (...) { return false; }
        hostPort = hostPort.substr(0, colon);
    }
    host = hostPort;
    return !host.empty() && port > 0 && port < 65536 && path.find_first_of(" \r\n") == std::string::npos;
}

inline std::string percentDecode(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

inline std::string decodeChunked(const std::string& raw) {
    std::string out;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto eol = raw.find("\r\n", pos);
        if (eol == std::string::npos) break;
        std::size_t size = 0;
        try { size = std::stoul(raw.substr(pos, eol - pos), nullptr, 16); } catch (...) { break; }
        pos = eol + 2;
        if (size == 0 || pos + size > raw.size()) { if (size != 0) out.append(raw, pos, std::string::npos); break; }
        out.append(raw, pos, size);
        pos += size + 2;
    }
    return out;
}

inline HttpResponse httpRequest(const std::string& method, const std::string& host, int port,
                                const std::string& path, const std::vector<std::string>& headers,
                                const std::string& body, int timeoutMs, std::size_t maxBytes = 4u << 20) {
    HttpResponse response;
    const auto address = resolveLan(host, port, response.error);
    if (!address) return response;
    const int fd = connectWithTimeout(*address, timeoutMs, response.error);
    if (fd < 0) return response;

    std::ostringstream request;
    request << method << ' ' << path << " HTTP/1.1\r\nHost: " << host << "\r\nConnection: close\r\n"
            << "User-Agent: ILrDOS\r\n";
    for (const auto& header : headers) request << header << "\r\n";
    if (!body.empty() || method == "POST") request << "Content-Length: " << body.size() << "\r\n";
    request << "\r\n" << body;
    if (!sendAllTimeout(fd, request.str(), timeoutMs)) {
        close(fd);
        response.error = "falha ao enviar";
        return response;
    }

    std::string raw;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    char buffer[4096];
    while (raw.size() < maxBytes + 4096) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) { response.error = "tempo esgotado"; break; }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left)) <= 0) { response.error = "tempo esgotado"; break; }
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        raw.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);

    const auto split = raw.find("\r\n\r\n");
    if (split == std::string::npos) {
        if (response.error.empty()) response.error = "resposta inválida";
        return response;
    }
    const std::string head = raw.substr(0, split);
    std::string payload = raw.substr(split + 4);
    const auto space = head.find(' ');
    if (space != std::string::npos) {
        try { response.status = std::stoi(head.substr(space + 1, 3)); } catch (...) { response.status = 0; }
    }
    const std::string lowerHead = [&] { std::string l = head; for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return l; }();
    if (lowerHead.find("transfer-encoding: chunked") != std::string::npos) payload = decodeChunked(payload);
    response.body = payload.substr(0, maxBytes);
    if (response.status != 0) response.error.clear();
    return response;
}

// ---------------------------------------------------------------- JSON mínimo

inline std::string jsonEscape(const std::string& in) {
    std::string out;
    for (const unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char tmp[8]; std::snprintf(tmp, sizeof(tmp), "\\u%04x", c); out += tmp; }
                else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

inline void appendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) { out.push_back(static_cast<char>(0xC0 | (code >> 6))); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
    else { out.push_back(static_cast<char>(0xE0 | (code >> 12))); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
}

// Extrai o valor string da primeira ocorrência de "chave":"..." (suficiente para Vosk e Ollama).
inline std::string jsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    std::string out;
    for (std::size_t i = pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') return out;
        if (c != '\\' || i + 1 >= json.size()) { out.push_back(c); continue; }
        const char e = json[++i];
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': break;
            case 'u':
                if (i + 4 < json.size()) {
                    try { appendUtf8(out, static_cast<unsigned>(std::stoul(json.substr(i + 1, 4), nullptr, 16))); } catch (...) {}
                    i += 4;
                }
                break;
            default: out.push_back(e);
        }
    }
    return out;
}

}  // namespace ilrdos
