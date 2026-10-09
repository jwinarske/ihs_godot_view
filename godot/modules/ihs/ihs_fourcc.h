/**************************************************************************/
/*  ihs_fourcc.h                                                          */
/**************************************************************************/
/* Copyright 2026 Joel Winarske. Licensed under the Apache License 2.0.   */
/**************************************************************************/

#pragma once

#include <cstdint>

// The DRM fourccs targets can carry, without depending on libdrm headers.
#define IHS_FOURCC(a, b, c, d) \
	(uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24))

static constexpr uint32_t IHS_FOURCC_ARGB8888 = IHS_FOURCC('A', 'R', '2', '4'); // B,G,R,A bytes
static constexpr uint32_t IHS_FOURCC_XRGB8888 = IHS_FOURCC('X', 'R', '2', '4');
static constexpr uint32_t IHS_FOURCC_ABGR8888 = IHS_FOURCC('A', 'B', '2', '4'); // R,G,B,A bytes
static constexpr uint32_t IHS_FOURCC_XBGR8888 = IHS_FOURCC('X', 'B', '2', '4');

static constexpr uint64_t IHS_MOD_LINEAR = 0;
static constexpr uint64_t IHS_MOD_INVALID = 0x00ffffffffffffffULL;

// True for the R,G,B,A byte order; false for B,G,R,A. Undefined for others.
inline bool ihs_fourcc_is_rgba(uint32_t p_fourcc) {
	return p_fourcc == IHS_FOURCC_ABGR8888 || p_fourcc == IHS_FOURCC_XBGR8888;
}

inline bool ihs_fourcc_supported(uint32_t p_fourcc) {
	return p_fourcc == IHS_FOURCC_ARGB8888 || p_fourcc == IHS_FOURCC_XRGB8888 ||
			p_fourcc == IHS_FOURCC_ABGR8888 || p_fourcc == IHS_FOURCC_XBGR8888;
}

static constexpr uint32_t IHS_SUPPORTED_FOURCCS[] = {
	IHS_FOURCC_ARGB8888,
	IHS_FOURCC_XRGB8888,
	IHS_FOURCC_ABGR8888,
	IHS_FOURCC_XBGR8888,
};
