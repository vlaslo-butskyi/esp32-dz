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
