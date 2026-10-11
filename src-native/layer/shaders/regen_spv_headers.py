# regen the word-array SPV headers from freshly compiled .spv files
# (the campaign embed pattern; heredoc-safe as a file)
import struct, sys

def regen(spv_path, header_path, guard, array_name, words_macro, comment):
    with open(spv_path, "rb") as f:
        blob = f.read()
    assert len(blob) % 4 == 0
    words = struct.unpack("<%dI" % (len(blob) // 4), blob)
    with open(header_path, "w", newline="\n") as f:
        f.write("#ifndef %s\n#define %s\n" % (guard, guard))
        f.write("// %s\n" % comment)
        f.write("static const uint32_t %s[] = {\n" % array_name)
        for i in range(0, len(words), 8):
            f.write("    " + " ".join("0x%08xu," % w for w in words[i:i+8]) + "\n")
        f.write("};\n")
        f.write("#define %s %d\n" % (words_macro, len(words)))
        f.write("#endif\n")
    print("%s: %d words (%d B)" % (header_path, len(words), len(blob)))

d = "C:/Users/David/.zcode/workspace/default/Armada-FSR4-NPU-PORT/layer/shaders/"
regen(d + "wv1_new.spv", d + "wv1_spv.h", "WV1_SPV_H", "wv1_spv", "WV1_SPV_WORDS",
      "NMS-PORT: wv1 RATIO ERA (runtime inv_scale via LR_W/LR_H game dims; u_rcps push word 10; MODEL grid consts)")
regen(d + "wv2a_new.spv", d + "wv2a_spv.h", "WV2A_SPV_H", "wv2a_spv", "WV2A_SPV_WORDS",
      "NMS-PORT: wv2a RATIO ERA (runtime inv_scale; s_out push word 9; 16x16 LDS tile)")
