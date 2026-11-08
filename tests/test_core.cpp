// test_core.cpp - checks of the platform-independent part (run by ./build.sh)
#include <cstdio>
#include <iostream>
#include "../src/core.hpp"

static int g_fail = 0, g_count = 0;
#define CHECK(c) do { ++g_count; if (!(c)) { ++g_fail; std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #c ") failed\n"; } } while (0)
#define EQ(a, b) do { ++g_count; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++g_fail; std::cerr << __FILE__ << ":" << __LINE__ << ": " #a " = [" << _a << "] expected [" << _b << "]\n"; } } while (0)

static std::string RoundTrip(const std::string& src, const std::string& translated, bool* ok = nullptr) {
  tg::Skeleton sk = tg::MakeSkeleton(src);
  tg::Protected p = tg::Protect(sk.key, sk.literal);
  std::string v;
  bool r = tg::Restore(p, translated, v);
  if (ok) *ok = r;
  return r ? tg::FillNumbers(v, sk.nums) : std::string("<fail>");
}

int main() {
  // JSON
  {
    tg::Json j;
    CHECK(tg::ParseJson(R"({"a":[1,2.5,"x\u00e9\ud83d\ude00",true,null],"b":{"c":"\n"}})", j));
    EQ(j.Get("a")->At(2)->str, std::string("x\xc3\xa9\xf0\x9f\x98\x80"));
    EQ(j.Get("a")->At(1)->num, 2.5);
    EQ(j.Get("b")->Get("c")->str, std::string("\n"));
    CHECK(!tg::ParseJson("[1,2", j));
    CHECK(!tg::ParseJson("{\"a\":1} x", j));
    EQ(tg::JsonQuote("a\"b\\c\n\x01"), std::string("\"a\\\"b\\\\c\\n\\u0001\""));
  }
  // Google answer (shape captured from translate.googleapis.com 2026-11-07)
  {
    std::string body = R"([[["Охотник за головами\n","Bounty Hunter\n",null,null,3,null,null,[[]],[[["8e6a","en_ru_2023q1.md"],true]]],["Отмена","Cancel",null,null,3]],null,"en",null,null,null,1,[],[["en"],null,[1],["en"]]])";
    std::string text, det;
    CHECK(tg::ParseGoogle(body, text, det));
    EQ(det, std::string("en"));
    auto lines = tg::SplitLines(text);
    EQ(lines.size(), (size_t)2);
    EQ(lines[0], std::string("Охотник за головами"));
    EQ(lines[1], std::string("Отмена"));
  }
  // DeepL and chat answers
  {
    std::vector<std::string> out;
    CHECK(tg::ParseDeepL(R"({"translations":[{"detected_source_language":"EN","text":"Привет"},{"text":"Мир"}]})", out));
    EQ(out.size(), (size_t)2);
    EQ(out[1], std::string("Мир"));
    std::string content;
    CHECK(tg::ParseChat(R"({"choices":[{"index":0,"message":{"role":"assistant","content":"```json\n[\"Да\", \"Нет {0}\"]\n```"}}]})", content));
    CHECK(tg::ParseStringArray(content, out));
    EQ(out.size(), (size_t)2);
    EQ(out[1], std::string("Нет {0}"));
    CHECK(!tg::ParseStringArray("no array here", out));
    CHECK(tg::ParseStringArray("Sure: [1] then [\"a\"]", out));
    EQ(out[0], std::string("a"));
    std::string b = tg::DeepLBody({"a", "b\"c"}, *tg::LangByGame(7));
    EQ(b, std::string(R"({"text":["a","b\"c"],"target_lang":"RU","preserve_formatting":true})"));
    tg::Json j;
    CHECK(tg::ParseJson(tg::ChatBody("m", "sys", "[\"x\"]"), j));
    EQ(j.Get("messages")->At(1)->Get("content")->str, std::string("[\"x\"]"));
  }
  // skeletons and placeholders
  {
    tg::Skeleton k = tg::MakeSkeleton("Reward: $1,250.50 for 3 men at 10:30.");
    EQ(k.key, std::string("Reward: $* for * men at *."));
    EQ(k.nums.size(), (size_t)3);
    EQ(k.nums[0], std::string("1,250.50"));
    EQ(tg::MakeSkeleton("Lancaster M1899 and ~COLOR_1~2~s~").key, std::string("Lancaster M1899 and ~COLOR_1~*~s~"));
    CHECK(tg::MakeSkeleton("5 * 3").literal);
    tg::Protected p = tg::Protect("~COLOR_RED~Bounty~s~: $*~n~Wanted", false);
    EQ(p.text, std::string("{0}Bounty{1}: ${2}{3}Wanted"));
    EQ(RoundTrip("Reward: $25", "Награда: ${0}."), std::string("Награда: $25"));
    EQ(RoundTrip("Press ~INPUT_JUMP~ to rob 2 banks.", "Нажмите {0}, чтобы ограбить {1} банка."),
       std::string("Нажмите ~INPUT_JUMP~, чтобы ограбить 2 банка."));
    EQ(RoundTrip("Kill 3 of 5", "Убейте { 0 } из {1}"), std::string("Убейте 3 из 5"));
    EQ(RoundTrip("Day 2, round 7", "Раунд {1}, день {0}"), std::string("Раунд 7, день 2"));
    bool ok = true;
    RoundTrip("Kill 3 of 5", "Убейте {0} из", &ok);
    CHECK(!ok);
    RoundTrip("Kill 3 of 5", "Убейте {0} из {0} {1}", &ok);
    CHECK(!ok);
    RoundTrip("Kill 3 of 5", "Убейте {0} из {7}", &ok);
    CHECK(!ok);
    EQ(RoundTrip("  Reward ", "Награда"), std::string("  Награда "));
    EQ(RoundTrip("MISSION FAILED", "Миссия провалена"), std::string("МИССИЯ ПРОВАЛЕНА"));
    EQ(RoundTrip("Loading...", "Загрузка..."), std::string("Загрузка..."));
    EQ(RoundTrip("Line one\nLine two", "Строка один{0}Строка два"), std::string("Строка один\nСтрока два"));
    EQ(RoundTrip("Use {name} now", "Используйте {0} сейчас"), std::string("Используйте {name} сейчас"));
    EQ(tg::FillNumbers("Награда: ${1} за {2}", {"5", "7"}), std::string("Награда: $5 за 7"));
    EQ(tg::FillNumbers("{3} stays", {"5"}), std::string("{3} stays"));
  }
  // pattern dictionary entries, sequential and numbered captures
  {
    std::string out;
    tg::Pattern a = tg::MakePattern("Load Slot *", "Загрузить слот *");
    CHECK(tg::ApplyPattern(a, "Load Slot 3", out));
    EQ(out, std::string("Загрузить слот 3"));
    tg::Pattern b = tg::MakePattern("Day * of *", "{2}: день {1}");
    CHECK(tg::ApplyPattern(b, "Day 4 of 9", out));
    EQ(out, std::string("9: день 4"));
    CHECK(!tg::ApplyPattern(b, "Night 4 of 9", out));
  }
  // text checks
  {
    CHECK(tg::AlreadyLocalized("Награда за голову"));
    CHECK(tg::AlreadyLocalized("~COLOR_RED~Розыск~s~"));
    CHECK(!tg::AlreadyLocalized("Wanted: Иван"));
    CHECK(tg::WorthTranslating("Wanted"));
    CHECK(!tg::WorthTranslating("$25.00"));
    CHECK(!tg::WorthTranslating("~INPUT_JUMP~"));
    EQ(tg::UpperUtf8("ёжик Łódź ß"), std::string("ЁЖИК ŁÓDŹ ß"));
    EQ(tg::UrlEncode("a b&ж"), std::string("a%20b%26%D0%B6"));
    EQ(tg::Unescape(tg::Escape("a\\nb\nc\t\\")), std::string("a\\nb\nc\t\\"));
  }
  // dictionary lines
  {
    std::vector<std::pair<std::string, std::string>> e;
    tg::ForEachEntry("\xEF\xBB\xBF# c\r\nA => Б\r\n\r\nbad line\n~X => Y => Z\n", [&](const std::string& k, const std::string& v) { e.emplace_back(k, v); });
    EQ(e.size(), (size_t)2);
    EQ(e[0].second, std::string("Б"));
    EQ(e[1].first, std::string("~X"));
    EQ(e[1].second, std::string("Y => Z"));
  }
  // scanner
  {
    EQ(tg::FormatToNumbers("Reward: $%d (%.2f%%)"), std::string("Reward: $0 (0%)"));
    EQ(tg::FormatToNumbers("Hello %s"), std::string());
    CHECK(tg::LooksLikeUiText("Hideout cleared. Loot the camp."));
    CHECK(tg::LooksLikeUiText("Cancel"));
    CHECK(tg::LooksLikeUiText("~COLOR_RED~Wanted~s~ in Lemoyne"));
    CHECK(!tg::LooksLikeUiText("bad allocation"));
    CHECK(!tg::LooksLikeUiText("GetProcAddress"));
    CHECK(!tg::LooksLikeUiText("WEAPON_REVOLVER_CATTLEMAN"));
    CHECK(!tg::LooksLikeUiText("C:\\Games\\RDR2\\mod.ini"));
    CHECK(!tg::LooksLikeUiText("STEALTH: body found - "));
    CHECK(!tg::LooksLikeUiText("Failed to load %s"));
    CHECK(!tg::LooksLikeUiText("Adn"));
    CHECK(!tg::LooksLikeUiText("B8K8K~F~F"));
    CHECK(!tg::LooksLikeUiText("Monday"));
    CHECK(!tg::LooksLikeUiText("Unknown exception"));
    CHECK(!tg::LooksLikeUiText("!This program cannot be run in DOS mode.$"));
    CHECK(tg::LooksLikeUiText("MISSION FAILED"));
    const char blob[] = "\0\0Hideout cleared\0xx\0Reward: $%d\0bad allocation\0\x90\x91Wanted dead or alive\0";
    auto keys = tg::UiCandidates((const uint8_t*)blob, sizeof blob - 1);
    EQ(keys.size(), (size_t)3);
    EQ(keys[1], std::string("Reward: $*"));
  }
  // Google Cloud, Azure, MyMemory
  {
    const tg::Lang& ru = *tg::LangByGame(7);
    EQ(tg::GoogleCloudBody({"a"}, ru), std::string(R"({"q":["a"],"target":"ru","format":"text"})"));
    std::vector<std::string> out;
    CHECK(tg::ParseGoogleCloud(R"({"data":{"translations":[{"translatedText":"Да","detectedSourceLanguage":"en"},{"translatedText":"Нет"}]}})", out));
    EQ(out.size(), (size_t)2);
    EQ(out[1], std::string("Нет"));
    CHECK(!tg::ParseGoogleCloud(R"({"error":{"code":403}})", out));
    EQ(tg::MicrosoftBody({"a", "b\""}), std::string(R"([{"Text":"a"},{"Text":"b\""}])"));
    CHECK(tg::ParseMicrosoft(R"([{"detectedLanguage":{"language":"en","score":1.0},"translations":[{"text":"Привет","to":"ru"}]},{"translations":[{"text":"Мир","to":"ru"}]}])", out));
    EQ(out.size(), (size_t)2);
    EQ(out[0], std::string("Привет"));
    EQ(std::string(tg::LangByGame(12)->microsoft), std::string("zh-Hans"));
    std::string t;
    // answer captured from api.mymemory.translated.net 2026-11-08
    CHECK(tg::ParseMyMemory(R"({"responseData":{"translatedText":"Скрытие очищено. Разграбьте лагерь и нажмите {0}.","match":0.85},"quotaFinished":false,"mtLangSupported":null,"responseDetails":"","responseStatus":200})", t) == tg::MyMemoryResult::Ok);
    EQ(t, std::string("Скрытие очищено. Разграбьте лагерь и нажмите {0}."));
    CHECK(tg::ParseMyMemory(R"({"responseData":{"translatedText":"MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY."},"quotaFinished":true,"responseStatus":429})", t) == tg::MyMemoryResult::Quota);
    CHECK(tg::ParseMyMemory(R"({"responseData":{"translatedText":"MYMEMORY WARNING: YOU USED ALL AVAILABLE FREE TRANSLATIONS FOR TODAY. NEXT AVAILABLE IN 10 HOURS"},"responseStatus":"200"})", t) == tg::MyMemoryResult::Quota);
    CHECK(tg::ParseMyMemory(R"({"responseData":{"translatedText":"x"},"responseStatus":"403"})", t) == tg::MyMemoryResult::Bad);
  }
  // languages
  {
    EQ(std::string(tg::LangByGame(7)->code), std::string("ru"));
    EQ(std::string(tg::LangByCode("ZH-cn")->deepl), std::string("ZH-HANS"));
    CHECK(!tg::LangByGame(99));
    std::string sys = tg::LlmSystemPrompt(*tg::LangByGame(7), {{"Big Valley", "Большая долина"}});
    CHECK(sys.find("into Russian") != std::string::npos);
    CHECK(sys.find("Big Valley = Большая долина") != std::string::npos);
  }
  printf("%d checks, %d failed\n", g_count, g_fail);
  return g_fail ? 1 : 0;
}
