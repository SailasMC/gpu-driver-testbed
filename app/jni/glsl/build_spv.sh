#!/usr/bin/env bash
# 编译 + 生成与 shaders.h 同格式的 C 数组
# 用法：bash build_spv.sh            （在当前目录产出 *.spv 并打印尺寸）
#       bash build_spv.sh --header   （额外产出 ../shaders_gen.h，可直接拼进 shaders.h）
set -e
GV=${GLSLANG:-glslangValidator}
ENV="--target-env vulkan1.1"        # 我们的 VkApplicationInfo 是 1.1
for f in fs_vert.vert fill_frag.frag fill_frag_float.frag geom_vert.vert geom_frag.frag fill_comp.comp; do
  $GV -V $ENV "$f" -o "${f%.*}.spv" >/dev/null
  printf "  %-22s -> %-22s %6s 字节 (%s words)\n" "$f" "${f%.*}.spv" \
     "$(stat -c%s ${f%.*}.spv)" "$(($(stat -c%s ${f%.*}.spv)/4))"
done
if [ "$1" = "--header" ]; then
  python3 spv2h.py fs_vert.spv fill_frag.spv geom_vert.spv geom_frag.spv fill_comp.spv > ../shaders_gen.h
  echo "  ✓ 已生成 ../shaders_gen.h"
fi
