/*
 * kernel_crypto.c - Xbox Crypto Functions
 *
 * Implements SHA-1, RC4, HMAC-SHA1, and public key operations.
 *
 * The Xbox uses these for:
 *   - Save game signing (SHA-1 + HMAC)
 *   - Xbox Live authentication (RSA, RC4)
 *   - Content verification (SHA-1)
 *
 * SHA-1, RC4, HMAC, modular exponentiation and DES use software implementations.
 * Private-key and crypto-service entry points below remain unimplemented.
 */

#include "kernel.h"
#include <string.h>

/* Portable SHA-1 state is stored directly in XBOX_SHA_CONTEXT. */

static ULONG sha1_rotate_left(ULONG value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_transform(ULONG state[5], const UCHAR buffer[64])
{
    ULONG a, b, c, d, e, f, k, temp;
    ULONG w[80];
    int i;

    /* Prepare message schedule */
    for (i = 0; i < 16; i++) {
        w[i] = ((ULONG)buffer[i * 4] << 24)
             | ((ULONG)buffer[i * 4 + 1] << 16)
             | ((ULONG)buffer[i * 4 + 2] << 8)
             | ((ULONG)buffer[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1_rotate_left(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        temp = sha1_rotate_left(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotate_left(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

VOID __stdcall xbox_XcSHAInit(PXBOX_SHA_CONTEXT ShaContext)
{
    if (!ShaContext)
        return;

    ShaContext->State[0] = 0x67452301;
    ShaContext->State[1] = 0xEFCDAB89;
    ShaContext->State[2] = 0x98BADCFE;
    ShaContext->State[3] = 0x10325476;
    ShaContext->State[4] = 0xC3D2E1F0;
    ShaContext->Count[0] = 0;
    ShaContext->Count[1] = 0;
    memset(ShaContext->Buffer, 0, 64);
}

VOID __stdcall xbox_XcSHAUpdate(PXBOX_SHA_CONTEXT ShaContext, const UCHAR* Input, ULONG InputLength)
{
    ULONG i, index, part_len;

    if (!ShaContext || !Input || InputLength == 0)
        return;

    index = (ShaContext->Count[0] >> 3) & 0x3F;

    /* Update bit count */
    ShaContext->Count[0] += (InputLength << 3);
    if (ShaContext->Count[0] < (InputLength << 3))
        ShaContext->Count[1]++;
    ShaContext->Count[1] += (InputLength >> 29);

    part_len = 64 - index;

    if (InputLength >= part_len) {
        memcpy(&ShaContext->Buffer[index], Input, part_len);
        sha1_transform(ShaContext->State, ShaContext->Buffer);

        for (i = part_len; i + 63 < InputLength; i += 64) {
            sha1_transform(ShaContext->State, &Input[i]);
        }

        index = 0;
    } else {
        i = 0;
    }

    memcpy(&ShaContext->Buffer[index], &Input[i], InputLength - i);
}

VOID __stdcall xbox_XcSHAFinal(PXBOX_SHA_CONTEXT ShaContext, UCHAR* Digest)
{
    UCHAR finalcount[8];
    UCHAR pad;
    ULONG i;

    if (!ShaContext || !Digest)
        return;

    /* Store bit count (big-endian) */
    for (i = 0; i < 4; i++) {
        finalcount[i]     = (UCHAR)((ShaContext->Count[1] >> ((3 - i) * 8)) & 0xFF);
        finalcount[i + 4] = (UCHAR)((ShaContext->Count[0] >> ((3 - i) * 8)) & 0xFF);
    }

    /* Pad to 56 mod 64 */
    pad = 0x80;
    xbox_XcSHAUpdate(ShaContext, &pad, 1);
    pad = 0x00;
    while ((ShaContext->Count[0] >> 3) % 64 != 56) {
        xbox_XcSHAUpdate(ShaContext, &pad, 1);
    }

    /* Append length */
    xbox_XcSHAUpdate(ShaContext, finalcount, 8);

    /* Output digest (big-endian) */
    for (i = 0; i < 5; i++) {
        Digest[i * 4]     = (UCHAR)((ShaContext->State[i] >> 24) & 0xFF);
        Digest[i * 4 + 1] = (UCHAR)((ShaContext->State[i] >> 16) & 0xFF);
        Digest[i * 4 + 2] = (UCHAR)((ShaContext->State[i] >> 8) & 0xFF);
        Digest[i * 4 + 3] = (UCHAR)((ShaContext->State[i]) & 0xFF);
    }
}

/* ============================================================================
 * RC4 (ARC4)
 *
 * Standard RC4 stream cipher. Used for Xbox Live encryption and some
 * save game protection. Implemented in software for portability.
 * ============================================================================ */

VOID __stdcall xbox_XcRC4Key(PXBOX_RC4_CONTEXT Rc4Context, ULONG KeyLength, const UCHAR* Key)
{
    UCHAR temp;
    ULONG i;
    UCHAR j = 0;

    if (!Rc4Context || !Key || KeyLength == 0)
        return;

    /* KSA (Key-Scheduling Algorithm) */
    for (i = 0; i < 256; i++)
        Rc4Context->S[i] = (UCHAR)i;

    for (i = 0; i < 256; i++) {
        j = j + Rc4Context->S[i] + Key[i % KeyLength];
        temp = Rc4Context->S[i];
        Rc4Context->S[i] = Rc4Context->S[j];
        Rc4Context->S[j] = temp;
    }

    Rc4Context->i = 0;
    Rc4Context->j = 0;
}

VOID __stdcall xbox_XcRC4Crypt(PXBOX_RC4_CONTEXT Rc4Context, ULONG Length, UCHAR* Data)
{
    UCHAR temp;
    ULONG n;

    if (!Rc4Context || !Data || Length == 0)
        return;

    /* PRGA (Pseudo-Random Generation Algorithm) */
    for (n = 0; n < Length; n++) {
        Rc4Context->i++;
        Rc4Context->j += Rc4Context->S[Rc4Context->i];

        temp = Rc4Context->S[Rc4Context->i];
        Rc4Context->S[Rc4Context->i] = Rc4Context->S[Rc4Context->j];
        Rc4Context->S[Rc4Context->j] = temp;

        Data[n] ^= Rc4Context->S[(UCHAR)(Rc4Context->S[Rc4Context->i] + Rc4Context->S[Rc4Context->j])];
    }
}

/* ============================================================================
 * HMAC-SHA1
 *
 * Standard RFC 2104 HMAC using SHA-1. Used for save game authentication.
 * Xbox XcHMAC takes two data buffers to hash (Data1 and Data2, concatenated).
 * ============================================================================ */

VOID __stdcall xbox_XcHMAC(
    const UCHAR* Key, ULONG KeyLength,
    const UCHAR* Data1, ULONG Data1Length,
    const UCHAR* Data2, ULONG Data2Length,
    UCHAR* Digest)
{
    XBOX_SHA_CONTEXT sha;
    UCHAR key_block[64];
    UCHAR ipad[64];
    UCHAR opad[64];
    UCHAR inner_digest[20];
    ULONG i;

    if (!Key || !Digest)
        return;

    /* If key is longer than block size, hash it first */
    memset(key_block, 0, 64);
    if (KeyLength > 64) {
        xbox_XcSHAInit(&sha);
        xbox_XcSHAUpdate(&sha, Key, KeyLength);
        xbox_XcSHAFinal(&sha, key_block);
    } else {
        memcpy(key_block, Key, KeyLength);
    }

    /* Inner padding */
    for (i = 0; i < 64; i++)
        ipad[i] = key_block[i] ^ 0x36;

    /* Outer padding */
    for (i = 0; i < 64; i++)
        opad[i] = key_block[i] ^ 0x5C;

    /* Inner hash: SHA1(ipad || data1 || data2) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, ipad, 64);
    if (Data1 && Data1Length > 0)
        xbox_XcSHAUpdate(&sha, Data1, Data1Length);
    if (Data2 && Data2Length > 0)
        xbox_XcSHAUpdate(&sha, Data2, Data2Length);
    xbox_XcSHAFinal(&sha, inner_digest);

    /* Outer hash: SHA1(opad || inner_digest) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, opad, 64);
    xbox_XcSHAUpdate(&sha, inner_digest, 20);
    xbox_XcSHAFinal(&sha, Digest);

    /* Clean up sensitive data */
    SecureZeroMemory(key_block, sizeof(key_block));
    SecureZeroMemory(ipad, sizeof(ipad));
    SecureZeroMemory(opad, sizeof(opad));
    SecureZeroMemory(inner_digest, sizeof(inner_digest));
}

/* ============================================================================
 * Public Key Operations
 *
 * Xbox RSA1 blobs contain five little-endian ULONGs (magic, buffer bytes,
 * modulus bits, maximum plaintext bytes, public exponent), then the modulus.
 * Signatures and raw RSA operands are little-endian too. See XboxDev/xbedump
 * xboxlib.c and xbox_kernel_test_suite/src/tests/xc/suite/PK.c.
 * ============================================================================ */

ULONG __stdcall xbox_XcPKGetKeyLen(PVOID PublicKey)
{
    ULONG length = 0;
    if (PublicKey)
        memcpy(&length, (const UCHAR *)PublicKey + 4, sizeof length);
    return length;
}

ULONG __stdcall xbox_XcPKDecPrivate(PVOID PrivateKey, PVOID Input, PVOID Output)
{
    (void)PrivateKey;
    (void)Input;
    (void)Output;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO, "XcPKDecPrivate: stubbed (no Xbox Live)");
    return 0; /* Failure */
}

ULONG __stdcall xbox_XcPKEncPublic(PVOID PublicKey, PVOID Input, PVOID Output)
{
    ULONG header[5], bytes, n, *work, *base, *exponent, *modulus;
    ULONG ok;
    if (!PublicKey || !Input || !Output)
        return 0;
    memcpy(header, PublicKey, sizeof header);
    bytes = header[2] / 8;
    if (header[0] != 0x31415352u || !bytes || bytes > 4096 ||
        (header[2] & 31) || header[1] < bytes || header[1] > bytes + 8 ||
        !header[4])
        return 0;
    n = bytes / 4;
    work = (ULONG *)calloc((size_t)n * 4, sizeof(ULONG));
    if (!work)
        return 0;
    base = work + n; exponent = base + n; modulus = exponent + n;
    memcpy(base, Input, bytes);
    memcpy(modulus, (const UCHAR *)PublicKey + 20, bytes);
    exponent[0] = header[4];
    /* An all-zero/even modulus is not an RSA key. */
    ok = (modulus[0] & 1) && ((const UCHAR *)modulus)[bytes - 1] &&
        xbox_XcModExp(work, base, exponent, modulus, n);
    if (ok) {
        memcpy(Output, work, bytes);
        memset((UCHAR *)Output + bytes, 0, header[1] - bytes);
    }
    free(work);
    return ok;
}

BOOLEAN __stdcall xbox_XcVerifyPKCS1Signature(PVOID Signature, PVOID PublicKey, PVOID Hash)
{
    static const UCHAR sha1_der[] = {
        0x30,0x21,0x30,0x09,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x05,0x00,0x04,0x14
    };
    static const UCHAR sha1_der_no_null[] = {
        0x30,0x1f,0x30,0x07,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x04,0x14
    };
    ULONG header[5], bytes, i, tail;
    UCHAR *decoded;
    int less = 0;
    BOOLEAN valid = FALSE;
    if (!Hash || !PublicKey || !Signature)
        return FALSE;
    memcpy(header, PublicKey, sizeof header);
    bytes = header[2] / 8;
    if (header[0] != 0x31415352u || bytes < 32 || bytes > 4096 ||
        (header[2] & 31) || header[1] < bytes || header[1] > bytes + 8)
        return FALSE;
    /* RSAVP1 rejects s >= n; reducing it would accept equivalent signatures. */
    for (i = bytes; i-- > 0;) {
        UCHAR s = ((const UCHAR *)Signature)[i];
        UCHAR m = ((const UCHAR *)PublicKey)[20 + i];
        if (s != m) { less = s < m; break; }
    }
    if (!less)
        return FALSE;
    decoded = (UCHAR *)malloc(header[1]);
    if (!decoded)
        return FALSE;
    if (!xbox_XcPKEncPublic(PublicKey, Signature, decoded))
        goto done;
    /* Work in network order for the PKCS#1 block; Xbox accepts SHA1 DER
     * with/without NULL and the historical raw-digest form. */
    for (i = 0; i < bytes / 2; i++) {
        UCHAR t = decoded[i]; decoded[i] = decoded[bytes - 1 - i]; decoded[bytes - 1 - i] = t;
    }
    if (decoded[0] || decoded[1] != 1)
        goto done;
    for (i = 2; i < bytes && decoded[i] == 0xff; i++) {}
    if (i < 10 || i >= bytes || decoded[i++] != 0)
        goto done;
    tail = bytes - i;
    if (tail == 20)
        valid = memcmp(decoded + i, Hash, 20) == 0;
    else if (tail == sizeof sha1_der + 20)
        valid = !memcmp(decoded + i, sha1_der, sizeof sha1_der) &&
                !memcmp(decoded + i + sizeof sha1_der, Hash, 20);
    else if (tail == sizeof sha1_der_no_null + 20)
        valid = !memcmp(decoded + i, sha1_der_no_null, sizeof sha1_der_no_null) &&
                !memcmp(decoded + i + sizeof sha1_der_no_null, Hash, 20);
done:
    free(decoded);
    return valid;
}

/* ============================================================================
 * XcModExp: Result = Base ^ Exponent mod Modulus, each ModulusLength
 * little-endian dwords. XNet's Diffie-Hellman key exchange runs on it: a
 * stub leaves each console with a different "shared" secret and secure
 * connections (system link joins) never authenticate.
 * ============================================================================ */

static int bn_cmp(const ULONG *a, const ULONG *b, ULONG n)
{
    while (n--)
        if (a[n] != b[n])
            return a[n] < b[n] ? -1 : 1;
    return 0;
}

/* r = (r + b) mod m, for r, b < m (b may be r). */
static void bn_addmod(ULONG *r, const ULONG *b, const ULONG *m, ULONG n)
{
    unsigned long long c = 0;
    ULONG i;
    for (i = 0; i < n; i++) {
        c += (unsigned long long)r[i] + b[i];
        r[i] = (ULONG)c;
        c >>= 32;
    }
    if (c || bn_cmp(r, m, n) >= 0) {
        long long borrow = 0;
        for (i = 0; i < n; i++) {
            borrow += (long long)r[i] - m[i];
            r[i] = (ULONG)borrow;
            borrow >>= 32;
        }
    }
}

/* out = a * b mod m, b < m. ponytail: bit-serial; consider Montgomery
 * multiplication if profiling identifies modular exponentiation as a bottleneck. */
static void bn_mulmod(ULONG *out, const ULONG *a, const ULONG *b, const ULONG *m, ULONG n, ULONG *r)
{
    long bit;
    memset(r, 0, n * 4);
    for (bit = (long)n * 32 - 1; bit >= 0; bit--) {
        bn_addmod(r, r, m, n);
        if (a[bit >> 5] >> (bit & 31) & 1)
            bn_addmod(r, b, m, n);
    }
    memcpy(out, r, n * 4);
}

ULONG __stdcall xbox_XcModExp(PULONG Result, PULONG Base, PULONG Exponent, PULONG Modulus, ULONG ModulusLength)
{
    ULONG n = ModulusLength, *t, *one, *base, *acc;
    long bit;
    int started = 0;
    if (!Result || !Base || !Exponent || !Modulus || !n || n > 1024)
        return 0;
    t = (ULONG *)calloc(4 * (size_t)n, 4);
    if (!t)
        return 0;
    one = t + n; base = one + n; acc = base + n;
    one[0] = 1;
    bn_mulmod(base, Base, one, Modulus, n, t);      /* Base mod m */
    bn_mulmod(acc, one, one, Modulus, n, t);        /* 1 mod m */
    for (bit = (long)n * 32 - 1; bit >= 0; bit--) {
        int set = Exponent[bit >> 5] >> (bit & 31) & 1;
        if (started)
            bn_mulmod(acc, acc, acc, Modulus, n, t);
        if (set) {
            bn_mulmod(acc, acc, base, Modulus, n, t);
            started = 1;
        }
    }
    memcpy(Result, acc, n * 4);
    free(t);
    return 1;
}

/* ============================================================================
 * DES / 3DES (FIPS 46-3). XNet encrypts secure LAN traffic with 3DES-CBC.
 * CipherSelect 0 = DES, 1 = two-key 3DES (EDE, 16-byte K1,K2; K3=K1); Operation 1 = encrypt,
 * 0 = decrypt. The key table is opaque to titles (XC_SERVICE_DES_TABLESIZE
 * 128, 3DES 384): here 16 round subkeys per DES key.
 * ============================================================================ */

static const UCHAR des_ip[64] = {58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,
    64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,
    63,55,47,39,31,23,15,7};
static const UCHAR des_fp[64] = {40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,
    37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,
    33,1,41,9,49,17,57,25};
static const UCHAR des_e[48] = {32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
    16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1};
static const UCHAR des_p[32] = {16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,
    19,13,30,6,22,11,4,25};
static const UCHAR des_pc1[56] = {57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,
    60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4};
static const UCHAR des_pc2[48] = {14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32};
static const UCHAR des_sbox[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
     4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
     0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
     13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
     10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
     4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
     9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
     1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
     7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}};

/* Output bit i (MSB first) is input bit t[i] (1-based, MSB first, of w bits). */
static unsigned long long des_perm(unsigned long long in, int w, const UCHAR *t, int n)
{
    unsigned long long out = 0;
    int i;
    for (i = 0; i < n; i++)
        out = out << 1 | (in >> (w - t[i]) & 1);
    return out;
}

static unsigned long long des_load(const UCHAR *p)
{
    unsigned long long v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

static void des_store(UCHAR *p, unsigned long long v)
{
    int i;
    for (i = 7; i >= 0; i--, v >>= 8)
        p[i] = (UCHAR)v;
}

static void des_schedule(unsigned long long sub[16], const UCHAR *key)
{
    static const UCHAR shifts[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
    unsigned long long cd = des_perm(des_load(key), 64, des_pc1, 56);
    ULONG c = (ULONG)(cd >> 28), d = (ULONG)cd & 0xFFFFFFF;
    int r;
    for (r = 0; r < 16; r++) {
        c = (c << shifts[r] | c >> (28 - shifts[r])) & 0xFFFFFFF;
        d = (d << shifts[r] | d >> (28 - shifts[r])) & 0xFFFFFFF;
        sub[r] = des_perm((unsigned long long)c << 28 | d, 56, des_pc2, 48);
    }
}

static unsigned long long des_block(unsigned long long x, const unsigned long long sub[16], int encrypt)
{
    ULONG l, r, f, s;
    int i, k;
    x = des_perm(x, 64, des_ip, 64);
    l = (ULONG)(x >> 32); r = (ULONG)x;
    for (i = 0; i < 16; i++) {
        unsigned long long t = des_perm(r, 32, des_e, 48) ^ sub[encrypt ? i : 15 - i];
        s = 0;
        for (k = 0; k < 8; k++) {
            unsigned six = (unsigned)(t >> (42 - 6 * k)) & 0x3F;
            s = s << 4 | des_sbox[k][((six & 0x20) >> 4 | (six & 1)) * 16 + (six >> 1 & 0xF)];
        }
        f = (ULONG)des_perm(s, 32, des_p, 32);
        f ^= l; l = r; r = f;
    }
    return des_perm((unsigned long long)r << 32 | l, 64, des_fp, 64);
}

/* One block through DES or 3DES-EDE with the table XcKeyTable built. */
static unsigned long long xc_block(ULONG cipher, unsigned long long x, const UCHAR *table, int encrypt)
{
    unsigned long long k[3][16];
    if (!cipher) {
        memcpy(k[0], table, sizeof k[0]);
        return des_block(x, k[0], encrypt);
    }
    memcpy(k, table, sizeof k);
    if (encrypt)
        return des_block(des_block(des_block(x, k[0], 1), k[1], 0), k[2], 1);
    return des_block(des_block(des_block(x, k[2], 0), k[1], 1), k[0], 0);
}

VOID __stdcall xbox_XcDESKeyParity(PUCHAR Key, ULONG KeyLength)
{
    ULONG i;
    for (i = 0; Key && i < KeyLength; i++) {
        UCHAR b = Key[i] & 0xFE, ones = 0, v;
        for (v = b; v; v &= v - 1)
            ones++;
        Key[i] = b | !(ones & 1);           /* odd parity */
    }
}

VOID __stdcall xbox_XcKeyTable(ULONG CipherSelect, PVOID KeyTable, const UCHAR* Key)
{
    unsigned long long k[3][16];
    int i, keys = CipherSelect ? 3 : 1;
    if (!KeyTable || !Key)
        return;
    for (i = 0; i < keys; i++)
        des_schedule(k[i], Key + 8 * (i == 2 ? 0 : i));
    memcpy(KeyTable, k, sizeof k[0] * keys);
}

VOID __stdcall xbox_XcBlockCrypt(ULONG CipherSelect, PVOID Output, PVOID Input, PVOID KeyTable, ULONG Operation)
{
    if (Output && Input && KeyTable)
        des_store((UCHAR *)Output, xc_block(CipherSelect, des_load((const UCHAR *)Input),
                                            (const UCHAR *)KeyTable, Operation != 0));
}

VOID __stdcall xbox_XcBlockCryptCBC(
    ULONG CipherSelect, ULONG OutputLength, PVOID Output, PVOID Input,
    PVOID KeyTable, ULONG Operation, PVOID FeedbackVector)
{
    UCHAR *out = (UCHAR *)Output;
    const UCHAR *in = (const UCHAR *)Input;
    unsigned long long fb, x;
    ULONG i;
    if (!Output || !Input || !KeyTable || !FeedbackVector)
        return;
    fb = des_load((const UCHAR *)FeedbackVector);
    for (i = 0; i + 8 <= OutputLength; i += 8) {
        x = des_load(in + i);
        if (Operation) {
            fb = xc_block(CipherSelect, x ^ fb, (const UCHAR *)KeyTable, 1);
            des_store(out + i, fb);
        } else {
            des_store(out + i, xc_block(CipherSelect, x, (const UCHAR *)KeyTable, 0) ^ fb);
            fb = x;
        }
    }
    des_store((UCHAR *)FeedbackVector, fb);
}

VOID __stdcall xbox_XcCryptService(ULONG Operation, PVOID Param)
{
    (void)Operation;
    (void)Param;
}

VOID __stdcall xbox_XcUpdateCrypto(PVOID Param1, PVOID Param2)
{
    (void)Param1;
    (void)Param2;
}
