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
	CHIP_HAWAII,	/* 8 */
	CHIP_LIVERPOOL,	/* 9 */
	CHIP_GLADIUS,	/* 10 */
	CHIP_MULLINS,	/* 11 */
	CHIP_TOPAZ,	/* 12 */
	CHIP_TONGA,	/* 13 */
	CHIP_FIJI,	/* 15 */
	CHIP_CARRIZO,	/* 16 */
	CHIP_STONEY,	/* 17 */
	CHIP_POLARIS10,	/* 18 */
	CHIP_POLARIS11,	/* 19 */
	CHIP_POLARIS12,	/* 20 */
	CHIP_VEGAM,	/* 21 */
	CHIP_VEGA10,	/* 22 */
	CHIP_VEGA12,	/* 23 */
	CHIP_VEGA20,	/* 24 */
	CHIP_RAVEN,	/* 25 */
	CHIP_ARCTURUS,	/* 26 */
	CHIP_RENOIR,	/* 27 */
	CHIP_ALDEBARAN, /* 28 */
	CHIP_NAVI10,	/* 29 */
	CHIP_CYAN_SKILLFISH,	/* 30 */
	CHIP_NAVI14,	/* 31 */
	CHIP_NAVI12,	/* 32 */
	CHIP_SIENNA_CICHLID,	/* 33 */
	CHIP_NAVY_FLOUNDER,	/* 34 */
	CHIP_VANGOGH,	/* 35 */
	CHIP_DIMGREY_CAVEFISH,	/* 36 */
	CHIP_BEIGE_GOBY,	/* 38 */
	CHIP_YELLOW_CARP,	/* 39 */
	CHIP_LAST,
};

extern const char *amdgpu_asic_name[];

#endif /*__AMD_ASIC_TYPE_H__ */
