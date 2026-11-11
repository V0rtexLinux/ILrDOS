# Projeto de exemplo/teste: usa o ILrDOS instalado como pacote (find_package).
cmake_minimum_required(VERSION 3.20)
project(ilrdos_consumer LANGUAGES CXX)
find_package(ilrdos CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ilrdos::ilrdos)
