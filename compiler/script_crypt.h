// ============================================================================
//  AutoHotkey Web  -  script_crypt.h
//  Shared obfuscation for the embedded compiled script resource (">AHKWEB SCRIPT<").
//
//  Goal: a compiled .exe must NOT reveal the script as plain UTF-8 to a resource
//  viewer / strings dump. This is obfuscation, not secrecy against a determined
//  reverse engineer (the key travels in the binary, as it must for a password-less
//  self-extracting exe), but it stops casual extraction and keeps the on-disk
//  script bytes from matching signature scanners.
//
//  Container written by AhkWeb2Exe and read by the engine (script.cpp):
//      [0..4]   magic  "AHKW1"
//      [5..8]   uint32 LE  original (decompressed) byte length
//      [9..]    keystream-XORed DEFLATE stream of the original bytes
//  The keystream is a SplitMix64-seeded PRNG; the seed is a fixed build constant
//  mixed with the payload length, so every script gets a different byte pattern.
//
//  Both the compiler (C) and the engine (C++) include this single header, so the
//  two sides can never drift out of sync.
// ============================================================================
#ifndef script_crypt_h
#define script_crypt_h

#include <stdint.h>
#include <string.h>

#define AHKW_SCRIPT_MAGIC   "AHKW1"
#define AHKW_SCRIPT_MAGIC_LEN 5
#define AHKW_SCRIPT_HDR_LEN (AHKW_SCRIPT_MAGIC_LEN + 4)

// Fixed per-build obfuscation constant. Changing it only changes the byte pattern;
// decoding still works because the engine uses the same value. (Not a secret.)
#define AHKW_SCRIPT_SEED 0x9E3779B97F4A7C15ULL

// SplitMix64 keystream. Deterministic, dependency-free, same result on both sides.
static void ahkw_xor_keystream(unsigned char *buf, uint32_t len, uint64_t seed)
{
    uint64_t x = seed;
    uint32_t i = 0;
    while (i < len)
    {
        x += 0x9E3779B97F4A7C15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z =  z ^ (z >> 31);
        for (int b = 0; b < 8 && i < len; ++b, ++i)
            buf[i] ^= (unsigned char)(z >> (b * 8));
    }
}

// Write the 9-byte header (magic + original length). Caller appends the payload.
static void ahkw_write_header(unsigned char *hdr, uint32_t originalLen)
{
    memcpy(hdr, AHKW_SCRIPT_MAGIC, AHKW_SCRIPT_MAGIC_LEN);
    hdr[AHKW_SCRIPT_MAGIC_LEN + 0] = (unsigned char)(originalLen      );
    hdr[AHKW_SCRIPT_MAGIC_LEN + 1] = (unsigned char)(originalLen >>  8);
    hdr[AHKW_SCRIPT_MAGIC_LEN + 2] = (unsigned char)(originalLen >> 16);
    hdr[AHKW_SCRIPT_MAGIC_LEN + 3] = (unsigned char)(originalLen >> 24);
}

// Return 1 and the original length if buf starts with our container magic.
static int ahkw_is_encoded(const unsigned char *buf, uint32_t len, uint32_t *outOriginalLen)
{
    if (len < AHKW_SCRIPT_HDR_LEN) return 0;
    if (memcmp(buf, AHKW_SCRIPT_MAGIC, AHKW_SCRIPT_MAGIC_LEN) != 0) return 0;
    if (outOriginalLen)
        *outOriginalLen = (uint32_t)buf[AHKW_SCRIPT_MAGIC_LEN + 0]
                        | ((uint32_t)buf[AHKW_SCRIPT_MAGIC_LEN + 1] <<  8)
                        | ((uint32_t)buf[AHKW_SCRIPT_MAGIC_LEN + 2] << 16)
                        | ((uint32_t)buf[AHKW_SCRIPT_MAGIC_LEN + 3] << 24);
    return 1;
}

#endif // script_crypt_h
