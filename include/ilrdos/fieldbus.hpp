// ILrDOS — barramentos de campo: Modbus (TCP e RTU/RS-485 por cabo) e escrita Bluetooth LE.
//
// Usado pelo IotHub para ligar/desligar e LER equipamentos de casa e de indústria por cabo
// (CLPs, relés/contatores com módulo Modbus, sensores) e atuadores Bluetooth declarados em ilrdos-iot.txt.
// Só fala com o que foi declarado, só com endereços da rede local (Modbus TCP) e nunca monta comando de shell.
#pragma once

#include <fcntl.h>
#include <termios.h>

#include "http.hpp"

namespace ilrdos {

struct ModbusLink {
    bool serial{false};
    std::string host;          // Modbus TCP
    int port{502};
    std::string tty;           // Modbus RTU (RS-485 por adaptador USB, ex.: /dev/ttyUSB0)
    int baud{9600};
    char parity{'N'};          // N, E ou O
    int unit{1};
};

inline std::mutex& fieldbusMutex() {
    static std::mutex mutex;   // um barramento por vez: evita quadros misturados no mesmo cabo
    return mutex;
}

inline std::uint16_t modbusCrc16(const std::string& data) {
    std::uint16_t crc = 0xFFFF;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) crc = (crc & 1) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001) : static_cast<std::uint16_t>(crc >> 1);
    }
    return crc;
}

inline std::string modbusPduWriteCoil(unsigned address, bool on) {
    std::string pdu;
    pdu.push_back(0x05);
    pdu.push_back(static_cast<char>((address >> 8) & 0xFF));
    pdu.push_back(static_cast<char>(address & 0xFF));
    pdu.push_back(on ? static_cast<char>(0xFF) : 0x00);
    pdu.push_back(0x00);
    return pdu;
}

inline std::string modbusPduRead(unsigned function, unsigned address, unsigned count) {
    std::string pdu;
    pdu.push_back(static_cast<char>(function));
    pdu.push_back(static_cast<char>((address >> 8) & 0xFF));
    pdu.push_back(static_cast<char>(address & 0xFF));
    pdu.push_back(static_cast<char>((count >> 8) & 0xFF));
    pdu.push_back(static_cast<char>(count & 0xFF));
    return pdu;
}

inline bool validSerialPath(const std::string& path) {
    if (path.size() < 9 || path.size() > 64 || path.find("..") != std::string::npos) return false;
    if (path.rfind("/dev/tty", 0) != 0 && path.rfind("/dev/serial/", 0) != 0) return false;
    return std::all_of(path.begin(), path.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-' || c == ':' || c == '.';
    });
}

inline speed_t baudConstant(int baud) {
    switch (baud) {
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B0;
    }
}

inline bool readExact(int fd, unsigned char* buffer, std::size_t count, int timeoutMs) {
    std::size_t got = 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got < count) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left)) <= 0) return false;
        const ssize_t n = read(fd, buffer + got, count - got);
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
    }
    return true;
}

inline bool modbusTcpExchange(const ModbusLink& link, const std::string& pdu, std::string& reply, std::string& error) {
    const auto address = resolveLan(link.host, link.port, error);
    if (!address) return false;
    const int fd = connectWithTimeout(*address, 2000, error);
    if (fd < 0) return false;
    static std::uint16_t transaction = 0;
    ++transaction;
    std::string frame;
    frame.push_back(static_cast<char>(transaction >> 8));
    frame.push_back(static_cast<char>(transaction & 0xFF));
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(static_cast<char>(((pdu.size() + 1) >> 8) & 0xFF));
    frame.push_back(static_cast<char>((pdu.size() + 1) & 0xFF));
    frame.push_back(static_cast<char>(link.unit));
    frame += pdu;
    bool ok = sendAllTimeout(fd, frame, 2000);
    unsigned char head[7];
    if (!ok) error = "falha ao enviar ao equipamento";
    else if (!readExact(fd, head, sizeof(head), 2000)) { ok = false; error = "o equipamento não respondeu"; }
    else {
        const std::size_t length = (static_cast<std::size_t>(head[4]) << 8) | head[5];
        if (head[2] != 0 || head[3] != 0 || length < 2 || length > 254 ||
            ((static_cast<unsigned>(head[0]) << 8) | head[1]) != transaction || head[6] != static_cast<unsigned char>(link.unit)) {
            ok = false;
            error = "resposta Modbus inválida";
        } else {
            std::string body(length - 1, '\0');
            if (!readExact(fd, reinterpret_cast<unsigned char*>(body.data()), body.size(), 2000)) { ok = false; error = "resposta incompleta"; }
            else reply = body;
        }
    }
    close(fd);
    return ok;
}

inline bool modbusRtuExchange(const ModbusLink& link, const std::string& pdu, std::string& reply, std::string& error) {
    const speed_t speed = baudConstant(link.baud);
    if (speed == B0) { error = "baud inválido"; return false; }
    const int fd = open(link.tty.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { error = "não consegui abrir " + link.tty; return false; }
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) { close(fd); error = "porta serial inválida"; return false; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
    tio.c_cflag |= CS8;
    if (link.parity == 'E') tio.c_cflag |= PARENB;
    if (link.parity == 'O') tio.c_cflag |= PARENB | PARODD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    std::string frame(1, static_cast<char>(link.unit));
    frame += pdu;
    const std::uint16_t crc = modbusCrc16(frame);
    frame.push_back(static_cast<char>(crc & 0xFF));
    frame.push_back(static_cast<char>(crc >> 8));
    bool ok = true;
    std::size_t written = 0;
    while (written < frame.size()) {
        const ssize_t n = write(fd, frame.data() + written, frame.size() - written);
        if (n <= 0) { ok = false; error = "falha ao escrever na serial"; break; }
        written += static_cast<std::size_t>(n);
    }
    tcdrain(fd);
    std::string response;
    if (ok) {
        unsigned char head[2];
        if (!readExact(fd, head, 2, 800)) { ok = false; error = "o equipamento não respondeu"; }
        else {
            response.assign(reinterpret_cast<char*>(head), 2);
            std::size_t more = 0;
            if (head[1] & 0x80) more = 3;                 // código de exceção + CRC
            else if (head[1] == 0x05 || head[1] == 0x06) more = 6;
            else if (head[1] >= 1 && head[1] <= 4) {
                unsigned char count = 0;
                if (readExact(fd, &count, 1, 400)) { response.push_back(static_cast<char>(count)); more = static_cast<std::size_t>(count) + 2; }
                else { ok = false; error = "resposta incompleta"; }
            } else { ok = false; error = "resposta Modbus inválida"; }
            if (ok && more > 0) {
                std::string rest(more, '\0');
                if (!readExact(fd, reinterpret_cast<unsigned char*>(rest.data()), more, 800)) { ok = false; error = "resposta incompleta"; }
                else response += rest;
            }
        }
    }
    close(fd);
    if (!ok) return false;
    if (response.size() < 5) { error = "resposta curta demais"; return false; }
    const std::string payload = response.substr(0, response.size() - 2);
    const std::uint16_t got = static_cast<std::uint16_t>(static_cast<unsigned char>(response[response.size() - 2]) |
                              (static_cast<unsigned char>(response[response.size() - 1]) << 8));
    if (modbusCrc16(payload) != got) { error = "CRC inválido na resposta"; return false; }
    if (static_cast<unsigned char>(payload[0]) != static_cast<unsigned char>(link.unit)) { error = "resposta de outro equipamento"; return false; }
    reply = payload.substr(1);
    return true;
}

inline std::string modbusExceptionText(unsigned code) {
    switch (code) {
        case 1: return "função não aceita pelo equipamento";
        case 2: return "endereço inexistente no equipamento";
        case 3: return "valor inválido";
        case 4: return "falha interna do equipamento";
        case 6: return "equipamento ocupado";
        default: return "exceção Modbus " + std::to_string(code);
    }
}

// Troca um PDU com o equipamento e já valida o formato básico da resposta.
inline bool modbusExec(const ModbusLink& link, const std::string& pdu, std::string& reply, std::string& error) {
    std::lock_guard<std::mutex> lock(fieldbusMutex());
    const bool ok = link.serial ? modbusRtuExchange(link, pdu, reply, error) : modbusTcpExchange(link, pdu, reply, error);
    if (!ok) return false;
    if (reply.empty()) { error = "resposta vazia"; return false; }
    if (static_cast<unsigned char>(reply[0]) & 0x80) {
        error = modbusExceptionText(reply.size() > 1 ? static_cast<unsigned char>(reply[1]) : 0);
        return false;
    }
    if (reply[0] != pdu[0]) { error = "resposta inesperada"; return false; }
    return true;
}

inline bool modbusWriteCoil(const ModbusLink& link, unsigned address, bool on, std::string& error) {
    const std::string pdu = modbusPduWriteCoil(address, on);
    std::string reply;
    if (!modbusExec(link, pdu, reply, error)) return false;
    if (reply != pdu) { error = "o equipamento não repetiu o comando"; return false; }
    return true;
}

// function: 1 = coil, 2 = entrada discreta
inline std::optional<bool> modbusReadBit(const ModbusLink& link, unsigned function, unsigned address, std::string& error) {
    std::string reply;
    if (!modbusExec(link, modbusPduRead(function, address, 1), reply, error)) return std::nullopt;
    if (reply.size() < 3 || reply[1] != 1) { error = "resposta inválida"; return std::nullopt; }
    return (static_cast<unsigned char>(reply[2]) & 1) != 0;
}

// function: 3 = holding register, 4 = input register
inline std::optional<int> modbusReadRegister(const ModbusLink& link, unsigned function, unsigned address, bool isSigned, std::string& error) {
    std::string reply;
    if (!modbusExec(link, modbusPduRead(function, address, 1), reply, error)) return std::nullopt;
    if (reply.size() < 4 || reply[1] != 2) { error = "resposta inválida"; return std::nullopt; }
    const unsigned value = (static_cast<unsigned>(static_cast<unsigned char>(reply[2])) << 8) | static_cast<unsigned char>(reply[3]);
    return isSigned ? static_cast<int>(static_cast<std::int16_t>(value)) : static_cast<int>(value);
}

// ---------------------------------------------------------------- Bluetooth LE (atuadores declarados)

inline bool validMac(const std::string& mac) {
    if (mac.size() != 17) return false;
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i % 3 == 2) { if (mac[i] != ':') return false; }
        else if (!std::isxdigit(static_cast<unsigned char>(mac[i]))) return false;
    }
    return true;
}

inline bool validHexBytes(const std::string& hex) {
    return hex.size() >= 2 && hex.size() <= 40 && hex.size() % 2 == 0 &&
           std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

inline bool validBleHandle(const std::string& handle) {
    return handle.size() >= 3 && handle.size() <= 6 && handle.rfind("0x", 0) == 0 &&
           std::all_of(handle.begin() + 2, handle.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

// Conecta, escreve a característica e desconecta (gatttool, do BlueZ). Argumentos sempre validados, sem shell.
inline bool bleWrite(const std::string& mac, const std::string& addressType, const std::string& handle,
                     const std::string& hex, std::string& error) {
    if (!validMac(mac) || !validBleHandle(handle) || !validHexBytes(hex) || (addressType != "public" && addressType != "random")) {
        error = "parâmetros Bluetooth inválidos";
        return false;
    }
    const std::string tool = envOr("ILRDOS_GATTTOOL", "/usr/bin/gatttool");
    std::lock_guard<std::mutex> lock(fieldbusMutex());
    for (int attempt = 0; attempt < 2; ++attempt) {   // BLE falha à toa de vez em quando
        if (runProgram({tool, "-b", mac, "-t", addressType, "--char-write-req", "-a", handle, "-n", hex}, {}, 15)) return true;
    }
    error = "não consegui conectar ao dispositivo Bluetooth (" + mac + ")";
    return false;
}

// ── Robôs feitos pelo usuário (tipo "diy") ─────────────────────────────────────
// Protocolo de linha, de propósito o mais simples possível para caber num Arduino/ESP32/Pico:
//   ILrDOS -> robô : uma palavra de comando terminada em \n  (START, STOP, PAUSE, DOCK ou STATUS)
//   robô -> ILrDOS : uma linha de resposta                   (OK | OK <estado> | ERR <motivo>)
// O robô só é dado como "iniciado/parado" quando responde OK: nada de comando às cegas.
// Transporte: TCP na rede local (Wi-Fi: ESP32, ESP8266, Pico W) ou serial/USB (Arduino, Pico, ESP32).
struct DiyLink {
    bool serial{false};
    std::string host;          // TCP (somente rede local)
    int port{7777};
    std::string tty;           // serial: /dev/ttyUSB0, /dev/ttyACM0, ...
    int baud{115200};
    int waitMs{0};             // espera após abrir a serial (placas que reiniciam ao abrir, como o Arduino Uno)
};

inline bool validDiyWord(const std::string& word) {
    if (word.empty() || word.size() > 32) return false;
    return std::all_of(word.begin(), word.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':';
    });
}

inline bool readLine(int fd, std::string& line, int timeoutMs) {
    line.clear();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (line.size() < 128) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left)) <= 0) return false;
        char c = 0;
        const ssize_t n = read(fd, &c, 1);
        if (n <= 0) return false;
        if (c == '\n') { line = trim(line); return true; }
        if (c != '\r') line.push_back(c);
    }
    return false;
}

// Envia `command` + '\n' e devolve a primeira linha de resposta (já sem espaços nas pontas).
inline bool diyExchange(const DiyLink& link, const std::string& command, std::string& reply, std::string& error) {
    if (!validDiyWord(command)) { error = "comando inválido"; return false; }
    int fd = -1;
    if (link.serial) {
        const speed_t speed = baudConstant(link.baud);
        if (speed == B0) { error = "baud inválido"; return false; }
        fd = open(link.tty.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { error = "não consegui abrir " + link.tty; return false; }
        termios tio{};
        if (tcgetattr(fd, &tio) != 0) { close(fd); error = "porta serial inválida"; return false; }
        cfmakeraw(&tio);
        cfsetispeed(&tio, speed);
        cfsetospeed(&tio, speed);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
        tio.c_cflag |= CS8;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
        if (link.waitMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(link.waitMs));
        tcflush(fd, TCIOFLUSH);
        const std::string out = command + "\n";
        std::size_t written = 0;
        while (written < out.size()) {
            const ssize_t n = write(fd, out.data() + written, out.size() - written);
            if (n <= 0) { close(fd); error = "falha ao escrever na serial"; return false; }
            written += static_cast<std::size_t>(n);
        }
        tcdrain(fd);
    } else {
        const auto address = resolveLan(link.host, link.port, error);
        if (!address) return false;
        fd = connectWithTimeout(*address, 2000, error);
        if (fd < 0) return false;
        if (!sendAllTimeout(fd, command + "\n", 2000)) { close(fd); error = "falha ao enviar ao robô"; return false; }
    }
    // Aceita só linhas de resposta (ignora lixo/boot da placa até achar OK ou ERR).
    bool got = false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(3000);
    std::string line;
    while (Clock::now() < deadline) {
        if (!readLine(fd, line, 3000)) break;
        if (line == "OK" || line.rfind("OK ", 0) == 0 || line == "ERR" || line.rfind("ERR ", 0) == 0) { got = true; break; }
    }
    close(fd);
    if (!got) { error = "o robô não respondeu"; return false; }
    reply = line;
    return true;
}

}  // namespace ilrdos
