#!/usr/bin/env python3
# gen_special_files.py - create files whose names are hostile to shells,
# rsync --files-from, and Windows encoding layers.
# Writes manifest.json next to the data dir: [{rel, sha256, size}]
import hashlib
import json
import os
import sys

NAMES = [
    "space name.spe",
    "it's a quote.spe",
    'say "hi".spe',
    "中文名称.spe",
    "混合 mixed 中文.spe",
    "emoji-\U0001F600-ok.spe",
    "-leading-dash.spe",
    "--looks-like-option.spe",
    "percent%20encoded.spe",
    "semi;colon.spe",
    "amp&and.spe",
    "dollar$var.spe",
    "paren(1).spe",
    "bracket[1].spe",
    "star*.spe",
    "question?.spe",
    "hash#tag.spe",
    "at@sign.spe",
    "excl!.spe",
    "tilde~.spe",
    "plus+.spe",
    "equals=.spe",
    "comma,.spe",
    "under_score.spe",
    "dot.name.spe",
    "UPPER.SPE",
    "x" * 250 + ".spe",
    ".spe",              # only an extension - Qt suffix() is empty for this
    "no-extension",      # no suffix at all (the known D1 case)
    "trailing.dot.",     # ends with a dot
]

SUBDIRS = [
    "sub dir/中文目录",
    "sub dir/it's dir",
]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    # The console here is GBK: an emoji in a name would blow up the print.
    # Never print raw names to stdout - only to the manifest file.
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    root = sys.argv[1]
    os.makedirs(root, exist_ok=True)
    created = []
    for d in SUBDIRS:
        os.makedirs(os.path.join(root, d), exist_ok=True)
    for n in NAMES:
        p = os.path.join(root, n)
        try:
            with open(p, "wb") as f:
                f.write(("payload for %s\n" % n).encode("utf-8"))
            created.append(n)
        except OSError as e:
            print("SKIP %r -> %s" % (n, e))
    # two files inside the hostile directories
    for d in SUBDIRS:
        rel = os.path.join(d, "inside file.spe")
        p = os.path.join(root, rel)
        with open(p, "wb") as f:
            f.write(("payload for %s\n" % rel).encode("utf-8"))
        created.append(rel)

    manifest = []
    for dirpath, dirnames, filenames in os.walk(root):
        for fn in filenames:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace("\\", "/")
            manifest.append({
                "rel": rel,
                "size": os.path.getsize(full),
                "sha256": sha256_of(full),
            })
    manifest.sort(key=lambda x: x["rel"])
    with open(os.path.join(root, "..", "special-manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    print("created=%d manifest=%d" % (len(created), len(manifest)))
    for m in manifest:
        print("  %s" % m["rel"])


if __name__ == "__main__":
    main()
