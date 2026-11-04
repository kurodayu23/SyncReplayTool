#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把本地 git 历史通过 GitHub Git Data API 推到远端。

为什么不用 git push：本机 github.com:443 的 git smart-HTTP 握手不通
（api.github.com 是通的）。所以走 REST 的 git 数据接口逐个提交上传。

关键点（都踩过）：
  1. `git ls-tree` / `git diff-tree` 默认把非 ASCII 路径转义成八进制串，
     必须加 `-z` 用 NUL 分隔，否则提交上去的路径是乱码。
  2. `diff-tree --raw` 的首列是 `:100644`（冒号与 mode 连写），别当独立字段。
  3. 修改型变更（mode 不变的 M）也要进 tree 条目，否则 tree 为空 ⇒ `Invalid tree info`。
  4. commit 里带上 author/committer 的原始时区，远端 SHA 就和本地**逐个一致**。

用法：
  set GH_TOKEN=<token>
  python scripts/push-github-api.py [分支] [起点sha|起点sha..终点sha]
token 只从环境变量读，不落盘。
"""
import base64
import datetime
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

OWNER = os.environ.get("SRT_GH_OWNER", "kurodayu23")
REPO = os.environ.get("SRT_GH_REPO", "SyncReplayTool")
API = "https://api.github.com"
REPO_DIR = os.environ.get("SRT_REPO_DIR", r"D:\AP\AP7000\SyncReplayTool")
TOKEN = os.environ.get("GH_TOKEN", "")
if not TOKEN:
    raise SystemExit("需要环境变量 GH_TOKEN")

CALLS = {"n": 0}


def api(method, path, data=None, allow_404=False):
    url = path if path.startswith("http") else API + path
    body = json.dumps(data).encode("utf-8") if data is not None else None
    hdrs = {
        "Authorization": "Bearer " + TOKEN,
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "sync-replay-tool-push",
    }
    if body:
        hdrs["Content-Type"] = "application/json"
    for attempt in range(6):
        req = urllib.request.Request(url, data=body, method=method)
        for k, v in hdrs.items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=90) as r:
                CALLS["n"] += 1
                raw = r.read().decode("utf-8")
                return json.loads(raw) if raw.strip() else None
        except urllib.error.HTTPError as e:
            code = e.code
            if code == 404 and allow_404:
                return None
            if code in (403, 429) and attempt < 5:
                wait = 70 if code == 403 else 20
                print("    [限流] %s -> 退避 %ss" % (code, wait), flush=True)
                time.sleep(wait)
                continue
            raise SystemExit("HTTP %s %s %s: %s" % (code, method, path,
                                                   e.read().decode("utf-8", "replace")[:500]))
    raise SystemExit("重试耗尽: %s %s" % (method, path))


def git(*args, binary=False):
    p = subprocess.run(["git", "-C", REPO_DIR] + list(args), capture_output=True)
    if p.returncode != 0:
        raise SystemExit("git %s 失败: %s" % (" ".join(args),
                                             p.stderr.decode("utf-8", "replace")[:500]))
    return p.stdout if binary else p.stdout.decode("utf-8", "replace")


def parse_commit(sha):
    raw = git("cat-file", "commit", sha, binary=True)
    head, _, body = raw.partition(b"\n\n")
    out = {"tree": None, "parents": [], "author": None, "committer": None,
           "message": body.decode("utf-8", "replace")}
    for ln in head.decode("utf-8", "replace").split("\n"):
        if ln.startswith("tree "):
            out["tree"] = ln[5:].strip()
        elif ln.startswith("parent "):
            out["parents"].append(ln[7:].strip())
        elif ln.startswith("author ") or ln.startswith("committer "):
            kind, _, rest = ln.partition(" ")
            name, _, rest2 = rest.rpartition(" <")
            email = rest2.rstrip(">").split(">")[0]
            ts, _, tz = rest2.rstrip(">").split(">")[1].strip().partition(" ")
            out[kind] = {"name": name, "email": email, "ts": int(ts), "tz": tz}
    return out


def iso_utc_minus_offset(ts, tz):
    """git 的 'ts +0800' -> GitHub 接受的 ISO8601（保持原时区偏移）"""
    sign = 1 if tz[0] == "+" else -1
    hh = int(tz[1:3])
    mm = int(tz[3:5])
    local = ts + sign * (hh * 3600 + mm * 60)
    dt = datetime.datetime.fromtimestamp(local, datetime.timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S") + ("%+03d:%02d" % (sign * hh, mm))


def changed_entries(commit_sha, parent_sha):
    """(path, mode, blob_sha) 列表；删除的用 mode=0 标记。必须用 -z（见文件头说明）。"""
    if parent_sha is None:
        raw = git("ls-tree", "-r", "-z", commit_sha)
        out = []
        for rec in raw.split("\0"):
            if not rec.strip():
                continue
            meta, path = rec.split("\t", 1)
            mode, otype, blob = meta.split()
            out.append((path, mode, blob))
        return out
    raw = git("diff-tree", "-r", "--raw", "--no-commit-id", "--no-renames", "-z",
              parent_sha, commit_sha)
    recs = [r for r in raw.split("\0") if r.strip()]
    out = []
    for i in range(0, len(recs) - 1, 2):
        meta, path = recs[i], recs[i + 1]
        parts = meta.split()
        old_mode = parts[0].lstrip(":")
        new_mode = parts[1]
        old_sha = parts[2]
        new_sha = parts[3]
        if old_sha == "0" * 40:
            out.append((path, new_mode, new_sha))          # 新增
        elif new_sha == "0" * 40:
            out.append((path, "0", None))                 # 删除
        else:
            out.append((path, new_mode, new_sha))          # 修改（模式可能同时变）
    return out


def blob_exists(sha):
    return api("GET", "/repos/%s/%s/git/blobs/%s" % (OWNER, REPO, sha), allow_404=True) is not None


def upload_blob(sha):
    content = git("cat-file", "blob", sha, binary=True)
    r = api("POST", "/repos/%s/%s/git/blobs" % (OWNER, REPO),
            {"content": base64.b64encode(content).decode(), "encoding": "base64"})
    if r.get("sha") != sha:
        raise SystemExit("blob sha 不一致: 本地 %s / 远端 %s" % (sha, r.get("sha")))
    return sha


def main():
    branch = sys.argv[1] if len(sys.argv) > 1 else "master"
    spec = sys.argv[2] if len(sys.argv) > 2 else "HEAD~1..HEAD"

    remote_head = api("GET", "/repos/%s/%s/git/ref/heads/%s" % (OWNER, REPO, branch),
                      allow_404=True)
    remote_sha = remote_head["object"]["sha"] if remote_head else None
    print("远端 %s/%s 当前 = %s" % (OWNER, REPO, remote_sha))

    if ".." in spec:
        since, to = spec.split("..", 1)
    else:
        since, to = spec, spec
    commits = git("rev-list", "--reverse", "%s..%s" % (since, to)).split()
    print("待推提交 %d 个" % len(commits))

    parent = remote_sha
    for sha in commits:
        c = parse_commit(sha)
        parent_sha = c["parents"][0] if c["parents"] else None
        entries = []
        for path, mode, blob in changed_entries(sha, parent_sha):
            if mode == "0" or blob is None:
                entries.append({"path": path, "mode": "100644", "type": "blob", "sha": None})
                continue
            if not blob_exists(blob):
                upload_blob(blob)
                time.sleep(0.5)
            entries.append({"path": path, "mode": mode,
                            "type": "commit" if mode == "160000" else "blob", "sha": blob})
        payload = {"tree": entries}
        if parent_sha:
            payload["base_tree"] = git("rev-parse", parent_sha + "^{tree}").strip()
        t = api("POST", "/repos/%s/%s/git/trees" % (OWNER, REPO), payload)
        if t.get("sha") != c["tree"]:
            print("  ! tree 不一致 本地 %s 远端 %s -> 改用完整树" % (c["tree"], t.get("sha")))
            full = []
            for rec in git("ls-tree", "-r", "-z", sha).split("\0"):
                if not rec.strip():
                    continue
                meta, path = rec.split("\t", 1)
                mode, otype, blob = meta.split()
                full.append({"path": path, "mode": mode, "type": otype, "sha": blob})
            t = api("POST", "/repos/%s/%s/git/trees" % (OWNER, REPO), {"tree": full})
        if t.get("sha") != c["tree"]:
            raise SystemExit("tree 无法对齐: 本地 %s / 远端 %s" % (c["tree"], t.get("sha")))
        time.sleep(0.5)

        body = {
            "message": c["message"],
            "tree": c["tree"],
            "parents": c["parents"],
            "author": {"name": c["author"]["name"], "email": c["author"]["email"],
                       "date": iso_utc_minus_offset(c["author"]["ts"], c["author"]["tz"])},
            "committer": {"name": c["committer"]["name"], "email": c["committer"]["email"],
                          "date": iso_utc_minus_offset(c["committer"]["ts"], c["committer"]["tz"])},
        }
        r = api("POST", "/repos/%s/%s/git/commits" % (OWNER, REPO), body)
        got = r.get("sha")
        print("  %s%s  %s" % ("OK " if got == sha else "!! ", sha[:10],
                             c["message"].split("\n")[0][:60]))
        if got != sha:
            raise SystemExit("commit SHA 不一致: 本地 %s / 远端 %s" % (sha, got))
        parent = sha
        time.sleep(0.5)

    api("PATCH", "/repos/%s/%s/git/refs/heads/%s" % (OWNER, REPO, branch),
        {"sha": parent, "force": True})
    print("已更新 %s/%s -> %s（API 调用 %d 次）" % (OWNER, REPO, parent, CALLS["n"]))


if __name__ == "__main__":
    main()
