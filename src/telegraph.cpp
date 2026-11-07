// Telegraph.asi - Mod Auto Translator for Red Dead Redemption 2.
//
// Shows the hard-coded English text of ScriptHookRDR2 mods in the language of the
// game. Text comes from, in this order:
//   1. your dictionaries: <game>\Telegraph\<Mod>.txt (and <game>\RuText\*.txt)
//   2. the translation cache: <game>\Telegraph\cache\<lang>\<Mod>.txt
//   3. a translation service (Google without a key by default; DeepL, DeepSeek or
//      any OpenAI-compatible model with your key), in a background thread. Until the
//      answer arrives the original text is shown; the next redraw shows the
//      translation and it is written to the cache.
//
// How: mods built against ScriptHookRDR2 call natives through three exports of
// ScriptHookRDR2.dll: nativeInit(hash), nativePush64(arg) for every argument and
// nativeCall(). Text reaches the screen through CREATE_VAR_STRING(flags,
// "LITERAL_STRING", text) and DATABINDING_ADD_DATA_STRING(parent, name, text).
// Telegraph rewrites the import table entries of those three exports in every loaded
// module that imports them, and when a text argument is pushed it swaps the pointer
// for the translated string. Mod files stay untouched. Plugins that chain behind
// Telegraph (HideoutMemory) patch after it and tail-call into it.
//
// Dictionary format (UTF-8): the part of the file name before the first dot selects
// the module (PedDamageOverhaul.txt applies to PedDamageOverhaul.asi); "_common"
// applies to every module.
//   English text => Translated text      exact match
//   Load Slot * => Загрузить слот *      pattern, each * captures text
//   Reward: * => Награда: {1}            pattern with numbered captures
//   ~Big Valley => Большая долина        replaced inside any longer string
//   # comment; \n and \t escapes work on both sides

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core.hpp"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace {

constexpr const char* kVersion = "1.0.0";

constexpr uint64_t kCreateVarString = 0xFA925AC00EB830B9ULL;
constexpr uint64_t kDatabindingAddDataString = 0x617FCA1C5652BBADULL;
constexpr uint64_t kGetCurrentLanguage = 0xDB917DA5C6835FCCULL;

constexpr const char* kExpInit = "?nativeInit@@YAX_K@Z";
constexpr const char* kExpPush = "?nativePush64@@YAX_K@Z";
constexpr const char* kExpCall = "?nativeCall@@YAPEA_KXZ";
constexpr const char* kExpScriptRegister = "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z";
constexpr const char* kExpScriptWait = "?scriptWait@@YAXK@Z";

using FnInit = void (*)(uint64_t);
using FnPush = void (*)(uint64_t);
using FnCall = uint64_t* (*)();
using FnScriptRegister = void (*)(HMODULE, void (*)());
using FnScriptWait = void (*)(DWORD);

HMODULE g_self = nullptr;
HMODULE g_sh = nullptr;
FnInit g_origInit = nullptr;
FnPush g_origPush = nullptr;
FnCall g_origCall = nullptr;
FnScriptWait g_scriptWait = nullptr;

std::wstring g_dir;      // <game>\Telegraph
std::wstring g_gameDir;  // <game>

// One lock for dictionaries, cache, queue, memo and settings. The game thread holds it
// only for lookups; the worker holds it only to take jobs and store results.
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_wake = CONDITION_VARIABLE_INIT;

struct Settings {
  bool enabled = true, autoTranslate = true, preTranslate = true, capture = false, useRuText = true;
  std::string language = "auto";
  std::vector<std::string> providers;
  int maxPerDay = 2000;
  int googleRate = 3, deeplRate = 30, deepseekRate = 30, openaiRate = 30;  // requests per minute
  int googleChars = 2000, googlePreHour = 30000;  // characters per request; pre-translation per hour
  std::string deeplKey;
  std::string deepseekKey, deepseekModel, deepseekUrl;
  std::string openaiKey, openaiModel, openaiUrl;
};
Settings g_set;
std::atomic<bool> g_enabled{true};  // read on every pushed argument

// ---------------------------------------------------------------- logging

FILE* g_log = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;

void Log(const char* fmt, ...) {
  if (!g_log) return;
  AcquireSRWLockExclusive(&g_logLock);
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
  ReleaseSRWLockExclusive(&g_logLock);
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && tg::IsSpace(s[a])) ++a;
  while (b > a && tg::IsSpace(s[b - 1])) --b;
  return s.substr(a, b - a);
}

std::string Short(const std::string& s, size_t n = 60) {
  std::string e = tg::Escape(s);
  if (e.size() <= n) return e;
  size_t k = n;
  while (k > 0 && ((unsigned char)e[k] & 0xC0) == 0x80) --k;
  return e.substr(0, k) + "...";
}

bool ReadFile(const std::wstring& path, std::string& data) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  data.clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
  fclose(f);
  return true;
}

bool DirExists(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------- dictionaries

// A long text a mod word-wraps itself before drawing: each drawn line reaches us
// alone. Paragraphs are separated by the mod's own "|n|" marker on both sides.
struct LongEntry {
  std::string en, ru;
  std::vector<std::pair<size_t, size_t>> enPara, ruPara;  // [begin, end) per paragraph
};

struct Scope {
  std::unordered_map<std::string, std::string> exact;
  std::unordered_map<std::string, std::string> lowerExact;  // ASCII-lowercased keys
  std::vector<tg::Pattern> patterns;
  std::vector<std::pair<std::string, std::string>> subs;  // longest first
  std::vector<LongEntry> longs;                           // for wrapped lines
};

std::vector<std::pair<size_t, size_t>> Paragraphs(const std::string& s) {
  std::vector<std::pair<size_t, size_t>> r;
  size_t a = 0;
  for (;;) {
    size_t b = s.find("|n|", a);
    r.emplace_back(a, b == std::string::npos ? s.size() : b);
    if (b == std::string::npos) break;
    a = b + 3;
  }
  return r;
}

using ScopeMap = std::unordered_map<std::string, Scope>;  // key: lower module name or "_common"
ScopeMap* g_dict = new ScopeMap;
std::vector<ScopeMap*> g_oldDicts;  // kept alive: the game may still hold pointers into them

// Entries of a file go into its scope. A lower-priority file (RuText folder) does not
// override exact entries that are already there.
void LoadDictFile(const std::wstring& path, const std::string& scopeName, bool lowPriority) {
  std::string data;
  if (!ReadFile(path, data)) return;
  Scope& sc = (*g_dict)[scopeName];
  int exact = 0, pats = 0, subs = 0;
  tg::ForEachEntry(std::move(data), [&](const std::string& rawKey, const std::string& rawVal) {
    std::string val = tg::Unescape(rawVal);
    if (rawKey[0] == '~') {
      std::string key = tg::Unescape(rawKey.substr(1));
      if (!key.empty()) { sc.subs.emplace_back(key, val); ++subs; }
    } else if (rawKey.find('*') != std::string::npos) {
      sc.patterns.push_back(tg::MakePattern(tg::Unescape(rawKey), val));
      ++pats;
    } else {
      std::string k = tg::Unescape(rawKey);
      if (lowPriority && sc.exact.count(k)) return;
      sc.lowerExact[Lower(k)] = val;
      sc.exact[k] = val;
      ++exact;
    }
  });
  Log("dictionary %s -> '%s': %d exact, %d patterns, %d substrings", Narrow(path).c_str(), scopeName.c_str(),
      exact, pats, subs);
}

void FinishScopes() {
  for (auto& [name, sc] : *g_dict) {
    std::stable_sort(sc.subs.begin(), sc.subs.end(),
                     [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    sc.longs.clear();
    for (const auto& [en, ru] : sc.exact) {
      if (en.size() < 40 || en.find(' ') == std::string::npos) continue;
      LongEntry le{en, ru, Paragraphs(en), Paragraphs(ru)};
      if (le.enPara.size() != le.ruPara.size()) {
        Log("  wrapped-line support off for an entry: %zu vs %zu paragraphs: %s", le.enPara.size(),
            le.ruPara.size(), Short(en).c_str());
        continue;
      }
      sc.longs.push_back(std::move(le));
    }
  }
}

void LoadDictFolder(const std::wstring& dir, bool lowPriority) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.txt").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::string name = Narrow(fd.cFileName);
    std::string scope = Lower(name.substr(0, name.find('.')));
    LoadDictFile(dir + L"\\" + fd.cFileName, scope, lowPriority);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

void LoadDictionaries() {
  LoadDictFolder(g_dir, false);
  std::wstring rt = g_gameDir + L"\\RuText";
  if (g_set.useRuText && DirExists(rt)) LoadDictFolder(rt, true);
  FinishScopes();
}

bool ApplySubs(const Scope* const* scopes, int ns, std::string& text) {
  bool changed = false;
  for (int i = 0; i < ns; ++i) {
    if (!scopes[i]) continue;
    for (const auto& [from, to] : scopes[i]->subs) {
      size_t p = 0;
      while ((p = text.find(from, p)) != std::string::npos) {
        text.replace(p, from.size(), to);
        p += to.size();
        changed = true;
      }
    }
  }
  return changed;
}

// Whole-string lookup: exact, then case-insensitive for ALL-CAPS text (result in
// caps), then patterns. Substring rules are applied by the caller.
bool LookupWhole(const Scope* const* scopes, int ns, const std::string& s, std::string& out) {
  for (int i = 0; i < ns; ++i) {
    if (!scopes[i]) continue;
    auto e = scopes[i]->exact.find(s);
    if (e != scopes[i]->exact.end()) { out = e->second; return true; }
  }
  if (tg::IsAllCapsText(s)) {
    std::string low = Lower(s);
    for (int i = 0; i < ns; ++i) {
      if (!scopes[i]) continue;
      auto e = scopes[i]->lowerExact.find(low);
      if (e != scopes[i]->lowerExact.end()) { out = tg::UpperUtf8(e->second); return true; }
    }
  }
  for (int i = 0; i < ns; ++i) {
    if (!scopes[i]) continue;
    for (const tg::Pattern& p : scopes[i]->patterns)
      if (tg::ApplyPattern(p, s.c_str(), out)) return true;
  }
  return false;
}

// Translated position for an English word boundary: same relative place inside the
// paragraph, snapped to the nearest space. Neighbouring lines share the boundary,
// so their translated pieces tile the paragraph without gaps or overlaps.
size_t SnapBoundary(const std::string& ru, size_t rb, size_t re, double frac) {
  if (frac <= 0.0) return rb;
  if (frac >= 1.0) return re;
  size_t t = rb + (size_t)((re - rb) * frac + 0.5);
  for (size_t d = 0; d < re - rb; ++d) {
    if (t >= rb + d && ru[t - d] == ' ') return t - d;
    if (t + d < re && ru[t + d] == ' ') return t + d;
  }
  return t;
}

bool UtfBoundary(const std::string& s, size_t i) {
  return i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80;
}

struct LastFrag {
  const LongEntry* entry = nullptr;
  size_t end = 0;
};
thread_local LastFrag t_last;
thread_local bool t_last_hit = false;

bool FragmentLookup(const Scope* const* scopes, int ns, const std::string& f, std::string& out,
                    bool& contextual) {
  const LongEntry* best = nullptr;
  size_t pos = std::string::npos;
  contextual = false;
  // the next line of the text matched last time comes first; this also catches
  // short last lines ("lands.") that are too ambiguous to search for on their own
  if (t_last.entry && !f.empty()) {
    size_t p = t_last.entry->en.find(f, t_last.end > 3 ? t_last.end - 3 : 0);
    if (p != std::string::npos && p <= t_last.end + 4) { best = t_last.entry; pos = p; contextual = true; }
  }
  if (!best && (f.size() < 12 || f.find(' ') == std::string::npos)) return false;
  for (int i = 0; i < ns && !best; ++i) {
    if (!scopes[i]) continue;
    for (const LongEntry& le : scopes[i]->longs) {
      size_t p = le.en.find(f);
      if (p == std::string::npos) continue;
      bool startOk = p == 0 || le.en[p - 1] == ' ' || le.en.compare(p >= 3 ? p - 3 : 0, 3, "|n|") == 0;
      if (!startOk) continue;
      if (!best || (p == 0 && pos != 0)) { best = &le; pos = p; }
      if (pos == 0) break;
    }
  }
  if (!best) return false;
  size_t endPos = pos + f.size();
  size_t k = 0;
  while (k + 1 < best->enPara.size() && pos >= best->enPara[k + 1].first) ++k;
  auto [eb, ee] = best->enPara[k];
  auto [rb, re] = best->ruPara[k];
  if (ee <= eb) return false;
  size_t startB = (pos > eb && best->en[pos - 1] == ' ') ? pos - 1 : pos;
  double fa = (double)(startB - eb) / (ee - eb), fb = (double)(std::min(endPos, ee) - eb) / (ee - eb);
  size_t a = SnapBoundary(best->ru, rb, re, fa), b = SnapBoundary(best->ru, rb, re, fb);
  if (a > rb && a < re && best->ru[a] == ' ') ++a;
  while (a < b && !UtfBoundary(best->ru, a)) ++a;
  while (b > a && !UtfBoundary(best->ru, b)) --b;
  out = b > a ? best->ru.substr(a, b - a) : std::string();
  while (!out.empty() && out.back() == ' ') out.pop_back();
  t_last.entry = best;
  t_last.end = endPos;
  return true;
}

// ---------------------------------------------------------------- modules

struct ModInfo {
  std::string name;  // lower base name, e.g. "peddamageoverhaul"
  std::string file;  // base name as on disk, e.g. "PedDamageOverhaul"
  std::wstring path;
  uintptr_t base = 0, end = 0;
  const Scope* scope = nullptr;
  std::unordered_set<std::string> logged;
  FILE* capture = nullptr;
  std::atomic<bool> scanned{false};
};

// patched modules; append-only so readers never see a reallocation
std::array<ModInfo*, 256> g_mods{};
std::atomic<size_t> g_modCount{0};

ModInfo* FindMod(uintptr_t addr) {
  size_t n = g_modCount.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (addr >= g_mods[i]->base && addr < g_mods[i]->end) return g_mods[i];
  return nullptr;
}

const Scope* g_common = nullptr;

// ---------------------------------------------------------------- machine translation state

const tg::Lang* g_target = nullptr;  // null: no machine translation (English game, off)
int g_generation = 0;                // bumped when the target or the cache changes
// module (lower) -> cache key -> translation
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_cache;
uint64_t g_cacheSig = 0;

struct Job {
  std::string module, key;
  bool literal = false, high = false;
  uint16_t maxBatch = 0;  // after an answer that did not fit: smaller batches
  ULONGLONG at = 0;       // when it was queued
  uint8_t tried = 0;  // provider bits that failed on this text
};
std::deque<Job> g_high, g_low;
ULONGLONG g_lastHighAt = 0;  // a menu that opens sends its lines over a few frames
std::unordered_set<std::string> g_pending, g_failed;  // module + '\x01' + key

// translated strings live forever (CREATE_VAR_STRING may keep the pointer briefly);
// set nodes never move, so their c_str() stays valid
std::unordered_set<std::string> g_intern;
const char* Intern(std::string s) { return g_intern.insert(std::move(s)).first->c_str(); }

struct MemoVal {
  const char* out;
  const LongEntry* entry;  // set when the text was a line of a word-wrapped entry
  size_t end;
};
std::unordered_map<std::string, MemoVal> g_memo;  // key: module + '\x01' + text
constexpr size_t kMemoMax = 200000;

std::wstring CacheDir() { return g_dir + L"\\cache\\" + Widen(g_target ? g_target->code : "none"); }

uint64_t FolderSignature(const std::wstring& dir, const wchar_t* const* exts, int nexts) {
  uint64_t sig = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::wstring n = fd.cFileName;
    if (n.size() < 4) continue;
    std::wstring ext = n.substr(n.size() - 4);
    bool want = false;
    for (int i = 0; i < nexts; ++i) want |= _wcsicmp(ext.c_str(), exts[i]) == 0;
    if (!want) continue;
    sig = sig * 1099511628211ULL + fd.nFileSizeLow + fd.ftLastWriteTime.dwLowDateTime;
    for (wchar_t c : n) sig = sig * 31 + c;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return sig;
}

uint64_t CacheSignature() {
  static const wchar_t* ext[] = {L".txt"};
  return FolderSignature(CacheDir(), ext, 1);
}

// g_lock held
void LoadCache() {
  g_cache.clear();
  if (!g_target) return;
  std::wstring dir = CacheDir();
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.txt").c_str(), &fd);
  int files = 0, entries = 0;
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      std::string name = Narrow(fd.cFileName);
      std::string module = Lower(name.substr(0, name.find('.')));
      std::string data;
      if (!ReadFile(dir + L"\\" + fd.cFileName, data)) continue;
      auto& map = g_cache[module];
      tg::ForEachEntry(std::move(data), [&](const std::string& k, const std::string& v) {
        map[tg::Unescape(k)] = tg::Unescape(v);
        ++entries;
      });
      ++files;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  g_cacheSig = CacheSignature();
  Log("cache %s: %d entries in %d files", Narrow(dir).c_str(), entries, files);
}

// g_lock held. Opened and closed per batch: NTFS updates the size and time that
// CacheSignature() sees when the handle closes.
void AppendCache(const ModInfo* m, const std::vector<std::pair<std::string, std::string>>& entries) {
  std::wstring dir = CacheDir();
  CreateDirectoryW((g_dir + L"\\cache").c_str(), nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);
  FILE* f = _wfopen((dir + L"\\" + Widen(m->file) + L".txt").c_str(), L"ab");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  {
    if (ftell(f) == 0) {
      fprintf(f,
              "# Telegraph machine translations of %s.asi into %s.\n"
              "# Edit a line to fix it (your edit is kept), delete a line to translate it again.\n"
              "# * in the English text stands for a number; {1} {2} put those numbers into the translation.\n"
              "# Lines you copy into Telegraph\\%s.txt win over this file.\n",
              m->file.c_str(), g_target->name, m->file.c_str());
    }
  }
  for (const auto& [key, value] : entries) fprintf(f, "%s => %s\n", tg::Escape(key).c_str(), tg::Escape(value).c_str());
  fclose(f);
}

ModInfo* ModByName(const std::string& name) {
  size_t n = g_modCount.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (g_mods[i]->name == name) return g_mods[i];
  return nullptr;
}

// g_lock held. Queues a text for the worker unless it is done, queued or given up.
void Enqueue(const std::string& module, const std::string& key, bool literal, bool high) {
  if (key.empty() || key[0] == '#' || key.find(" => ") != std::string::npos) return;  // would not survive the cache file
  std::string id = module + '\x01' + key;
  if (g_failed.count(id)) return;
  auto c = g_cache.find(module);
  if (c != g_cache.end() && c->second.count(key)) return;
  if (g_pending.count(id)) {
    if (!high) return;
    // on screen now: move it ahead of the pre-translation queue
    for (auto it = g_low.begin(); it != g_low.end(); ++it) {
      if (it->module == module && it->key == key) {
        Job j = *it;
        g_low.erase(it);
        j.high = true;
        g_high.push_front(j);
        WakeConditionVariable(&g_wake);
        break;
      }
    }
    return;
  }
  g_pending.insert(id);
  Job j;
  j.module = module;
  j.key = key;
  j.literal = literal;
  j.high = high;
  j.at = GetTickCount64();
  if (high) g_lastHighAt = j.at;
  (high ? g_high : g_low).push_back(std::move(j));
  WakeConditionVariable(&g_wake);
}

// ---------------------------------------------------------------- translate

void Capture(ModInfo* m, const char* s) {
  if (!g_set.capture || !m || !tg::WorthTranslating(s)) return;
  if (strlen(s) > 4000) return;
  if (!m->logged.insert(s).second) return;
  if (!m->capture) {
    std::wstring dir = g_dir + L"\\untranslated";
    CreateDirectoryW(dir.c_str(), nullptr);
    m->capture = _wfopen((dir + L"\\" + Widen(m->file) + L".txt").c_str(), L"ab");
  }
  if (m->capture) {
    fprintf(m->capture, "%s => \n", tg::Escape(s).c_str());
    fflush(m->capture);
  }
}

const char* Translate(ModInfo* m, const char* s) {
  if (!s || !*s) return s;
  std::string key = (m ? m->name : std::string("?")) + '\x01' + s;
  AcquireSRWLockExclusive(&g_lock);
  auto it = g_memo.find(key);
  if (it != g_memo.end()) {
    const MemoVal& mv = it->second;
    if (mv.entry) { t_last.entry = mv.entry; t_last.end = mv.end; }
    else t_last.entry = nullptr;  // something else was drawn between the lines
    const char* r = mv.out;
    ReleaseSRWLockExclusive(&g_lock);
    return r ? r : s;
  }
  bool memoize = true;
  t_last_hit = false;
  const Scope* scopes[2] = {m ? m->scope : nullptr, g_common};
  const char* result = nullptr;
  for (const Scope* sc : scopes) {
    if (!sc) continue;
    auto e = sc->exact.find(s);
    if (e != sc->exact.end()) { result = e->second.c_str(); break; }
  }
  std::string text = s, out;
  bool partial = false;  // shown until a full translation arrives
  if (!result) {
    bool whole = LookupWhole(scopes, 2, text, out);
    if (!whole) {
      // "Label: value" built at runtime: translate both sides on their own
      size_t c = text.find(": ");
      if (c != std::string::npos && c > 0) {
        std::string left = text.substr(0, c), right = text.substr(c + 2), l2, r2;
        bool lt = LookupWhole(scopes, 2, left, l2), rt = LookupWhole(scopes, 2, right, r2);
        if (lt || rt) {
          out = (lt ? l2 : left) + ": " + (rt ? r2 : right);
          if ((lt || !tg::WorthTranslating(left)) && (rt || !tg::WorthTranslating(right))) whole = true;
          else partial = true;
        }
      }
    }
    if (!whole && !partial) {
      bool contextual = false;
      whole = FragmentLookup(scopes, 2, text, out, contextual);
      if (whole) {
        t_last_hit = true;
        // a short piece found only through the previous line may belong to another
        // text elsewhere: do not cache it
        if (contextual && (text.size() < 12 || text.find(' ') == std::string::npos)) memoize = false;
      }
    }
    if (whole) {
      ApplySubs(scopes, 2, out);
      result = Intern(std::move(out));
    }
  }
  if (!result && m && g_target) {
    tg::Skeleton sk = tg::MakeSkeleton(text);
    auto c = g_cache.find(m->name);
    if (c != g_cache.end()) {
      auto e = c->second.find(sk.key);
      if (e != c->second.end()) result = Intern(tg::FillNumbers(e->second, sk.nums));
    }
    if (!result && tg::WorthTranslating(text) && sk.key.find(" => ") == std::string::npos)
      Enqueue(m->name, sk.key, sk.literal, true);
  }
  if (!result) {
    if (!partial) out = text;
    if (ApplySubs(scopes, 2, out)) partial = true;
    if (partial) result = Intern(std::move(out));
    else Capture(m, s);
  }
  if (!t_last_hit) t_last.entry = nullptr;
  if (memoize && g_memo.size() < kMemoMax)
    g_memo.emplace(std::move(key), MemoVal{result, t_last_hit ? t_last.entry : nullptr, t_last.end});
  ReleaseSRWLockExclusive(&g_lock);
  return result ? result : s;
}

bool ReadableCString(const void* p) {
  if ((uintptr_t)p < 0x10000) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
  if (mbi.State != MEM_COMMIT) return false;
  DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
             PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD)) return false;
  const char* end = (const char*)mbi.BaseAddress + mbi.RegionSize;
  for (const char* c = (const char*)p; c < end; ++c)
    if (*c == '\0') return true;
  return false;
}

// ---------------------------------------------------------------- hooks

struct CallState {
  uint64_t hash = 0;
  int index = 0;
  bool literal = false;
  bool active = false;
  ModInfo* mod = nullptr;
};
thread_local CallState t_call;

void HookInit(uint64_t hash) {
  t_call.hash = hash;
  t_call.index = 0;
  t_call.literal = false;
  t_call.active = (hash == kCreateVarString || hash == kDatabindingAddDataString);
  t_call.mod = t_call.active ? FindMod((uintptr_t)__builtin_return_address(0)) : nullptr;
  g_origInit(hash);
}

void HookPush(uint64_t value) {
  if (t_call.active && g_enabled.load(std::memory_order_relaxed)) {
    int i = t_call.index;
    if (t_call.hash == kCreateVarString) {
      if (i == 1) {
        const char* label = (const char*)value;
        t_call.literal = label && strcmp(label, "LITERAL_STRING") == 0;
      } else if (i == 2 && t_call.literal) {
        value = (uint64_t)Translate(t_call.mod, (const char*)value);
      }
    } else if (t_call.hash == kDatabindingAddDataString && i == 2) {
      if (ReadableCString((const void*)value)) value = (uint64_t)Translate(t_call.mod, (const char*)value);
    }
  }
  ++t_call.index;
  g_origPush(value);
}

uint64_t* HookCall() {
  t_call.active = false;
  return g_origCall();
}

// ---------------------------------------------------------------- import patching

// RuText is the older dictionary-only version of Telegraph: both rewrite the same
// import entries, so only one of them may run.
bool g_conflict = false;

bool RuTextLoaded() {
  if (g_conflict) return true;
  if (!GetModuleHandleW(L"RuText.asi")) return false;
  g_conflict = true;
  Log("RuText.asi is installed too. Telegraph replaces it: remove RuText.asi (keep the RuText folder, "
      "Telegraph reads its dictionaries). Telegraph stays off until then.");
  return true;
}

bool PatchModule(HMODULE mod) {
  auto base = (uint8_t*)mod;
  auto dos = (IMAGE_DOS_HEADER*)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  int patched = 0;
  for (auto d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
    if (_stricmp((const char*)(base + d->Name), "ScriptHookRDR2.dll") != 0) continue;
    if (!d->OriginalFirstThunk) continue;
    auto names = (IMAGE_THUNK_DATA64*)(base + d->OriginalFirstThunk);
    auto slots = (IMAGE_THUNK_DATA64*)(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
      const char* nm = (const char*)ibn->Name;
      uint64_t hook = 0;
      if (!strcmp(nm, kExpInit)) hook = (uint64_t)&HookInit;
      else if (!strcmp(nm, kExpPush)) hook = (uint64_t)&HookPush;
      else if (!strcmp(nm, kExpCall)) hook = (uint64_t)&HookCall;
      if (!hook || slots->u1.Function == hook) continue;
      DWORD old;
      if (VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) {
        slots->u1.Function = hook;
        VirtualProtect(&slots->u1.Function, 8, old, &old);
        ++patched;
      }
    }
  }
  return patched > 0;
}

SRWLOCK g_patchLock = SRWLOCK_INIT;

void PatchAll() {
  if (RuTextLoaded()) return;
  AcquireSRWLockExclusive(&g_patchLock);
  struct Unlock {
    ~Unlock() { ReleaseSRWLockExclusive(&g_patchLock); }
  } unlock;
  HMODULE mods[1024];
  DWORD need = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
  size_t n = std::min<size_t>(need / sizeof(HMODULE), 1024);
  for (size_t i = 0; i < n; ++i) {
    HMODULE m = mods[i];
    if (m == g_self || m == g_sh) continue;
    if (FindMod((uintptr_t)m)) continue;
    if (g_modCount.load() >= g_mods.size()) return;
    // only modules that import ScriptHookRDR2.dll
    if (!PatchModule(m)) continue;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring wp = path;
    std::wstring file = wp.substr(wp.find_last_of(L"\\/") + 1);
    std::string baseName = Narrow(file.substr(0, file.find_last_of(L'.')));
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), m, &info, sizeof info);
    auto mi = new ModInfo;
    mi->name = Lower(baseName);
    mi->file = baseName;
    mi->path = wp;
    mi->base = (uintptr_t)info.lpBaseOfDll;
    mi->end = mi->base + info.SizeOfImage;
    AcquireSRWLockExclusive(&g_lock);
    auto sc = g_dict->find(mi->name);
    mi->scope = sc == g_dict->end() ? nullptr : &sc->second;
    ReleaseSRWLockExclusive(&g_lock);
    g_mods[g_modCount.load()] = mi;
    g_modCount.fetch_add(1, std::memory_order_release);
    Log("patched %s (dictionary: %s)", Narrow(file).c_str(), mi->scope ? "yes" : "no");
  }
}

// ---------------------------------------------------------------- settings

std::wstring IniPath() { return g_dir + L"\\Telegraph.ini"; }

const char* kDefaultIni =
    "[Telegraph]\r\n"
    "; 0 = Telegraph changes nothing\r\n"
    "Enabled=1\r\n"
    "; machine translation of text that is not in your dictionaries\r\n"
    "AutoTranslate=1\r\n"
    "; target language: auto = the language of the game, or one of\r\n"
    "; fr de it es pt-BR pl ru ko zh-TW ja es-MX zh-CN\r\n"
    "Language=auto\r\n"
    "; services in the order they are tried; one without a key is skipped\r\n"
    "Providers=google,deepl,deepseek,openai\r\n"
    "; translate the text found inside each mod in the background, before it is shown\r\n"
    "PreTranslate=1\r\n"
    "; requests per day to all services together (0 = no limit)\r\n"
    "MaxPerDay=2000\r\n"
    "; 1 = list text without any translation in Telegraph\\untranslated\\<Mod>.txt\r\n"
    "Capture=0\r\n"
    "; 1 = also read the dictionaries in the RuText folder\r\n"
    "UseRuTextDictionaries=1\r\n"
    "\r\n"
    "[Google]\r\n"
    "; Google Translate without a key (the endpoint browser extensions use). It answers\r\n"
    "; \"too many requests\" for hours when asked too much: keep these low\r\n"
    "MaxPerMinute=3\r\n"
    "; characters per request\r\n"
    "MaxChars=2000\r\n"
    "; characters per hour for background pre-translation (0 = only text on screen)\r\n"
    "PreTranslateCharsPerHour=30000\r\n"
    "\r\n"
    "[DeepL]\r\n"
    "; API key (Free or Pro) from https://www.deepl.com/your-account/keys\r\n"
    "Key=\r\n"
    "MaxPerMinute=30\r\n"
    "\r\n"
    "[DeepSeek]\r\n"
    "; API key from https://platform.deepseek.com/api_keys\r\n"
    "Key=\r\n"
    "Model=deepseek-chat\r\n"
    "Endpoint=https://api.deepseek.com/chat/completions\r\n"
    "MaxPerMinute=30\r\n"
    "\r\n"
    "[OpenAI]\r\n"
    "; any OpenAI-compatible chat completions service: OpenAI, OpenRouter, a local\r\n"
    "; LM Studio or Ollama (http://localhost:11434/v1/chat/completions, no key)\r\n"
    "Key=\r\n"
    "Model=\r\n"
    "Endpoint=https://api.openai.com/v1/chat/completions\r\n"
    "MaxPerMinute=30\r\n";

std::string IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(sec, key, def, buf, 2048, IniPath().c_str());
  return Trim(Narrow(buf));
}

int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
  return (int)GetPrivateProfileIntW(sec, key, def, IniPath().c_str());
}

// g_lock held (or before any thread starts)
void ReadIni() {
  if (GetFileAttributesW(IniPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
    FILE* f = _wfopen(IniPath().c_str(), L"wb");
    if (f) {
      fputs(kDefaultIni, f);
      fclose(f);
      Log("wrote default Telegraph.ini");
    }
  }
  Settings s;
  s.enabled = IniInt(L"Telegraph", L"Enabled", 1) != 0;
  s.autoTranslate = IniInt(L"Telegraph", L"AutoTranslate", 1) != 0;
  s.preTranslate = IniInt(L"Telegraph", L"PreTranslate", 1) != 0;
  s.capture = IniInt(L"Telegraph", L"Capture", 0) != 0;
  s.useRuText = IniInt(L"Telegraph", L"UseRuTextDictionaries", 1) != 0;
  s.language = IniStr(L"Telegraph", L"Language", L"auto");
  std::string prov = Lower(IniStr(L"Telegraph", L"Providers", L"google,deepl,deepseek,openai"));
  for (size_t a = 0; a <= prov.size();) {
    size_t b = prov.find(',', a);
    if (b == std::string::npos) b = prov.size();
    std::string p = Trim(prov.substr(a, b - a));
    if (!p.empty()) s.providers.push_back(p);
    a = b + 1;
  }
  s.googleRate = std::max(1, IniInt(L"Google", L"MaxPerMinute", 3));
  s.googleChars = std::max(200, IniInt(L"Google", L"MaxChars", 2000));
  s.googlePreHour = std::max(0, IniInt(L"Google", L"PreTranslateCharsPerHour", 30000));
  s.deeplRate = std::max(1, IniInt(L"DeepL", L"MaxPerMinute", 30));
  s.deepseekRate = std::max(1, IniInt(L"DeepSeek", L"MaxPerMinute", 30));
  s.openaiRate = std::max(1, IniInt(L"OpenAI", L"MaxPerMinute", 30));
  s.maxPerDay = std::max(0, IniInt(L"Telegraph", L"MaxPerDay", 2000));
  s.deeplKey = IniStr(L"DeepL", L"Key", L"");
  s.deepseekKey = IniStr(L"DeepSeek", L"Key", L"");
  s.deepseekModel = IniStr(L"DeepSeek", L"Model", L"deepseek-chat");
  s.deepseekUrl = IniStr(L"DeepSeek", L"Endpoint", L"https://api.deepseek.com/chat/completions");
  s.openaiKey = IniStr(L"OpenAI", L"Key", L"");
  s.openaiModel = IniStr(L"OpenAI", L"Model", L"");
  s.openaiUrl = IniStr(L"OpenAI", L"Endpoint", L"https://api.openai.com/v1/chat/completions");
  g_set = s;
  g_enabled = s.enabled;
}

// ---------------------------------------------------------------- HTTP

HINTERNET g_http = nullptr;

struct HttpResp {
  int status = 0;  // 0 = no answer
  DWORD error = 0;
  std::string body;
};

HttpResp Http(const std::string& url, const std::string& headers, const std::string& body, int timeoutMs) {
  HttpResp r;
  std::wstring wurl = Widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  uc.dwHostNameLength = (DWORD)-1;
  uc.dwUrlPathLength = (DWORD)-1;
  uc.dwExtraInfoLength = (DWORD)-1;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    r.error = GetLastError();
    return r;
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  if (path.empty()) path = L"/";
  HINTERNET c = WinHttpConnect(g_http, host.c_str(), uc.nPort, 0);
  if (!c) {
    r.error = GetLastError();
    return r;
  }
  HINTERNET q = WinHttpOpenRequest(c, body.empty() ? L"GET" : L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!q) {
    r.error = GetLastError();
    WinHttpCloseHandle(c);
    return r;
  }
  WinHttpSetTimeouts(q, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
  std::wstring wh = Widen(headers);
  BOOL ok = WinHttpSendRequest(q, wh.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wh.c_str(), (DWORD)-1L,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                               (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(q, nullptr);
  if (ok) {
    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st,
                        &sz, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)st;
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(q, &avail) || !avail) break;
      size_t old = r.body.size();
      r.body.resize(old + avail);
      DWORD got = 0;
      if (!WinHttpReadData(q, &r.body[old], avail, &got)) {
        r.body.resize(old);
        break;
      }
      r.body.resize(old + got);
      if (r.body.size() > (8u << 20)) break;
    }
  } else {
    r.error = GetLastError();
  }
  WinHttpCloseHandle(q);
  WinHttpCloseHandle(c);
  return r;
}

// ---------------------------------------------------------------- providers

enum class Kind { Google, DeepL, Chat };

struct Provider {
  std::string name;
  Kind kind = Kind::Google;
  uint8_t bit = 0;
  std::string key, model, url;
  size_t maxItems = 1, maxChars = 1;
  int timeoutMs = 15000, perMinute = 30;
  ULONGLONG firstPause = 30000, maxPause = 15 * 60000;
  bool usable = false, disabled = false;
  ULONGLONG cooldownUntil = 0, backoff = 0;
  std::deque<ULONGLONG> sent;
  size_t preCharsPerHour = 0;  // 0 = no limit
  bool preAllowed = true;      // false: only text that is on screen
  std::deque<std::pair<ULONGLONG, size_t>> preSent;  // pre-translation characters of the last hour
};

enum class Result { Ok, Retry, Auth, Bad };

struct Reply {
  Result result = Result::Retry;
  std::vector<std::string> texts;  // one per input when Ok
  std::string why;
};

std::string Why(const HttpResp& r) {
  char b[64];
  if (!r.status) snprintf(b, sizeof b, "no answer (error %lu)", (unsigned long)r.error);
  else snprintf(b, sizeof b, "HTTP %d", r.status);
  std::string s = b;
  if (r.status && !r.body.empty() && r.body[0] != '<') s += ": " + Short(r.body, 160);
  return s;
}

Result Classify(const HttpResp& r, bool keyed) {
  if (r.status == 200) return Result::Ok;
  if (!r.status || r.status == 429 || r.status >= 500) return Result::Retry;
  if (keyed && (r.status == 401 || r.status == 403 || r.status == 456)) return Result::Auth;
  if (r.status == 403) return Result::Retry;  // keyless Google: blocked for a while
  return Result::Bad;
}

Reply CallGoogle(const Provider& p, const std::vector<std::string>& texts, const tg::Lang& lang) {
  Reply rep;
  std::string joined;
  for (size_t i = 0; i < texts.size(); ++i) joined += (i ? "\n" : "") + texts[i];
  std::string url = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=" +
                    std::string(lang.google) + "&dt=t";
  HttpResp r = Http(url, "Content-Type: application/x-www-form-urlencoded;charset=UTF-8\r\n",
                    "q=" + tg::UrlEncode(joined), p.timeoutMs);
  rep.result = Classify(r, false);
  if (rep.result != Result::Ok) {
    rep.why = Why(r);
    return rep;
  }
  std::string text, detected;
  if (!tg::ParseGoogle(r.body, text, detected)) {
    rep.result = Result::Bad;
    rep.why = "unreadable answer: " + Short(r.body, 160);
    return rep;
  }
  rep.texts = tg::SplitLines(text);
  if (rep.texts.size() != texts.size()) {
    rep.result = Result::Bad;
    rep.why = "line count changed";
    rep.texts.clear();
  }
  return rep;
}

Reply CallDeepL(const Provider& p, const std::vector<std::string>& texts, const tg::Lang& lang) {
  Reply rep;
  bool free = p.key.size() > 3 && p.key.compare(p.key.size() - 3, 3, ":fx") == 0;
  std::string url = free ? "https://api-free.deepl.com/v2/translate" : "https://api.deepl.com/v2/translate";
  HttpResp r = Http(url, "Authorization: DeepL-Auth-Key " + p.key + "\r\nContent-Type: application/json\r\n",
                    tg::DeepLBody(texts, lang), p.timeoutMs);
  rep.result = Classify(r, true);
  if (rep.result != Result::Ok) {
    rep.why = Why(r);
    return rep;
  }
  if (!tg::ParseDeepL(r.body, rep.texts) || rep.texts.size() != texts.size()) {
    rep.result = Result::Bad;
    rep.why = "unreadable answer: " + Short(r.body, 160);
    rep.texts.clear();
  }
  return rep;
}

Reply CallChat(const Provider& p, const std::vector<std::string>& texts, const tg::Lang& lang,
               const std::vector<std::pair<std::string, std::string>>& glossary) {
  Reply rep;
  std::string headers = "Content-Type: application/json\r\n";
  if (!p.key.empty()) headers += "Authorization: Bearer " + p.key + "\r\n";
  HttpResp r = Http(p.url, headers,
                    tg::ChatBody(p.model, tg::LlmSystemPrompt(lang, glossary), tg::JsonStringArray(texts)),
                    p.timeoutMs);
  rep.result = Classify(r, !p.key.empty());
  if (rep.result != Result::Ok) {
    rep.why = Why(r);
    return rep;
  }
  std::string content;
  if (!tg::ParseChat(r.body, content) || !tg::ParseStringArray(content, rep.texts) ||
      rep.texts.size() != texts.size()) {
    rep.result = Result::Bad;
    rep.why = "unreadable answer: " + Short(content.empty() ? r.body : content, 160);
    rep.texts.clear();
  }
  return rep;
}

std::vector<Provider> BuildProviders(const Settings& s) {
  std::vector<Provider> v;
  for (const std::string& n : s.providers) {
    Provider p;
    p.name = n;
    if (n == "google") {
      // few large requests: the keyless endpoint blocks an address that asks often
      p.kind = Kind::Google, p.bit = 1, p.maxItems = 100, p.maxChars = (size_t)s.googleChars, p.usable = true;
      p.perMinute = s.googleRate, p.firstPause = 120000, p.maxPause = 60 * 60000;
      p.preCharsPerHour = (size_t)s.googlePreHour;
      p.preAllowed = s.googlePreHour > 0;
    } else if (n == "deepl") {
      p.kind = Kind::DeepL, p.bit = 2, p.maxItems = 40, p.maxChars = 6000, p.key = s.deeplKey, p.perMinute = s.deeplRate;
      p.usable = !p.key.empty();
    } else if (n == "deepseek") {
      p.kind = Kind::Chat, p.bit = 4, p.maxItems = 30, p.maxChars = 3000, p.key = s.deepseekKey;
      p.model = s.deepseekModel, p.url = s.deepseekUrl, p.timeoutMs = 90000, p.perMinute = s.deepseekRate;
      p.usable = !p.key.empty() && !p.url.empty();
    } else if (n == "openai") {
      p.kind = Kind::Chat, p.bit = 8, p.maxItems = 30, p.maxChars = 3000, p.key = s.openaiKey;
      p.model = s.openaiModel, p.url = s.openaiUrl, p.timeoutMs = 90000, p.perMinute = s.openaiRate;
      p.usable = !p.model.empty() && !p.url.empty();
    } else {
      Log("Providers: unknown service '%s' skipped", n.c_str());
      continue;
    }
    v.push_back(std::move(p));
  }
  return v;
}

bool WordAt(const std::string& text, size_t p, size_t n) {
  bool left = p == 0 || !tg::IsAsciiLetter((unsigned char)text[p - 1]);
  bool right = p + n >= text.size() || !tg::IsAsciiLetter((unsigned char)text[p + n]);
  return left && right;
}

// g_lock held. Dictionary entries that occur in the texts, for the LLM prompt.
std::vector<std::pair<std::string, std::string>> Glossary(const std::string& module,
                                                          const std::vector<std::string>& texts) {
  std::vector<std::pair<std::string, std::string>> g;
  auto sc = g_dict->find(module);
  const Scope* scopes[2] = {sc == g_dict->end() ? nullptr : &sc->second, g_common};
  auto add = [&](const std::string& en, const std::string& tr) {
    if (g.size() >= 40 || en.size() < 3 || en.size() > 40) return;
    for (const auto& e : g)
      if (e.first == en) return;
    for (const std::string& t : texts) {
      size_t p = t.find(en);
      if (p != std::string::npos && WordAt(t, p, en.size())) {
        g.emplace_back(en, tr);
        return;
      }
    }
  };
  for (const Scope* s : scopes) {
    if (!s) continue;
    for (const auto& [en, tr] : s->subs) add(en, tr);
    for (const auto& [en, tr] : s->exact)
      if (std::count(en.begin(), en.end(), ' ') < 4) add(en, tr);
  }
  return g;
}

// ---------------------------------------------------------------- worker

std::string g_usageDay;
int g_usageCount = 0;

std::string Today() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  char b[16];
  snprintf(b, sizeof b, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
  return b;
}

void LoadUsage() {
  std::string data;
  g_usageDay = Today();
  g_usageCount = 0;
  if (!ReadFile(g_dir + L"\\cache\\usage.txt", data)) return;
  char day[16] = {};
  int n = 0;
  if (sscanf(data.c_str(), "%15s %d", day, &n) == 2 && g_usageDay == day) g_usageCount = n;
}

void CountRequest() {
  std::string today = Today();
  if (today != g_usageDay) g_usageDay = today, g_usageCount = 0;
  ++g_usageCount;
  CreateDirectoryW((g_dir + L"\\cache").c_str(), nullptr);
  FILE* f = _wfopen((g_dir + L"\\cache\\usage.txt").c_str(), L"wb");
  if (f) {
    fprintf(f, "%s %d\n", g_usageDay.c_str(), g_usageCount);
    fclose(f);
  }
}

// Background pre-translation: the UI-looking string literals of each patched mod.
void ScanModules(const tg::Lang* target, int gen) {
  size_t n = g_modCount.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    ModInfo* m = g_mods[i];
    if (m->scanned.load()) continue;
    m->scanned = true;
    std::string data;
    if (!ReadFile(m->path, data)) continue;
    size_t dropped = 0;
    std::vector<std::string> keys = tg::UiCandidates((const uint8_t*)data.data(), data.size(), &dropped);
    int queued = 0, known = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (g_generation != gen || g_target != target) {
      ReleaseSRWLockExclusive(&g_lock);
      return;
    }
    const Scope* scopes[2] = {m->scope, g_common};
    auto c = g_cache.find(m->name);
    for (const std::string& k : keys) {
      std::string out;
      if (LookupWhole(scopes, 2, k, out) || (c != g_cache.end() && c->second.count(k))) {
        ++known;
        continue;
      }
      size_t before = g_low.size();
      Enqueue(m->name, k, false, false);
      queued += g_low.size() != before;
    }
    ReleaseSRWLockExclusive(&g_lock);
    Log("pre-translate %s: %d UI strings, %d already translated, %d queued%s", m->file.c_str(), (int)keys.size(),
        known, queued, dropped ? " (a name list of single words left for on-screen translation)" : "");
  }
}

bool RateOk(Provider& p, ULONGLONG now, int perMinute) {
  while (!p.sent.empty() && now - p.sent.front() >= 60000) p.sent.pop_front();
  return (int)p.sent.size() < perMinute;
}

DWORD WINAPI Worker(LPVOID) {
  g_http = WinHttpOpen(L"Telegraph/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                       WINHTTP_NO_PROXY_BYPASS, 0);
  if (!g_http)
    g_http = WinHttpOpen(L"Telegraph/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
  if (!g_http) {
    Log("WinHTTP unavailable (error %lu): no machine translation", GetLastError());
    return 0;
  }
  LoadUsage();
  std::vector<Provider> provs;
  std::string provKey;
  bool capLogged = false, noneLogged = false;
  int okTotal = 0, failTotal = 0;
  for (;;) {
    AcquireSRWLockExclusive(&g_lock);
    const tg::Lang* target = g_target;
    int gen = g_generation;
    Settings set = g_set;
    bool idle = !target || (g_high.empty() && g_low.empty());
    ReleaseSRWLockExclusive(&g_lock);
    if (!target) {
      Sleep(1000);
      continue;
    }
    if (set.preTranslate) ScanModules(target, gen);
    // providers follow Telegraph.ini; state (cooldowns) is kept per name
    std::string pk;
    for (const auto& p : set.providers) pk += p + ",";
    pk += std::to_string(set.googleRate) + "|" + std::to_string(set.googleChars) + "|" + std::to_string(set.googlePreHour) + "|" + std::to_string(set.deeplRate) + "|" + std::to_string(set.deepseekRate) + "|" +
          std::to_string(set.openaiRate) + "|" + set.deeplKey + "|" + set.deepseekKey + "|" + set.deepseekModel + "|" + set.deepseekUrl + "|" +
          set.openaiKey + "|" + set.openaiModel + "|" + set.openaiUrl;
    if (pk != provKey) {
      provKey = pk;
      std::vector<Provider> next = BuildProviders(set);
      for (auto& n : next)
        for (auto& o : provs)
          if (o.name == n.name && o.key == n.key)
            n.cooldownUntil = o.cooldownUntil, n.backoff = o.backoff, n.sent = o.sent, n.preSent = o.preSent;
      provs = std::move(next);
      std::string list;
      for (const auto& p : provs)
        if (p.usable) list += (list.empty() ? "" : ", ") + p.name;
      Log("services in order: %s", list.empty() ? "none" : list.c_str());
      noneLogged = false;
    }
    if (idle) {
      AcquireSRWLockExclusive(&g_lock);
      if (g_high.empty() && g_low.empty()) SleepConditionVariableSRW(&g_wake, &g_lock, 1000, 0);
      ReleaseSRWLockExclusive(&g_lock);
      continue;
    }
    if (set.maxPerDay && g_usageCount >= set.maxPerDay && Today() == g_usageDay) {
      if (!capLogged) Log("daily limit of %d requests reached (MaxPerDay): waiting for tomorrow", set.maxPerDay);
      capLogged = true;
      Sleep(30000);
      continue;
    }
    capLogged = false;

    // take a batch for the first service that may take the head job
    ULONGLONG now = GetTickCount64();
    Provider* p = nullptr;
    std::vector<Job> batch;
    ULONGLONG waitMs = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (g_generation != gen) {
      ReleaseSRWLockExclusive(&g_lock);
      continue;
    }
    std::deque<Job>* q = !g_high.empty() ? &g_high : !g_low.empty() ? &g_low : nullptr;
    // text that just appeared: wait until the screen settles (0.8 s without new text,
    // at most 2.5 s) and send it in one request
    if (q == &g_high && now < g_lastHighAt + 800 && now < q->front().at + 2500) {
      ReleaseSRWLockExclusive(&g_lock);
      Sleep(200);
      continue;
    }
    if (q) {
      Job& head = q->front();
      bool left = false;
      for (auto& pr : provs) {
        if (!pr.usable || pr.disabled || (head.tried & pr.bit)) continue;
        left = true;
        if (q == &g_low && !pr.preAllowed) {
          if (!waitMs || 60000 < waitMs) waitMs = 60000;
          continue;
        }
        // pre-translation leaves one request a minute for text that is on screen now
        int rate = q == &g_low && pr.perMinute > 1 ? pr.perMinute - 1 : pr.perMinute;
        bool budget = true;
        if (q == &g_low && pr.preCharsPerHour) {
          while (!pr.preSent.empty() && now - pr.preSent.front().first >= 3600000) pr.preSent.pop_front();
          size_t used = 0;
          for (const auto& e : pr.preSent) used += e.second;
          budget = used < pr.preCharsPerHour;
          if (!budget && !pr.preSent.empty()) {
            ULONGLONG w = 3600000 - (now - pr.preSent.front().first);
            if (!waitMs || w < waitMs) waitMs = w;
          }
        }
        if (now >= pr.cooldownUntil && budget && RateOk(pr, now, rate)) {
          p = &pr;
          break;
        }
        ULONGLONG w = now < pr.cooldownUntil ? pr.cooldownUntil - now
                                             : (pr.sent.empty() ? 1000 : 60000 - (now - pr.sent.front()));
        if (!waitMs || w < waitMs) waitMs = w;
      }
      if (!left) {
        bool any = false;
        for (auto& pr : provs) any |= pr.usable && !pr.disabled;
        if (!any) {
          if (!noneLogged) Log("no translation service available: text stays as it is");
          noneLogged = true;
          waitMs = 5000;
        } else {
          g_failed.insert(head.module + '\x01' + head.key);
          g_pending.erase(head.module + '\x01' + head.key);
          if (++failTotal <= 50) Log("no service could translate [%s] %s", head.module.c_str(), Short(head.key).c_str());
          q->pop_front();
        }
      } else if (p) {
        size_t chars = 0;
        size_t limit = head.maxBatch ? std::min<size_t>(head.maxBatch, p->maxItems) : p->maxItems;
        while (!q->empty() && batch.size() < limit) {
          Job& j = q->front();
          if ((j.tried & p->bit) || (p->kind == Kind::Chat && !batch.empty() && j.module != batch[0].module))
            break;
          if (!batch.empty() && chars + j.key.size() > p->maxChars) break;
          chars += j.key.size() + 1;
          batch.push_back(std::move(j));
          q->pop_front();
        }
      }
    }
    std::vector<std::pair<std::string, std::string>> glossary;
    if (p && p->kind == Kind::Chat && !batch.empty()) {
      std::vector<std::string> keys;
      for (const auto& j : batch) keys.push_back(j.key);
      glossary = Glossary(batch[0].module, keys);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!p || batch.empty()) {
      if (waitMs) Sleep((DWORD)std::min<ULONGLONG>(waitMs, 2000));
      continue;
    }

    std::vector<tg::Protected> prot;
    std::vector<std::string> texts;
    for (const auto& j : batch) {
      prot.push_back(tg::Protect(j.key, j.literal));
      texts.push_back(prot.back().text);
    }
    p->sent.push_back(now);
    if (!batch[0].high) {
      size_t chars = 0;
      for (const auto& t : texts) chars += t.size() + 1;
      p->preSent.emplace_back(now, chars);
    }
    CountRequest();
    ULONGLONG t0 = GetTickCount64();
    Reply rep = p->kind == Kind::Google  ? CallGoogle(*p, texts, *target)
                : p->kind == Kind::DeepL ? CallDeepL(*p, texts, *target)
                                         : CallChat(*p, texts, *target, glossary);
    ULONGLONG ms = GetTickCount64() - t0;

    AcquireSRWLockExclusive(&g_lock);
    if (g_generation != gen) {
      ReleaseSRWLockExclusive(&g_lock);
      continue;
    }
    auto requeueFront = [&](std::vector<Job>& jobs) {
      for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) (it->high ? g_high : g_low).push_front(std::move(*it));
    };
    int ok = 0, bad = 0;
    if (rep.result == Result::Retry || rep.result == Result::Auth) {
      if (rep.result == Result::Auth) {
        p->disabled = true;
        Log("%s: key rejected (%s): service off until Telegraph.ini changes", p->name.c_str(), rep.why.c_str());
      } else {
        p->backoff = p->backoff ? std::min<ULONGLONG>(p->backoff * 2, p->maxPause) : p->firstPause;
        p->cooldownUntil = GetTickCount64() + p->backoff;
        Log("%s: %s: pause %llu s", p->name.c_str(), rep.why.c_str(), p->backoff / 1000);
      }
      requeueFront(batch);
    } else if (rep.result == Result::Bad && batch.size() > 1) {
      // answer did not fit the batch: try again in halves
      uint16_t half = (uint16_t)std::max<size_t>(1, batch.size() / 2);
      for (auto& j : batch) j.maxBatch = half;
      Log("%s: %s: retrying %d texts in batches of %d", p->name.c_str(), rep.why.c_str(), (int)batch.size(), half);
      requeueFront(batch);
    } else {
      p->backoff = 0;
      std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> fresh;
      for (size_t i = 0; i < batch.size(); ++i) {
        Job& j = batch[i];
        std::string value;
        bool good = rep.result == Result::Ok && tg::Restore(prot[i], rep.texts[i], value) && !value.empty();
        if (good) {
          g_cache[j.module][j.key] = value;
          fresh[j.module].emplace_back(j.key, value);
          g_pending.erase(j.module + '\x01' + j.key);
          ++ok;
        } else {
          // a placeholder got lost: another service may do better
          j.tried |= p->bit;
          j.maxBatch = 0;
          ++bad;
          if (bad <= 3)
            Log("%s: rejected for [%s] %s -> %s", p->name.c_str(), j.module.c_str(), Short(j.key).c_str(),
                rep.result == Result::Ok ? Short(rep.texts[i]).c_str() : rep.why.c_str());
          (j.high ? g_high : g_low).push_back(std::move(j));
        }
      }
      for (const auto& [module, entries] : fresh)
        if (ModInfo* m = ModByName(module)) AppendCache(m, entries);
      if (ok) {
        g_memo.clear();  // redraws pick the new text up
        g_cacheSig = CacheSignature();
      }
      okTotal += ok;
      Log("%s: %d of %d translated in %llu ms (total %d; waiting: %d on screen, %d pre-translation; %d requests today)",
          p->name.c_str(), ok, (int)batch.size(), ms, okTotal, (int)g_high.size(), (int)g_low.size(), g_usageCount);
    }
    ReleaseSRWLockExclusive(&g_lock);
  }
}

// ---------------------------------------------------------------- script

uint64_t DictSignature() {
  static const wchar_t* ext[] = {L".txt", L".ini"};
  uint64_t sig = FolderSignature(g_dir, ext, 2);
  if (g_set.useRuText) sig = sig * 1099511628211ULL + FolderSignature(g_gameDir + L"\\RuText", ext, 1);
  return sig;
}

// g_lock held
void SetTarget(const tg::Lang* t, int gameLang) {
  if (t == g_target) return;
  g_target = t;
  ++g_generation;
  g_high.clear();
  g_low.clear();
  g_pending.clear();
  g_failed.clear();
  g_memo.clear();
  size_t n = g_modCount.load();
  for (size_t i = 0; i < n; ++i) g_mods[i]->scanned = false;
  LoadCache();
  if (t) Log("game language %d: translating into %s (%s)", gameLang, t->name, t->code);
  else Log("game language %d: machine translation off", gameLang);
}

void ReloadDictionaries() {
  AcquireSRWLockExclusive(&g_lock);
  g_oldDicts.push_back(g_dict);
  g_dict = new ScopeMap;
  ReadIni();
  LoadDictionaries();
  auto c = g_dict->find("_common");
  g_common = c == g_dict->end() ? nullptr : &c->second;
  size_t n = g_modCount.load();
  for (size_t i = 0; i < n; ++i) {
    auto sc = g_dict->find(g_mods[i]->name);
    g_mods[i]->scope = sc == g_dict->end() ? nullptr : &sc->second;
  }
  g_memo.clear();
  ReleaseSRWLockExclusive(&g_lock);
  Log("dictionaries and settings reloaded (Enabled=%d AutoTranslate=%d Language=%s)", g_set.enabled,
      g_set.autoTranslate, g_set.language.c_str());
}

int GameLanguage() {
  g_origInit(kGetCurrentLanguage);
  uint64_t* r = g_origCall();
  return r ? (int)(uint32_t)*r : -1;
}

void UpdateTarget() {
  int game = GameLanguage();
  static int lastGame = -2;
  if (game != lastGame) Log("game language: %d", game);
  lastGame = game;
  AcquireSRWLockExclusive(&g_lock);
  const tg::Lang* t = nullptr;
  if (g_set.autoTranslate) {
    if (Lower(g_set.language) == "auto" || g_set.language.empty()) t = tg::LangByGame(game);
    else {
      t = tg::LangByCode(g_set.language);
      static bool warned = false;
      if (!t && !warned) Log("Language=%s is not a known code: using the game language", g_set.language.c_str());
      warned = true;
      if (!t) t = tg::LangByGame(game);
    }
    if (t && t->game == 0) t = nullptr;  // English: nothing to translate
  }
  SetTarget(t, game);
  ReleaseSRWLockExclusive(&g_lock);
}

// Runs as a ScriptHook script: between other scripts' ticks, so no mod is in the
// middle of a nativeInit/nativePush64/nativeCall sequence while imports change.
void ScriptMain() {
  Log("script started");
  uint64_t sig = DictSignature();
  for (int tick = 0;; ++tick) {
    if (tick % 3 == 0) PatchAll();
    UpdateTarget();
    uint64_t now = DictSignature();
    if (now != sig) {
      sig = now;
      ReloadDictionaries();
      UpdateTarget();
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_target && CacheSignature() != g_cacheSig) {
      Log("cache files changed outside Telegraph: reloading");
      LoadCache();
      g_memo.clear();
    }
    ReleaseSRWLockExclusive(&g_lock);
    g_scriptWait(2000);
  }
}

bool Register() {
  g_origInit = (FnInit)GetProcAddress(g_sh, kExpInit);
  g_origPush = (FnPush)GetProcAddress(g_sh, kExpPush);
  g_origCall = (FnCall)GetProcAddress(g_sh, kExpCall);
  auto reg = (FnScriptRegister)GetProcAddress(g_sh, kExpScriptRegister);
  g_scriptWait = (FnScriptWait)GetProcAddress(g_sh, kExpScriptWait);
  if (!g_origInit || !g_origPush || !g_origCall || !reg || !g_scriptWait) {
    Log("ScriptHookRDR2 exports not found, Telegraph inactive");
    return false;
  }
  reg(g_self, ScriptMain);
  Log("registered with ScriptHookRDR2");
  HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  return true;
}

// Patch the mods that are already loaded right away, long before any script runs, so
// text a mod shows in its very first tick is caught too. Later modules are picked up
// by ScriptMain. Plugins that chain behind Telegraph (HideoutMemory) patch after this.
DWORD WINAPI EarlyPatch(LPVOID) {
  PatchAll();
  Log("early patch done (%d modules)", (int)g_modCount.load());
  return 0;
}

// Fallback when Telegraph loads before any module that imports ScriptHookRDR2.dll.
DWORD WINAPI Boot(LPVOID) {
  for (int i = 0; i < 600 && !(g_sh = GetModuleHandleW(L"ScriptHookRDR2.dll")); ++i) Sleep(100);
  if (!g_sh) {
    Log("ScriptHookRDR2.dll not loaded, Telegraph inactive");
    return 0;
  }
  if (RuTextLoaded()) return 0;
  if (Register()) EarlyPatch(nullptr);
  return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  g_self = inst;
  DisableThreadLibraryCalls(inst);
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(inst, path, MAX_PATH);
  std::wstring p = path;
  g_gameDir = p.substr(0, p.find_last_of(L"\\/"));
  g_dir = g_gameDir + L"\\Telegraph";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  g_log = _wfopen((g_dir + L"\\Telegraph.log").c_str(), L"w");
  Log("Telegraph %s loaded", kVersion);
  ReadIni();
  Log("Enabled=%d AutoTranslate=%d Language=%s PreTranslate=%d", g_set.enabled, g_set.autoTranslate,
      g_set.language.c_str(), g_set.preTranslate);
  LoadDictionaries();
  auto c = g_dict->find("_common");
  g_common = c == g_dict->end() ? nullptr : &c->second;
  if (RuTextLoaded()) return TRUE;
  // ScriptHookRDR2.dll is normally loaded already: earlier .asi files import it.
  if ((g_sh = GetModuleHandleW(L"ScriptHookRDR2.dll"))) {
    if (Register()) {
      HANDLE t = CreateThread(nullptr, 0, EarlyPatch, nullptr, 0, nullptr);
      if (t) CloseHandle(t);
    }
  } else {
    HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
  }
  return TRUE;
}
