# [AS] Hide — невидимка для админа

Модуль для [Admin System](https://github.com/Pisex/cs2-admin_system) от Pisex (Metamod:Source, CS2).
Команда `!hide` и пункт **«Режим невидимки»** в админ-меню полностью прячут админа:

- **таб (scoreboard)** — админа нет ни в командах, ни в списке наблюдателей
  (он переводится в «без команды», при этом свободно наблюдает за игрой);
- **spec list** — observer-pawn админа не отправляется другим игрокам, поэтому ни spec list'ы,
  ни HUD-моды, ни читы не видят, что админ за кем-то следит;
- **чат и киллфид** — нет сообщений «… присоединяется к наблюдателям», самоубийства при
  включении невидимки и сообщения о выходе скрытого админа с сервера.

Это замена стандартного модуля `Hide` из [cs2-admin_system_modules](https://github.com/Pisex/cs2-admin_system_modules):
файлы называются так же (`as_hide`), команды те же (`!hide` / `mm_hide`, право `@admin/hide`).

## Требования

- [Admin System](https://github.com/Pisex/cs2-admin_system)
- [Utils](https://github.com/Pisex/cs2-menus) **1.9.1+** — нужен для скрытия из spec list'ов и тихих событий.
  На Utils 1.7.5–1.9.0 модуль работает в упрощённом режиме (только таб) и пишет об этом в лог ошибок.
- Metamod:Source с SourceHook (1.12 / 2.0 до перехода на KHook) — как и сами Utils/Admin System.

## Установка

1. Если стоит стандартный `Hide` — удалите `addons/metamod/as_hide.vdf` и `addons/admin_modules/as_hide.so`
   (или просто перезапишите их файлами из архива).
2. Скопируйте папку `addons` из архива в `game/csgo/`.
3. Перезапустите сервер или выполните `meta load addons/admin_modules/as_hide`.

Архив собирается в GitHub Actions (вкладка **Actions → Build → артефакт `Hide`**).

## Использование

| Команда | Право | Что делает |
| --- | --- | --- |
| `!hide` / `mm_hide` | `@admin/hide` | Включить / выключить невидимку |
| Админ-меню → «Управление сервером» → «Режим невидимки [ВКЛ/ВЫКЛ]» | `@admin/hide` | То же самое |

- Невидимка выключается, когда админ сам выбирает команду в меню (`jointeam`).
- Если скрытого админа перевёл сервер или другой плагин (`mp_force_pick_time`, баланс, AFK-менеджер),
  он снова скрывается — не больше 3 раз за 30 секунд, чтобы не «воевать» с другим плагином.
  Таймер автоназначения команды (`mp_force_pick_time`) для скрытых админов отключается.
- После смены карты / переподключения невидимка возвращается сама (`keep_hidden`):
  автоматический вход в старую команду в первые секунды после загрузки карты блокируется.
  Если вы сами выберете команду в меню — невидимка не вернётся.
- Чтобы пункт стоял на нужном месте, добавьте `"item" "hide"` в нужную категорию `sorting.ini`.

## Настройки

`addons/configs/admin_system/hide.ini`:

| Параметр | По умолчанию | Описание |
| --- | --- | --- |
| `permission` | `@admin/hide` | Право для команды и пункта меню |
| `chat_commands` | `!hide` | Чат-команды через `;` |
| `console_commands` | `mm_hide` | Консольные команды через `;` (Utils создаёт только команды с префиксом `mm_`) |
| `menu_item` | `1` | Добавлять пункт в админ-меню |
| `menu_category` | `server` | Категория админ-меню |
| `menu_category_name` | `Category_Server` | Название категории (ключ перевода Admin System или текст) |
| `hide_pawn` | `1` | Прятать observer-pawn от других игроков (spec list) |
| `silent_team_change` | `1` | Без сообщений о смене команды |
| `silent_death` | `1` | Без киллфида при включении невидимки |
| `silent_disconnect` | `1` | Без сообщения о выходе скрытого админа |
| `rehide_on_spectator` | `1` | Снова скрывать, если перевели в наблюдатели |
| `rehide_on_team_change` | `1` | Снова скрывать, если перевели в T/CT не по своему выбору (иначе невидимка выключается) |
| `block_auto_team` | `1` | Не давать `mp_force_pick_time` назначить скрытому админу команду |
| `keep_hidden` | `1` | Возвращать невидимку после смены карты / реконнекта |
| `auto_hide_on_spectate` | `0` | Включать невидимку, когда админ сам выбирает «Наблюдатели» в меню команд |
| `teamselect_menu_restore` | `0` | Значение `sv_disable_teamselect_menu`, которое возвращается после скрытия |

Переводы — `addons/translations/as_hide.phrases.txt` (ru / ua / en, язык берётся из настроек Utils).

## Совместимость с другими плагинами

- **Spec list'ы на сервере** (например, [SpectatorList-CS2](https://github.com/srwiruwiru/SpectatorList-CS2))
  смотрят, за кем следит игрок, прямо на сервере и команду не проверяют — скрыть админа от них снаружи нельзя.
  У SpectatorList-CS2 есть `ExclusionFlag` (по умолчанию `@css/generic`): выдайте админам этот флаг
  в `admins.json` CounterStrikeSharp, и они не будут попадать в список. Metamod-плагины могут проверять
  невидимку через API ниже.
- **[CS2-SpecKeeper](https://github.com/vindict6/CS2-SpecKeeper)** совместим: скрытый админ находится «без команды»,
  и SpecKeeper его не запоминает (`KeepUnassignedPlayers: false` по умолчанию). Если он всё же переведёт админа
  в наблюдатели после смены карты, модуль снова его скроет.

## Для разработчиков других плагинов

Spec list'ы и списки админов, работающие на сервере, видят состояние игры напрямую,
поэтому такие плагины должны сами пропускать скрытых админов. Для этого модуль отдаёт интерфейс
[`include/hide.h`](include/hide.h):

```cpp
#include "hide.h"

int ret;
IHideApi* pHide = (IHideApi*)g_SMAPI->MetaFactory(HIDE_INTERFACE, &ret, nullptr);
if (ret != META_IFACE_FAILED && pHide && pHide->IsClientHidden(iSlot))
    continue; // не показывать в spec list
```

Также, как и стандартный модуль, он отправляет в Admin System действия `hide_on` / `hide_off`
(`IAdminApi::OnAction`).

## Сборка

Нужны [AMBuild](https://github.com/alliedmodders/ambuild), [hl2sdk](https://github.com/alliedmodders/hl2sdk) (ветка `cs2`)
и [Metamod:Source](https://github.com/alliedmodders/metamod-source) с SourceHook. Точные коммиты — в
[`.github/workflows/build.yml`](../.github/workflows/build.yml). Собирайте на glibc ≤ 2.31 (например, Ubuntu 20.04),
иначе модуль не загрузится в Steam Runtime.

```bash
cd Hide && mkdir build && cd build
python3 ../configure.py -s cs2 --targets x86_64 --enable-optimize \
  --hl2sdk-manifests=./hl2sdk-manifests --mms_path=/path/to/metamod-source --hl2sdk-root=/path/to/sdks
ambuild
```

Готовые файлы появятся в `build/package/addons`.
