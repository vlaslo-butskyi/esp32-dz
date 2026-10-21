# tools

Шаблон README для домашніх завдань і команди для підготовки відео до GitHub.

## Структура папки ДЗ

```
dzX.Y (ТЕМА)/
└── варіант/
    ├── README.md      ← з tools/README-template.md
    ├── main.cpp
    ├── diagram.json   ← схема Wokwi
    ├── wokwi.toml     ← копія з іншого варіанту (шляхи ../../.pio/build/... розраховані на цю глибину)
    ├── schema.png     ← скриншот схеми з Wokwi
    ├── photo.jpeg
    ├── video.MOV      ← оригінал з телефона
    ├── video.mp4      ← tools/media.sh
    └── demo.gif       ← tools/media.sh
```

ESP-IDF ДЗ (2.6 і далі) мають іншу структуру варіанту — це повноцінний CMake-проєкт:

```
dzX.Y (ТЕМА)/
└── варіант/
    ├── README.md
    ├── CMakeLists.txt       ← проєкт ESP-IDF
    ├── main/
    │   ├── main.cpp
    │   └── CMakeLists.txt   ← idf_component_register(SRCS "main.cpp")
    ├── sdkconfig.defaults    ← CONFIG_IDF_TARGET і те, що відрізняє варіант
    ├── diagram.json
    └── wokwi.toml            ← шлях до .pio/build/idf_<варіант>/firmware.bin
```

`sdkconfig` у репо не лежить: його генерують і `idf.py`, і `pio`.

## ESP-IDF (ДЗ 2.6 і далі)

Ці ДЗ збираються не PlatformIO, а нативним `idf.py`. Кожен варіант — окремий CMake-проєкт: `CMakeLists.txt`, `main/main.cpp`, `main/CMakeLists.txt`, `sdkconfig.defaults`.

ESP-IDF поставлений менеджером версій eim від Espressif:

```sh
brew tap espressif/eim
brew install --cask eim-gui          # /Applications/eim.app + CLI `eim` у $(brew --prefix)/bin
brew install dfu-util                # пререквізит, без нього eim install не стартує
eim install -n true -i v5.5.5 -t esp32s3
```

Де що лежить: IDF у `~/.espressif/v5.5.5/esp-idf`, тулчейни в `~/.espressif/tools`, python-venv у `~/.espressif/tools/python/v5.5.5/venv`, активаційний скрипт `~/.espressif/tools/activate_idf_v5.5.5.sh`. `eim list` показує встановлені версії, `eim select` перемикає активну.

```sh
./tools/idf.sh "dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt" build
./tools/idf.sh "dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt" flash monitor
./tools/idf.sh "dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt" menuconfig
```

`idf.sh` існує через три речі:

- активує середовище скриптом від eim, який сам виставляє `IDF_PATH` і `IDF_PYTHON_ENV_PATH`. Старий `export.sh` із ручної установки падав через `~/.platformio/penv/bin` на початку `PATH` (`can not create a virtual environment again`) — з eim цього немає;
- кличе `idf.py` без `exec`: активаційний скрипт робить його shell-функцією, а не кладе на `PATH`;
- виносить build-каталог у `~/.cache/idf-<номер ДЗ>/<варіант>` (перебивається `IDF_BUILD_DIR`). У назві папки ДЗ є дужки, а CMake і ninja підставляють шляхи в `/bin/sh`, екрануючи пробіли й не екрануючи `(`, тож збірка всередині папки ДЗ падає. Заодно в репо не лишається `build/`.

Версія прибита в скрипті; інша — через `IDF_ACTIVATE=~/.espressif/tools/activate_idf_<версія>.sh`. Порт прошитий той самий, що в `platformio.ini`; інший — через `ESPPORT`.

### Той самий код через PlatformIO

ESP-IDF прошивки збираються і звичним `pio` — у `platformio.ini` є env на кожен варіант:

```sh
pio run -e idf_raw_interrupt -t upload
pio device monitor -e idf_raw_interrupt
```

Код не дублюється: env читає той самий `main/main.cpp` і той самий `sdkconfig.defaults`, що й нативна збірка. Новий варіант — три рядки:

```ini
[env:idf_<варіант>]
extends = espidf
custom_dz_variant = dzX.Y (ТЕМА)/<варіант>
board_build.esp-idf.sdkconfig_path = .pio/build/idf_<варіант>/sdkconfig
```

Як це працює: `tools/pio_idf_variant.py` (pre-script у секції `[espidf]`) кладе `custom_dz_variant` у змінні середовища `DZ_VARIANT` і `SDKCONFIG_DEFAULTS`, а `src/CMakeLists.txt` реєструє рівно один файл прошивки. Через `-D` у `board_build.cmake_extra_args` це не працює: ESP-IDF під час expansion компонентів включає `src/CMakeLists.txt` окремим процесом `cmake -P`, куди визначення з командного рядка не доходять.

Що варто знати про цей шлях:

- IDF тут не той, що в нативній збірці, а з пакета `framework-espidf` — зараз 6.1.0, і версія змінюється разом із платформою. Заміри знімаю на нативній 5.5.5, PlatformIO — для зручності й Wokwi;
- `pio run` без `-e` збирає тільки Arduino-env: у `[platformio]` стоїть `default_envs`;
- будь-яка правка `platformio.ini` змушує PlatformIO стерти всі каталоги в `.pio/build` — перша збірка після цього довга;
- корневий `CMakeLists.txt` і `src/CMakeLists.txt` потрібні саме цьому шляху. У `.git/info/exclude` для них додані винятки — решта кореня, як і раніше, не трекається.

Після зміни версії IDF видали `sdkconfig` у папках прошивок: згенерований старою версією, він дає попередження про неіснуючі або переназвані опції (`SOC_SDM_GROUPS`, `LWIP_DHCP_OPTIONS_LEN`). `sdkconfig.defaults` чіпати не треба.

Для code insight в IDEA компіляційна база генерується сама при збірці — `~/.cache/idf-<номер ДЗ>/<варіант>/compile_commands.json`, `tools/compiledb.sh` для цих проєктів не потрібен.

## Новий README

```sh
DIR="dz1.5 (ТЕМА)/варіант"
cp tools/README-template.md "$DIR/README.md"
grep -n '{{' "$DIR/README.md"   # що ще треба заповнити
```

## Відео → MP4 + GIF

```sh
tools/media.sh "dz1.4 (LED + BUTTON)/modes"     # одна папка
tools/media.sh "dz1.4 (LED + BUTTON)"/*/        # усі варіанти ДЗ
tools/media.sh path/to/clip.mov                 # конкретний файл (результат поруч із ним)
```

Скрипт не чіпає `video.mp4` і `demo.gif`, якщо вони новіші за вихідне відео.

| Змінна | За замовчуванням | Навіщо |
|---|---|---|
| `GIF_START`, `GIF_DURATION` | `0`, до кінця | обрізати GIF до найцікавішого шматка (секунди) |
| `GIF_WIDTH` / `GIF_FPS` / `GIF_COLORS` | `420` / `10` / `96` | менші значення дають менший GIF |
| `MP4_WIDTH` / `MP4_FPS` / `MP4_CRF` | `1280` / `30` / `26` | більший CRF дає менший файл і гіршу якість |
| `KEEP_AUDIO` | `1` | `0` вирізає звук |
| `FORCE` | — | `1` перегенерує файли, навіть якщо вони свіжі |

```sh
GIF_START=2 GIF_DURATION=5 FORCE=1 tools/media.sh "dz1.4 (LED + BUTTON)/toggle"
```

## Ручні команди ffmpeg

> У zsh аргументи з `?`, `*`, `[]` треба брати в лапки, наприклад `'0:a:0?'`, інакше буде `no matches found`.

**MOV → MP4** (H.264, 720p, 30 fps, звук AAC, `faststart` для перегляду в браузері):

```sh
ffmpeg -i video.MOV -map 0:v:0 -map '0:a:0?' \
  -vf "scale=1280:-2,fps=30" -c:v libx264 -preset slow -crf 26 -pix_fmt yuv420p \
  -c:a aac -b:a 96k -movflags +faststart video.mp4
```

**Відео → GIF** (палітра будується під конкретне відео, тому кольори кращі й файл менший):

```sh
ffmpeg -i video.MOV \
  -vf "fps=10,scale=420:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" \
  demo.gif
```

**GIF з шматка відео** (з 2-ї секунди, тривалість 5 с):

```sh
ffmpeg -ss 2 -t 5 -i video.MOV -vf "fps=10,scale=420:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" demo.gif
```

**Фото з відео** (кадр на 3-й секунді), якщо окремого фото немає:

```sh
ffmpeg -ss 3 -i video.MOV -frames:v 1 -q:v 2 photo.jpeg
```

**Розкадровка** (кадр щосекунди, сітка 4×3), щоб швидко знайти потрібний момент для `-ss`:

```sh
ffmpeg -i video.MOV -vf "fps=1,scale=320:-1,tile=4x3" -frames:v 1 frames.png
```

**Інформація про відео** (кодек, роздільна здатність, тривалість):

```sh
ffprobe -v error -show_entries format=duration:stream=codec_name,width,height -of compact video.MOV
```

## Відео на GitHub

- `.mov` чи `.mp4`, закомічені в репозиторій, **не програються** в README. Вони відкриваються лише як посилання («View raw»). Прямо в README видно тільки картинки й GIF, тому шаблон показує `demo.gif` з посиланням на `video.mp4`.
- Щоб у README був справжній плеєр, відкрий README на github.com → Edit і перетягни `video.mp4` у текст. Ліміт такого завантаження 10 МБ на безкоштовному плані. GitHub вставить посилання `https://github.com/user-attachments/assets/...`, яке показується як плеєр.
- Ліміти для файлів у git: від 50 МБ GitHub попереджає, файли понад 100 МБ відхиляє. Великі `video.MOV` можна не комітити, а додати в `.gitignore` рядок `*.MOV`.
