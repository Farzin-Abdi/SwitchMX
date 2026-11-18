#!/usr/bin/env python3
"""Lists UI text in the source that isn't in tools/lang_keys.txt, so nothing ships untranslated.

  tools/check_lang.py            report
  tools/check_lang.py --add      append what's missing to the key list
Only strings the app actually shows are looked at: the labels and descriptions of settings, menu
items, toasts, confirmations and the on-screen keyboard's prompts.
"""
import re, sys, os, glob

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
keys = set(k for k in open(f"{root}/tools/lang_keys.txt", encoding="utf-8").read().split("\n") if k)

# Calls whose given arguments are shown to the user, with the positions that are text.
CALLS = {
    "tr": [0], "trf": [0], "toast": [0], "askText": [0, 1], "confirm": [0, 1, 2],
    "openMenu": [0], "hint": [2], "drawEmpty": [2, 3], "choice": [2, 3], "toggle": [2, 3],
    "slider": [2, 3], "panelRow": [2, 3],
}
STR = r'"((?:[^"\\]|\\.)*)"'
found = {}
for path in sorted(glob.glob(f"{root}/source/*.cpp")):
    if re.search(r"lang_\w\w\.cpp$|lang_tables|font_data|codepages", path): continue
    src = open(path, encoding="utf-8").read()
    for name, slots in CALLS.items():
        for m in re.finditer(rf"\b{name}\s*\(", src):
            i = m.end(); depth = 1; args = [""]; q = False; esc = False
            while i < len(src) and depth:
                c = src[i]
                if esc: esc = False; args[-1] += c
                elif c == "\\": esc = True; args[-1] += c
                elif c == '"': q = not q; args[-1] += c
                elif not q and c in "([{": depth += 1; args[-1] += c
                elif not q and c in ")]}":
                    depth -= 1
                    if depth: args[-1] += c
                elif not q and c == "," and depth == 1: args.append("")
                else: args[-1] += c
                i += 1
            for s in slots:
                if s >= len(args): continue
                a = args[s].strip()
                mm = re.fullmatch(STR, a)
                if mm:
                    t = mm.group(1)
                    if t and t not in keys and not re.fullmatch(r"[\W\d_]*", t):
                        found.setdefault(t, set()).add(os.path.basename(path))
if "--add" in sys.argv and found:
    with open(f"{root}/tools/lang_keys.txt", "a", encoding="utf-8") as f:
        for t in found: f.write(t + "\n")
    print(f"added {len(found)} keys")
elif found:
    print(f"{len(found)} shown strings are not in lang_keys.txt:")
    for t, where in sorted(found.items()): print(f"  [{','.join(sorted(where))}] {t[:95]}")
else:
    print("every shown string is in the key list")
sys.exit(1 if found and "--add" not in sys.argv else 0)
