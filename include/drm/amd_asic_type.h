/*
 * Copyright 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __AMD_ASIC_TYPE_H__
#define __AMD_ASIC_TYPE_H__
/*
 * Supported ASIC types
 */
enum amd_asic_type {
	CHIP_TAHITI = 0,
	CHIP_PITCAIRN,	/* 1 */
	CHIP_VERDE,	/* 2 */
	CHIP_OLAND,	/* 3 */
	CHIP_HAINAN,	/* 4 */
	CHIP_BONAIRE,	/* 5 */
	CHIP_KAVERI,	/* 6 */
	CHIP_KABINI,	/* 7 */
	CHIP_LIVERPOOL, /* 8 - PS4 Phat/Slim */
	CHIP_GLADIUS,   /* 9 - PS4 Pro */
	CHIP_HAWAII,	/* 10 */
	CHIP_MULLINS,	/* 11 */
	CHIP_TOPAZ,	/* 12 */
	CHIP_TONGA,	/* 13 */
	CHIP_FIJI,	/* 14 */
	CHIP_CARRIZO,	/* 15 */
	CHIP_STONEY,	/* 16 */
	CHIP_POLARIS10,	/* 17 */
	CHIP_POLARIS11,	/* 18 */
	CHIP_POLARIS12,	/* 19 */
	CHIP_VEGAM,	/* 20 */
	CHIP_VEGA10,	/* 21 */
	CHIP_VEGA12,	/* 22 */
	CHIP_VEGA20,	/* 23 */
	CHIP_RAVEN,	/* 24 */
	CHIP_ARCTURUS,	/* 25 */
	CHIP_RENOIR,	/* 26 */
	CHIP_NAVI10,	/* 27 */
	CHIP_NAVI14,	/* 28 */
	CHIP_NAVI12,	/* 29 */
	CHIP_LAST,
};

extern const char *amdgpu_asic_name[];

#endif /*__AMD_ASIC_TYPE_H__ */
