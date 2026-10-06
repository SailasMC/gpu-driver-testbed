#!/usr/bin/env python3
# SPIR-V -> 与现有 shaders.h 完全同格式的 C 数组（_len = 字数，即 字节数/4）
# 用法: python3 spv2h.py fs_vert.spv fill_frag.spv ...   > shaders_gen.h
import sys, struct, os
print("// 自动生成（勿手改）：SPIR-V -> C 数组；_len 是 uint32 字数（codeSize = _len*4）")
print("#pragma once")
print("#include <stdint.h>")
for path in sys.argv[1:]:
    name = os.path.splitext(os.path.basename(path))[0]
    data = open(path, "rb").read()
    assert len(data) % 4 == 0, path + " 不是 4 字节对齐的 SPIR-V"
    words = struct.unpack("<%dI" % (len(data)//4), data)
    assert words[0] == 0x07230203, path + " 不是 SPIR-V 魔数"
    print("\nstatic const uint32_t %s_spv[] = {" % name)
    for i in range(0, len(words), 8):
        print("    " + " ".join("0x%08x," % w for w in words[i:i+8]))
    print("};")
    print("static const unsigned %s_spv_len = %d;   // %d 字节" % (name, len(words), len(data)))
