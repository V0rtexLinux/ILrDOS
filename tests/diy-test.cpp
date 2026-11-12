// Teste do tipo "diy": sobe um robô falso (TCP em 127.0.0.1) e comanda pelo FleetHub, sem hardware.
#include <ilrdos/fleet.hpp>
#include <fstream>
#include <iostream>

using namespace ilrdos;

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = {}) {
    std::cout << (ok ? "[ok]   " : "[FALHA] ") << what << (detail.empty() ? "" : " -> " + detail) << '\n';
    if (!ok) ++fails;
}

int main() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listener, 4);
    socklen_t len = sizeof(addr);
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const int port = ntohs(addr.sin_port);

    std::atomic<bool> run{true};
    std::string robotState = "stopped";
    std::vector<std::string> seen;
    std::thread robot([&] {
        while (run.load()) {
            pollfd p{listener, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            const int c = accept(listener, nullptr, nullptr);
            if (c < 0) continue;
            std::string line;
            readLine(c, line, 1000);
            seen.push_back(line);
            std::string reply = "ERR comando desconhecido";
            if (line == "START") { robotState = "working"; reply = "OK"; }
            else if (line == "STOP") { robotState = "stopped"; reply = "OK"; }
            else if (line == "PAUSE") reply = "ERR sem pausa neste robo";
            else if (line == "STATUS") reply = "OK " + robotState;
            reply += "\n";
            send(c, reply.data(), reply.size(), 0);
            close(c);
        }
    });

    const std::string cfg = "/tmp/ilrdos-diy-fleet.txt";
    {
        std::ofstream f(cfg);
        f << "robot carrinho diy host=127.0.0.1 port=" << port << " class=carrinho alias=meu_carrinho,carrinho interval=0\n";
        f << "robot pc_do_joao diy host=127.0.0.1 port=" << port << " class=computador\n";        // deve ser recusado
        f << "robot externo diy host=8.8.8.8 port=7777 class=carrinho\n";                            // aceito no parse; recusado ao enviar
        f << "robot sem_transporte diy class=carrinho\n";                                            // sem host nem serial
        f << "robot comando_ruim diy host=127.0.0.1 start_payload=a;b class=carrinho\n";             // palavra inválida
        f << "robot serial_ruim diy serial=/etc/passwd class=carrinho\n";                            // caminho inválido
    }
    FleetHub hub(cfg, false);
    const std::string loaded = hub.load();
    std::cout << loaded << '\n';
    check(hub.count() == 2, "só robôs válidos foram carregados (carrinho + externo)", std::to_string(hub.count()));

    std::string r = hub.command("carrinho", "start", Origin::Terminal);
    check(robotState == "working", "START chegou ao robô", r);
    r = hub.status("carrinho");
    check(r.find("trabalhando") != std::string::npos && r.find("informado pelo robô") != std::string::npos, "STATUS vem do robô", r);
    r = hub.command("carrinho", "pause", Origin::Terminal);
    check(r.find("recus") != std::string::npos || r.find("falh") != std::string::npos || r.find("sem pausa") != std::string::npos, "ERR do robô é reportado", r);
    r = hub.command("carrinho", "stop", Origin::Voice);
    check(robotState == "stopped", "STOP chegou ao robô", r);
    r = hub.command("externo", "start", Origin::Terminal);
    check(r.find("rede local") != std::string::npos, "endereço fora da rede local é recusado", r);

    run.store(false);
    robot.join();
    close(listener);
    std::cout << (fails == 0 ? "DIY: tudo certo\n" : "DIY: falhas\n");
    return fails == 0 ? 0 : 1;
}
