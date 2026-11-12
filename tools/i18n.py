#!/usr/bin/env python3
"""Translation helper for the launcher and menu text (see src/app/i18n.h).

  python3 tools/i18n.py extract   writes assets/lang/messages.pot: every text in tr(), trf()
                                  and N_() calls under src/
  python3 tools/i18n.py check     checks each assets/lang/*.po against the code: every text
                                  translated, the same {0} / %s placeholders, and only characters
                                  the menu font has (Latin-1). Exits 1 on a problem.
  python3 tools/i18n.py update    adds missing texts (untranslated) to each .po and drops
                                  ones the code no longer uses.

To add a language: copy messages.pot to <code>.po, translate the msgstr lines, then add it to
the table in src/app/i18n.cpp and to the language list in CMakeLists.txt.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LANG = ROOT / "assets" / "lang"

# A call's first argument: one or more adjacent string literals.
LIT = r'"(?:[^"\\\n]|\\.)*"'
CALL = re.compile(r'\b(?:tr|trf|N_)\(\s*((?:' + LIT + r'\s*)+)[,)]')
LIT_RE = re.compile(LIT)


def unescape(lit):
    body = lit[1:-1]
    out, i = [], 0
    while i < len(body):
        ch = body[i]
        if ch == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            if nxt == "x":
                m = re.match(r"[0-9a-fA-F]+", body[i + 2:])
                out.append(bytes([int(m.group(0), 16)]))
                i += 2 + len(m.group(0))
                continue
            out.append({"n": b"\n", "t": b"\t", '"': b'"', "\\": b"\\"}.get(nxt, nxt.encode()))
            i += 2
            continue
        out.append(ch.encode("utf-8"))
        i += 1
    return b"".join(out).decode("utf-8")


def strip_comments(src):
    # Drops // and /* */ comments but keeps string literals intact.
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif src[i] == '"':
            m = LIT_RE.match(src, i)
            if not m:
                out.append(src[i]); i += 1; continue
            out.append(m.group(0)); i = m.end()
        elif src[i] == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1]); i = j + 1
        else:
            out.append(src[i]); i += 1
    return "".join(out)


def extract():
    found = {}
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix not in (".cpp", ".h") or path.name.startswith("i18n."):
            continue
        src = strip_comments(path.read_text(encoding="utf-8"))
        for m in CALL.finditer(src):
            text = "".join(unescape(l) for l in LIT_RE.findall(m.group(1)))
            if text:
                line = src.count("\n", 0, m.start()) + 1
                found.setdefault(text, f"{path.relative_to(ROOT)}:{line}")
    return found


def quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    return f'"{s}"'


def parse_po(path):
    entries, field, cur = {}, None, {"id": None, "str": ""}
    def flush():
        if cur["id"] is not None and cur["id"] != "":
            entries[cur["id"]] = cur["str"]
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("msgid "):
            flush()
            cur = {"id": unescape(line[6:]), "str": ""}
            field = "id"
        elif line.startswith("msgstr "):
            cur["str"] = unescape(line[7:])
            field = "str"
        elif line.startswith('"') and field:
            cur[field] = (cur[field] or "") + unescape(line)
    flush()
    return entries


HEADER = 'msgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n"Language: {lang}\\n"\n'


def write_po(path, lang, messages, translations):
    out = [HEADER.format(lang=lang)]
    for text, where in messages.items():
        out.append(f"\n#: {where}\nmsgid {quote(text)}\nmsgstr {quote(translations.get(text, ''))}\n")
    path.write_text("".join(out), encoding="utf-8")


PLACEHOLDER = re.compile(r"\{\d+\}|%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[sdufxXcp%]")


def check(messages):
    ok = True
    for po in sorted(LANG.glob("*.po")):
        tr = parse_po(po)
        missing = [m for m in messages if not tr.get(m)]
        unused = [m for m in tr if m not in messages]
        for m in missing:
            print(f"{po.name}: not translated: {m!r}"); ok = False
        for m in unused:
            print(f"{po.name}: no longer used (run update): {m!r}")
        for m, t in tr.items():
            if not t or m not in messages:
                continue
            a = sorted(PLACEHOLDER.findall(m)) if "{" in m else PLACEHOLDER.findall(m)
            b = sorted(PLACEHOLDER.findall(t)) if "{" in m else PLACEHOLDER.findall(t)
            if a != b:
                print(f"{po.name}: placeholders differ: {m!r} -> {t!r}"); ok = False
            bad = sorted({c for c in t if ord(c) > 0xFF and not 0xE000 <= ord(c) <= 0xE5FF})
            if bad:
                print(f"{po.name}: characters the font can't show {bad}: {t!r}"); ok = False
            if m.endswith("...") != t.endswith("..."):
                print(f"{po.name}: '...' ending differs: {m!r} -> {t!r}"); ok = False
            if ("##" in m) != ("##" in t):
                print(f"{po.name}: ImGui ## id differs: {m!r} -> {t!r}"); ok = False
    return ok


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    messages = extract()
    if cmd == "extract":
        write_po(LANG / "messages.pot", "", messages, {})
        print(f"{len(messages)} texts -> {LANG / 'messages.pot'}")
    elif cmd == "update":
        write_po(LANG / "messages.pot", "", messages, {})
        for po in sorted(LANG.glob("*.po")):
            write_po(po, po.stem, messages, parse_po(po))
    elif cmd == "check":
        sys.exit(0 if check(messages) else 1)
    else:
        print(__doc__); sys.exit(2)


if __name__ == "__main__":
    main()
