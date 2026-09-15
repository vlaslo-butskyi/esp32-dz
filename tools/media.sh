#!/usr/bin/env bash
# Готує відео з папки ДЗ для GitHub:
#   video.mp4 — стиснуте H.264-відео (відкривається в браузері, можна перетягнути в README на github.com)
#   demo.gif  — прев'ю, яке GitHub показує прямо в README
#
# Використання:
#   tools/media.sh <папка або відеофайл> [...]
#
# Приклади:
#   tools/media.sh "dz1.4 (LED + BUTTON)/modes"
#   tools/media.sh "dz1.4 (LED + BUTTON)"/*/            # усі варіанти ДЗ
#   GIF_START=2 GIF_DURATION=6 tools/media.sh "dz1.4 (LED + BUTTON)/toggle"
#   FORCE=1 tools/media.sh "dz1.4 (LED + BUTTON)/modes"  # перегенерувати, навіть якщо файли свіжі
#
# У папці шукається video.mov (регістр неважливий), потім будь-який *.mov / *.m4v.
# Результат кладеться поруч із вихідним відео. Якщо video.mp4 і demo.gif новіші за вихідне відео,
# їх не перезаписує (FORCE=1 змушує перегенерувати).
#
# Налаштування (змінні середовища, у дужках значення за замовчуванням):
#   MP4_WIDTH (1280)   MP4_FPS (30)   MP4_CRF (26, більше = менший файл)   KEEP_AUDIO (1)
#   GIF_WIDTH (420)    GIF_FPS (10)   GIF_COLORS (96)
#   GIF_START (0)      GIF_DURATION (порожньо = до кінця)  — обрізка тільки для GIF
#   FORCE (порожньо)

set -euo pipefail

MP4_WIDTH=${MP4_WIDTH:-1280}
MP4_FPS=${MP4_FPS:-30}
MP4_CRF=${MP4_CRF:-26}
KEEP_AUDIO=${KEEP_AUDIO:-1}
GIF_WIDTH=${GIF_WIDTH:-420}
GIF_FPS=${GIF_FPS:-10}
GIF_COLORS=${GIF_COLORS:-96}
GIF_START=${GIF_START:-0}
GIF_DURATION=${GIF_DURATION:-}
FORCE=${FORCE:-}

GIF_WARN_BYTES=$((5 * 1024 * 1024))    # великі GIF повільно вантажаться в README
MP4_WARN_BYTES=$((10 * 1024 * 1024))   # ліміт drag-and-drop завантаження на GitHub (Free)

if [[ $# -eq 0 ]]; then
  sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

command -v ffmpeg >/dev/null || { echo "ffmpeg не знайдено: brew install ffmpeg" >&2; exit 1; }

shopt -s nullglob nocaseglob

find_source() {
  local dir=$1 f
  # [v] робить шаблон глобом, щоб nocaseglob знаходив і video.MOV на Linux
  for f in "$dir"/video.mo[v] "$dir"/*.mov "$dir"/*.m4v; do
    [[ -f $f ]] || continue
    echo "$f"
    return 0
  done
  return 1
}

size_of() { wc -c <"$1" | tr -d ' '; }

report() {
  local file=$1 warn=$2 hint=$3 bytes
  bytes=$(size_of "$file")
  printf '    %6.1f MB  %s\n' "$(awk "BEGIN { print $bytes / 1048576 }")" "$file"
  if (( bytes > warn )); then
    echo "    ⚠️  завеликий файл: $hint"
  fi
}

is_fresh() { [[ -z $FORCE && -f $1 && $1 -nt $2 ]]; }

make_mp4() {
  local src=$1 out=$2
  local audio=(-an)
  [[ $KEEP_AUDIO == 1 ]] && audio=(-map '0:a:0?' -c:a aac -b:a 96k)

  ffmpeg -v error -stats -y -i "$src" \
    -map 0:v:0 "${audio[@]}" \
    -vf "scale=${MP4_WIDTH}:-2,fps=${MP4_FPS}" \
    -c:v libx264 -preset slow -crf "$MP4_CRF" -pix_fmt yuv420p \
    -movflags +faststart \
    "$out"
}

make_gif() {
  local src=$1 out=$2
  local trim=(-ss "$GIF_START")
  [[ -n $GIF_DURATION ]] && trim+=(-t "$GIF_DURATION")

  ffmpeg -v error -stats -y "${trim[@]}" -i "$src" \
    -vf "fps=${GIF_FPS},scale=${GIF_WIDTH}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=${GIF_COLORS}:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" \
    "$out"
}

status=0
for arg in "$@"; do
  if [[ -d $arg ]]; then
    dir=${arg%/}
    if ! src=$(find_source "$dir"); then
      echo "✗ $dir: не знайдено video.mov / *.mov / *.m4v" >&2
      status=1
      continue
    fi
  elif [[ -f $arg ]]; then
    src=$arg
    dir=$(dirname "$arg")
  else
    echo "✗ $arg: немає такого файлу чи папки" >&2
    status=1
    continue
  fi

  mp4=$dir/video.mp4
  gif=$dir/demo.gif
  echo "▶ $src"

  if is_fresh "$mp4" "$src"; then
    echo "  video.mp4 свіжий, пропускаю"
  else
    echo "  → video.mp4"
    make_mp4 "$src" "$mp4"
  fi

  if is_fresh "$gif" "$src"; then
    echo "  demo.gif свіжий, пропускаю"
  else
    echo "  → demo.gif"
    make_gif "$src" "$gif"
  fi

  report "$mp4" "$MP4_WARN_BYTES" "збільш MP4_CRF (напр. 30) або зменш MP4_WIDTH (напр. 960)"
  report "$gif" "$GIF_WARN_BYTES" "зменш GIF_WIDTH / GIF_FPS або обріж через GIF_START / GIF_DURATION"
done

exit $status
