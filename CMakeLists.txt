cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ILRDOS_VERSION_FILE LIMIT_COUNT 1)
project(ilrdos VERSION ${ILRDOS_VERSION_FILE} DESCRIPTION "ILrDOS robot runtime library" LANGUAGES CXX)

option(ILRDOS_BUILD_DAEMON "Compilar o daemon ilrdosd e o ilrdos-client" ON)
option(ILRDOS_WITH_OPENCV  "Habilitar câmera/rostos (OpenCV 4)" ON)
option(ILRDOS_WITH_ALSA    "Habilitar áudio (ALSA)" ON)
option(ILRDOS_WITH_VOSK    "Habilitar reconhecimento de fala (libvosk)" OFF)
option(ILRDOS_INSTALL      "Gerar regras de instalação" ${PROJECT_IS_TOP_LEVEL})

include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# ── Biblioteca header-only: ilrdos::ilrdos ─────────────────────────────────────
add_library(ilrdos INTERFACE)
add_library(ilrdos::ilrdos ALIAS ilrdos)
target_compile_features(ilrdos INTERFACE cxx_std_20)
target_include_directories(ilrdos INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(ilrdos INTERFACE Threads::Threads OpenSSL::Crypto)

if(ILRDOS_WITH_OPENCV)
  find_package(OpenCV QUIET COMPONENTS core imgproc objdetect dnn imgcodecs videoio)
  if(OpenCV_FOUND)
    target_compile_definitions(ilrdos INTERFACE ILRDOS_HAVE_OPENCV=1)
    target_link_libraries(ilrdos INTERFACE ${OpenCV_LIBS})
    target_include_directories(ilrdos INTERFACE ${OpenCV_INCLUDE_DIRS})
  else()
    message(STATUS "ILrDOS: OpenCV 4 não encontrado; sem câmera.")
  endif()
endif()

if(ILRDOS_WITH_ALSA)
  find_package(ALSA QUIET)
  if(ALSA_FOUND)
    target_compile_definitions(ilrdos INTERFACE ILRDOS_HAVE_ALSA=1)
    target_link_libraries(ilrdos INTERFACE ALSA::ALSA)
  else()
    message(STATUS "ILrDOS: ALSA não encontrado; sem áudio.")
  endif()
endif()

if(ILRDOS_WITH_VOSK)
  find_library(VOSK_LIBRARY vosk REQUIRED)
  find_path(VOSK_INCLUDE_DIR vosk_api.h REQUIRED)
  target_compile_definitions(ilrdos INTERFACE ILRDOS_HAVE_VOSK=1)
  target_include_directories(ilrdos INTERFACE ${VOSK_INCLUDE_DIR})
  target_link_libraries(ilrdos INTERFACE ${VOSK_LIBRARY})
endif()

# ── Daemon e cliente (consomem a biblioteca como qualquer outro projeto) ───────
if(ILRDOS_BUILD_DAEMON)
  add_executable(ilrdosd src/robotd.cpp)
  target_link_libraries(ilrdosd PRIVATE ilrdos::ilrdos)
  target_compile_options(ilrdosd PRIVATE -Wall -Wextra -Wpedantic -fstack-protector-strong)

  add_executable(ilrdos-client client/ilrdos_client.cpp)
  target_link_libraries(ilrdos-client PRIVATE OpenSSL::Crypto)
  target_compile_features(ilrdos-client PRIVATE cxx_std_20)
  target_compile_options(ilrdos-client PRIVATE -Wall -Wextra -Wpedantic -fstack-protector-strong)
endif()

# ── Instalação + pacote CMake (find_package(ilrdos CONFIG)) ────────────────────
if(ILRDOS_INSTALL)
  include(CMakePackageConfigHelpers)
  install(DIRECTORY include/ilrdos DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(TARGETS ilrdos EXPORT ilrdosTargets)
  if(ILRDOS_BUILD_DAEMON)
    install(TARGETS ilrdosd ilrdos-client RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
  endif()
  install(EXPORT ilrdosTargets NAMESPACE ilrdos:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ilrdos)
  configure_package_config_file(cmake/ilrdosConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ilrdosConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ilrdos)
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ilrdosConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ilrdosConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ilrdosConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ilrdos)
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()
