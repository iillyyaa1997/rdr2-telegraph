// llm_probe.cpp - builds the chat request Telegraph sends and checks an answer.
//   llm_probe request <model>   prints the JSON body for a sample batch
//   llm_probe answer            reads a chat completions answer on stdin, restores placeholders
#include <iostream>
#include <iterator>
#include "../src/core.hpp"
static const char* kTexts[] = {"Hideout cleared. Loot the camp.", "~COLOR_RED~Wanted~s~ in Big Valley: $25",
                               "Press ~INPUT_JUMP~ to rob 2 banks.", "MISSION FAILED", "Day 3 of 7"};
int main(int argc, char** argv) {
  std::string mode = argc > 1 ? argv[1] : "";
  std::vector<tg::Protected> prot;
  std::vector<std::string> texts;
  for (const char* t : kTexts) {
    tg::Skeleton sk = tg::MakeSkeleton(t);
    prot.push_back(tg::Protect(sk.key, sk.literal));
    texts.push_back(prot.back().text);
  }
  const tg::Lang& ru = *tg::LangByGame(7);
  if (mode == "request") {
    std::cout << tg::ChatBody(argc > 2 ? argv[2] : "", tg::LlmSystemPrompt(ru, {{"Big Valley", "Большая долина"}}),
                              tg::JsonStringArray(texts));
    return 0;
  }
  std::string body((std::istreambuf_iterator<char>(std::cin)), {});
  std::string content;
  std::vector<std::string> out;
  if (!tg::ParseChat(body, content)) { std::cout << "ParseChat failed: " << body.substr(0, 300) << "\n"; return 1; }
  if (!tg::ParseStringArray(content, out) || out.size() != texts.size()) { std::cout << "array failed: " << content << "\n"; return 1; }
  int ok = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    std::string v;
    bool r = tg::Restore(prot[i], out[i], v);
    ok += r;
    std::cout << (r ? "OK  " : "BAD ") << kTexts[i] << " => " << (r ? v : out[i]) << "\n";
  }
  std::cout << ok << "/" << out.size() << " restored\n";
  return ok == (int)out.size() ? 0 : 1;
}
