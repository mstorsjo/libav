/*
 * OMX Video encoder/decoder
 * Copyright (C) 2011 Martin Storsjo
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_OMX_CORE_H
#define AVCODEC_OMX_CORE_H

#include "config.h"

#if CONFIG_OMX_RPI
#define OMX_SKIP64BIT
#endif

#include <stdint.h>
#include <OMX_Core.h>
#include <OMX_Component.h>

#ifdef OMX_SKIP64BIT
static inline OMX_TICKS to_omx_ticks(int64_t value)
{
    OMX_TICKS s;
    s.nLowPart  = value & 0xffffffff;
    s.nHighPart = value >> 32;
    return s;
}
static inline int64_t from_omx_ticks(OMX_TICKS value)
{
    return (((int64_t)value.nHighPart) << 32) | value.nLowPart;
}
#else
#define to_omx_ticks(x) (x)
#define from_omx_ticks(x) (x)
#endif


#define OMX_QCOM_COLOR_FormatYVU420SemiPlanar 0x7FA30C00
#define OMX_TI_COLOR_FormatYUV420PackedSemiPlanar 0x7F000100

typedef struct OMXContext {
    int users;
    void *lib;
    void *lib2;
    OMX_ERRORTYPE (*ptr_Init)(void);
    OMX_ERRORTYPE (*ptr_Deinit)(void);
    OMX_ERRORTYPE (*ptr_ComponentNameEnum)(OMX_STRING, OMX_U32, OMX_U32);
    OMX_ERRORTYPE (*ptr_GetHandle)(OMX_HANDLETYPE*, OMX_STRING, OMX_PTR, OMX_CALLBACKTYPE*);
    OMX_ERRORTYPE (*ptr_FreeHandle)(OMX_HANDLETYPE);
    OMX_ERRORTYPE (*ptr_GetComponentsOfRole)(OMX_STRING, OMX_U32*, OMX_U8**);
    OMX_ERRORTYPE (*ptr_GetRolesOfComponent)(OMX_STRING, OMX_U32*, OMX_U8**);
    void (*host_init)(void);
} OMXContext;

extern OMXContext *ff_omx_context;

int ff_omx_init(void *logctx, const char *libname, const char *prefix);
void ff_omx_deinit(void);

#endif
