/* Portable part of the shader cache: SHA-256 and the cache key. The D3DCompile
 * file cache is test_main.c, Windows only. */
#include <stdio.h>
#include <string.h>
#include "nv2a_shader_cache.h"

static unsigned failures;

static void check(int condition, const char *label)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) failures++;
}

int main(void)
{
    static const char source[] =
        "float4 main() : SV_Target { return float4(VALUE,0,0,1); }\n"
        "float4 other() : SV_Target { return float4(VALUE,0,0,1); }\n";
    static const uint8_t abc_sha256[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    static const char long_message[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t long_sha256[32] = {
        0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
        0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1
    };
    static const D3D_SHADER_MACRO boundary_a[] = {{"AB", "C"}, {NULL, NULL}};
    static const D3D_SHADER_MACRO boundary_b[] = {{"A", "BC"}, {NULL, NULL}};
    uint8_t key[32], alternate[32], digest[32];
    NvShaderHash hash;

    nv_shader_hash_init(&hash);
    nv_shader_hash_add(&hash, "a", 1);
    nv_shader_hash_add(&hash, "bc", 2);
    nv_shader_hash_finish(&hash, digest);
    check(!memcmp(digest, abc_sha256, 32), "SHA-256 known digest with split updates");
    nv_shader_hash_init(&hash);
    nv_shader_hash_add(&hash, long_message, sizeof long_message - 1);
    nv_shader_hash_finish(&hash, digest);
    check(!memcmp(digest, long_sha256, 32), "SHA-256 multi-block padding known digest");
    nv_shader_cache_key(source, sizeof source - 1, "test", boundary_a, "main", "ps_5_0", 0, 0, key);
    nv_shader_cache_key(source, sizeof source - 1, "test", boundary_b, "main", "ps_5_0", 0, 0, alternate);
    check(memcmp(key, alternate, 32) != 0, "macro names and values have separate boundaries");
    nv_shader_cache_key(source, sizeof source - 1, "test", boundary_a, "main", "ps_5_0", 0, 1, alternate);
    check(memcmp(key, alternate, 32) != 0, "secondary compile flags change cache identity");
    printf("%u failed assertions\n", failures);
    return failures ? 1 : 0;
}
