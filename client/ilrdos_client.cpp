#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <conio.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string hexEncode(const unsigned char* bytes, std::size_t length) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i) {
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

std::string hmacSha256Hex(const std::string& secret, const std::string& message) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
        throw std::runtime_error("Não foi possível abrir o provedor SHA-256 do Windows.");
    }
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                      sizeof(objectLength), &returned, 0);
    BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                      sizeof(hashLength), &returned, 0);
    std::vector<UCHAR> object(objectLength);
    std::vector<UCHAR> digest(hashLength);
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
                                       reinterpret_cast<PUCHAR>(const_cast<char*>(secret.data())),
                                       static_cast<ULONG>(secret.size()), 0);
    if (status >= 0) {
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())),
                                static_cast<ULONG>(message.size()), 0);
    }
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), hashLength, 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Falha ao calcular a autenticação.");
    return hexEncode(digest.data(), digest.size());
#else
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(),
             digest.data(), &digestLength) == nullptr) {
        throw std::runtime_error("Falha ao calcular a autenticação.");
    }
    return hexEncode(digest.data(), digestLength);
#endif
}

std::string readSecret() {
    std::cout << "Token ILrDOS: ";
    std::string secret;
#ifdef _WIN32
    while (true) {
        const int key = _getch();
        if (key == '\r' || key == '\n') break;
        if (key == '\b') {
            if (!secret.empty()) secret.pop_back();
        } else if (key >= 32 && key <= 126 && secret.size() < 256) {
            secret.push_back(static_cast<char>(key));
        }
    }
#else
    termios previous{};
    const bool changed = tcgetattr(STDIN_FILENO, &previous) == 0;
    if (changed) {
        termios hidden = previous;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    }
    std::getline(std::cin, secret);
    if (changed) tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous);
#endif
    std::cout << '\n';
    if (secret.size() < 24) throw std::runtime_error("Token muito curto.");
    return secret;
}

bool sendAll(Socket socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const int count = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        const ssize_t count = send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool receiveLine(Socket socket, std::string& line, std::size_t limit = 1024) {
    line.clear();
    char byte = 0;
    while (line.size() < limit) {
#ifdef _WIN32
        const int count = recv(socket, &byte, 1, 0);
#else
        const ssize_t count = recv(socket, &byte, 1, 0);
#endif
        if (count <= 0) return false;
        if (byte == '\n') return true;
        if (byte != '\r') line.push_back(byte);
    }
    return false;
}

Socket connectTo(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
        throw std::runtime_error("Endereço ou porta inválidos.");
    }
    Socket connected = kInvalidSocket;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        Socket candidate = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (candidate == kInvalidSocket) continue;
        if (connect(candidate, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0) {
            connected = candidate;
            break;
        }
#ifdef _WIN32
        closesocket(candidate);
#else
        close(candidate);
#endif
    }
    freeaddrinfo(result);
    if (connected == kInvalidSocket) throw std::runtime_error("Não foi possível conectar ao robô.");
    return connected;
}

void closeSocket(Socket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void enableTerminalColors() {
#ifdef _WIN32
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(console, &mode)) {
        SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

void printHeader() {
    std::cout << "\033[31m"
                 "  ██╗██╗     ██████╗ ██████╗  ██████╗ ███████╗\n"
                 "  ██║██║     ██╔══██╗██╔══██╗██╔═══██╗██╔════╝\n"
                 "  ██║██║     ██████╔╝██║  ██║██║   ██║███████╗\n"
                 "  ██║██║     ██╔══██╗██║  ██║██║   ██║╚════██║\n"
                 "  ██║███████╗██║  ██║██████╔╝╚██████╔╝███████║\n"
                 "  ╚═╝╚══════╝╚═╝  ╚═╝╚═════╝  ╚═════╝ ╚══════╝\n"
                 "\033[0m  ILrDOS — Intelligent Lifeform robot Disk Operating System\n\n";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::cerr << "Falha ao iniciar a rede do Windows.\n";
        return 1;
    }
#endif
    enableTerminalColors();
    printHeader();

    std::string host;
    std::string port = "4317";
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = argv[2];
    if (host.empty()) {
        std::cout << "IP do robô (ex.: 192.168.1.50): ";
        std::getline(std::cin, host);
    }
    if (host.empty()) {
        std::cerr << "Informe o IP do Raspberry Pi.\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 2;
    }

    Socket socket = kInvalidSocket;
    try {
        socket = connectTo(host, port);
        std::string handshake;
        if (!receiveLine(socket, handshake) || handshake.rfind("ILrDOS/1 CHALLENGE ", 0) != 0) {
            throw std::runtime_error("O serviço ILrDOS não respondeu ao desafio de autenticação.");
        }
        const std::string nonce = handshake.substr(std::string("ILrDOS/1 CHALLENGE ").size());
        const std::string token = readSecret();
        if (!sendAll(socket, hmacSha256Hex(token, nonce) + "\n")) {
            throw std::runtime_error("A conexão foi interrompida.");
        }
        std::string response;
        if (!receiveLine(socket, response) || response != "ILrDOS/1 OK") {
            throw std::runtime_error("Acesso negado. Confira o token e a rede Wi-Fi.");
        }

        std::cout << "\033[31mConectado ao ILrDOS em " << host << ":" << port
                  << "\033[0m\n";
        while (receiveLine(socket, response)) {
            std::cout << response << '\n';
            if (response == "BYE") break;
            if (response == "ILrDOS conectado. Digite help para listar comandos.") break;
        }

        std::string command;
        while (true) {
            std::cout << "\033[31milrdos>\033[0m ";
            if (!std::getline(std::cin, command)) break;
            if (command.size() > 500) {
                std::cout << "Comando muito longo; limite de 500 caracteres.\n";
                continue;
            }
            if (!sendAll(socket, command + "\n")) {
                std::cerr << "\nConexão encerrada.\n";
                break;
            }
            if (!receiveLine(socket, response)) {
                std::cerr << "Conexão encerrada.\n";
                break;
            }
            std::cout << response << '\n';
            if (command == "exit" || command == "quit") break;
        }
    } catch (const std::exception& error) {
        std::cerr << "\033[31mErro: " << error.what() << "\033[0m\n";
        if (socket != kInvalidSocket) closeSocket(socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    if (socket != kInvalidSocket) closeSocket(socket);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
