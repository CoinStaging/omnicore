/*
 * Copyright 2009 Colin Percival, 2011 ArtForz, 2012-2013 pooler
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file was originally written by Colin Percival as part of the Tarsnap
 * online backup system.
 */

#include <crypto/scrypt.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <crypto/pbkdf2_hmac.h>
//#include <sync.h>

#if defined(USE_SSE2) && !defined(USE_SSE2_ALWAYS)
#ifdef _MSC_VER
// MSVC 64bit is unable to use inline asm
#include <intrin.h>
#else
// GCC Linux or i686-w64-mingw32
#include <cpuid.h>
#endif
#endif

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))

//__attribute__((no_sanitize("integer")))
static inline void xor_salsa8(uint32_t B[16], const uint32_t Bx[16])
{
    uint32_t x00,x01,x02,x03,x04,x05,x06,x07,x08,x09,x10,x11,x12,x13,x14,x15;
    int i;

    x00 = (B[ 0] ^= Bx[ 0]);
    x01 = (B[ 1] ^= Bx[ 1]);
    x02 = (B[ 2] ^= Bx[ 2]);
    x03 = (B[ 3] ^= Bx[ 3]);
    x04 = (B[ 4] ^= Bx[ 4]);
    x05 = (B[ 5] ^= Bx[ 5]);
    x06 = (B[ 6] ^= Bx[ 6]);
    x07 = (B[ 7] ^= Bx[ 7]);
    x08 = (B[ 8] ^= Bx[ 8]);
    x09 = (B[ 9] ^= Bx[ 9]);
    x10 = (B[10] ^= Bx[10]);
    x11 = (B[11] ^= Bx[11]);
    x12 = (B[12] ^= Bx[12]);
    x13 = (B[13] ^= Bx[13]);
    x14 = (B[14] ^= Bx[14]);
    x15 = (B[15] ^= Bx[15]);
    for (i = 0; i < 8; i += 2) {
        /* Operate on columns. */
        x04 ^= ROTL(x00 + x12,  7);  x09 ^= ROTL(x05 + x01,  7);
        x14 ^= ROTL(x10 + x06,  7);  x03 ^= ROTL(x15 + x11,  7);

        x08 ^= ROTL(x04 + x00,  9);  x13 ^= ROTL(x09 + x05,  9);
        x02 ^= ROTL(x14 + x10,  9);  x07 ^= ROTL(x03 + x15,  9);

        x12 ^= ROTL(x08 + x04, 13);  x01 ^= ROTL(x13 + x09, 13);
        x06 ^= ROTL(x02 + x14, 13);  x11 ^= ROTL(x07 + x03, 13);

        x00 ^= ROTL(x12 + x08, 18);  x05 ^= ROTL(x01 + x13, 18);
        x10 ^= ROTL(x06 + x02, 18);  x15 ^= ROTL(x11 + x07, 18);

        /* Operate on rows. */
        x01 ^= ROTL(x00 + x03,  7);  x06 ^= ROTL(x05 + x04,  7);
        x11 ^= ROTL(x10 + x09,  7);  x12 ^= ROTL(x15 + x14,  7);

        x02 ^= ROTL(x01 + x00,  9);  x07 ^= ROTL(x06 + x05,  9);
        x08 ^= ROTL(x11 + x10,  9);  x13 ^= ROTL(x12 + x15,  9);

        x03 ^= ROTL(x02 + x01, 13);  x04 ^= ROTL(x07 + x06, 13);
        x09 ^= ROTL(x08 + x11, 13);  x14 ^= ROTL(x13 + x12, 13);

        x00 ^= ROTL(x03 + x02, 18);  x05 ^= ROTL(x04 + x07, 18);
        x10 ^= ROTL(x09 + x08, 18);  x15 ^= ROTL(x14 + x13, 18);
    }
    B[ 0] += x00;
    B[ 1] += x01;
    B[ 2] += x02;
    B[ 3] += x03;
    B[ 4] += x04;
    B[ 5] += x05;
    B[ 6] += x06;
    B[ 7] += x07;
    B[ 8] += x08;
    B[ 9] += x09;
    B[10] += x10;
    B[11] += x11;
    B[12] += x12;
    B[13] += x13;
    B[14] += x14;
    B[15] += x15;
}

void scrypt_N_1_1_256_sp_generic(const char *input, unsigned int inputLen, char *output, char *scratchpad, unsigned int N)
{
    uint8_t B[128];
    uint32_t X[32];
    uint32_t *V;
    uint32_t i, j, k;

    V = (uint32_t *)(((uintptr_t)(scratchpad) + 63) & ~(uintptr_t)(63));

    pbkdf2_hmac_sha256((const uint8_t *)input, inputLen, (const uint8_t *)input, inputLen, 1, 128, B);

    for (k = 0; k < 32; k++)
        X[k] = le32dec(&B[4 * k]);

    for (i = 0; i < N; i++) {
        memcpy(&V[i * 32], X, 128);
        xor_salsa8(&X[0], &X[16]);
        xor_salsa8(&X[16], &X[0]);
    }
    for (i = 0; i < N; i++) {
        j = 32 * (X[16] & (N-1));
        for (k = 0; k < 32; k++)
            X[k] ^= V[j + k];
        xor_salsa8(&X[0], &X[16]);
        xor_salsa8(&X[16], &X[0]);
    }

    for (k = 0; k < 32; k++)
        le32enc(&B[4 * k], X[k]);

    pbkdf2_hmac_sha256((const uint8_t *)input, inputLen, B, 128, 1, 32, (uint8_t *)output);
}

#if defined(USE_SSE2)
// By default, set to generic scrypt function. This will prevent crash in case when scrypt_detect_sse2() wasn't called
void (*scrypt_N_1_1_256_sp_detected)(const char *input, unsigned int inputLen, char *output, char *scratchpad, unsigned int N) = &scrypt_N_1_1_256_sp_generic;

std::string scrypt_detect_sse2()
{
    std::string ret;
#if defined(USE_SSE2_ALWAYS)
    ret = "scrypt: using scrypt-sse2 as built.";
#else // USE_SSE2_ALWAYS
    // 32bit x86 Linux or Windows, detect cpuid features
    unsigned int cpuid_edx=0;
#if defined(_MSC_VER)
    // MSVC
    int x86cpuid[4];
    __cpuid(x86cpuid, 1);
    cpuid_edx = (unsigned int)buffer[3];
#else // _MSC_VER
    // Linux or i686-w64-mingw32 (gcc-4.6.3)
    unsigned int eax, ebx, ecx;
    __get_cpuid(1, &eax, &ebx, &ecx, &cpuid_edx);
#endif // _MSC_VER

    if (cpuid_edx & 1<<26)
    {
        scrypt_N_1_1_256_sp_detected = &scrypt_N_1_1_256_sp_sse2;
        ret = "scrypt: using scrypt-sse2 as detected";
    }
    else
    {
        scrypt_N_1_1_256_sp_detected = &scrypt_N_1_1_256_sp_generic;
        ret = "scrypt: using scrypt-generic, SSE2 unavailable";
    }
#endif // USE_SSE2_ALWAYS
    return ret;
}
#endif

//Mutex cs_scrypt_hash;
//Mutex cs_scrypt2_hash;

void scrypt_N_1_1_256(const char *input, unsigned int inputLen, char *output, unsigned int N)
{
    /*if (N == 1024) {
        LOCK(cs_scrypt_hash);

        // TODO: free scratchpad at application close
        static char *scratchpad = (char*)malloc((size_t)SCRYPT_SCRATCHPAD_SIZE); // Allocate memory once for the lifetime of the application
        if (!scratchpad)
            return;
        scrypt_N_1_1_256_sp(input, inputLen, output, scratchpad, 1024);
    } else if (N == 1048576) {
        LOCK(cs_scrypt2_hash);

        // TODO: free scratchpad at application close
        static char *scratchpad = (char*)malloc((size_t)SCRYPT2_SCRATCHPAD_SIZE); // Allocate memory once for the lifetime of the application
        if (!scratchpad)
            return;
        scrypt_N_1_1_256_sp(input, inputLen, output, scratchpad, 1048576);
    } else*/ {
        char *scratchpad = (char*)malloc((size_t)N * 128 + 63);
        if (!scratchpad)
            return;
        scrypt_N_1_1_256_sp(input, inputLen, output, scratchpad, N);
        free(scratchpad);
    }
}
