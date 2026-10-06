#!/bin/bash
# 用法: bash scripts/release.sh <版本号> "<一句话说明>"
# 做四件事: 构建 APK → 提交并打 tag → 推送 → 建 GitHub Release 并附上 APK
set -e
V="$1"; NOTE="${2:-发布 v$V}"
[ -n "$V" ] || { echo "用法: bash scripts/release.sh <版本> [说明]"; exit 1; }
echo "==> 构建 v$V"
bash /root/mk.sh "$V" | tail -6
APK="/data/dsh_downloads/GPUTest-$V.apk"
[ -f "$APK" ] || { echo "✗ 没找到产物 $APK"; exit 1; }
CRED=$(grep -oE 'https://[^@]+@github\.com' /root/repo-verify/.git/config | head -1 | sed 's|https://||;s|@github.com||')
TOK=$(echo "$CRED" | sed 's|.*:||'); USER=$(echo "$CRED" | sed 's|:.*||')
AUTH="https://${USER}:${TOK}@github.com/SailasMC/gpu-driver-testbed.git"
echo "==> 同步源码到仓库树"
cp -r /root/appsrc/src /root/appsrc/jni /root/appsrc/res /root/appsrc/AndroidManifest.xml /root/appsrc/build_app.sh "$PWD/app/" 2>/dev/null || true
cd "$PWD"
git add -A && git commit -m "v$V: $NOTE" || echo "（无改动）"
git tag "v$V" 2>/dev/null || echo "（tag 已存在）"
git remote set-url origin "$AUTH" 2>/dev/null || git remote add origin "$AUTH"
git push -q origin HEAD:main --tags
echo "==> 建 Release v$V"
python3 - "$V" "$NOTE" "$APK" <<'PY'
import sys, json, urllib.request, urllib.error
V, NOTE, APK = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = open("/root/repo-verify/.git/config").read()
import re
cred = re.search(r'https://([^@]+)@github\.com', cfg).group(1)
tok = cred.split(":",1)[1] if ":" in cred else cred
H = {"Authorization": "token " + tok, "User-Agent": "dsh", "Accept": "application/vnd.github+json"}
def api(path, method="GET", payload=None, raw=None, ctype=None):
    data = raw if raw is not None else (json.dumps(payload).encode() if payload else None)
    h = dict(H)
    if ctype: h["Content-Type"] = ctype
    r = urllib.request.Request("https://api.github.com" + path, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(r, timeout=60) as resp: return resp.status, json.load(resp) if resp.headers.get("content-type","").startswith("application/json") else {}
    except urllib.error.HTTPError as e:
        return e.code, json.load(e) if e.headers.get("content-type","").startswith("application/json") else {}
st, rel = api("/repos/SailasMC/gpu-driver-testbed/releases", "POST",
              {"tag_name": "v"+V, "name": "GPU 驱动测试台 v"+V, "body": NOTE, "draft": False, "prerelease": False})
if st not in (200,201):
    print("· Release 创建失败(可能已存在) HTTP", st, rel.get("message"))
    sys.exit(0)
up = rel["upload_url"].split("{")[0]
data = open(APK, "rb").read()
st2, a = api(up.replace("https://api.github.com","") + "?name=GPUTest-%s.apk" % V, "POST",
             raw=data, ctype="application/vnd.android.package-archive")
print("✓ Release:", rel["html_url"])
print("✓ 附件:", a.get("browser_download_url", "(失败)"))
PY
echo "==> 完成"
