#!/usr/bin/env python3
"""建/更新 GitHub Release 并上传 APK（带重试；token 只从服务器凭据文件读，不落任何日志）。
用法: python3 scripts/gh_release.py <版本> "<说明>" [APK路径]"""
import re, json, sys, time, os, urllib.request, urllib.error

def token():
    for p in ("/root/repo-verify/.git/config", os.path.expanduser("~/.git-credentials")):
        try:
            s = open(p).read()
        except Exception:
            continue
        m = re.search(r'https://([^@\s]+)@github\.com', s)
        if m:
            c = m.group(1)
            return c.split(":", 1)[1] if ":" in c else c
    sys.exit("✗ 取不到 GitHub token")

TOK = token()
H = {"Authorization": "token " + TOK, "User-Agent": "dsh", "Accept": "application/vnd.github+json"}
REPO = "SailasMC/gpu-driver-testbed"

def call(url, method="GET", data=None, ctype=None, tries=5):
    last = None
    for i in range(tries):
        h = dict(H)
        if ctype:
            h["Content-Type"] = ctype
        try:
            r = urllib.request.Request(url, data=data, headers=h, method=method)
            with urllib.request.urlopen(r, timeout=45) as resp:
                b = resp.read()
                return resp.status, (json.loads(b) if b[:1] in (b"{", b"[") else {})
        except urllib.error.HTTPError as e:
            b = e.read()
            return e.code, (json.loads(b) if b[:1] in (b"{", b"[") else {})
        except Exception as e:
            last = e
            print("  · 第 %d/%d 次失败，5s 后重试：%s" % (i + 1, tries, e))
            time.sleep(5)
    return 0, {"error": str(last)}

def main():
    if len(sys.argv) < 3:
        sys.exit("用法: gh_release.py <版本> <说明> [APK]")
    V, NOTE = sys.argv[1], sys.argv[2]
    APK = sys.argv[3] if len(sys.argv) > 3 else "/data/dsh_downloads/GPUTest-%s.apk" % V
    tag = "v" + V
    st, rel = call("https://api.github.com/repos/%s/releases/tags/%s" % (REPO, tag))
    if st != 200:
        st, rel = call("https://api.github.com/repos/%s/releases" % REPO, "POST",
                       {"tag_name": tag, "name": "GPU 驱动测试台 " + tag, "body": NOTE,
                        "draft": False, "prerelease": False})
        if st not in (200, 201):
            sys.exit("✗ 建 Release 失败 HTTP %s %s" % (st, rel.get("message")))
        print("✓ 已建 Release:", rel["html_url"])
    else:
        print("· Release 已存在:", rel["html_url"])
    name = os.path.basename(APK)
    if name in [a["name"] for a in rel.get("assets", [])]:
        print("· 附件已存在，跳过上传"); return
    if not os.path.isfile(APK):
        sys.exit("✗ 找不到 APK: " + APK)
    data = open(APK, "rb").read()
    up = rel["upload_url"].split("{")[0] + "?name=" + name
    st2, a = call(up, "POST", data=data, ctype="application/vnd.android.package-archive")
    if st2 in (200, 201):
        print("✓ 附件已上传: %s (%.1f KB)" % (a.get("browser_download_url"), a.get("size", 0) / 1024.0))
    else:
        sys.exit("✗ 上传失败 HTTP %s %s" % (st2, a.get("message") or a.get("error")))

main()
