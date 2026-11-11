# Contribuindo com o ILrDOS

O ILrDOS é open source sob a licença [MIT](LICENSE): você pode usar, modificar e redistribuir, inclusive em
produtos próprios, mantendo o aviso de copyright e a licença.

## Estrutura

- `include/ilrdos/` — a biblioteca (header-only, C++20). Esta é a **API pública**.
- `src/robotd.cpp` — o daemon `ilrdosd`, que é só um cliente da biblioteca.
- `client/` — terminal `ilrdos-client`.
- `os/` — imagem Buildroot para Raspberry Pi Zero 2 W.
- `tests/consumer/` — projeto mínimo que usa o ILrDOS via `find_package`.

## Compilar e testar

```bash
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
cmake --install build-cmake --prefix "$PWD/_install"
cmake -S tests/consumer -B build-consumer -DCMAKE_PREFIX_PATH="$PWD/_install"
cmake --build build-consumer && ./build-consumer/consumer
```

O `scripts/build-linux.sh` continua funcionando para compilar só o daemon.

## Regras para a API pública

- Tudo em `include/ilrdos/` deve ser `inline`/`constexpr`/template (header-only), sem variáveis globais não-inline.
- Mudança que quebra a API pública exige subir a versão maior em `VERSION`.
- `stop`, `pause` e `dock` da frota são sempre permitidos; não adicione caminhos que os bloqueiem.

## Pull requests

Abra um PR pequeno, descreva o que mudou e confirme que o workflow **Library (CMake package)** passou.
