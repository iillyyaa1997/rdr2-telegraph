// core.hpp - the platform-independent part of Telegraph: UTF-8 helpers, a small
// JSON reader/writer, markup and number placeholders, request and response formats
// of the translation services, and the scanner that finds UI text inside a mod.
// No Windows headers here, so tests/test_core.cpp runs on any host.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace tg {

// ---------------------------------------------------------------- UTF-8

inline void AppendUtf8(std::string& o, uint32_t c) {
  if (c < 0x80) {
    o += (char)c;
  } else if (c < 0x800) {
    o += (char)(0xC0 | (c >> 6));
    o += (char)(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    o += (char)(0xE0 | (c >> 12));
    o += (char)(0x80 | ((c >> 6) & 0x3F));
    o += (char)(0x80 | (c & 0x3F));
  } else {
    o += (char)(0xF0 | (c >> 18));
    o += (char)(0x80 | ((c >> 12) & 0x3F));
    o += (char)(0x80 | ((c >> 6) & 0x3F));
    o += (char)(0x80 | (c & 0x3F));
  }
}

// Decodes one code point at s[i] and advances i; invalid bytes come out as themselves.
inline uint32_t NextUtf8(const std::string& s, size_t& i) {
  unsigned char c = (unsigned char)s[i++];
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t cp = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : extra == 1 ? c & 0x1F : c;
  for (int k = 0; k < extra; ++k) {
    if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return c;
    cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
  }
  return cp;
}

inline uint32_t UpperCp(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;            // Latin-1
  if (c >= 0x430 && c <= 0x44F) return c - 32;                         // Cyrillic а-я
  if (c >= 0x450 && c <= 0x45F) return c - 80;                         // ѐ-џ
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;           // Greek
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & ~1u;  // Latin Ext-A pairs
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
  return c;
}

inline std::string UpperUtf8(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size();) AppendUtf8(o, UpperCp(NextUtf8(s, i)));
  return o;
}

inline bool IsAsciiLetter(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// ---------------------------------------------------------------- JSON

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* Get(const char* key) const {
    if (type != Object) return nullptr;
    for (const auto& p : obj)
      if (p.first == key) return &p.second;
    return nullptr;
  }
  const Json* At(size_t i) const { return type == Array && i < arr.size() ? &arr[i] : nullptr; }
};

class JsonReader {
 public:
  JsonReader(const char* p, const char* e) : p_(p), e_(e) {}
  bool Parse(Json& out) {
    if (!Value(out, 0)) return false;
    Ws();
    return p_ == e_;
  }
  bool ParsePrefix(Json& out) { return Value(out, 0); }
  const char* pos() const { return p_; }

 private:
  const char* p_;
  const char* e_;

  void Ws() {
    while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
  }
  bool Lit(const char* w) {
    size_t n = strlen(w);
    if ((size_t)(e_ - p_) < n || strncmp(p_, w, n) != 0) return false;
    p_ += n;
    return true;
  }
  static int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  bool Hex4(uint32_t& v) {
    if (e_ - p_ < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      int h = Hex(p_[i]);
      if (h < 0) return false;
      v = v * 16 + h;
    }
    p_ += 4;
    return true;
  }
  bool Str(std::string& s) {
    if (p_ >= e_ || *p_ != '"') return false;
    ++p_;
    while (p_ < e_ && *p_ != '"') {
      if (*p_ != '\\') {
        s += *p_++;
        continue;
      }
      if (++p_ >= e_) return false;
      char c = *p_++;
      switch (c) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!Hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2;
            uint32_t lo;
            if (!Hex4(lo)) return false;
            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(s, cp);
          break;
        }
        default: return false;
      }
    }
    if (p_ >= e_) return false;
    ++p_;
    return true;
  }
  bool Value(Json& v, int depth) {
    if (depth > 64) return false;
    Ws();
    if (p_ >= e_) return false;
    char c = *p_;
    if (c == '{') {
      ++p_;
      v.type = Json::Object;
      Ws();
      if (p_ < e_ && *p_ == '}') return ++p_, true;
      for (;;) {
        Ws();
        std::string k;
        if (!Str(k)) return false;
        Ws();
        if (p_ >= e_ || *p_ != ':') return false;
        ++p_;
        v.obj.emplace_back(std::move(k), Json());
        if (!Value(v.obj.back().second, depth + 1)) return false;
        Ws();
        if (p_ < e_ && *p_ == ',') { ++p_; continue; }
        if (p_ < e_ && *p_ == '}') return ++p_, true;
        return false;
      }
    }
    if (c == '[') {
      ++p_;
      v.type = Json::Array;
      Ws();
      if (p_ < e_ && *p_ == ']') return ++p_, true;
      for (;;) {
        v.arr.emplace_back();
        if (!Value(v.arr.back(), depth + 1)) return false;
        Ws();
        if (p_ < e_ && *p_ == ',') { ++p_; continue; }
        if (p_ < e_ && *p_ == ']') return ++p_, true;
        return false;
      }
    }
    if (c == '"') {
      v.type = Json::String;
      return Str(v.str);
    }
    if (Lit("true")) { v.type = Json::Bool; v.b = true; return true; }
    if (Lit("false")) { v.type = Json::Bool; return true; }
    if (Lit("null")) { v.type = Json::Null; return true; }
    const char* s = p_;
    while (p_ < e_ && (strchr("+-.eE", *p_) || (*p_ >= '0' && *p_ <= '9'))) ++p_;
    if (p_ == s) return false;
    v.type = Json::Number;
    v.num = strtod(std::string(s, p_).c_str(), nullptr);
    return true;
  }
};

inline bool ParseJson(const std::string& s, Json& out) {
  JsonReader r(s.data(), s.data() + s.size());
  return r.Parse(out);
}

inline std::string JsonQuote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += (char)c;
        }
    }
  }
  return o + "\"";
}

inline std::string JsonStringArray(const std::vector<std::string>& v) {
  std::string o = "[";
  for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + JsonQuote(v[i]);
  return o + "]";
}

// ---------------------------------------------------------------- markup and numbers

// Length of a markup token at s[i] that must reach the screen unchanged, or 0:
// ~COLOR_RED~ ~n~ ~INPUT_X~, <font ...>, printf specs, {0}, |n| (word-wrap marker
// of some mods), line breaks and tabs.
inline size_t MarkupAt(const std::string& s, size_t i) {
  char c = s[i];
  if (c == '\n' || c == '\r' || c == '\t') return 1;
  if (c == '~') {
    size_t j = i + 1;
    while (j < s.size() && j - i <= 48 && (IsAsciiLetter((unsigned char)s[j]) || s[j] == '_' ||
                                            (s[j] >= '0' && s[j] <= '9')))
      ++j;
    if (j < s.size() && s[j] == '~' && j > i + 1) return j - i + 1;
    return 0;
  }
  if (c == '<') {
    size_t j = i + 1;
    if (j >= s.size() || !(IsAsciiLetter((unsigned char)s[j]) || s[j] == '/')) return 0;
    while (j < s.size() && j - i <= 96 && s[j] != '>' && s[j] != '<') ++j;
    if (j < s.size() && s[j] == '>') return j - i + 1;
    return 0;
  }
  if (c == '{') {
    size_t j = i + 1;
    while (j < s.size() && j - i <= 24 && s[j] != '}' && s[j] != '{') ++j;
    if (j < s.size() && s[j] == '}') return j - i + 1;
    return 0;
  }
  if (c == '|' && s.compare(i, 3, "|n|") == 0) return 3;
  if (c == '%') {
    size_t j = i + 1;
    if (j < s.size() && s[j] == '%') return 2;
    while (j < s.size() && strchr("-+ #0", s[j])) ++j;
    while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '*')) ++j;
    if (j < s.size() && s[j] == '.') {
      ++j;
      while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    }
    while (j < s.size() && strchr("hlLzjt", s[j])) ++j;
    if (j < s.size() && strchr("diouxXfFeEgGaAsScp", s[j])) return j - i + 1;
    return 0;
  }
  return 0;
}

// A number token at s[i]: digits, with inner . , : between digits ("1,250.50", "10:30").
inline size_t NumberAt(const std::string& s, size_t i) {
  if (!(s[i] >= '0' && s[i] <= '9')) return 0;
  size_t j = i;
  for (;;) {
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    if (j + 1 < s.size() && (s[j] == '.' || s[j] == ',' || s[j] == ':') && s[j + 1] >= '0' && s[j + 1] <= '9') {
      ++j;
      continue;
    }
    return j - i;
  }
}

// Text without markup tokens (for counting letters).
inline std::string StripMarkup(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    size_t m = MarkupAt(s, i);
    if (m) {
      o += ' ';
      i += m;
    } else {
      o += s[i++];
    }
  }
  return o;
}

// The cache key of a text: every number becomes '*', so "Reward: $25" and "Reward: $40"
// share one translation "Награда: ${1}". Texts that already contain '*' or '{' keep
// their numbers (literal key, no captures).
struct Skeleton {
  std::string key;
  std::vector<std::string> nums;
  bool literal = false;
};

inline Skeleton MakeSkeleton(const std::string& s) {
  Skeleton k;
  if (s.find('*') != std::string::npos || s.find('{') != std::string::npos) {
    k.key = s;
    k.literal = true;
    return k;
  }
  for (size_t i = 0; i < s.size();) {
    size_t m = MarkupAt(s, i);
    if (m) {
      k.key.append(s, i, m);
      i += m;
      continue;
    }
    // digits glued to a letter on the left belong to a word or name (M1899)
    if (i > 0 && (IsAsciiLetter((unsigned char)s[i - 1]) || s[i - 1] == '_') && s[i] >= '0' && s[i] <= '9') {
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') k.key += s[i++];
      continue;
    }
    size_t n = NumberAt(s, i);
    if (n) {
      k.nums.emplace_back(s, i, n);
      k.key += '*';
      i += n;
      continue;
    }
    k.key += s[i++];
  }
  return k;
}

// Puts the numbers back: {1} {2} ... in a cached translation refer to the '*' of its key.
inline std::string FillNumbers(const std::string& value, const std::vector<std::string>& nums) {
  if (nums.empty()) return value;
  std::string o;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '{' && i + 2 < value.size() && value[i + 1] >= '1' && value[i + 1] <= '9') {
      size_t j = i + 1;
      int n = 0;
      while (j < value.size() && value[j] >= '0' && value[j] <= '9' && j - i < 4) n = n * 10 + (value[j++] - '0');
      if (j < value.size() && value[j] == '}' && n >= 1 && (size_t)n <= nums.size()) {
        o += nums[n - 1];
        i = j;
        continue;
      }
    }
    o += value[i];
  }
  return o;
}

// What goes to a translation service: markup and numbers replaced with {0} {1} ...
struct Protected {
  std::string text;
  std::vector<std::string> marks;  // literal markup per placeholder ("" for a number)
  std::vector<int> number;         // per placeholder: 0-based number capture, or -1
};

inline Protected Protect(const std::string& key, bool literal) {
  Protected p;
  int nnum = 0;
  for (size_t i = 0; i < key.size();) {
    size_t m = MarkupAt(key, i);
    if (m || (!literal && key[i] == '*')) {
      p.text += "{" + std::to_string(p.marks.size()) + "}";
      if (m) {
        p.marks.push_back(key.substr(i, m));
        p.number.push_back(-1);
        i += m;
      } else {
        p.marks.emplace_back();
        p.number.push_back(nnum++);
        ++i;
      }
      continue;
    }
    p.text += key[i++];
  }
  return p;
}

inline bool IsAllCapsText(const std::string& s) {
  int letters = 0;
  for (unsigned char c : s) {
    if (c >= 'a' && c <= 'z') return false;
    if (c >= 'A' && c <= 'Z') ++letters;
  }
  return letters >= 2;
}

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Tidies a translation of a protected text: keeps the source's outer spaces, drops a
// full stop the service added, keeps ALL CAPS, then puts the markup back and turns
// number placeholders into {1} {2}. False if a placeholder got lost or duplicated.
inline bool Restore(const Protected& p, std::string tr, std::string& value) {
  const std::string& src = p.text;
  size_t a = 0, b = src.size();
  while (a < b && src[a] == ' ') ++a;
  while (b > a && src[b - 1] == ' ') --b;
  std::string lead = src.substr(0, a), trail = src.substr(b);
  size_t x = 0, y = tr.size();
  while (x < y && IsSpace(tr[x])) ++x;
  while (y > x && IsSpace(tr[y - 1])) --y;
  tr = tr.substr(x, y - x);
  if (tr.empty() && b > a) return false;
  char last = b > a ? src[b - 1] : 0;
  if (last && !strchr(".!?:;", last) && tr.size() >= 2 && tr.back() == '.' && tr[tr.size() - 2] != '.') tr.pop_back();
  if (IsAllCapsText(src)) tr = UpperUtf8(tr);
  // placeholders, tolerating spaces inside the braces
  std::vector<int> seen(p.marks.size(), 0);
  std::string out;
  for (size_t i = 0; i < tr.size(); ++i) {
    if (tr[i] == '{') {
      size_t j = i + 1;
      while (j < tr.size() && tr[j] == ' ') ++j;
      size_t d = j;
      int n = 0;
      while (j < tr.size() && tr[j] >= '0' && tr[j] <= '9' && j - d < 4) n = n * 10 + (tr[j++] - '0');
      bool digits = j > d;
      while (j < tr.size() && tr[j] == ' ') ++j;
      if (digits && j < tr.size() && tr[j] == '}') {
        if (n < 0 || (size_t)n >= p.marks.size()) return false;
        if (++seen[n] > 1) return false;
        if (p.number[n] >= 0) out += "{" + std::to_string(p.number[n] + 1) + "}";
        else out += p.marks[n];
        i = j;
        continue;
      }
    }
    out += tr[i];
  }
  for (int s : seen)
    if (s != 1) return false;
  value = lead + out + trail;
  return true;
}

// Text that is mostly in a non-Latin script is already localized (the mod has its
// own translation, or it is a name in the player's language).
inline bool AlreadyLocalized(const std::string& s) {
  std::string t = StripMarkup(s);
  int ascii = 0, other = 0;
  for (size_t i = 0; i < t.size();) {
    uint32_t c = NextUtf8(t, i);
    if (c < 0x80) ascii += IsAsciiLetter((unsigned char)c);
    else if (c >= 0xC0) ++other;
  }
  return other > 0 && other >= ascii;
}

// Worth sending at all: at least two Latin letters outside markup, not already
// localized, not huge.
inline bool WorthTranslating(const std::string& s) {
  if (s.size() > 2000 || AlreadyLocalized(s)) return false;
  std::string t = StripMarkup(s);
  int letters = 0;
  for (unsigned char c : t) letters += IsAsciiLetter(c);
  return letters >= 2;
}

// ---------------------------------------------------------------- languages

struct Lang {
  int game;          // LOCALIZATION::GET_CURRENT_LANGUAGE value
  const char* code;  // cache folder and Telegraph.ini Language=
  const char* google;
  const char* deepl;
  const char* name;  // for the LLM prompt
};

inline const Lang* Langs(size_t& n) {
  static const Lang k[] = {
      {0, "en", "en", "EN-US", "English"},
      {1, "fr", "fr", "FR", "French"},
      {2, "de", "de", "DE", "German"},
      {3, "it", "it", "IT", "Italian"},
      {4, "es", "es", "ES", "Spanish (Spain)"},
      {5, "pt-BR", "pt", "PT-BR", "Brazilian Portuguese"},
      {6, "pl", "pl", "PL", "Polish"},
      {7, "ru", "ru", "RU", "Russian"},
      {8, "ko", "ko", "KO", "Korean"},
      {9, "zh-TW", "zh-TW", "ZH-HANT", "Traditional Chinese"},
      {10, "ja", "ja", "JA", "Japanese"},
      {11, "es-MX", "es", "ES", "Latin American Spanish"},
      {12, "zh-CN", "zh-CN", "ZH-HANS", "Simplified Chinese"},
  };
  n = sizeof k / sizeof k[0];
  return k;
}

inline const Lang* LangByGame(int id) {
  size_t n;
  const Lang* k = Langs(n);
  for (size_t i = 0; i < n; ++i)
    if (k[i].game == id) return &k[i];
  return nullptr;
}

inline bool SameCode(const char* a, const std::string& b) {
  if (strlen(a) != b.size()) return false;
  for (size_t i = 0; i < b.size(); ++i)
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}

inline const Lang* LangByCode(const std::string& code) {
  size_t n;
  const Lang* k = Langs(n);
  for (size_t i = 0; i < n; ++i)
    if (SameCode(k[i].code, code)) return &k[i];
  return nullptr;
}

// ---------------------------------------------------------------- services

inline std::string UrlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (IsAsciiLetter(c) || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      o += (char)c;
    } else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

// translate.googleapis.com/translate_a/single?client=gtx&dt=t: [[["out","in",...],...],null,"en",...]
inline bool ParseGoogle(const std::string& body, std::string& text, std::string& detected) {
  Json j;
  if (!ParseJson(body, j) || j.type != Json::Array) return false;
  const Json* segs = j.At(0);
  if (!segs || segs->type != Json::Array) return false;
  text.clear();
  for (const Json& seg : segs->arr) {
    const Json* t = seg.At(0);
    if (t && t->type == Json::String) text += t->str;
  }
  const Json* d = j.At(2);
  detected = d && d->type == Json::String ? d->str : std::string();
  return true;
}

inline std::vector<std::string> SplitLines(const std::string& s) {
  std::vector<std::string> r;
  size_t a = 0;
  for (;;) {
    size_t b = s.find('\n', a);
    std::string line = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    r.push_back(line);
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return r;
}

inline std::string DeepLBody(const std::vector<std::string>& texts, const Lang& lang) {
  return std::string("{\"text\":") + JsonStringArray(texts) + ",\"target_lang\":" + JsonQuote(lang.deepl) +
         ",\"preserve_formatting\":true}";
}

inline bool ParseDeepL(const std::string& body, std::vector<std::string>& out) {
  Json j;
  if (!ParseJson(body, j)) return false;
  const Json* tr = j.Get("translations");
  if (!tr || tr->type != Json::Array) return false;
  out.clear();
  for (const Json& t : tr->arr) {
    const Json* x = t.Get("text");
    if (!x || x->type != Json::String) return false;
    out.push_back(x->str);
  }
  return true;
}

inline std::string LlmSystemPrompt(const Lang& lang, const std::vector<std::pair<std::string, std::string>>& glossary) {
  std::string p =
      "You translate user-interface text of Red Dead Redemption 2 mods (story mode, the American West in 1899) "
      "into " + std::string(lang.name) +
      ". The user message is a JSON array of strings: menu items, prompts, notifications, subtitles. "
      "Reply with ONLY a JSON array of the same length and order with the translations, no comments. "
      "Keep every placeholder such as {0} or {1} exactly as written, in a natural position. "
      "Keep the style short like game UI and use the names the official " + std::string(lang.name) +
      " version of the game uses for places, items and people. Keep ALL CAPS strings in capitals. "
      "If a string is already in " + std::string(lang.name) + " or is not natural language, return it unchanged.";
  if (!glossary.empty()) {
    p += "\nGlossary (use these translations):";
    for (const auto& g : glossary) p += "\n" + g.first + " = " + g.second;
  }
  return p;
}

inline std::string ChatBody(const std::string& model, const std::string& system, const std::string& user) {
  std::string b = "{";
  if (!model.empty()) b += "\"model\":" + JsonQuote(model) + ",";
  b += "\"temperature\":0.2,\"messages\":[{\"role\":\"system\",\"content\":" + JsonQuote(system) +
       "},{\"role\":\"user\",\"content\":" + JsonQuote(user) + "}]}";
  return b;
}

inline bool ParseChat(const std::string& body, std::string& content) {
  Json j;
  if (!ParseJson(body, j)) return false;
  const Json* ch = j.Get("choices");
  const Json* c0 = ch ? ch->At(0) : nullptr;
  const Json* msg = c0 ? c0->Get("message") : nullptr;
  const Json* ct = msg ? msg->Get("content") : nullptr;
  if (!ct || ct->type != Json::String) return false;
  content = ct->str;
  return true;
}

// The first JSON array of strings inside an LLM reply (it may wrap it in ```json).
inline bool ParseStringArray(const std::string& content, std::vector<std::string>& out) {
  for (size_t start = content.find('['); start != std::string::npos; start = content.find('[', start + 1)) {
    JsonReader r(content.data() + start, content.data() + content.size());
    Json j;
    if (!r.ParsePrefix(j) || j.type != Json::Array) continue;
    out.clear();
    bool ok = true;
    for (const Json& e : j.arr) {
      if (e.type != Json::String) { ok = false; break; }
      out.push_back(e.str);
    }
    if (ok) return true;
  }
  return false;
}

// ---------------------------------------------------------------- dictionary files

inline std::string Unescape(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[i + 1];
      if (n == 'n') { r += '\n'; ++i; continue; }
      if (n == 't') { r += '\t'; ++i; continue; }
      if (n == 'r') { r += '\r'; ++i; continue; }
      if (n == '\\') { r += '\\'; ++i; continue; }
    }
    r += s[i];
  }
  return r;
}

inline std::string Escape(const std::string& s) {
  std::string r;
  for (char c : s) {
    if (c == '\n') r += "\\n";
    else if (c == '\t') r += "\\t";
    else if (c == '\r') r += "\\r";
    else if (c == '\\') r += "\\\\";
    else r += c;
  }
  return r;
}

// "key => value" lines; # comments; BOM tolerated. Calls f(key, value) per entry
// with the raw (still escaped) key, so the caller can look at a leading '~'.
template <class F>
void ForEachEntry(std::string data, F f) {
  if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
      (unsigned char)data[2] == 0xBF)
    data.erase(0, 3);
  size_t pos = 0;
  while (pos < data.size()) {
    size_t e = data.find('\n', pos);
    if (e == std::string::npos) e = data.size();
    std::string line = data.substr(pos, e - pos);
    pos = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t sep = line.find(" => ");
    if (sep == std::string::npos || sep == 0) continue;
    f(line.substr(0, sep), line.substr(sep + 4));
  }
}

// ---------------------------------------------------------------- '*' patterns

struct Pattern {
  std::vector<std::string> parts;  // literal pieces between '*'
  std::string out;                 // '*' = next capture, or {1} {2} = capture by number
  bool indexed = false;
};

inline bool HasIndexRefs(const std::string& out) {
  for (size_t i = 0; i + 2 < out.size(); ++i)
    if (out[i] == '{' && out[i + 1] >= '1' && out[i + 1] <= '9' && out[i + 2] == '}') return true;
  return false;
}

inline Pattern MakePattern(const std::string& key, const std::string& out) {
  Pattern p;
  size_t a = 0;
  for (;;) {
    size_t b = key.find('*', a);
    p.parts.push_back(key.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  p.out = out;
  p.indexed = HasIndexRefs(out);
  return p;
}

// Glob with '*' captures, leftmost-shortest per star with backtracking.
inline bool MatchParts(const std::vector<std::string>& parts, size_t pi, const char* s, std::vector<std::string>& caps) {
  if (pi == parts.size()) return *s == '\0';
  const std::string& lit = parts[pi];
  if (pi == 0) {
    if (strncmp(s, lit.c_str(), lit.size()) != 0) return false;
    return MatchParts(parts, 1, s + lit.size(), caps);
  }
  // a capture precedes this literal
  if (pi == parts.size() - 1 && lit.empty()) {
    caps.emplace_back(s);
    return true;
  }
  for (const char* p = s; *p || lit.empty(); ++p) {
    if (strncmp(p, lit.c_str(), lit.size()) == 0) {
      caps.emplace_back(s, p - s);
      if (MatchParts(parts, pi + 1, p + lit.size(), caps)) return true;
      caps.pop_back();
    }
    if (!*p) break;
  }
  return false;
}

inline bool ApplyPattern(const Pattern& p, const char* s, std::string& out) {
  std::vector<std::string> caps;
  if (!MatchParts(p.parts, 0, s, caps)) return false;
  if (p.indexed) {
    out = FillNumbers(p.out, caps);
    return true;
  }
  out.clear();
  size_t ci = 0;
  for (char c : p.out) {
    if (c == '*' && ci < caps.size()) out += caps[ci++];
    else out += c;
  }
  return true;
}

// ---------------------------------------------------------------- the .asi scanner

// NUL-terminated printable ASCII runs of a file (string literals of a compiled mod).
inline std::vector<std::string> ExtractStrings(const uint8_t* d, size_t n, size_t minLen = 3) {
  std::vector<std::string> r;
  size_t start = 0;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = d[i];
    bool printable = (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\t';
    if (printable) continue;
    if (c == 0 && i - start >= minLen) r.emplace_back((const char*)d + start, i - start);
    start = i + 1;
  }
  return r;
}

// Turns printf number specs into "0" so the scanned text gets the same key as the
// formatted text on screen ("Reward: $%d" and "Reward: $25" -> "Reward: $*").
// Empty if the text has other specs (%s: the shown text differs anyway).
inline std::string FormatToNumbers(const std::string& s) {
  if (s.find('%') == std::string::npos) return s;
  std::string o;
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '%') {
      size_t m = MarkupAt(s, i);
      if (m == 2 && s[i + 1] == '%') { o += '%'; i += 2; continue; }
      if (!m) return std::string();
      char conv = s[i + m - 1];
      if (!strchr("diufFeEgG", conv)) return std::string();
      o += '0';
      i += m;
      continue;
    }
    o += s[i++];
  }
  return o;
}

inline bool ContainsAny(const std::string& s, std::initializer_list<const char*> list) {
  for (const char* w : list)
    if (s.find(w) != std::string::npos) return true;
  return false;
}

inline bool StartsWithAny(const std::string& s, std::initializer_list<const char*> list) {
  for (const char* w : list)
    if (s.compare(0, strlen(w), w) == 0) return true;
  return false;
}

// Does a string literal of a mod look like text a player will see? Errs on the side
// of skipping: anything missed here is still translated when it reaches the screen.
inline bool LooksLikeUiText(const std::string& raw) {
  if (raw.size() < 2 || raw.size() > 600) return false;
  if (ContainsAny(raw, {"\\", "::", "->", "@@", "__", "//", "==", "!=", "&&", "||", "http", "www.", ".dll", ".asi",
                        ".ini", ".xml", ".dat", ".txt", ".json", ".log", ".exe", ".cpp", ".hpp", ".ytd", ".rpf",
                        ".ymt", ".meta", ".png", ".dds", ".wav", ".lua", "[%", "This program cannot be run", "%s", "%S", "%ls", "%p", "%x", "%X",
                        "%c", "{}", "${", "\t"}))
    return false;
  std::string t = StripMarkup(raw);
  // characters that do not occur in game UI text
  for (char c : t)
    if (strchr("_=;[]{}<>|#`^@\\", c)) return false;
  size_t a = 0, b = t.size();
  while (a < b && IsSpace(t[a])) ++a;
  while (b > a && IsSpace(t[b - 1])) --b;
  t = t.substr(a, b - a);
  if (t.size() < 2) return false;
  int letters = 0, upper = 0, digits = 0, spaces = 0, other = 0;
  for (unsigned char c : t) {
    if (IsAsciiLetter(c)) { ++letters; upper += (c >= 'A' && c <= 'Z'); }
    else if (c >= '0' && c <= '9') ++digits;
    else if (c == ' ') ++spaces;
    else ++other;
  }
  if (letters < 3) return false;
  if (letters * 10 < (int)(t.size() - spaces) * 6) return false;  // < 60% letters
  // the first letter is a capital
  for (unsigned char c : t) {
    if (IsAsciiLetter(c)) {
      if (c >= 'a' && c <= 'z') return false;
      break;
    }
  }
  // camelCase or PascalCase inside a word = code identifier
  for (size_t i = 1; i < t.size(); ++i)
    if (t[i - 1] >= 'a' && t[i - 1] <= 'z' && t[i] >= 'A' && t[i] <= 'Z') return false;
  if (!spaces) {
    // a single word: only "Cancel", "Bounty" style, not ALLCAPS ids or "key.value"
    if (t.size() > 24) return false;
    if (!(t[0] >= 'A' && t[0] <= 'Z')) return false;
    size_t i = 1;
    while (i < t.size() && t[i] >= 'a' && t[i] <= 'z') ++i;
    if (i < 4) return false;  // 3-letter runs are mostly bytes of code
    while (i < t.size() && strchr(".!?:", t[i])) ++i;
    if (i != t.size()) return false;
  }
  // a word that mixes letters and digits (ids, hashes, "B8K8K")
  for (size_t i = 0; i < t.size();) {
    size_t j = i;
    bool l = false, d = false;
    while (j < t.size() && t[j] != ' ') {
      l |= IsAsciiLetter((unsigned char)t[j]);
      d |= t[j] >= '0' && t[j] <= '9';
      ++j;
    }
    if (l && d) return false;
    i = j + 1;
  }
  // "STEALTH: body found", "LOG impact": an upper-case tag in front of a lower-case message
  {
    size_t i = 0;
    int caps = 0;
    while (i < t.size() && ((t[i] >= 'A' && t[i] <= 'Z') || t[i] == '-')) caps += t[i++] != '-';
    if (caps >= 3 && i < t.size() && (t[i] == ' ' || t[i] == ':')) {
      for (size_t k = i; k < t.size(); ++k)
        if (t[k] >= 'a' && t[k] <= 'z') return false;
    }
  }
  // C/C++ runtime and library messages, calendar names of the C runtime
  if (StartsWithAny(t, {"Unknown exception", "Microsoft", "Runtime Error", "Assertion", "Visual C++", "Program:",
                        "DOMAIN error", "SING error", "TLOSS error", "An application has made", "Attempt to use",
                        "Copyright", "This program cannot", "Rich"}))
    return false;
  if (!spaces) {
    static const char* kCrt[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
                                 "January", "February", "March", "April", "May", "June", "July", "August",
                                 "September", "October", "November", "December", "Sun", "Mon", "Tue", "Wed",
                                 "Thu", "Fri", "Sat", "Jan", "Feb", "Mar", "Apr", "Jun", "Jul", "Aug", "Sep",
                                 "Oct", "Nov", "Dec", "True", "False", "None", "Null", "Default"};
    for (const char* w : kCrt)
      if (t == w) return false;
  }
  (void)upper;
  (void)digits;
  (void)other;
  return true;
}

// Keys of the UI-looking string literals of a mod file, for pre-translation. A mod
// with hundreds of single words carries name lists (bandits, victims): those are
// left to on-screen translation.
inline std::vector<std::string> UiCandidates(const uint8_t* d, size_t n, size_t* droppedWords = nullptr) {
  std::vector<std::string> words, phrases;
  std::vector<std::string> seen;
  for (const std::string& s : ExtractStrings(d, n)) {
    if (!LooksLikeUiText(s)) continue;
    std::string f = FormatToNumbers(s);
    if (f.empty() || !WorthTranslating(f)) continue;
    std::string key = MakeSkeleton(f).key;
    (key.find(' ') == std::string::npos ? words : phrases).push_back(key);
  }
  if (droppedWords) *droppedWords = 0;
  if (words.size() > 200) {
    if (droppedWords) *droppedWords = words.size();
    words.clear();
  }
  phrases.insert(phrases.end(), words.begin(), words.end());
  std::vector<std::string> out;
  for (auto& k : phrases) {
    bool dup = false;
    for (auto& o : out)
      if (o == k) { dup = true; break; }
    if (!dup) out.push_back(k);
  }
  return out;
}

}  // namespace tg
