/*
 * Copyright © 2003-2018 Acronis International GmbH.
 */

#include "crc32.h"
#include "log.h"

#ifdef HAVE_CRC32_SSE

/*
 * This is Intel SSE4.2 optimized implementation of CRC32, spending 3 cycles on 8 bytes
 * and achieveing ~3GB/sec on practice.
 * http://download.intel.com/design/intarch/papers/323405.pdf
 */

#ifndef __WINDOWS__

#define sse_crc32_u8(crc, bptr)	do {__asm__ __volatile__("crc32b %2,%1" : "=r"(crc) : "0"(crc), "r"(*(unsigned char *)(bptr))); } while(0)
#define sse_crc32_u64(crc,lptr)	do {__asm__ __volatile__("crc32q %2,%1" : "=r"(crc) : "0"(crc), "r"(*(uint64_t*)(lptr))); } while(0)

#else

/* In Visual Studio 2013 _mm_crc32_u8() generates incorrect code */
#define sse_crc32_u8(crc, bptr) do {crc = crc32_table[0][(crc ^ *(unsigned char *)(bptr)) & 0xff] ^ (crc >> 8); } while(0)
#define sse_crc32_u64(crc,lptr) do {crc = _mm_crc32_u64(crc, *(uint64_t*)(lptr)); } while(0)

#endif

/* ----------------------------------- taken from original crc32_hw1.c SMHasher ----------------------------- */
/* Compile with gcc -O3 -msse4.2 ... */

/* crc32c.c -- compute CRC-32C using the Intel crc32 instruction
 * Copyright (C) 2013 Mark Adler
 * Version 1.1  1 Aug 2013  Mark Adler
 */

/*
   This software is provided 'as-is', without any express or implied
   warranty.  In no event will the author be held liable for any damages
   arising from the use of this software.

   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it
   freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
   3. This notice may not be removed or altered from any source distribution.

   Mark Adler
   madler@alumni.caltech.edu
   */

/* Use hardware CRC instruction on Intel SSE 4.2 processors.  This computes a
   CRC-32C, *not* the CRC-32 used by Ethernet and zip, gzip, etc.
   */

/* Version history:
   1.0  10 Feb 2013  First version
   1.1   1 Aug 2013  Correct comments on why three crc instructions in parallel
   */

#include <stdint.h>
#include <pthread.h>

int crc32_use_sse = -1;

/* CRC-32C (iSCSI) polynomial in reversed bit order. */
#define POLY 0x82f63b78

/* Block sizes for three-way parallel crc computation.  LONG and SHORT must
   both be powers of two.  The associated string constants must be set
   accordingly, for use in constructing the assembler instructions. */
#define LONG 8192
#define LONGx1 8192
#define LONGx2 16384
#define SHORT 256
#define SHORTx1 256
#define SHORTx2 512

/* Tables for hardware crc that shift a crc by LONG and SHORT zeros. */
static pthread_once_t crc32c_once_hw = PTHREAD_ONCE_INIT;
static uint32_t crc32c_long[4][256];
static uint32_t crc32c_short[4][256];

/* Multiply a matrix times a vector over the Galois field of two elements,
   GF(2).  Each element is a bit in an unsigned integer.  mat must have at
   least as many entries as the power of two for most significant one bit in
   vec. */
static inline uint32_t gf2_matrix_times(uint32_t *mat, uint32_t vec)
{
	uint32_t sum;

	sum = 0;
	while (vec) {
		if (vec & 1)
			sum ^= *mat;
		vec >>= 1;
		mat++;
	}
	return sum;
}

/* Multiply a matrix by itself over GF(2).  Both mat and square must have 32
   rows. */
static inline void gf2_matrix_square(uint32_t *square, uint32_t *mat)
{
	int n;

	for (n = 0; n < 32; n++)
		square[n] = gf2_matrix_times(mat, mat[n]);
}

/* Construct an operator to apply len zeros to a crc.  len must be a power of
   two.  If len is not a power of two, then the result is the same as for the
   largest power of two less than len.  The result for len == 0 is the same as
   for len == 1.  A version of this routine could be easily written for any
   len, but that is not needed for this application. */
static void crc32c_zeros_op(uint32_t *even, size_t len)
{
	int n;
	uint32_t row;
	uint32_t odd[32];       /* odd-power-of-two zeros operator */

	/* put operator for one zero bit in odd */
	odd[0] = POLY;              /* CRC-32C polynomial */
	row = 1;
	for (n = 1; n < 32; n++) {
		odd[n] = row;
		row <<= 1;
	}

	/* put operator for two zero bits in even */
	gf2_matrix_square(even, odd);

	/* put operator for four zero bits in odd */
	gf2_matrix_square(odd, even);

	/* first square will put the operator for one zero byte (eight zero bits),
	   in even -- next square puts operator for two zero bytes in odd, and so
	   on, until len has been rotated down to zero */
	do {
		gf2_matrix_square(even, odd);
		len >>= 1;
		if (len == 0)
			return;
		gf2_matrix_square(odd, even);
		len >>= 1;
	} while (len);

	/* answer ended up in odd -- copy to even */
	for (n = 0; n < 32; n++)
		even[n] = odd[n];
}

/* Take a length and build four lookup tables for applying the zeros operator
   for that length, byte-by-byte on the operand. */
static void crc32c_zeros(uint32_t zeros[][256], size_t len)
{
	uint32_t n;
	uint32_t op[32];

	crc32c_zeros_op(op, len);
	for (n = 0; n < 256; n++) {
		zeros[0][n] = gf2_matrix_times(op, n);
		zeros[1][n] = gf2_matrix_times(op, n << 8);
		zeros[2][n] = gf2_matrix_times(op, n << 16);
		zeros[3][n] = gf2_matrix_times(op, n << 24);
	}
}

/* Apply the zeros operator table to crc. */
static inline uint32_t crc32c_shift(uint32_t zeros[][256], uint32_t crc)
{
	return zeros[0][crc & 0xff] ^ zeros[1][(crc >> 8) & 0xff] ^
		zeros[2][(crc >> 16) & 0xff] ^ zeros[3][crc >> 24];
}

/* Initialize tables for shifting crcs. */
static void crc32c_init_hw(void)
{
	crc32c_zeros(crc32c_long, LONG);
	crc32c_zeros(crc32c_short, SHORT);
}

/* Compute CRC-32C using the Intel hardware instruction. */
uint32_t crc32c(const void *buf, size_t len, uint32_t crc)
{
	const unsigned char *next = buf;
	const unsigned char *end;
	uint64_t crc0, crc1, crc2;      /* need to be 64 bits for crc32q */

	/* populate shift tables the first time through */
	pthread_once(&crc32c_once_hw, crc32c_init_hw);

	/* pre-process the crc */
	crc0 = crc ^ 0xffffffff;

	/* compute the crc for up to seven leading bytes to bring the data pointer
	   to an eight-byte boundary */
	while (len && ((uintptr_t)next & 7) != 0) {
		sse_crc32_u8(crc0, next);
		next++;
		len--;
	}

	/* compute the crc on sets of LONG*3 bytes, executing three independent crc
	   instructions, each on LONG bytes -- this is optimized for the Nehalem,
	   Westmere, Sandy Bridge, and Ivy Bridge architectures, which have a
	   throughput of one crc per cycle, but a latency of three cycles */
	while (len >= LONG*3) {
		crc1 = 0;
		crc2 = 0;
		end = next + LONG;
		do {
			sse_crc32_u64(crc0, next);
			sse_crc32_u64(crc1, next + LONGx1);
			sse_crc32_u64(crc2, next + LONGx2);
			next += 8;
		} while (next < end);
		crc0 = crc32c_shift(crc32c_long, crc0) ^ crc1;
		crc0 = crc32c_shift(crc32c_long, crc0) ^ crc2;
		next += LONG*2;
		len -= LONG*3;
	}

	/* do the same thing, but now on SHORT*3 blocks for the remaining data less
	   than a LONG*3 block */
	while (len >= SHORT*3) {
		crc1 = 0;
		crc2 = 0;
		end = next + SHORT;
		do {
			sse_crc32_u64(crc0, next);
			sse_crc32_u64(crc1, next + SHORTx1);
			sse_crc32_u64(crc2, next + SHORTx2);
			next += 8;
		} while (next < end);
		crc0 = crc32c_shift(crc32c_short, crc0) ^ crc1;
		crc0 = crc32c_shift(crc32c_short, crc0) ^ crc2;
		next += SHORT*2;
		len -= SHORT*3;
	}

	/* compute the crc on the remaining eight-byte units less than a SHORT*3
	   block */
	end = next + (len - (len & 7));
	while (next < end) {
		sse_crc32_u64(crc0, next);
		next += 8;
	}
	len &= 7;

	/* compute the crc for up to seven trailing bytes */
	while (len) {
		sse_crc32_u8(crc0, next);
		next++;
		len--;
	}

	/* return a post-processed crc */
	return (uint32_t)(crc0 ^ 0xffffffff);
}
/* ----------------------------------- end of taken from original crc32_hw1.c SMHasher ----------------------------- */

unsigned int crc32up_sse(unsigned int crc, const unsigned char *s, unsigned int len)
{
	return crc32c(s, len, crc);
}

unsigned int crc32_sse(const unsigned char *s, unsigned int len)
{
	return crc32up_sse(0, s, len);
}

#endif /* HAVE_CRC32_SSE */
