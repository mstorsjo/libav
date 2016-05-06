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

#include "config.h"

#if CONFIG_OMX_RPI
#define OMX_SKIP64BIT
#endif

#include <dlfcn.h>
#include <OMX_Core.h>
#include <OMX_Component.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "h264.h"
#include "internal.h"

#ifdef OMX_SKIP64BIT
static OMX_TICKS to_omx_ticks(int64_t value)
{
    OMX_TICKS s;
    s.nLowPart  = value & 0xffffffff;
    s.nHighPart = value >> 32;
    return s;
}
static int64_t from_omx_ticks(OMX_TICKS value)
{
    return (((int64_t)value.nHighPart) << 32) | value.nLowPart;
}
#else
#define to_omx_ticks(x) (x)
#define from_omx_ticks(x) (x)
#endif

#define INIT_STRUCT(x) do {                                               \
        x.nSize = sizeof(x);                                              \
        x.nVersion = s->version;                                          \
    } while (0)
#define CHECK(x) do {                                                     \
        if (x != OMX_ErrorNone) {                                         \
            av_log(avctx, AV_LOG_ERROR,                                   \
                   "err %x (%d) on line %d\n", x, x, __LINE__);           \
            return AVERROR_UNKNOWN;                                       \
        }                                                                 \
    } while (0)

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

static OMXContext *omx_context;
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
    OMXContext *s = omx_context;
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

static av_cold int omx_init(void *logctx, const char *libname, const char *prefix)
{
    static const char * const libnames[] = {
#if CONFIG_OMX_RPI
        "/opt/vc/lib/libopenmaxil.so", "/opt/vc/lib/libbcm_host.so",
#else
        "libOMX_Core.so", NULL,
        "libOmxCore.so", NULL,
#endif
        NULL
    };
    const char* const* nameptr;
    int ret = AVERROR_ENCODER_NOT_FOUND;

    pthread_mutex_lock(&omx_context_mutex);
    if (omx_context) {
        omx_context->users++;
        pthread_mutex_unlock(&omx_context_mutex);
        return 0;
    }

    omx_context = av_mallocz(sizeof(*omx_context));
    if (!omx_context) {
        pthread_mutex_unlock(&omx_context_mutex);
        return AVERROR(ENOMEM);
    }
    omx_context->users = 1;
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

    if (omx_context->host_init)
        omx_context->host_init();
    omx_context->ptr_Init();
    pthread_mutex_unlock(&omx_context_mutex);
    return 0;
}

static av_cold void omx_deinit(void)
{
    pthread_mutex_lock(&omx_context_mutex);
    if (!omx_context) {
        pthread_mutex_unlock(&omx_context_mutex);
        return;
    }
    omx_context->users--;
    if (!omx_context->users) {
        omx_context->ptr_Deinit();
        dlclose(omx_context->lib);
        av_freep(&omx_context);
    }
    pthread_mutex_unlock(&omx_context_mutex);
}

typedef struct OMXCodecContext {
    const AVClass *class;
    char *libname;
    char *libprefix;
    int omx_inited;

    AVCodecContext *avctx;

    char component_name[OMX_MAX_STRINGNAME_SIZE];
    OMX_VERSIONTYPE version;
    OMX_HANDLETYPE handle;
    int in_port, out_port;
    int reconfigure_out, update_out_def;
    int disabled, enabled;
    OMX_COLOR_FORMATTYPE color_format;
    int stride, plane_size;

    int num_in_buffers, num_out_buffers;
    OMX_BUFFERHEADERTYPE **in_buffer_headers;
    OMX_BUFFERHEADERTYPE **out_buffer_headers;
    int num_free_in_buffers;
    OMX_BUFFERHEADERTYPE **free_in_buffers;
    int num_done_out_buffers;
    OMX_BUFFERHEADERTYPE **done_out_buffers;
    pthread_mutex_t input_mutex;
    pthread_cond_t input_cond;
    pthread_mutex_t output_mutex;
    pthread_cond_t output_cond;

    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    OMX_STATETYPE state;
    OMX_ERRORTYPE error;

    int mutex_cond_inited;

    int eos_sent, got_eos;

    uint8_t *output_buf;
    int output_buf_size;

    int input_zerocopy;
} OMXCodecContext;

static void append_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
                          int* array_size, OMX_BUFFERHEADERTYPE **array,
                          OMX_BUFFERHEADERTYPE *buffer)
{
    pthread_mutex_lock(mutex);
    array[(*array_size)++] = buffer;
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);
}

static OMX_BUFFERHEADERTYPE *get_buffer(pthread_mutex_t *mutex, pthread_cond_t *cond,
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

static OMX_ERRORTYPE event_handler(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_EVENTTYPE event,
                                   OMX_U32 data1, OMX_U32 data2, OMX_PTR event_data)
{
    OMXCodecContext *s = app_data;
    // This uses casts in the printfs, since OMX_U32 actually is a typedef for
    // unsigned long in official header versions (but there are also modified
    // versions where it is something else).
    switch (event) {
    case OMX_EventError:
        pthread_mutex_lock(&s->state_mutex);
        av_log(s->avctx, AV_LOG_ERROR, "OMX error %"PRIx32"\n", (uint32_t) data1);
        s->error = data1;
        pthread_cond_broadcast(&s->state_cond);
        pthread_mutex_unlock(&s->state_mutex);
        break;
    case OMX_EventCmdComplete:
        if (data1 == OMX_CommandStateSet) {
            pthread_mutex_lock(&s->state_mutex);
            s->state = data2;
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX state changed to %"PRIu32"\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortDisable) {
            pthread_mutex_lock(&s->state_mutex);
            s->disabled = 1;
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX port %"PRIu32" disabled\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortEnable) {
            pthread_mutex_lock(&s->state_mutex);
            s->enabled = 1;
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX port %"PRIu32" enabled\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else {
            av_log(s->avctx, AV_LOG_VERBOSE, "OMX command complete, command %"PRIu32", value %"PRIu32"\n",
                                             (uint32_t) data1, (uint32_t) data2);
        }
        break;
    case OMX_EventPortSettingsChanged:
        av_log(s->avctx, AV_LOG_VERBOSE, "OMX port %"PRIu32" settings changed\n", (uint32_t) data1);
        pthread_mutex_lock(&s->input_mutex);
        pthread_mutex_lock(&s->output_mutex);
        if (s->out_port == data1 && (data2 == 0 || data2 == OMX_IndexParamPortDefinition)) {
            s->reconfigure_out = 1;
            pthread_cond_broadcast(&s->input_cond);
            pthread_cond_broadcast(&s->output_cond);
        } else if (s->out_port == data1 && data2 == OMX_IndexConfigCommonOutputCrop) {
            s->update_out_def = 1;
            pthread_cond_broadcast(&s->input_cond);
            pthread_cond_broadcast(&s->output_cond);
        }
        pthread_mutex_unlock(&s->output_mutex);
        pthread_mutex_unlock(&s->input_mutex);
        break;
    default:
        av_log(s->avctx, AV_LOG_VERBOSE, "OMX event %d %"PRIx32" %"PRIx32"\n",
                                         event, (uint32_t) data1, (uint32_t) data2);
        break;
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE empty_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                       OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecContext *s = app_data;
    if (s->input_zerocopy) {
        if (buffer->pAppPrivate) {
            if (buffer->pOutputPortPrivate)
                av_free(buffer->pAppPrivate);
            else
                av_frame_free((AVFrame**)&buffer->pAppPrivate);
            buffer->pAppPrivate = NULL;
        }
    }
    append_buffer(&s->input_mutex, &s->input_cond,
                  &s->num_free_in_buffers, s->free_in_buffers, buffer);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data,
                                      OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecContext *s = app_data;
    append_buffer(&s->output_mutex, &s->output_cond,
                  &s->num_done_out_buffers, s->done_out_buffers, buffer);
    return OMX_ErrorNone;
}

static const OMX_CALLBACKTYPE callbacks = {
    event_handler,
    empty_buffer_done,
    fill_buffer_done
};

static av_cold int find_component(void *logctx, const char *role, char *str,
                                  int str_size)
{
    OMX_U32 i, num = 0;
    char **components;
    int ret = 0;

#if CONFIG_OMX_RPI
    if (av_strstart(role, "video_decoder.", NULL)) {
        av_strlcpy(str, "OMX.broadcom.video_decode", str_size);
        return 0;
    }
    if (av_strstart(role, "video_encoder.", NULL)) {
        av_strlcpy(str, "OMX.broadcom.video_encode", str_size);
        return 0;
    }
#endif
    omx_context->ptr_GetComponentsOfRole((OMX_STRING) role, &num, NULL);
    if (!num) {
        av_log(logctx, AV_LOG_WARNING, "No component for role %s found\n", role);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    components = av_mallocz_array(num, sizeof(*components));
    if (!components)
        return AVERROR(ENOMEM);
    for (i = 0; i < num; i++) {
        components[i] = av_mallocz(OMX_MAX_STRINGNAME_SIZE);
        if (!components[i]) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }
    omx_context->ptr_GetComponentsOfRole((OMX_STRING) role, &num, (OMX_U8**) components);
    av_strlcpy(str, components[0], str_size);
end:
    for (i = 0; i < num; i++)
        av_free(components[i]);
    av_free(components);
    return ret;
}

static av_cold int wait_for_state(OMXCodecContext *s, OMX_STATETYPE state)
{
    int ret = 0;
    pthread_mutex_lock(&s->state_mutex);
    while (s->state != state && s->error == OMX_ErrorNone)
        pthread_cond_wait(&s->state_cond, &s->state_mutex);
    if (s->error != OMX_ErrorNone)
        ret = AVERROR_ENCODER_NOT_FOUND;
    pthread_mutex_unlock(&s->state_mutex);
    return ret;
}

static av_cold int wait_for_port_event(OMXCodecContext *s, int enabled)
{
    int ret = 0;
    pthread_mutex_lock(&s->state_mutex);
    while (((enabled && !s->enabled) || (!enabled && !s->disabled)) && s->error == OMX_ErrorNone)
        pthread_cond_wait(&s->state_cond, &s->state_mutex);
    if (s->error != OMX_ErrorNone)
        ret = AVERROR_INVALIDDATA;
    if (enabled)
        s->enabled = 0;
    else
        s->disabled = 0;
    pthread_mutex_unlock(&s->state_mutex);
    return ret;
}

static av_cold int omx_component_init(AVCodecContext *avctx, const char *role, int encode)
{
    OMXCodecContext *s = avctx->priv_data;
    OMX_PARAM_COMPONENTROLETYPE role_params = { 0 };
    OMX_PORT_PARAM_TYPE video_port_params = { 0 };
    OMX_PARAM_PORTDEFINITIONTYPE in_port_params = { 0 }, out_port_params = { 0 };
    OMX_VIDEO_PARAM_PORTFORMATTYPE video_port_format = { 0 };
    OMX_VIDEO_PARAM_BITRATETYPE vid_param_bitrate = { 0 };
    OMX_ERRORTYPE err;
    int i;

    s->version.s.nVersionMajor = 1;
    s->version.s.nVersionMinor = 1;
    s->version.s.nRevision     = 2;

    err = omx_context->ptr_GetHandle(&s->handle, s->component_name, s, (OMX_CALLBACKTYPE*) &callbacks);
    if (err != OMX_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "OMX_GetHandle(%s) failed: %x\n", s->component_name, err);
        return AVERROR_UNKNOWN;
    }

    // This one crashes the mediaserver on qcom, if used over IOMX
    INIT_STRUCT(role_params);
    av_strlcpy(role_params.cRole, role, sizeof(role_params.cRole));
    // Intentionally ignore errors on this one
    OMX_SetParameter(s->handle, OMX_IndexParamStandardComponentRole, &role_params);

    INIT_STRUCT(video_port_params);
    err = OMX_GetParameter(s->handle, OMX_IndexParamVideoInit, &video_port_params);
    CHECK(err);

    s->in_port = s->out_port = -1;
    for (i = 0; i < video_port_params.nPorts; i++) {
        int port = video_port_params.nStartPortNumber + i;
        OMX_PARAM_PORTDEFINITIONTYPE port_params = { 0 };
        INIT_STRUCT(port_params);
        port_params.nPortIndex = port;
        err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &port_params);
        if (err != OMX_ErrorNone) {
            av_log(avctx, AV_LOG_WARNING, "port %d error %x\n", port, err);
            break;
        }
        if (port_params.eDir == OMX_DirInput && s->in_port < 0) {
            in_port_params = port_params;
            s->in_port = port;
        } else if (port_params.eDir == OMX_DirOutput && s->out_port < 0) {
            out_port_params = port_params;
            s->out_port = port;
        }
    }
    if (s->in_port < 0 || s->out_port < 0) {
        av_log(avctx, AV_LOG_ERROR, "No in or out port found (in %d out %d)\n", s->in_port, s->out_port);
        return AVERROR_UNKNOWN;
    }

    if (encode) {
        s->color_format = 0;
        for (i = 0; ; i++) {
            INIT_STRUCT(video_port_format);
            video_port_format.nIndex = i;
            video_port_format.nPortIndex = s->in_port;
            if (OMX_GetParameter(s->handle, OMX_IndexParamVideoPortFormat, &video_port_format) != OMX_ErrorNone)
                break;
            if (video_port_format.eColorFormat == OMX_COLOR_FormatYUV420Planar ||
                video_port_format.eColorFormat == OMX_COLOR_FormatYUV420PackedPlanar) {
                s->color_format = video_port_format.eColorFormat;
                break;
            }
        }
        if (s->color_format == 0) {
            av_log(avctx, AV_LOG_ERROR, "No supported pixel formats (%d formats available)\n", i);
            return AVERROR_UNKNOWN;
        }
    }

    in_port_params.bEnabled   = OMX_TRUE;
    in_port_params.bPopulated = OMX_FALSE;
    in_port_params.eDomain    = OMX_PortDomainVideo;

    in_port_params.format.video.pNativeRender         = NULL;
    in_port_params.format.video.bFlagErrorConcealment = OMX_FALSE;
    if (encode) {
        in_port_params.format.video.eColorFormat = s->color_format;
        s->stride     = avctx->width;
        s->plane_size = avctx->height;
        // If specific codecs need to manually override the stride/plane_size,
        // that can be done here.
        in_port_params.format.video.nStride      = s->stride;
        in_port_params.format.video.nSliceHeight = s->plane_size;
        if (avctx->framerate.den > 0 && avctx->framerate.num > 0)
            in_port_params.format.video.xFramerate = (1 << 16) * avctx->framerate.num / avctx->framerate.den;
        else
            in_port_params.format.video.xFramerate = (1 << 16) * avctx->time_base.den / avctx->time_base.num;
    } else {
        if (avctx->codec->id == AV_CODEC_ID_MPEG4)
            in_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        else if (avctx->codec->id == AV_CODEC_ID_H264)
            in_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
        in_port_params.format.video.nStride = -1;
        in_port_params.format.video.nSliceHeight = -1;
        in_port_params.format.video.xFramerate = 30 << 16;
    }
    in_port_params.format.video.nFrameWidth  = avctx->width;
    in_port_params.format.video.nFrameHeight = avctx->height;

    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);
    if (encode) {
        s->stride     = in_port_params.format.video.nStride;
        s->plane_size = in_port_params.format.video.nSliceHeight;
    }
    s->num_in_buffers = in_port_params.nBufferCountActual;

    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    out_port_params.bEnabled   = OMX_TRUE;
    out_port_params.bPopulated = OMX_FALSE;
    out_port_params.eDomain    = OMX_PortDomainVideo;
    out_port_params.format.video.pNativeRender = NULL;
    out_port_params.format.video.nFrameWidth  = avctx->width;
    out_port_params.format.video.nFrameHeight = avctx->height;
    if (encode) {
        out_port_params.format.video.nStride      = 0;
        out_port_params.format.video.nSliceHeight = 0;
        out_port_params.format.video.nBitrate     = avctx->bit_rate;
        out_port_params.format.video.xFramerate   = in_port_params.format.video.xFramerate;
    }
    out_port_params.format.video.bFlagErrorConcealment  = OMX_FALSE;
    if (encode) {
        if (avctx->codec->id == AV_CODEC_ID_MPEG4)
            out_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        else if (avctx->codec->id == AV_CODEC_ID_H264)
            out_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
    }

    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);
    s->num_out_buffers = out_port_params.nBufferCountActual;

    if (encode) {
        INIT_STRUCT(vid_param_bitrate);
        vid_param_bitrate.nPortIndex     = s->out_port;
        vid_param_bitrate.eControlRate   = OMX_Video_ControlRateVariable;
        vid_param_bitrate.nTargetBitrate = avctx->bit_rate;
        err = OMX_SetParameter(s->handle, OMX_IndexParamVideoBitrate, &vid_param_bitrate);
        if (err != OMX_ErrorNone)
            av_log(avctx, AV_LOG_WARNING, "Unable to set video bitrate parameter\n");

        if (avctx->codec->id == AV_CODEC_ID_H264) {
            OMX_VIDEO_PARAM_AVCTYPE avc = { 0 };
            INIT_STRUCT(avc);
            avc.nPortIndex = s->out_port;
            err = OMX_GetParameter(s->handle, OMX_IndexParamVideoAvc, &avc);
            CHECK(err);
            avc.nBFrames = 0;
            avc.nPFrames = avctx->gop_size - 1;
            err = OMX_SetParameter(s->handle, OMX_IndexParamVideoAvc, &avc);
            CHECK(err);
        }
    }

    err = OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
    CHECK(err);

    s->in_buffer_headers  = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_in_buffers);
    s->free_in_buffers    = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_in_buffers);
    s->out_buffer_headers = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_out_buffers);
    s->done_out_buffers   = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_out_buffers);
    if (!s->in_buffer_headers || !s->free_in_buffers || !s->out_buffer_headers || !s->done_out_buffers)
        return AVERROR(ENOMEM);
    for (i = 0; i < s->num_in_buffers && err == OMX_ErrorNone; i++) {
        if (s->input_zerocopy)
            err = OMX_UseBuffer(s->handle, &s->in_buffer_headers[i], s->in_port, s, in_port_params.nBufferSize, NULL);
        else
            err = OMX_AllocateBuffer(s->handle, &s->in_buffer_headers[i],  s->in_port,  s, in_port_params.nBufferSize);
        if (err == OMX_ErrorNone)
            s->in_buffer_headers[i]->pAppPrivate = s->in_buffer_headers[i]->pOutputPortPrivate = NULL;
    }
    CHECK(err);
    s->num_in_buffers = i;
    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++)
        err = OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i], s->out_port, s, out_port_params.nBufferSize);
    CHECK(err);
    s->num_out_buffers = i;

    if (wait_for_state(s, OMX_StateIdle) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Didn't get OMX_StateIdle\n");
        return AVERROR_UNKNOWN;
    }
    err = OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    CHECK(err);
    if (wait_for_state(s, OMX_StateExecuting) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Didn't get OMX_StateExecuting\n");
        return AVERROR_UNKNOWN;
    }

    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++)
        err = OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);
    if (err != OMX_ErrorNone) {
        for (; i < s->num_out_buffers; i++)
            s->done_out_buffers[s->num_done_out_buffers++] = s->out_buffer_headers[i];
    }
    for (i = 0; i < s->num_in_buffers; i++)
        s->free_in_buffers[s->num_free_in_buffers++] = s->in_buffer_headers[i];
    return err != OMX_ErrorNone ? AVERROR_UNKNOWN : 0;
}

static av_cold void cleanup(OMXCodecContext *s)
{
    int i, executing;

    pthread_mutex_lock(&s->state_mutex);
    executing = s->state == OMX_StateExecuting;
    pthread_mutex_unlock(&s->state_mutex);

    if (executing) {
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
        wait_for_state(s, OMX_StateIdle);
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
        for (i = 0; i < s->num_in_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->input_mutex, &s->input_cond,
                                                      &s->num_free_in_buffers, s->free_in_buffers, 1);
            if (s->input_zerocopy)
                buffer->pBuffer = NULL;
            OMX_FreeBuffer(s->handle, s->in_port, buffer);
        }
        for (i = 0; i < s->num_out_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                                      &s->num_done_out_buffers, s->done_out_buffers, 1);
            OMX_FreeBuffer(s->handle, s->out_port, buffer);
        }
        wait_for_state(s, OMX_StateLoaded);
    }
    if (s->handle) {
        omx_context->ptr_FreeHandle(s->handle);
        s->handle = NULL;
    }

    if (s->omx_inited)
        omx_deinit();
    s->omx_inited = 0;
    if (s->mutex_cond_inited) {
        pthread_cond_destroy(&s->state_cond);
        pthread_mutex_destroy(&s->state_mutex);
        pthread_cond_destroy(&s->input_cond);
        pthread_mutex_destroy(&s->input_mutex);
        pthread_cond_destroy(&s->output_cond);
        pthread_mutex_destroy(&s->output_mutex);
        s->mutex_cond_inited = 0;
    }
    av_freep(&s->in_buffer_headers);
    av_freep(&s->out_buffer_headers);
    av_freep(&s->free_in_buffers);
    av_freep(&s->done_out_buffers);
    av_freep(&s->output_buf);
}

static av_cold int omx_encode_init(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    int ret = AVERROR_ENCODER_NOT_FOUND;
    const char *role;
    OMX_BUFFERHEADERTYPE *buffer;
    OMX_ERRORTYPE err;

#if CONFIG_OMX_RPI
    s->input_zerocopy = 1;
#endif

    if ((ret = omx_init(avctx, s->libname, s->libprefix)) < 0)
        return ret;
    s->omx_inited = 1;

    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->state_cond, NULL);
    pthread_mutex_init(&s->input_mutex, NULL);
    pthread_cond_init(&s->input_cond, NULL);
    pthread_mutex_init(&s->output_mutex, NULL);
    pthread_cond_init(&s->output_cond, NULL);
    s->mutex_cond_inited = 1;
    s->avctx = avctx;
    s->state = OMX_StateLoaded;
    s->error = OMX_ErrorNone;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_MPEG4:
        role = "video_encoder.mpeg4";
        break;
    case AV_CODEC_ID_H264:
        role = "video_encoder.avc";
        break;
    default:
        return AVERROR(ENOSYS);
    }

    if ((ret = find_component(avctx, role, s->component_name, sizeof(s->component_name))) < 0)
        goto fail;

    av_log(avctx, AV_LOG_INFO, "Using %s\n", s->component_name);

    if ((ret = omx_component_init(avctx, role, 1)) < 0)
        goto fail;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        while (1) {
            buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                &s->num_done_out_buffers, s->done_out_buffers, 1);
            if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
                if ((ret = av_reallocp(&avctx->extradata, avctx->extradata_size + buffer->nFilledLen + AV_INPUT_BUFFER_PADDING_SIZE)) < 0) {
                    avctx->extradata_size = 0;
                    goto fail;
                }
                memcpy(avctx->extradata + avctx->extradata_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
                avctx->extradata_size += buffer->nFilledLen;
                memset(avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            }
            err = OMX_FillThisBuffer(s->handle, buffer);
            if (err != OMX_ErrorNone) {
                append_buffer(&s->output_mutex, &s->output_cond,
                              &s->num_done_out_buffers, s->done_out_buffers, buffer);
                av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
                ret = AVERROR_UNKNOWN;
                goto fail;
            }
            if (avctx->codec->id == AV_CODEC_ID_H264) {
                // For H.264, the extradata can be returned in two separate buffers
                // (the videocore encoder on raspberry pi does this);
                // therefore check that we have got both SPS and PPS before continuing.
                int nals[32] = { 0 };
                int i;
                for (i = 0; i + 4 < avctx->extradata_size; i++) {
                     if (!avctx->extradata[i + 0] &&
                         !avctx->extradata[i + 1] &&
                         !avctx->extradata[i + 2] &&
                         avctx->extradata[i + 3] == 1) {
                         nals[avctx->extradata[i + 4] & 0x1f]++;
                     }
                }
                if (nals[H264_NAL_SPS] && nals[H264_NAL_PPS])
                    break;
            } else {
                if (avctx->extradata_size > 0)
                    break;
            }
        }
    }

    return 0;
fail:
    return ret;
}


static int omx_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    OMXCodecContext *s = avctx->priv_data;
    int ret = 0;
    OMX_BUFFERHEADERTYPE* buffer;
    OMX_ERRORTYPE err;

    if (frame) {
        uint8_t *dst[4];
        int linesize[4];
        int need_copy;
        buffer = get_buffer(&s->input_mutex, &s->input_cond,
                            &s->num_free_in_buffers, s->free_in_buffers, 1);

        buffer->nFilledLen = av_image_fill_arrays(dst, linesize, buffer->pBuffer, avctx->pix_fmt, s->stride, s->plane_size, 1);

        if (s->input_zerocopy) {
            uint8_t *src[4] = { NULL };
            int src_linesize[4];
            av_image_fill_arrays(src, src_linesize, frame->data[0], avctx->pix_fmt, s->stride, s->plane_size, 1);
            if (frame->linesize[0] == src_linesize[0] &&
                frame->linesize[1] == src_linesize[1] &&
                frame->linesize[2] == src_linesize[2] &&
                frame->data[1] == src[1] &&
                frame->data[2] == src[2]) {
                // If the input frame happens to have all planes stored contiguously,
                // with the right strides, just clone the frame and set the OMX
                // buffer header to point to it
                AVFrame *local = av_frame_clone(frame);
                if (!local) {
                    // Return the buffer to the queue so it's not lost
                    append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
                    return AVERROR(ENOMEM);
                } else {
                    buffer->pAppPrivate = local;
                    buffer->pOutputPortPrivate = NULL;
                    buffer->pBuffer = local->data[0];
                    need_copy = 0;
                }
            } else {
                // If not, we need to allocate a new buffer with the right
                // size and copy the input frame into it.
                uint8_t *buf = av_malloc(av_image_get_buffer_size(avctx->pix_fmt, s->stride, s->plane_size, 1));
                if (!buf) {
                    // Return the buffer to the queue so it's not lost
                    append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
                    return AVERROR(ENOMEM);
                } else {
                    buffer->pAppPrivate = buf;
                    // Mark that pAppPrivate is an av_malloc'ed buffer, not an AVFrame
                    buffer->pOutputPortPrivate = (void*) 1;
                    buffer->pBuffer = buf;
                    need_copy = 1;
                    buffer->nFilledLen = av_image_fill_arrays(dst, linesize, buffer->pBuffer, avctx->pix_fmt, s->stride, s->plane_size, 1);
                }
            }
        } else {
            need_copy = 1;
        }
        if (need_copy)
            av_image_copy(dst, linesize, (const uint8_t**) frame->data, frame->linesize, avctx->pix_fmt, avctx->width, avctx->height);
        buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
        buffer->nOffset = 0;
        // Convert the timestamps to microseconds; some encoders can ignore
        // the framerate and do VFR bit allocation based on timestamps.
        buffer->nTimeStamp = to_omx_ticks(av_rescale_q(frame->pts, avctx->time_base, AV_TIME_BASE_Q));
        err = OMX_EmptyThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_EmptyThisBuffer failed: %x\n", err);
            return AVERROR_UNKNOWN;
        }
    } else if (!s->eos_sent) {
        buffer = get_buffer(&s->input_mutex, &s->input_cond,
                            &s->num_free_in_buffers, s->free_in_buffers, 1);

        buffer->nFilledLen = 0;
        buffer->nFlags = OMX_BUFFERFLAG_EOS;
        buffer->pAppPrivate = buffer->pOutputPortPrivate = NULL;
        err = OMX_EmptyThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->input_mutex, &s->input_cond, &s->num_free_in_buffers, s->free_in_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_EmptyThisBuffer failed: %x\n", err);
            return AVERROR_UNKNOWN;
        }
        s->eos_sent = 1;
    }

    while (!*got_packet && ret == 0 && !s->got_eos) {
        // If not flushing, just poll the queue if there's finished packets.
        // If flushing, do a blocking wait until we either get a completed
        // packet, or get EOS.
        buffer = get_buffer(&s->output_mutex, &s->output_cond,
                            &s->num_done_out_buffers, s->done_out_buffers,
                            !frame);
        if (!buffer)
            break;

        if (buffer->nFlags & OMX_BUFFERFLAG_EOS)
            s->got_eos = 1;

        if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG && avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
            if ((ret = av_reallocp(&avctx->extradata, avctx->extradata_size + buffer->nFilledLen + AV_INPUT_BUFFER_PADDING_SIZE)) < 0) {
                avctx->extradata_size = 0;
                goto end;
            }
            memcpy(avctx->extradata + avctx->extradata_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
            avctx->extradata_size += buffer->nFilledLen;
            memset(avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        } else {
            if (!(buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) || !pkt->data) {
                // If the output packet isn't preallocated, just concatenate everything in our
                // own buffer
                int newsize = s->output_buf_size + buffer->nFilledLen + AV_INPUT_BUFFER_PADDING_SIZE;
                if ((ret = av_reallocp(&s->output_buf, newsize)) < 0) {
                    s->output_buf_size = 0;
                    goto end;
                }
                memcpy(s->output_buf + s->output_buf_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
                s->output_buf_size += buffer->nFilledLen;
                if (buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
                    if ((ret = av_packet_from_data(pkt, s->output_buf, s->output_buf_size)) < 0) {
                        av_freep(&s->output_buf);
                        s->output_buf_size = 0;
                        goto end;
                    }
                    s->output_buf = NULL;
                    s->output_buf_size = 0;
                }
            } else {
                // End of frame, and the caller provided a preallocated frame
                if ((ret = ff_alloc_packet(pkt, s->output_buf_size + buffer->nFilledLen)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Error getting output packet of size %d.\n",
                           (int)(s->output_buf_size + buffer->nFilledLen));
                    goto end;
                }
                memcpy(pkt->data, s->output_buf, s->output_buf_size);
                memcpy(pkt->data + s->output_buf_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
                av_freep(&s->output_buf);
                s->output_buf_size = 0;
            }
            if (buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
                pkt->pts = av_rescale_q(from_omx_ticks(buffer->nTimeStamp), AV_TIME_BASE_Q, avctx->time_base);
                // We don't currently enable B-frames for the encoders, so set
                // pkt->dts = pkt->pts. (The calling code behaves worse if the encoder
                // doesn't set the dts).
                pkt->dts = pkt->pts;
                if (buffer->nFlags & OMX_BUFFERFLAG_SYNCFRAME)
                    pkt->flags |= AV_PKT_FLAG_KEY;
                *got_packet = 1;
            }
        }
end:
        err = OMX_FillThisBuffer(s->handle, buffer);
        if (err != OMX_ErrorNone) {
            append_buffer(&s->output_mutex, &s->output_cond, &s->num_done_out_buffers, s->done_out_buffers, buffer);
            av_log(avctx, AV_LOG_ERROR, "OMX_FillThisBuffer failed: %x\n", err);
            ret = AVERROR_UNKNOWN;
        }
    }
    return ret;
}

static av_cold int omx_encode_end(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;

    cleanup(s);
    return 0;
}

static int omx_update_out_def(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params = { 0 };
    OMX_ERRORTYPE err;
    OMX_CONFIG_RECTTYPE crop_rect;

    INIT_STRUCT(out_port_params);
    out_port_params.nPortIndex = s->out_port;
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    avctx->width = out_port_params.format.video.nFrameWidth;
    avctx->height = out_port_params.format.video.nFrameHeight;
    s->stride = out_port_params.format.video.nStride;
    s->plane_size = out_port_params.format.video.nSliceHeight;
    s->color_format = out_port_params.format.video.eColorFormat;

    INIT_STRUCT(crop_rect);
    crop_rect.nPortIndex = s->out_port;
    err = OMX_GetConfig(s->handle, OMX_IndexConfigCommonOutputCrop, &crop_rect);
    if (err == OMX_ErrorNone) {
        avctx->width = crop_rect.nWidth;
        avctx->height = crop_rect.nHeight;
    }

    if (s->plane_size < avctx->height)
        s->plane_size = avctx->height;
    if (s->stride < avctx->width)
        s->stride = avctx->width;

    switch (s->color_format) {
    case OMX_COLOR_FormatYUV420Planar:
    case OMX_COLOR_FormatYUV420PackedPlanar:
    default:
        avctx->pix_fmt = AV_PIX_FMT_YUV420P;
        break;
    case OMX_COLOR_FormatYUV420SemiPlanar:
    case OMX_COLOR_FormatYUV420PackedSemiPlanar:
        avctx->pix_fmt = AV_PIX_FMT_NV12;
        break;
    }
    return 0;
}

static av_cold int omx_decode_init(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    int ret = AVERROR_ENCODER_NOT_FOUND;
    const char *role;

    if ((ret = omx_init(avctx, s->libname, s->libprefix)) < 0)
        return ret;

    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->state_cond, NULL);
    pthread_mutex_init(&s->input_mutex, NULL);
    pthread_cond_init(&s->input_cond, NULL);
    pthread_mutex_init(&s->output_mutex, NULL);
    pthread_cond_init(&s->output_cond, NULL);
    s->avctx = avctx;
    s->state = OMX_StateLoaded;
    s->error = OMX_ErrorNone;

    switch (avctx->codec->id) {
    case AV_CODEC_ID_MPEG4:
        role = "video_decoder.mpeg4";
        break;
    case AV_CODEC_ID_H264:
        role = "video_decoder.avc";
        break;
    default:
        return AVERROR(ENOSYS);
    }

    if ((ret = find_component(avctx, role, s->component_name, sizeof(s->component_name))) < 0)
        return ret;

    av_log(avctx, AV_LOG_INFO, "Using %s\n", s->component_name);

    if ((ret = omx_component_init(avctx, role, 0)) < 0)
        return ret;

    // If we have MP4 style H264, it is filtered and the extradata is prepended to packets;
    // don't feed the MP4 style extradata to the decoder.
    if (avctx->extradata_size && (avctx->codec->id != AV_CODEC_ID_H264 || avctx->extradata[0] != 1)) {
        OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->input_mutex, &s->input_cond,
                                                  &s->num_free_in_buffers, s->free_in_buffers, 1);

        memcpy(buffer->pBuffer, avctx->extradata, avctx->extradata_size);
        buffer->nFilledLen = avctx->extradata_size;
        buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;
        buffer->nOffset = 0;
        buffer->nTimeStamp = to_omx_ticks(0);
        OMX_EmptyThisBuffer(s->handle, buffer);
    }

    if (omx_update_out_def(avctx))
        return AVERROR_INVALIDDATA;

    return 0;
}

static int omx_reconfigure_out(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params = { 0 };
    OMX_ERRORTYPE err;
    int i;

    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->out_port, NULL);
    CHECK(err);

    for (i = 0; i < s->num_out_buffers; i++) {
        OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                                  &s->num_done_out_buffers, s->done_out_buffers, 1);
        OMX_FreeBuffer(s->handle, s->out_port, buffer);
    }

    av_freep(&s->out_buffer_headers);
    av_freep(&s->done_out_buffers);

    if (wait_for_port_event(s, 0))
        return AVERROR_INVALIDDATA;

    INIT_STRUCT(out_port_params);
    out_port_params.nPortIndex = s->out_port;
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);
    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    s->num_done_out_buffers = 0;
    s->num_out_buffers = out_port_params.nBufferCountActual;

    err = OMX_SendCommand(s->handle, OMX_CommandPortEnable, s->out_port, NULL);
    CHECK(err);

    s->out_buffer_headers = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_out_buffers);
    s->done_out_buffers   = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_out_buffers);

    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++)
        err = OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i], s->out_port, s, out_port_params.nBufferSize);
    CHECK(err);
    s->num_out_buffers = i;

    if (wait_for_port_event(s, 1))
        return AVERROR_INVALIDDATA;

    for (i = 0; i < s->num_out_buffers; i++)
        OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);

    omx_update_out_def(avctx);
    return 0;
}

static int omx_decode_frame(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    OMXCodecContext *s = avctx->priv_data;
    OMX_BUFFERHEADERTYPE *buffer;
    int ret;

    while (!*got_frame) {
        if (avpkt && avpkt->size) {
            // TODO: Check num_done_out_buffers too, unless we've returned data already
            uint8_t *ptr = avpkt->data;
            int size = avpkt->size, n;
            while (size > 0) {
                pthread_mutex_lock(&s->input_mutex);
                while (!s->num_free_in_buffers && !s->reconfigure_out && !s->update_out_def)
                    pthread_cond_wait(&s->input_cond, &s->input_mutex);
                if (s->reconfigure_out) {
                    s->reconfigure_out = 0;
                    pthread_mutex_unlock(&s->input_mutex);
                    if (omx_reconfigure_out(avctx))
                        return AVERROR_INVALIDDATA;
                    continue;
                }
                if (s->update_out_def) {
                    s->update_out_def = 0;
                    pthread_mutex_unlock(&s->input_mutex);
                    if (omx_update_out_def(avctx))
                        return AVERROR_INVALIDDATA;
                    continue;
                }
                buffer = s->free_in_buffers[--s->num_free_in_buffers];
                pthread_mutex_unlock(&s->input_mutex);

                n = FFMIN(size, buffer->nAllocLen);
                memcpy(buffer->pBuffer, ptr, n);
                ptr += n;
                size -= n;
                buffer->nFilledLen = n;
                buffer->nFlags = size == 0 ? OMX_BUFFERFLAG_ENDOFFRAME : 0;
                buffer->nOffset = 0;
                buffer->nTimeStamp = to_omx_ticks(avpkt->pts);
                OMX_EmptyThisBuffer(s->handle, buffer);
            }
        } else if (!s->eos_sent) {
            buffer = get_buffer(&s->input_mutex, &s->input_cond,
                                &s->num_free_in_buffers, s->free_in_buffers, 1);
            buffer->nFilledLen = 0;
            buffer->nFlags = OMX_BUFFERFLAG_EOS;
            OMX_EmptyThisBuffer(s->handle, buffer);
            s->eos_sent = 1;
        }

        pthread_mutex_lock(&s->output_mutex);
        if (!avpkt && !s->got_eos) {
            while (!s->num_done_out_buffers)
                pthread_cond_wait(&s->output_cond, &s->output_mutex);
        }
        if (s->reconfigure_out) {
            s->reconfigure_out = 0;
            pthread_mutex_unlock(&s->output_mutex);
            if (omx_reconfigure_out(avctx))
                return AVERROR_INVALIDDATA;
            pthread_mutex_lock(&s->output_mutex);
        }
        if (s->update_out_def) {
            s->update_out_def = 0;
            pthread_mutex_unlock(&s->output_mutex);
            if (omx_update_out_def(avctx))
                return AVERROR_INVALIDDATA;
            pthread_mutex_lock(&s->output_mutex);
        }
        if (s->num_done_out_buffers) {
            buffer = s->done_out_buffers[0];
            s->num_done_out_buffers--;
            memmove(&s->done_out_buffers[0], &s->done_out_buffers[1], s->num_done_out_buffers * sizeof(OMX_BUFFERHEADERTYPE*));
        } else {
            buffer = NULL;
        }
        pthread_mutex_unlock(&s->output_mutex);

        if (buffer && buffer->nFlags & OMX_BUFFERFLAG_EOS)
            s->got_eos = 1;

        if (buffer && !buffer->nFilledLen) {
            OMX_FillThisBuffer(s->handle, buffer);
            buffer = NULL;
        }

        if (buffer) {
            const uint8_t *src[4];
            int linesize[4];
            AVFrame *frame = data;
            if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
                return ret;
            av_image_fill_arrays((uint8_t**) src, linesize, buffer->pBuffer, avctx->pix_fmt, s->stride, s->plane_size, 1);
            av_image_copy(frame->data, frame->linesize, src, linesize, avctx->pix_fmt, avctx->width, avctx->height);

            frame->pts = from_omx_ticks(buffer->nTimeStamp);
            frame->pkt_dts = AV_NOPTS_VALUE;
#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
            frame->pkt_pts = frame->pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
            *got_frame = 1;
            OMX_FillThisBuffer(s->handle, buffer);
        }
        break;
    }
    return avpkt->size;
}

static av_cold int omx_decode_end(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;

    cleanup(s);
    return 0;
}

#define OFFSET(x) offsetof(OMXCodecContext, x)
#define VDE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM
#define VE  AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "omx_libname", "OpenMAX library name", OFFSET(libname), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VDE },
    { "omx_libprefix", "OpenMAX library prefix", OFFSET(libprefix), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VDE },
    { "zerocopy", "Try to avoid copying input frames if possible", OFFSET(input_zerocopy), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    { NULL }
};

static const enum AVPixelFormat omx_encoder_pix_fmts[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE
};

static const AVClass omx_mpeg4enc_class = {
    .class_name = "mpeg4_omx",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_mpeg4_omx_encoder = {
    .name             = "mpeg4_omx",
    .long_name        = NULL_IF_CONFIG_SMALL("OpenMAX IL MPEG-4 video encoder"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_MPEG4,
    .priv_data_size   = sizeof(OMXCodecContext),
    .init             = omx_encode_init,
    .encode2          = omx_encode_frame,
    .close            = omx_encode_end,
    .pix_fmts         = omx_encoder_pix_fmts,
    .capabilities     = AV_CODEC_CAP_DELAY,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class       = &omx_mpeg4enc_class,
};

static const AVClass omx_h264enc_class = {
    .class_name = "h264_omx",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_h264_omx_encoder = {
    .name             = "h264_omx",
    .long_name        = NULL_IF_CONFIG_SMALL("OpenMAX IL H.264 video encoder"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_H264,
    .priv_data_size   = sizeof(OMXCodecContext),
    .init             = omx_encode_init,
    .encode2          = omx_encode_frame,
    .close            = omx_encode_end,
    .pix_fmts         = omx_encoder_pix_fmts,
    .capabilities     = AV_CODEC_CAP_DELAY,
    .caps_internal    = FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class       = &omx_h264enc_class,
};

static const AVClass omx_h264dec_class = {
    .class_name = "h264_omx_dec",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_h264_omx_decoder = {
    .name             = "h264_omx",
    .long_name        = NULL_IF_CONFIG_SMALL("OpenMAX IL H264 video decoder"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_H264,
    .priv_data_size   = sizeof(OMXCodecContext),
    .init             = omx_decode_init,
    .decode           = omx_decode_frame,
    .close            = omx_decode_end,
    .capabilities     = AV_CODEC_CAP_DELAY,
    .caps_internal    = FF_CODEC_CAP_SETS_PKT_DTS | FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class       = &omx_h264dec_class,
    .bsfs             = "h264_mp4toannexb",
};

static const AVClass omx_mpeg4dec_class = {
    .class_name = "mpeg4_omx_dec",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_mpeg4_omx_decoder = {
    .name             = "mpeg4_omx",
    .long_name        = NULL_IF_CONFIG_SMALL("OpenMAX IL MPEG4 video decoder"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_MPEG4,
    .priv_data_size   = sizeof(OMXCodecContext),
    .init             = omx_decode_init,
    .decode           = omx_decode_frame,
    .close            = omx_decode_end,
    .capabilities     = AV_CODEC_CAP_DELAY,
    .caps_internal    = FF_CODEC_CAP_SETS_PKT_DTS | FF_CODEC_CAP_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
    .priv_class       = &omx_mpeg4dec_class,
};
