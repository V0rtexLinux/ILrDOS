// ILrDOS — cabeçalho "guarda-chuva": inclui a API pública da biblioteca.
//   #include <ilrdos/ilrdos.hpp>
// Biblioteca header-only (C++20). Dependências: OpenSSL (libcrypto); OpenCV, ALSA e Vosk são opcionais
// e habilitados pelas macros ILRDOS_HAVE_OPENCV / ILRDOS_HAVE_ALSA / ILRDOS_HAVE_VOSK.
#pragma once

#include <ilrdos/common.hpp>
#include <ilrdos/structure.hpp>   // estrutura física do robô (energia, cérebro, dados, casco, visor)
#include <ilrdos/robot.hpp>       // olho (LED amarelo) e alerta (LED vermelho)
#include <ilrdos/motors.hpp>
#include <ilrdos/impact.hpp>
#include <ilrdos/iot.hpp>
#include <ilrdos/fleet.hpp>
#include <ilrdos/autonomy.hpp>
#include <ilrdos/conversation.hpp>

#define ILRDOS_VERSION_STRING "2.3.0"
