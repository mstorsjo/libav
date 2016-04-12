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

#include "omx_core.h"
#include "libavutil/avutil.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/time.h>

OMXContext *ff_omx_context;
static pthread_mutex_t omx_context_mutex = PTHREAD_MUTEX_INITIALIZER;

static av_cold void *dlsym_prefixed(void *handle, const char *symbol, const char *prefix)
{
    char buf[50];
    snprintf(buf, sizeof(buf), "%s%s", prefix ? prefix : "", symbol);
    return dlsym(handle, buf);
}

static av_cold int omx_try_load(void *logctx,
                                const char *libname, const char *prefix,
                                const char *libname2)
{
    OMXContext *s = ff_omx_context;
    if (libname2) {
        s->lib2 = dlopen(libname2, RTLD_NOW | RTLD_GLOBAL);
        if (!s->lib2) {
            av_log(logctx, AV_LOG_WARNING, "%s not found\n", libname);
            return AVERROR_ENCODER_NOT_FOUND;
        }
        s->host_init = dlsym(s->lib2, "bcm_host_init");
        if (!s->host_init) {
            av_log(logctx, AV_LOG_WARNING, "bcm_host_init not found\n");
            dlclose(s->lib2);
            s->lib2 = NULL;
            return AVERROR_ENCODER_NOT_FOUND;
        }
    }
    s->lib = dlopen(libname, RTLD_NOW | RTLD_GLOBAL);
    if (!s->lib) {
        av_log(logctx, AV_LOG_WARNING, "%s not found\n", libname);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    s->ptr_Init                = dlsym_prefixed(s->lib, "OMX_Init", prefix);
    s->ptr_Deinit              = dlsym_prefixed(s->lib, "OMX_Deinit", prefix);
    s->ptr_ComponentNameEnum   = dlsym_prefixed(s->lib, "OMX_ComponentNameEnum", prefix);
    s->ptr_GetHandle           = dlsym_prefixed(s->lib, "OMX_GetHandle", prefix);
    s->ptr_FreeHandle          = dlsym_prefixed(s->lib, "OMX_FreeHandle", prefix);
    s->ptr_GetComponentsOfRole = dlsym_prefixed(s->lib, "OMX_GetComponentsOfRole", prefix);
    s->ptr_GetRolesOfComponent = dlsym_prefixed(s->lib, "OMX_GetRolesOfComponent", prefix);
    if (!s->ptr_Init || !s->ptr_Deinit || !s->ptr_ComponentNameEnum ||
        !s->ptr_GetHandle || !s->ptr_FreeHandle ||
        !s->ptr_GetComponentsOfRole || !s->ptr_GetRolesOfComponent) {
        av_log(logctx, AV_LOG_WARNING, "Not all functions found in %s\n", libname);
        dlclose(s->lib);
        s->lib = NULL;
        if (s->lib2)
            dlclose(s->lib2);
        s->lib2 = NULL;
        return AVERROR_ENCODER_NOT_FOUND;
    }
    return 0;
}

av_cold int ff_omx_init(void *logctx, const char *libname, const char *prefix) {
    static const char * const libnames[] = {
#if CONFIG_OMX_RPI
        "/opt/vc/lib/libopenmaxil.so", "/opt/vc/lib/libbcm_host.so",
#else
        "libOMX_Core.so", NULL,
        "libOmxCore.so", NULL,
        "libomxil-bellagio.so", NULL,
#endif
        NULL
    };
    const char* const* nameptr;
    int ret = AVERROR_ENCODER_NOT_FOUND;

    pthread_mutex_lock(&omx_context_mutex);
    if (ff_omx_context) {
        ff_omx_context->users++;
        pthread_mutex_unlock(&omx_context_mutex);
        return 0;
    }

    ff_omx_context = av_mallocz(sizeof(*ff_omx_context));
    if (!ff_omx_context) {
        pthread_mutex_unlock(&omx_context_mutex);
        return AVERROR(ENOMEM);
    }
    ff_omx_context->users = 1;
    if (libname) {
        ret = omx_try_load(logctx, libname, prefix, NULL);
        if (ret < 0) {
            pthread_mutex_unlock(&omx_context_mutex);
            return ret;
        }
    } else {
        for (nameptr = libnames; *nameptr; nameptr += 2)
            if (!(ret = omx_try_load(logctx, nameptr[0], prefix, nameptr[1])))
                break;
        if (!*nameptr) {
            pthread_mutex_unlock(&omx_context_mutex);
            return ret;
        }
    }

    if (ff_omx_context->host_init)
        ff_omx_context->host_init();
    ff_omx_context->ptr_Init();
    pthread_mutex_unlock(&omx_context_mutex);
    return 0;
}

av_cold void ff_omx_deinit(void) {
    pthread_mutex_lock(&omx_context_mutex);
    if (!ff_omx_context) {
        pthread_mutex_unlock(&omx_context_mutex);
        return;
    }
    ff_omx_context->users--;
    if (!ff_omx_context->users) {
        ff_omx_context->ptr_Deinit();
        dlclose(ff_omx_context->lib);
        av_freep(&ff_omx_context);
    }
    pthread_mutex_unlock(&omx_context_mutex);
}

static const struct {
    OMX_COLOR_FORMATTYPE color_format;
    enum AVPixelFormat pix_fmt;
} supported_color_formats[] = {
    { OMX_COLOR_FormatYUV420Planar,              AV_PIX_FMT_YUV420P },
    { OMX_COLOR_FormatYUV420PackedPlanar,        AV_PIX_FMT_YUV420P },
    { OMX_COLOR_FormatYUV420SemiPlanar,          AV_PIX_FMT_NV12    },
    { OMX_COLOR_FormatYUV420PackedSemiPlanar,    AV_PIX_FMT_NV12    },
    { OMX_TI_COLOR_FormatYUV420PackedSemiPlanar, AV_PIX_FMT_NV12    },
    { OMX_QCOM_COLOR_FormatYVU420SemiPlanar,     AV_PIX_FMT_NV21    },
    { OMX_COLOR_FormatUnused,                    AV_PIX_FMT_NONE    },
};

enum AVPixelFormat ff_omx_get_pix_fmt(OMX_COLOR_FORMATTYPE color_format)
{
    int i;
    for (i = 0; supported_color_formats[i].pix_fmt != AV_PIX_FMT_NONE; i++) {
        if (supported_color_formats[i].color_format == color_format)
            return supported_color_formats[i].pix_fmt;
    }
    return AV_PIX_FMT_NONE;
}
