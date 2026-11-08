# Telegraph — Mod Auto Translator for Red Dead Redemption 2

An ASI plugin for RDR2 Story Mode. Many ScriptHook mods have menus, prompts, notifications and subtitles in English compiled into the `.asi`. Telegraph shows that text in the language of your game, automatically:

1. **Your dictionaries first.** Plain-text files in `<game>\Telegraph\`, one per mod. Hand-made lines always win.
2. **Then the translation cache.** Everything translated before, in `<game>\Telegraph\cache\<language>\<Mod>.txt`. Plain text: fix a line and your fix stays.
3. **Then a translation service.** Out of the box, without any key: Google Translate, and MyMemory for text on screen when Google is unavailable. With your own key: Microsoft Azure Translator or Google Cloud Translation (both have a free monthly allowance), DeepL, DeepSeek or any OpenAI-compatible model (OpenAI, OpenRouter, a local LM Studio or Ollama). Services are tried in order; if one fails, is blocked or hits its limit, the next one takes over.

The game never waits: text without a translation is shown as it is, a background thread asks the service, and the next redraw (usually within a second) shows the translation. On the first run Telegraph also reads the text found inside each mod and translates it in the background, so most menus are translated before you open them.

Telegraph is the successor of [RuText](https://github.com/iillyyaa1997/rdr2-rutext): RuText dictionaries keep working.

## Requirements

- RDR2 1.0.1491.50 (Steam), Story Mode
- [ScriptHookRDR2 V2](https://www.nexusmods.com/reddeadredemption2/mods/1472) and an ASI loader (`dinput8.dll` or `version.dll`)
- The game in the language you want (Telegraph translates into the game language; set `Language=` to override)

## Install

Copy `Telegraph.asi` into the game folder and the `Telegraph` folder next to it.
With Vortex: install the archive and set the mod type that deploys into the game folder (`reddeadredemption2asi` in the RDR2 extension).

Coming from RuText: remove `RuText.asi` (Telegraph stays off while RuText is loaded, both hook the same calls). Keep the `RuText` folder or move its `*.txt` into `Telegraph\`: Telegraph reads both.

## Settings: `Telegraph\Telegraph.ini`

| Key | Default | |
|---|---|---|
| `Enabled` | 1 | 0 = Telegraph changes nothing |
| `AutoTranslate` | 1 | 0 = dictionaries only (RuText behaviour) |
| `Language` | auto | the game language, or `fr de it es pt-BR pl ru ko zh-TW ja es-MX zh-CN` |
| `Providers` | `deepl,googlecloud,microsoft,deepseek,openai,google,mymemory` | order in which services are tried; one without a key is skipped |
| `PreTranslate` | 1 | translate the text found inside each mod in the background |
| `MaxPerDay` | 2000 | requests per day to all services together (0 = no limit) |
| `Capture` | 0 | 1 = list text with no translation at all in `Telegraph\untranslated\<Mod>.txt` |
| `UseRuTextDictionaries` | 1 | also read `<game>\RuText\*.txt` |

Per service (each also has `MaxPerMinute`):

| Section | Needs | |
|---|---|---|
| `[Google]` | nothing | `MaxPerMinute=3`, `MaxChars=2000`, `PreTranslateCharsPerHour=30000` (0 = only text on screen) |
| `[MyMemory]` | nothing | only text on screen; 5,000 characters a day, 50,000 with `Email=` |
| `[Microsoft]` | `Key=`, `Region=` | [Azure AI Translator](https://azure.microsoft.com/pricing/details/translator/), free tier F0: 2 million characters a month |
| `[GoogleCloud]` | `Key=` | [Cloud Translation API](https://cloud.google.com/translate/pricing), 500,000 characters a month free, billing account needed |
| `[DeepL]` | `Key=` | DeepL API key (the free API plan is no longer offered to new users) |
| `[DeepSeek]` | `Key=` | `Model=deepseek-chat` |
| `[OpenAI]` | `Model=`, `Endpoint=`, `Key=` | any OpenAI-compatible chat completions service; a local model needs no key |

Changes are picked up while the game runs.

**About Google without a key.** It is the unofficial endpoint browser extensions use, not an API Google offers. Since August 2026 Google has been answering it with "too many requests" for hours at a time, in some regions for everyone; heavy use makes it more likely. Telegraph asks it gently (3 requests a minute, background pre-translation capped per hour), pauses it for 10 to 60 minutes when it refuses, and lets the next service take over. It does not try to get around the block. For many mods or steady results, enter a key for one of the official services: the Azure free tier is the largest.

## Dictionaries

One UTF-8 file per mod, named after the mod's `.asi`: `PedDamageOverhaul.txt` is used for `PedDamageOverhaul.asi`, `_common.txt` for every mod. See [`examples/ExampleMod.txt`](examples/ExampleMod.txt).

```
Open Menu => Открыть меню                        exact line
You paid $*. They rode off. => Ты заплатил $*.   pattern, * captures text
Day * of * => {2}: день {1}                       numbered captures
~Valentine => Валентайн                           replaced inside longer lines (also a glossary hint for AI services)
```

## The cache

`Telegraph\cache\<language>\<Mod>.txt` uses the same format. In the English text `*` stands for a number, and `{1}` `{2}` put the numbers back, so `Reward: $25` and `Reward: $40` share one line `Reward: $* => Награда: ${1}`. Edit a line to fix it; delete a line to have it translated again; copy a line into your dictionary to make it permanent.

Markup is kept: `~COLOR_RED~`, `~n~`, `~INPUT_...~`, `<font ...>`, `%d`, `|n|`, numbers. A translation that loses any of them is rejected and the next service is asked.

## Privacy

Only the text strings of your mods are sent, to the services listed in `Providers`, and only when they are not in your dictionaries or the cache. Nothing else leaves your PC. Set `AutoTranslate=0` to keep everything offline.

## How it works

ScriptHook mods call natives through three exports of `ScriptHookRDR2.dll`: `nativeInit`, `nativePush64`, `nativeCall`. Telegraph rewrites those import-table entries in every loaded `.asi`. For `CREATE_VAR_STRING(…, "LITERAL_STRING", text)` and `DATABINDING_ADD_DATA_STRING` it swaps the text pointer. The game language comes from `LOCALIZATION::GET_CURRENT_LANGUAGE`. Translation requests run in a separate thread over WinHTTP. Plugins that hook the same calls of one mod after Telegraph, such as [Hideout Memory](https://github.com/iillyyaa1997/rdr2-hideout-memory), chain to it.

## Build

`./build.sh` runs the tests of the platform-independent part (`tests/test_core.cpp`) and cross-compiles `out/Telegraph.asi` with mingw-w64 in a throwaway Docker container.

## License

MIT

---

## По-русски

ASI-плагин для сюжетного режима RDR2. Показывает зашитый в ScriptHook-моды английский текст (меню, подсказки, уведомления, субтитры) на языке игры, сам:

1. сначала ваши словари в `<game>\Telegraph\` — ручные строки всегда главнее;
2. потом кэш уже переведённого в `Telegraph\cache\<язык>\<Мод>.txt` — обычный текст, правку можно вносить руками;
3. потом сервис перевода: без ключа — Google и MyMemory (для текста на экране), с вашим ключом — Microsoft Azure или Google Cloud (у обоих бесплатный месячный объём), DeepL, DeepSeek или любая OpenAI-совместимая модель. Сервисы идут по очереди: если один упал, заблокирован или упёрся в лимит, переводит следующий.

Google без ключа — неофициальный адрес, с августа 2026 Google периодически блокирует его на часы. Telegraph спрашивает его бережно, при отказе ставит на паузу и переходит к следующему сервису, обходить блок не пытается. Для стабильной работы с большим числом модов лучше ключ Azure (бесплатно 2 млн символов в месяц).

Игра не ждёт: пока перевода нет, виден оригинал, при следующей перерисовке (обычно меньше секунды) — перевод. При первом запуске Telegraph заранее переводит в фоне текст, найденный внутри каждого мода.

Наследник RuText: словари RuText работают как были, `RuText.asi` нужно удалить. Нужны ScriptHookRDR2 V2 и ASI-загрузчик. На сервисы уходят только строки модов, которых нет в словарях и кэше.
