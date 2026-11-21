#!/usr/bin/env python3
"""Перевірка Markdown на правила tools/README-style.md (СТУ).

    python3 tools/readme_lint.py                 # усі README.md у репо
    python3 tools/readme_lint.py path/README.md  # конкретні файли
    python3 tools/readme_lint.py --errors-only ...

ERR — порушення, яке треба виправити. WARN — місце, яке треба перечитати.
Код виходу 1, якщо є хоч один ERR.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP_DIRS = {".pio", ".git", "tools", "managed_components", "build", ".idea", ".vscode"}

MAX_WORDS_STEP = 16
MAX_WORDS_TEXT = 20
MAX_SENTENCES_PARA = 6

# (регулярний вираз, підказка). Межі слів — через lookaround, бо \b у Python
# не враховує апостроф.
W = r"(?<![\w'ʼ’])"
E = r"(?![\w'ʼ’])"

ERR_WORDS = [
    (r"дан(ий|а|е|ого|ому|ій)", "даний → цей (§12)"),
    (r"явля(ється|ються|є собою|ють собою)", "являється → є (§12)"),
    (r"шляхом", "шляхом → через (§12)"),
    (r"з метою", "з метою → щоб (§12)"),
    (r"в якості", "в якості → як (§12)"),
    (r"у (випадку|разі),? якщо", "у випадку якщо → якщо (§12)"),
    (r"на протязі", "на протязі → протягом (§12)"),
    (r"є можливість", "є можливість → можна (§12)"),
    (r"слідуюч\w*", "слідуючий → наступний (§12)"),
    (r"прийма\w* участь", "приймати участь → брати участь (§12)"),
    (r"по замовчуванню", "по замовчуванню → за замовчуванням (§12)"),
    (r"варто зазначити|важливо розуміти|слід підкреслити|давайте", "штамп, видаліть (§12)"),
    (r"(ключов|потужн|елегантн)\w*", "оцінне слово, замініть фактом (§12)"),
    (r"(анти)?дребезг\w*|антибрязк\w*", "брязкіт / debounce (§11)"),
    (r"співпад\w*", "співпадіння → збіг (§11)"),
    (r"таск\w*", "таск → задача (§11)"),
    (r"шедулер\w*", "шедулер → планувальник (§11)"),
    (r"суперлуп\w*", "суперлуп → суперцикл (§11)"),
    (r"фірмвар\w*|скетч\w*", "→ прошивка (§11)"),
    (r"брейдборд\w*|макетк\w*", "→ макетна плата (§11)"),
    (r"ніжк\w*", "ніжка → пін (§11)"),
    (r"пул-?(ап|даун)\w*", "→ підтяжка до живлення / до землі (§11)"),
    (r"інтерапт\w*|хендлер\w*", "→ переривання / обробник переривання (§11)"),
    (r"скважн\w*|дьюті", "→ заповнення (§11)"),
    (r"логік\w*", "→ логічний аналізатор (§11)"),
    (r"(під|при)'?єдна\w*|законект\w*", "→ підключити (§10)"),
    (r"від'?єдна\w*", "→ відключити (§10)"),
    (r"включ(ити|ить|ив|ила|или|іть|и|ає|ають|ений|ена|ено|ення)", "включити → увімкнути (§10)"),
    (r"виключ(ити|ить|ив|ила|или|іть|и|ає|ають|ений|ена|ено)", "виключити → вимкнути (§10)"),
    (r"затисн\w*|зажм\w*|зажа\w*", "→ утримувати (§10)"),
    (r"вистав(ити|ив|ила|или|те|ляти|ляє|ляють|лений|лено)|пропис(ати|ав|ала|али|ую|ує|уємо|уйте)", "→ задати (§10)"),
    (r"збілд\w*|скомпіл(ит|юва)\w*|компіл(ит|юва)\w*", "→ зібрати (§10)"),
    (r"зали(ти|в|ла|ли|вати)|зашит\w*|флешн\w*", "→ прошити (§10)"),
    (r"стартува\w*|стопну\w*", "→ запустити / зупинити (§10)"),
    (r"чекну\w*|протест(ит|ува)\w*", "→ перевірити (§10)"),
    (r"(за|про)мір(ят|юва)\w*", "→ виміряти (§10)"),
    (r"засейв\w*", "→ записати (§10)"),
    (r"(з|ви)читат\w*|(з|ви)читув\w*", "→ прочитати (§10)"),
    (r"поміня\w*", "→ замінити (§10)"),
    (r"переключ\w*", "→ перемикати (§10)"),
    (r"тригер(ну|ит)\w*", "→ спрацювати (§10)"),
]

WARN_WORDS = [
    (r"callback\w*", "callback → колбек (§11)"),
    (r"superloop\w*", "superloop → суперцикл (§11)"),
    (r"PWM", "PWM → ШІМ у тексті (§11)"),
    (r"LEDs?", "LED → світлодіод у тексті (§11)"),
    (r"bounce", "bounce → брязкіт (§11)"),
    (r"вірн\w*", "якщо означає «правильний» → правильний (§12)"),
    (r"(надійн|надзвичайн)\w*", "оцінне слово (§12)"),
]

# §3.3 дієприслівники, §3.4 активні дієприкметники
GERUND = re.compile(W + r"\w{3,}(ючи|учи|ачи|ячи|вши|вшись)" + E, re.I)
PARTICIPLE = re.compile(
    W + r"(\w{2,}?)(учий|ючий|ачий|ячий|учого|ючого|учому|ючому|учим|ючим|"
    r"уча|юча|учої|ючої|учою|ючою|учу|ючу|учих|ючих|учі|ючі|уче|юче)" + E, re.I)
PARTICIPLE_OK = ("ключ", "гаряч", "ледач", "зряч", "незряч", "блискуч", "колюч",
                 "смердюч", "пахуч", "летюч", "плакуч", "живуч", "текуч", "балакуч")

UNITS_UA = r"(мс|мкс|нс|с|В|мВ|мА|А|Ом|кОм|МОм|Гц|кГц|МГц|%)"
UNITS_LAT = r"(ms|us|µs|ns|V|mV|mA|Ω|kΩ|Hz|kHz|MHz)"
NUM_CHECKS = [
    (re.compile(r"\d+\.\d+\s?" + UNITS_UA + E), "ERR", "дробова кома: 3,3 В (§8.2)"),
    (re.compile(r"\d" + UNITS_UA + E), "ERR", "пробіл між числом і одиницею (§8.3)"),
    (re.compile(r"\d\s?" + UNITS_LAT + E), "ERR", "українські одиниці: мс, В, Ом (§8.3)"),
]

EMOJI = re.compile("[\U0001F300-\U0001FAFF☀-➿⭐⭕⏩-⏺✅❌]")
NOT_BUT = re.compile(W + r"не" + E + r"[^.!?\n]{1,80}?,\s*а" + E, re.I)
WORD = re.compile(r"[\w'ʼ’-]*\w[\w'ʼ’-]*")
SENT_SPLIT = re.compile(r"(?<=[.!?…])\s+(?=[«\"(*]*[A-ZА-ЯІЇЄҐ0-9])")
NUMBERED = re.compile(r"^\s*\d+[.)]\s+")
BULLET = re.compile(r"^\s*[-*+]\s+")

ERR_RE = [(re.compile(W + p + E, re.I), h) for p, h in ERR_WORDS]
WARN_RE = [(re.compile(W + p + E), h) for p, h in WARN_WORDS]


def clean(text):
    text = re.sub(r"`[^`]*`", " CODE ", text)
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", " ", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"https?://\S+", " URL ", text)
    return text.replace("**", "").replace("__", "")


def blocks(lines):
    """Повертає (тип, перший_рядок, текст): para, step, item, heading."""
    in_code = in_comment = False
    buf, start, kind = [], 0, "para"

    def flush():
        if buf:
            yield kind, start, " ".join(buf)

    for i, raw in enumerate(lines, 1):
        line = raw.rstrip("\n")
        s = line.strip()
        if s.startswith("```") or s.startswith("~~~"):
            yield from flush(); buf = []
            in_code = not in_code
            continue
        if in_code:
            continue
        if in_comment or s.startswith("<!--"):
            in_comment = "-->" not in s
            yield from flush(); buf = []
            continue
        if not s or s.startswith("|") or s in ("---", "***"):
            yield from flush(); buf = []
            continue
        if s.startswith("#"):
            yield from flush(); buf = []
            yield "heading", i, s.lstrip("#").strip()
            continue
        s = s.lstrip(">").strip()
        if NUMBERED.match(s) or BULLET.match(s):
            yield from flush(); buf = []
            kind, start = ("step" if NUMBERED.match(s) else "item"), i
            buf = [NUMBERED.sub("", BULLET.sub("", s))]
            continue
        if not buf:
            kind, start = "para", i
        buf.append(s)
    yield from flush()


def lint(path):
    out = []
    raw = path.read_text(encoding="utf-8")
    lines = raw.splitlines()

    def add(n, level, msg):
        out.append((n, level, msg))

    for n, line in enumerate(lines, 1):
        if "{{" in line:
            add(n, "ERR", "плейсхолдер {{ }} (§9)")

    not_but = 0
    for kind, n, text in blocks(lines):
        t = clean(text)
        for rx, hint in ERR_RE:
            for m in rx.finditer(t):
                add(n, "ERR", f"«{m.group(0)}»: {hint}")
        for rx, hint in WARN_RE:
            for m in rx.finditer(t):
                add(n, "WARN", f"«{m.group(0)}»: {hint}")
        for rx, level, hint in NUM_CHECKS:
            for m in rx.finditer(t):
                add(n, level, f"«{m.group(0)}»: {hint}")
        if EMOJI.search(text):
            add(n, "ERR", "emoji в тексті (§12)")
        for m in GERUND.finditer(t):
            add(n, "WARN", f"«{m.group(0)}»: дієприслівник, розбийте речення (§3.3)")
        for m in PARTICIPLE.finditer(t):
            if not m.group(1).lower().startswith(PARTICIPLE_OK) and \
               not m.group(0).lower().startswith(PARTICIPLE_OK):
                add(n, "WARN", f"«{m.group(0)}»: активний дієприкметник → «який …» (§3.4)")
        not_but += len(NOT_BUT.findall(t))

        if kind == "heading":
            if t.rstrip().endswith("?"):
                add(n, "WARN", "питання в заголовку (§12)")
            continue

        sentences = [s for s in SENT_SPLIT.split(t) if WORD.search(s)]
        limit = MAX_WORDS_STEP if kind == "step" else MAX_WORDS_TEXT
        for s in sentences:
            words = len(WORD.findall(s))
            if words > limit:
                add(n, "WARN", f"речення {words} слів > {limit} (§4.2): «{s[:60].strip()}…»")
            if kind == "para" and s.rstrip().endswith("?"):
                add(n, "WARN", "риторичне питання (§12)")
        if kind == "step":
            first = WORD.search(t)
            if first and re.search(r"(ти|тися)$", first.group(0)):
                add(n, "WARN", f"«{first.group(0)}»: інфінітив у кроці → наказовий спосіб (§5.2)")
        if kind == "para" and len(sentences) > MAX_SENTENCES_PARA:
            add(n, "WARN", f"абзац {len(sentences)} речень > {MAX_SENTENCES_PARA} (§6.1)")

    if not_but > 1:
        add(0, "WARN", f"«не X, а Y» трапляється {not_but} разів, дозволено 1 (§12)")
    return sorted(out)


def main(argv):
    errors_only = "--errors-only" in argv
    args = [a for a in argv if not a.startswith("--")]
    if args:
        files = [Path(a) for a in args]
    else:
        files = sorted(p for p in ROOT.rglob("README.md")
                       if not SKIP_DIRS.intersection(p.relative_to(ROOT).parts))
    n_err = n_warn = 0
    for f in files:
        for n, level, msg in lint(f):
            if level == "ERR":
                n_err += 1
            else:
                n_warn += 1
                if errors_only:
                    continue
            try:
                name = f.resolve().relative_to(ROOT)
            except ValueError:
                name = f
            print(f"{name}:{n}: {level} {msg}")
    print(f"\n{len(files)} файл(ів): {n_err} ERR, {n_warn} WARN", file=sys.stderr)
    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
