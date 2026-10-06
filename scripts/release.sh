#!/bin/bash
# 一个大版本 = 一条命令：构建 → 提交并打 tag → 推送 → 发 GitHub Release（附 APK）
# 用法: bash scripts/release.sh <版本> "<一句话说明>"
set -e
V="$1"; NOTE="${2:-发布 v$V}"
[ -n "$V" ] || { echo "用法: bash scripts/release.sh <版本> [说明]"; exit 1; }
case "$V" in
  *.0) : ;;
  *) echo '✗ 只发大版本（X.0）到 GitHub；当前是小版本 ⇒ 请只在本机/手机测试' >&2; exit 2 ;;
esac
cd "$(dirname "$0")/.."                     # 保证在仓库根
echo "==> [1/4] 构建 v$V"
bash /root/mk.sh "$V" | tail -5
echo "==> [2/4] 同步源码到仓库"
cp -r /root/appsrc/src /root/appsrc/jni /root/appsrc/res /root/appsrc/AndroidManifest.xml /root/appsrc/build_app.sh app/
cp -f /root/appsrc/docs/使用说明.md docs/ 2>/dev/null || true
echo "==> [3/4] 提交并推送"
git add -A
git commit -q -m "v$V: $NOTE" || echo "（无源码改动）"
git tag "v$V" 2>/dev/null || echo "（tag 已存在）"
git push -q origin HEAD:main --tags
echo "==> [4/4] 发 Release"
python3 scripts/gh_release.py "$V" "$NOTE"
echo "==> 完成: https://github.com/SailasMC/gpu-driver-testbed/releases/tag/v$V"
