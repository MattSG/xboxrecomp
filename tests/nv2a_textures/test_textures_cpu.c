/* CPU texture decoding and mip generation, shared by every renderer. These
 * exercise nv2a_render_common.h and the swizzle/DXT helpers only, so they run
 * on Windows and Linux alike. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/video/nv2a_d3d11.h"
#include "../../src/video/nv2a_render_common.h"
#include "texture_cpu_tests.h"

static int test_mip_policy(int expected)
{
    NvD3DTexture t = {0}; TexLayout layout;
    t.color = 0x06; t.width = t.height = 32; t.levels = 2;
    CHECK(tex_layout(&t, &layout) && layout.levels == 2);
    CHECK(host_levels(&layout, &t) == expected);
    /* Startup-fixed policy cannot silently change existing cache semantics. */
#ifdef _WIN32
    _putenv_s("RECOMP_COMPLETE_MIPS", expected == 2 ? "1" : "0");
#else
    setenv("RECOMP_COMPLETE_MIPS", expected == 2 ? "1" : "0", 1);
#endif
    CHECK(host_levels(&layout, &t) == expected);
    t.levels = 1;
    CHECK(tex_layout(&t, &layout) && host_levels(&layout, &t) == 1);
    t.levels = 2; t.format = (3u << 4) | (2u << 28);
    CHECK(tex_layout(&t, &layout) && host_levels(&layout, &t) == 2);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    if (!strcmp(argv[1], "mip_policy")) { CHECK(argc == 3); return test_mip_policy(atoi(argv[2])); }
    CHECK(argc == 2);
    if (!strcmp(argv[1], "decode_equivalence")) return test_decode_equivalence();
    if (!strcmp(argv[1], "mip_inplace")) return test_mip_inplace();
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 1;
}
