#include <ilrdos/ilrdos.hpp>
#include <iostream>

int main() {
    std::cout << "ILrDOS " << ILRDOS_VERSION_STRING << ": "
              << ilrdos::partStateName(ilrdos::PartState::Ok) << '\n';
    return ilrdos::foldPt("Câmera") == "camera" ? 0 : 1;
}
