// scan.cpp - prints what the pre-translation scanner would queue for each .asi
#include <fstream>
#include <iostream>
#include <iterator>
#include "../src/core.hpp"
int main(int argc, char** argv) {
  bool verbose = argc > 1 && std::string(argv[1]) == "-v";
  for (int a = verbose ? 2 : 1; a < argc; ++a) {
    std::ifstream f(argv[a], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    size_t dropped = 0;
    auto keys = tg::UiCandidates(d.data(), d.size(), &dropped);
    size_t chars = 0;
    for (auto& k : keys) chars += k.size();
    std::cout << "== " << argv[a] << ": " << keys.size() << " UI, " << chars << " chars, single words dropped " << dropped << "\n";
    if (verbose) for (auto& k : keys) std::cout << "  " << tg::Escape(k) << "\n";
  }
}
