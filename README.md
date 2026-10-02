# CS2SPY — невидимка для админа (`!hide`)

Модуль [Admin System](https://github.com/Pisex/cs2-admin_system) для **Metamod:Source** на C++.
Команда `!hide` и пункт **«Режим невидимки»** в админ-меню полностью прячут админа: его нет в табе,
spec list'ы не видят, за кем он следит, а его сообщения и голос не раскрывают его присутствие.

**Готовая сборка для сервера (Linux):**

- Metamod:Source 2.0 с KHook (сборки от 8 сентября 2026 и новее): [`release/hide-linux-x86_64.zip`](release/hide-linux-x86_64.zip)
- Metamod:Source 1.12 / старый 2.0 с SourceHook: [`release/hide-sourcehook-linux-x86_64.zip`](release/hide-sourcehook-linux-x86_64.zip)

Версия **2.2.0**, изменения — в [CHANGELOG.md](CHANGELOG.md).

## Что умеет

| Что скрыто | Как сделано |
|---|---|
| Таб (scoreboard) | Админ переводится через наблюдателей в «без команды»: таких игроков в табе нет, а наблюдать за игрой он может как обычно. |
| Spec list'ы, HUD-моды, читы | `CheckTransmit`: камера наблюдателя админа (в ней записано, за кем он следит) и его тело не отправляются другим игрокам. Нужен Utils 1.9.1+. |
| Сообщения в чате и киллфиде | Нет «… присоединяется к наблюдателям», киллфида при включении невидимки, сообщений о входе и выходе скрытого админа. |
| Случайное раскрытие | Сообщения скрытого админа в чат никто не видит, голос никто не слышит. Команды (`!...`, `/...`) и админ-чат (`@текст` в чат команды) работают. |
| Сама команда | `!hide` в чат не попадает — её никто не видит. |

Невидимка держится сама:

- после смены карты и переподключения она возвращается, автоматический вход в старую команду блокируется;
- таймер автоназначения команды (`mp_force_pick_time`) для скрытых админов отключается;
- если сервер или другой плагин переведёт скрытого админа в команду, он снова скрывается (не больше 3 раз за 30 секунд, чтобы не «воевать» с другим плагином).

## Требования

- Сервер CS2 на **Linux x64**.
- [Admin System](https://github.com/Pisex/cs2-admin_system) и [Utils](https://github.com/Pisex/cs2-menus) **1.9.1+**.
  На более старых Utils модуль прячет только таб, чат и голос и пишет об этом в лог ошибок.
- Metamod:Source. Сборку берите под **тот же** Metamod, под который собраны ваши Utils и Admin System:
  Metamod 2.0 с KHook не загружает плагины для SourceHook, и наоборот.

## Установка

1. Если стоит стандартный модуль `Hide` из `cs2-admin_system_modules`, удалите его: `addons/metamod/as_hide.vdf`,
   `addons/admin_modules/as_hide.so` (или просто перезапишите их файлами из архива).
2. Распакуйте архив прямо в `game/csgo/`:
   ```
   addons/admin_modules/as_hide.so
   addons/metamod/as_hide.vdf
   addons/configs/admin_system/hide.ini
   addons/translations/as_hide.phrases.txt
   ```
3. Перезапустите сервер или выполните `meta load addons/admin_modules/as_hide`.
4. `meta list` должен показать `[AS] Hide (2.2.0) by glazki`, а консоль при загрузке — `[Hide] Utils 1.9.1: full mode`.

**Обновление:** перед распаковкой сохраните свой `addons/configs/admin_system/hide.ini` — архив перезапишет его
конфигом по умолчанию. Новые настройки, которых нет в старом конфиге, работают со значениями по умолчанию.

## Команды

| Команда | Кто | Что делает |
|---|---|---|
| `!hide`, `/hide`, `mm_hide` | админ с `@admin/hide` | Включить / выключить невидимку. Сообщение в чат не попадает. |
| Админ-меню → «Управление сервером» → «Режим невидимки [ВКЛ/ВЫКЛ]» | админ с `@admin/hide` | То же самое. |
| `!hidelist`, `mm_hidelist` | админ с `@admin/hide`, консоль сервера | Кто из админов сейчас скрыт. Видно только вам. |

## Когда невидимка выключается

| Событие | Что происходит |
|---|---|
| Повторный `!hide` | Админ становится обычным наблюдателем (без сообщения в чате). |
| Админ сам выбрал команду в меню | Невидимка выключается, вход в команду виден как обычно. После смены карты она не вернётся. |
| Сервер / плагин перевёл админа в команду | Админ снова скрывается; после 3 раз за 30 секунд невидимка выключается. |
| Смена карты, переподключение | Невидимка возвращается сама (`keep_hidden`), до рестарта сервера. |
| Выход с сервера | Сообщения о выходе нет (`silent_disconnect`). |

## Конфиг `addons/configs/admin_system/hide.ini`

| Ключ | По умолчанию | Назначение |
|---|---|---|
| `permission` | `@admin/hide` | Право для команд и пункта меню; несколько — через `\|` |
| `auto_hide_permission` | пусто | Право «заходить скрытым»; выдаётся явно, `@admin/root` его не даёт |
| `chat_commands` | `!hide;/hide` | Чат-команды через `;` |
| `console_commands` | `mm_hide` | Консольные команды через `;` (Utils создаёт только команды с префиксом `mm_`) |
| `list_chat_commands` / `list_console_commands` | `!hidelist` / `mm_hidelist` | Команды списка скрытых админов |
| `menu_item` | `1` | Пункт в админ-меню |
| `menu_category` | `server` | Категория админ-меню (место в ней — `"item" "hide"` в `sorting.ini`) |
| `menu_category_name` | `Category_Server` | Название категории (ключ перевода Admin System или текст) |
| `hide_pawn` | `1` | Не отправлять другим игрокам камеру и тело скрытого админа (spec list) |
| `silent_team_change` | `1` | Без сообщений о смене команды |
| `silent_death` | `1` | Без киллфида при включении невидимки |
| `silent_connect` | `1` | Без сообщения о входе админа, который вернётся скрытым |
| `silent_disconnect` | `1` | Без сообщения о выходе скрытого админа |
| `block_chat` | `1` | Сообщения скрытого админа в чат никто не видит |
| `block_voice` | `1` | Голос скрытого админа никто не слышит |
| `rehide_on_spectator` | `1` | Снова скрывать, если перевели в наблюдатели |
| `rehide_on_team_change` | `1` | Снова скрывать, если перевели в T/CT не по своему выбору |
| `block_auto_team` | `1` | Не давать `mp_force_pick_time` назначить скрытому админу команду |
| `keep_hidden` | `1` | Возвращать невидимку после смены карты / переподключения |
| `auto_hide_on_spectate` | `0` | Включать невидимку, когда админ сам выбирает «Наблюдатели» в меню команд |
| `teamselect_menu_restore` | `0` | Значение `sv_disable_teamselect_menu` после скрытия (`1`, если меню выбора команды на сервере выключено всегда) |

Переводы — `addons/translations/as_hide.phrases.txt` (ru / ua / en, язык берётся из настроек Utils).

## Если в `meta list` модуль показан как `<ERROR>`

Выполните `meta info <номер>` или посмотрите строку загрузки модуля в начале лога.

- `Plugin uses old SourceHook Metamod build (17 < 18)` — у вас Metamod 2.0 с KHook, а стоит сборка для SourceHook.
  Возьмите `release/hide-linux-x86_64.zip`.
- `Plugin requires newer Metamod version (18 > 17)` — у вас Metamod с SourceHook. Возьмите
  `release/hide-sourcehook-linux-x86_64.zip`.
- `version 'GLIBC_2.xx' not found` — сборка не под Steam Runtime. Сборки из `release/` требуют не больше GLIBC 2.17.
- Модуль выгрузился сам, в консоли `[Hide] Missing Utils system plugin` или `Missing Admin system plugin` —
  не загружены Utils или Admin System. Проверьте их в `meta list`: если они в `<ERROR>`, модуль работать не будет.

## Диагностика

- В логе ошибок `Utils is older than 1.9.1` — обновите Utils: без него spec list'ы видят, за кем следит админ.
- В логе ошибок `Schema field ... not found` — после обновления CS2 поменялись данные игры, нужна новая сборка модуля.
- Spec list'ы, которые работают **на сервере** (например, SpectatorList-CS2), видят состояние игры напрямую и команду
  не проверяют. У SpectatorList-CS2 есть `ExclusionFlag` (по умолчанию `@css/generic`): выдайте этот флаг админам
  в `admins.json` CounterStrikeSharp. Metamod-плагины могут проверять невидимку через API ниже.

## Для разработчиков других плагинов

Модуль отдаёт интерфейс [`Hide/include/hide.h`](Hide/include/hide.h):

```cpp
#include "hide.h"

int ret;
IHideApi* pHide = (IHideApi*)g_SMAPI->MetaFactory(HIDE_INTERFACE, &ret, nullptr);
if (ret != META_IFACE_FAILED && pHide && pHide->IsClientHidden(iSlot))
    continue; // не показывать в spec list
```

В Admin System отправляются действия `hide_on` / `hide_off` (`IAdminApi::OnAction`).

## Сборка

```bash
./build.sh              # обе сборки в release/
./build.sh khook        # только Metamod 2.0 (KHook)
./build.sh sourcehook   # только SourceHook
```

Нужны `git`, `python3` с [AMBuild](https://github.com/alliedmodders/ambuild), `clang` и `zip`. Скрипт сам скачивает
hl2sdk и Metamod:Source на проверенных коммитах в `external/`. Собирайте на glibc 2.31 или старше (Ubuntu 20.04),
иначе модуль не загрузится в Steam Runtime. GitHub Actions собирает обе версии после каждого коммита
(вкладка [Actions](https://github.com/glazki2/CS2SPY/actions/workflows/build.yml), артефакт `hide-linux-x86_64`).

После крупных обновлений CS2 модуль нужно пересобрать с более новым hl2sdk (`HL2SDK_REF` в `build.sh`).

## Лицензия

[GNU GPL v3](LICENSE). Сторонние компоненты — в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
