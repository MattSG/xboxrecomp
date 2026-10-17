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

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "decode_equivalence")) return test_decode_equivalence();
    if (!strcmp(argv[1], "mip_inplace")) return test_mip_inplace();
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 1;
}
