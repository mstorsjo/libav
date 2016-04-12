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
#include <pthread.h>
#include <sys/time.h>
#include <string.h>
#include "libavutil/attributes.h"

#define INIT_STRUCT(x) do {                                               \
        x.nSize = sizeof(x);                                              \
        x.nVersion = s->version;                                          \
    } while (0)

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

static inline void append_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
                                 int* array_size, OMX_BUFFERHEADERTYPE **array,
                                 OMX_BUFFERHEADERTYPE *buffer)
{
    pthread_mutex_lock(mutex);
    array[(*array_size)++] = buffer;
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);
}

static inline OMX_BUFFERHEADERTYPE *get_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
                                               int* array_size, OMX_BUFFERHEADERTYPE **array,
                                               int wait)
{
    OMX_BUFFERHEADERTYPE *buffer;
    pthread_mutex_lock(mutex);
    if (wait) {
        while (!*array_size)
           pthread_cond_wait(cond, mutex);
    }
    if (*array_size > 0) {
        buffer = array[0];
        (*array_size)--;
        memmove(&array[0], &array[1], (*array_size) * sizeof(OMX_BUFFERHEADERTYPE*));
    } else {
        buffer = NULL;
    }
    pthread_mutex_unlock(mutex);
    return buffer;
}

static inline av_cold void timed_wait(pthread_cond_t *cond, pthread_mutex_t *mutex, int ms)
{
    struct timeval tv;
    struct timespec ts;
    int64_t nsec;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000;
    nsec = ts.tv_nsec + ms * 1000000LL;
    ts.tv_sec += nsec / 1000000000;
    ts.tv_nsec = nsec % 1000000000;
    pthread_cond_timedwait(cond, mutex, &ts);
}

enum AVPixelFormat ff_omx_get_pix_fmt(OMX_COLOR_FORMATTYPE color_format);

#endif
