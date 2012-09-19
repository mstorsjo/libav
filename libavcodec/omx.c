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

#include <stdio.h>
#include <stdlib.h>

#include "libavutil/avutil.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"
#include <OMX_Core.h>
#include <OMX_Component.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/time.h>
#include "h264.h"

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
    void (*host_deinit)(void);
} OMXContext;

static OMXContext *omx_context;

static av_cold void *dlsym2(void *handle, const char *symbol, const char *prefix)
{
    char buf[50];
    snprintf(buf, sizeof(buf), "%s%s", prefix ? prefix : "", symbol);
    return dlsym(handle, buf);
}

static av_cold int omx_try_load(void *logctx, const char *libname, const char *prefix, const char *libname2)
{
    OMXContext *s = omx_context;
    if (libname2) {
        s->lib2 = dlopen(libname2, RTLD_NOW | RTLD_GLOBAL);
        if (!s->lib2) {
            av_log(logctx, AV_LOG_WARNING, "%s not found\n", libname);
            return AVERROR_ENCODER_NOT_FOUND;
        }
        s->host_init = dlsym(s->lib2, "bcm_host_init");
        s->host_deinit = dlsym(s->lib2, "bcm_host_deinit");
        if (!s->host_init || !s->host_deinit) {
            av_log(logctx, AV_LOG_WARNING, "bcm_host_(de)init not found\n");
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
    s->ptr_Init                = dlsym2(s->lib, "OMX_Init", prefix);
    s->ptr_Deinit              = dlsym2(s->lib, "OMX_Deinit", prefix);
    s->ptr_ComponentNameEnum   = dlsym2(s->lib, "OMX_ComponentNameEnum", prefix);
    s->ptr_GetHandle           = dlsym2(s->lib, "OMX_GetHandle", prefix);
    s->ptr_FreeHandle          = dlsym2(s->lib, "OMX_FreeHandle", prefix);
    s->ptr_GetComponentsOfRole = dlsym2(s->lib, "OMX_GetComponentsOfRole", prefix);
    s->ptr_GetRolesOfComponent = dlsym2(s->lib, "OMX_GetRolesOfComponent", prefix);
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

static av_cold int omx_init(void *logctx, const char *libname, const char *prefix) {
    static const char * const libnames[] = {
        "/opt/vc/lib/libopenmaxil.so", "/opt/vc/lib/libbcm_host.so",
        "libOMX_Core.so", NULL,
        "libOmxCore.so", NULL,
        "libomxil-bellagio.so", NULL,
        NULL
    };
    const char* const* nameptr;
    int ret = AVERROR_ENCODER_NOT_FOUND;

    if (omx_context) {
        omx_context->users++;
        return 0;
    }

    omx_context = av_mallocz(sizeof(*omx_context));
    omx_context->users = 1;
    if (libname) {
        ret = omx_try_load(logctx, libname, prefix, NULL);
        if (ret < 0)
            return ret;
    } else {
        for (nameptr = libnames; *nameptr; nameptr += 2)
            if (!(ret = omx_try_load(logctx, nameptr[0], prefix, nameptr[1])))
                break;
        if (!*nameptr)
            return ret;
    }

    if (omx_context->host_init)
        omx_context->host_init();
    omx_context->ptr_Init();
    return 0;
}

static av_cold void omx_deinit(void) {
    if (!omx_context)
        return;
    omx_context->users--;
    if (!omx_context->users) {
        omx_context->ptr_Deinit();
        dlclose(omx_context->lib);
        if (omx_context->host_deinit)
            omx_context->host_deinit();
//        if (omx_context->lib2)
//            dlclose(omx_context->lib2);
        av_freep(&omx_context);
    }
}

typedef struct OMXCodecContext {
    const AVClass *class;
    char *libname;
    char *libprefix;

    AVCodecContext *avctx;
    AVFrame frame;

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

    int num_in_frames, num_out_frames;

    AVBitStreamFilterContext *bsfc;
    uint8_t *extradata;
    int extradata_size;
    uint8_t *extradata_free;

    uint8_t *output_buf;
    int output_buf_size;
} OMXCodecContext;

static OMX_ERRORTYPE eventHandler(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2, OMX_PTR event_data)
{
    OMXCodecContext *s = app_data;
    switch (event) {
    case OMX_EventError:
        pthread_mutex_lock(&s->state_mutex);
        av_log(s->avctx, AV_LOG_ERROR, "OMX error %x\n", (uint32_t) data1);
        s->error = data1;
        pthread_cond_broadcast(&s->state_cond);
        pthread_mutex_unlock(&s->state_mutex);
        break;
    case OMX_EventCmdComplete:
        if (data1 == OMX_CommandStateSet) {
            pthread_mutex_lock(&s->state_mutex);
            s->state = data2;
            av_log(s->avctx, AV_LOG_INFO, "OMX state changed to %d\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortDisable) {
            pthread_mutex_lock(&s->state_mutex);
            s->disabled = 1;
            av_log(s->avctx, AV_LOG_INFO, "OMX port %d disabled\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortEnable) {
            pthread_mutex_lock(&s->state_mutex);
            s->enabled = 1;
            av_log(s->avctx, AV_LOG_INFO, "OMX port %d enabled\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else {
            av_log(s->avctx, AV_LOG_INFO, "OMX command complete, command %d, value %d\n", (uint32_t) data1, (uint32_t) data2);
        }
        break;
    case OMX_EventPortSettingsChanged:
        av_log(s->avctx, AV_LOG_INFO, "OMX port %d settings changed\n", (uint32_t) data1);
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
        av_log(s->avctx, AV_LOG_INFO, "OMX event %d %x %x\n", event, (uint32_t) data1, (uint32_t) data2);
        break;
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE emptyBufferDone(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecContext *s = app_data;
    pthread_mutex_lock(&s->input_mutex);
    s->free_in_buffers[s->num_free_in_buffers++] = buffer;
    pthread_cond_broadcast(&s->input_cond);
    pthread_mutex_unlock(&s->input_mutex);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fillBufferDone(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCodecContext *s = app_data;
    pthread_mutex_lock(&s->output_mutex);
    s->done_out_buffers[s->num_done_out_buffers++] = buffer;
    pthread_cond_broadcast(&s->output_cond);
    pthread_mutex_unlock(&s->output_mutex);
    return OMX_ErrorNone;
}

static const OMX_CALLBACKTYPE callbacks = {
    eventHandler,
    emptyBufferDone,
    fillBufferDone
};

static av_cold int find_component(void *logctx, const char *role, char *str,
                                  int str_size)
{
    OMX_U32 i, num = 0;
    char **components;

    if (av_strstart(role, "video_decoder.", NULL)) {
        av_strlcpy(str, "OMX.broadcom.video_decode", str_size);
        return 0;
    }
    if (av_strstart(role, "video_encoder.", NULL)) {
        av_strlcpy(str, "OMX.broadcom.video_encode", str_size);
        return 0;
    }
    omx_context->ptr_GetComponentsOfRole(role, &num, NULL);
    if (!num) {
        av_log(logctx, AV_LOG_WARNING, "No component for role %s found\n", role);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    components = av_malloc(sizeof(char*) * num);
    for (i = 0; i < num; i++)
        components[i] = av_mallocz(OMX_MAX_STRINGNAME_SIZE);
    omx_context->ptr_GetComponentsOfRole(role, &num, (OMX_U8**) components);
    av_strlcpy(str, components[0], str_size);
/*
    // Leak the component strings - TI OMAP3 OMX replaces these pointers with
    // pointers to its internal strings, and freeing them causes a crash.
    for (i = 0; i < num; i++)
        av_free(components[i]);
*/
    av_free(components);
    return 0;
}

static av_cold int get_component(AVCodecContext *avctx, const char *role)
{
    OMXCodecContext *s = avctx->priv_data;
    return find_component(avctx, role, s->component_name, sizeof(s->component_name));
}

static av_cold void timed_wait(pthread_cond_t *cond, pthread_mutex_t *mutex, int ms)
{
    struct timeval tv;
    struct timespec ts;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000;
    ts.tv_nsec += ms * 1000000;
    ts.tv_sec += ts.tv_nsec / 1000000000;
    ts.tv_nsec = ts.tv_nsec % 1000000000;
    pthread_cond_timedwait(cond, mutex, &ts);
}

static av_cold int wait_for_state(OMXCodecContext *s, OMX_STATETYPE state)
{
    int ret = 0;
    pthread_mutex_lock(&s->state_mutex);
    while (s->state != state && s->error == OMX_ErrorNone)
        timed_wait(&s->state_cond, &s->state_mutex, 50);
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
        timed_wait(&s->state_cond, &s->state_mutex, 50);
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
    int i, default_size, decode = !encode;
    int ducati = !!strstr(s->component_name, "OMX.TI.DUCATI1.");

    s->version.s.nVersionMajor = 1;
    s->version.s.nVersionMinor = 1; // Required by Bellagio, set to 0 for other uses
    if (av_strstart(s->component_name, "OMX.broadcom.", NULL))
        s->version.s.nRevision = 2;
#define INIT_STRUCT(x) do { x.nSize = sizeof(x); x.nVersion = s->version; } while (0)

    err = omx_context->ptr_GetHandle(&s->handle, s->component_name, s, (OMX_CALLBACKTYPE*) &callbacks);
    if (err != OMX_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "OMX_GetHandle(%s) failed: %x\n", s->component_name, err);
        return AVERROR_ENCODER_NOT_FOUND;
    }

    // This one crashes the mediaserver on qcom, if used over IOMX
    INIT_STRUCT(role_params);
    av_strlcpy(role_params.cRole, role, sizeof(role_params.cRole));
    // Intentionally ignore errors on this one
    OMX_SetParameter(s->handle, OMX_IndexParamStandardComponentRole, &role_params);

    INIT_STRUCT(video_port_params);
#define CHECK(x) do { if (x != OMX_ErrorNone) { av_log(avctx, AV_LOG_ERROR, "err %x (%d) on line %d\n", x, x, __LINE__); return AVERROR_ENCODER_NOT_FOUND; } } while (0)
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
        return AVERROR_ENCODER_NOT_FOUND;
    }

    INIT_STRUCT(video_port_format);
    video_port_format.nIndex = 0;
    video_port_format.nPortIndex = encode ? s->in_port : s->out_port;
    OMX_GetParameter(s->handle, OMX_IndexParamVideoPortFormat, &video_port_format);
    s->color_format = video_port_format.eColorFormat;

    in_port_params.bEnabled = OMX_TRUE;
    in_port_params.bPopulated = OMX_FALSE;
    in_port_params.eDomain = OMX_PortDomainVideo;
    s->num_in_buffers = in_port_params.nBufferCountActual;

    in_port_params.format.video.pNativeRender = NULL;
    in_port_params.format.video.nStride = -1;
    in_port_params.format.video.nSliceHeight = -1;
    in_port_params.format.video.xFramerate = 30 << 16;
    if (!strcmp(s->component_name, "OMX.st.video_encoder"))
        in_port_params.format.video.xFramerate >>= 16;
    in_port_params.format.video.bFlagErrorConcealment = OMX_FALSE;
    if (encode) {
        in_port_params.format.video.eColorFormat = s->color_format;
        s->stride = avctx->width;
        s->plane_size = avctx->height;
        if (!strcmp(s->component_name, "OMX.broadcom.video_encode")) {
            s->stride = FFALIGN(s->stride, 16);
            s->plane_size = FFALIGN(s->plane_size, 16);
        }
        in_port_params.format.video.nStride = s->stride;
        in_port_params.format.video.nSliceHeight = s->plane_size;
    } else {
        if (avctx->codec->id == AV_CODEC_ID_MPEG4)
            in_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        else if (avctx->codec->id == AV_CODEC_ID_H264)
            in_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
        else if (avctx->codec->id == AV_CODEC_ID_VC1 || avctx->codec->id == AV_CODEC_ID_WMV3)
            in_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;
    }
    in_port_params.format.video.nFrameWidth = avctx->width;
    in_port_params.format.video.nFrameHeight = avctx->height;
    default_size = avctx->width * avctx->height + 2 * avctx->width/2 * avctx->height/2;
    if (encode && in_port_params.nBufferSize < default_size)
        in_port_params.nBufferSize = default_size;

    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);

    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    out_port_params.bEnabled = OMX_TRUE;
    out_port_params.bPopulated = OMX_FALSE;
    out_port_params.eDomain = OMX_PortDomainVideo;
    out_port_params.format.video.pNativeRender = NULL;
    out_port_params.format.video.nFrameWidth = avctx->width;
    out_port_params.format.video.nFrameHeight = avctx->height;
    if (encode) {
        out_port_params.format.video.nStride = 0;
        out_port_params.format.video.nSliceHeight = 0;
        // The qcom OMX component doesn't accept any framerate (25 isn't supported), only certain (at least 15 and 30),
        // so ask for 30 and adjust the bitrate accordingly
        out_port_params.format.video.nBitrate = avctx->bit_rate * 30 * avctx->time_base.num / avctx->time_base.den;
        if (!ducati)
            out_port_params.format.video.xFramerate = 30 << 16;
        if (!strcmp(s->component_name, "OMX.st.video_encoder"))
            out_port_params.format.video.xFramerate >>= 16;
    }
    out_port_params.format.video.bFlagErrorConcealment = OMX_FALSE;
    if (encode) {
        if (avctx->codec->id == AV_CODEC_ID_MPEG4)
            out_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        else if (avctx->codec->id == AV_CODEC_ID_H264)
            out_port_params.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
    }
    s->num_out_buffers = out_port_params.nBufferCountActual;
    if (decode && out_port_params.nBufferSize < default_size)
        out_port_params.nBufferSize = default_size;

    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    if (encode) {
        INIT_STRUCT(vid_param_bitrate);
        vid_param_bitrate.nPortIndex = s->out_port;
        vid_param_bitrate.eControlRate = OMX_Video_ControlRateVariable;
        vid_param_bitrate.nTargetBitrate = avctx->bit_rate * 30 / avctx->time_base.den;
        err = OMX_SetParameter(s->handle, OMX_IndexParamVideoBitrate, &vid_param_bitrate);
//        CHECK(err);

        if (avctx->codec->id == AV_CODEC_ID_H264) {
            OMX_VIDEO_PARAM_AVCTYPE avc = { 0 };
            INIT_STRUCT(avc);
            avc.nPortIndex = s->out_port;
            err = OMX_GetParameter(s->handle, OMX_IndexParamVideoAvc, &avc);
            CHECK(err);
            if (ducati) {
                avc.eProfile = OMX_VIDEO_AVCProfileBaseline; // stagefright does this
                avc.nBFrames = 0; // This is necessary for the encoder to work
            }
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
    for (i = 0; i < s->num_in_buffers && err == OMX_ErrorNone; i++)
        err = OMX_AllocateBuffer(s->handle, &s->in_buffer_headers[i],  s->in_port,  s, in_port_params.nBufferSize);
    CHECK(err);
    s->num_in_buffers = i;
    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++)
        err = OMX_AllocateBuffer(s->handle, &s->out_buffer_headers[i], s->out_port, s, out_port_params.nBufferSize);
    CHECK(err);
    s->num_out_buffers = i;

    if (wait_for_state(s, OMX_StateIdle) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Didn't get OMX_StateIdle\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    err = OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    CHECK(err);
    if (wait_for_state(s, OMX_StateExecuting) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Didn't get OMX_StateExecuting\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    for (i = 0; i < s->num_out_buffers; i++)
        OMX_FillThisBuffer(s->handle, s->out_buffer_headers[i]);
    for (i = 0; i < s->num_in_buffers; i++)
        s->free_in_buffers[s->num_free_in_buffers++] = s->in_buffer_headers[i];
    return 0;
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
            OMX_BUFFERHEADERTYPE *buffer;
            pthread_mutex_lock(&s->input_mutex);
            while (!s->num_free_in_buffers)
                pthread_cond_wait(&s->input_cond, &s->input_mutex);
            buffer = s->free_in_buffers[0];
            s->num_free_in_buffers--;
            memmove(&s->free_in_buffers[0], &s->free_in_buffers[1], s->num_free_in_buffers * sizeof(OMX_BUFFERHEADERTYPE*));
            pthread_mutex_unlock(&s->input_mutex);
            OMX_FreeBuffer(s->handle, s->in_port, buffer);
        }
        for (i = 0; i < s->num_out_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer;
            pthread_mutex_lock(&s->output_mutex);
            while (!s->num_done_out_buffers)
                pthread_cond_wait(&s->output_cond, &s->output_mutex);
            buffer = s->done_out_buffers[0];
            s->num_done_out_buffers--;
            memmove(&s->done_out_buffers[0], &s->done_out_buffers[1], s->num_done_out_buffers * sizeof(OMX_BUFFERHEADERTYPE*));
            pthread_mutex_unlock(&s->output_mutex);
            OMX_FreeBuffer(s->handle, s->out_port, buffer);
        }
        wait_for_state(s, OMX_StateLoaded);
    }
    if (s->handle) {
        omx_context->ptr_FreeHandle(s->handle);
        s->handle = NULL;
    }

    omx_deinit();
    pthread_cond_destroy(&s->state_cond);
    pthread_mutex_destroy(&s->state_mutex);
    pthread_cond_destroy(&s->input_cond);
    pthread_mutex_destroy(&s->input_mutex);
    pthread_cond_destroy(&s->output_cond);
    pthread_mutex_destroy(&s->output_mutex);
    av_freep(&s->in_buffer_headers);
    av_freep(&s->out_buffer_headers);
    av_freep(&s->free_in_buffers);
    av_freep(&s->done_out_buffers);
    if (s->bsfc)
        av_bitstream_filter_close(s->bsfc);
    av_freep(&s->extradata_free);
    av_freep(&s->output_buf);
}

static enum AVPixelFormat omx_encoder_pix_fmts[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE
};
static int omx_encoder_static_inited = 0;

static av_cold void omx_encode_init_static(AVCodec *codec)
{
    if (omx_encoder_static_inited)
        return;
    /* Note, we can't pass parameters about which lib to use here */
    if (!omx_init(NULL, NULL, NULL)) {
        char component_name[128];
        if (!find_component(NULL, "video_encoder.avc", component_name, sizeof(component_name))) {
            if (strstr(component_name, "OMX.TI.DUCATI1.")) {
                omx_encoder_pix_fmts[0] = AV_PIX_FMT_NV12;
            } else if (!strcmp(component_name, "OMX.TI.Video.encoder")) {
                omx_encoder_pix_fmts[0] = AV_PIX_FMT_YUV420P;
            } else if (strstr(component_name, "OMX.qcom.video.encoder")) {
                // Some of them use NV12, too
                omx_encoder_pix_fmts[0] = AV_PIX_FMT_NV21;
            }
        }
        omx_deinit();
    }
    omx_encoder_static_inited = 1;
}

static av_cold int omx_encode_init(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    int ret = AVERROR_ENCODER_NOT_FOUND;
    const char *role;
    OMX_BUFFERHEADERTYPE *buffer;

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
        role = "video_encoder.mpeg4";
        break;
    case AV_CODEC_ID_H264:
        role = "video_encoder.avc";
        break;
    default:
        return AVERROR(ENOSYS);
    }

    if ((ret = get_component(avctx, role)) < 0)
        goto fail;

    av_log(avctx, AV_LOG_INFO, "Using %s\n", s->component_name);

    if ((ret = omx_component_init(avctx, role, 1)) < 0)
        goto fail;

#ifdef OMX_BUFFERFLAG_CODECCONFIG
    if ((av_strstart(s->component_name, "OMX.qcom.video.encoder", NULL) || av_strstart(s->component_name, "OMX.broadcom.video_encode", NULL)) && avctx->flags & CODEC_FLAG_GLOBAL_HEADER) {
retry:
        pthread_mutex_lock(&s->output_mutex);
        while (!s->num_done_out_buffers)
            pthread_cond_wait(&s->output_cond, &s->output_mutex);
        buffer = s->done_out_buffers[0];
        s->num_done_out_buffers--;
        memmove(&s->done_out_buffers[0], &s->done_out_buffers[1], s->num_done_out_buffers * sizeof(OMX_BUFFERHEADERTYPE*));
        pthread_mutex_unlock(&s->output_mutex);
        if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            avctx->extradata = av_realloc(avctx->extradata, avctx->extradata_size + buffer->nFilledLen + FF_INPUT_BUFFER_PADDING_SIZE);
            memcpy(avctx->extradata + avctx->extradata_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
            avctx->extradata_size += buffer->nFilledLen;
            memset(avctx->extradata + avctx->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
        }
        if (av_strstart(s->component_name, "OMX.broadcom.video_encode", NULL) && avctx->codec->id == AV_CODEC_ID_H264) {
            if (buffer->pBuffer[buffer->nOffset + 4] & 0x1f != NAL_PPS) {
                OMX_FillThisBuffer(s->handle, buffer);
                goto retry;
            }
        } else {
            if (!(buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG)) {
                OMX_FillThisBuffer(s->handle, buffer);
                goto retry;
            }
        }
        OMX_FillThisBuffer(s->handle, buffer);
    }
#endif

    return 0;
fail:
    cleanup(s);
    return ret;
}


static int omx_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    OMXCodecContext *s = avctx->priv_data;
    int res = 0, i;
    OMX_BUFFERHEADERTYPE* buffer;
    uint8_t *ptr;

    if (frame) {
        int chroma_linesize, chroma_stride;
        pthread_mutex_lock(&s->input_mutex);
        while (!s->num_free_in_buffers)
            pthread_cond_wait(&s->input_cond, &s->input_mutex);
        buffer = s->free_in_buffers[--s->num_free_in_buffers];
        pthread_mutex_unlock(&s->input_mutex);

        ptr = buffer->pBuffer;
        for (i = 0; i < avctx->height; i++) {
            memcpy(ptr, frame->data[0] + i*frame->linesize[0], avctx->width);
            ptr += s->stride;
        }
        ptr += s->stride * (s->plane_size - avctx->height);
        if (avctx->pix_fmt == AV_PIX_FMT_NV12 || avctx->pix_fmt == AV_PIX_FMT_NV21) {
            chroma_linesize = avctx->width;
            chroma_stride = s->stride;
        } else {
            chroma_linesize = avctx->width/2;
            chroma_stride = s->stride/2;
        }
        for (i = 0; i < avctx->height/2; i++) {
            memcpy(ptr, frame->data[1] + i*frame->linesize[1], chroma_linesize);
            ptr += chroma_stride;
        }
        if (avctx->pix_fmt != AV_PIX_FMT_NV12 && avctx->pix_fmt != AV_PIX_FMT_NV21) {
            ptr += chroma_stride * (s->plane_size/2 - avctx->height/2);
            for (i = 0; i < avctx->height/2; i++) {
                memcpy(ptr, frame->data[2] + i*frame->linesize[2], chroma_linesize);
                ptr += chroma_stride;
            }
        }
        buffer->nFilledLen = s->stride*s->plane_size + 2*s->stride/2*s->plane_size/2;
        buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
        buffer->nOffset = 0;
        // Convert the timestamps to microseconds; some encoders can ignore
        // the framerate and do VFR bit allocation based on timestamps.
        buffer->nTimeStamp = av_rescale_q(frame->pts, avctx->time_base, AV_TIME_BASE_Q);
        OMX_EmptyThisBuffer(s->handle, buffer);
        s->num_in_frames++;
    }


retry:
    pthread_mutex_lock(&s->output_mutex);
    if (!frame && s->num_out_frames < s->num_in_frames) {
        while (!s->num_done_out_buffers)
            pthread_cond_wait(&s->output_cond, &s->output_mutex);
    }
    if (s->num_done_out_buffers) {
        buffer = s->done_out_buffers[0];
        s->num_done_out_buffers--;
        memmove(&s->done_out_buffers[0], &s->done_out_buffers[1], s->num_done_out_buffers * sizeof(OMX_BUFFERHEADERTYPE*));
    } else {
        buffer = NULL;
    }
    pthread_mutex_unlock(&s->output_mutex);

    if (buffer) {
#ifdef OMX_BUFFERFLAG_CODECCONFIG
        if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG && avctx->flags & CODEC_FLAG_GLOBAL_HEADER) {
            avctx->extradata = av_realloc(avctx->extradata, avctx->extradata_size + buffer->nFilledLen + FF_INPUT_BUFFER_PADDING_SIZE);
            memcpy(avctx->extradata + avctx->extradata_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
            avctx->extradata_size += buffer->nFilledLen;
            memset(avctx->extradata + avctx->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
            OMX_FillThisBuffer(s->handle, buffer);
            goto retry;
        }
#endif
        if (!(buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME)) {
            int newsize = s->output_buf_size + buffer->nFilledLen;
            if ((res = av_reallocp(&s->output_buf, newsize)) < 0) {
                s->output_buf_size = 0;
                return res;
            }
            memcpy(s->output_buf + s->output_buf_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
            s->output_buf_size += buffer->nFilledLen;
            OMX_FillThisBuffer(s->handle, buffer);
            goto retry;
        }
        s->num_out_frames++;
        if ((res = ff_alloc_packet(pkt, s->output_buf_size + buffer->nFilledLen)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Error getting output packet of size %d.\n", (int)buffer->nFilledLen);
            OMX_FillThisBuffer(s->handle, buffer);
            return res;
        }
        memcpy(pkt->data, s->output_buf, s->output_buf_size);
        memcpy(pkt->data + s->output_buf_size, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
        res = s->output_buf_size + buffer->nFilledLen;
        av_freep(&s->output_buf);
        s->output_buf_size = 0;
        avctx->coded_frame = &s->frame;
        pkt->pts = avctx->coded_frame->pts = av_rescale_q(buffer->nTimeStamp, AV_TIME_BASE_Q, avctx->time_base);
        if (buffer->nFlags & OMX_BUFFERFLAG_SYNCFRAME)
            pkt->flags |= AV_PKT_FLAG_KEY;
        *got_packet = 1;
        OMX_FillThisBuffer(s->handle, buffer);
    }
    return res;
}

static av_cold int omx_encode_end(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;

    cleanup(s);
    return 0;
}

#undef CHECK
#define CHECK(x) do { if (x != OMX_ErrorNone) { av_log(avctx, AV_LOG_ERROR, "err %x (%d) on line %d\n", x, x, __LINE__); return AVERROR_INVALIDDATA; } } while (0)

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
    if (strstr(s->component_name, "OMX.qcom.video.decoder") && s->color_format == OMX_COLOR_FormatYUV420Planar)
        s->color_format = OMX_QCOM_COLOR_FormatYVU420SemiPlanar;

    INIT_STRUCT(crop_rect);
    crop_rect.nPortIndex = s->out_port;
    err = OMX_GetConfig(s->handle, OMX_IndexConfigCommonOutputCrop, &crop_rect);
    if (err == OMX_ErrorNone) {
        avctx->width = crop_rect.nWidth;
        avctx->height = crop_rect.nHeight;
        if (out_port_params.format.video.eColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar)
            s->plane_size -= crop_rect.nTop/2;
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
    case OMX_TI_COLOR_FormatYUV420PackedSemiPlanar:
    case OMX_COLOR_FormatYUV420PackedSemiPlanar:
        avctx->pix_fmt = AV_PIX_FMT_NV12;
        break;
    case OMX_QCOM_COLOR_FormatYVU420SemiPlanar:
        avctx->pix_fmt = AV_PIX_FMT_NV21;
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
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
        role = "video_decoder.vc1";
        break;
    default:
        return AVERROR(ENOSYS);
    }

    if ((ret = get_component(avctx, role)) < 0)
        goto fail;

    av_log(avctx, AV_LOG_INFO, "Using %s\n", s->component_name);

    if ((ret = omx_component_init(avctx, role, 0)) < 0)
        goto fail;

    if (avctx->codec->id == AV_CODEC_ID_H264 && avctx->extradata && avctx->extradata[0] == 1) {
        uint8_t *dummy_p, *tmp;
        int dummy_int, tmpsize;
        s->bsfc = av_bitstream_filter_init("h264_mp4toannexb");
        if (!s->bsfc) {
            av_log(avctx, AV_LOG_ERROR, "Cannot open the h264_mp4toannexb BSF!\n");
            ret = AVERROR(ENOSYS);
            goto fail;
        }
        tmp = av_malloc(avctx->extradata_size);
        if (!tmp) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        memcpy(tmp, avctx->extradata, avctx->extradata_size);
        tmpsize = avctx->extradata_size;
        av_bitstream_filter_filter(s->bsfc, avctx, NULL, &dummy_p, &dummy_int, NULL, 0, 0);
        s->extradata_free = s->extradata = avctx->extradata;
        s->extradata_size = avctx->extradata_size;
        avctx->extradata = tmp;
        avctx->extradata_size = tmpsize;
    } else {
        s->extradata = avctx->extradata;
        s->extradata_size = avctx->extradata_size;
    }

    if (s->extradata_size) {
        OMX_BUFFERHEADERTYPE *buffer;
        pthread_mutex_lock(&s->input_mutex);
        while (!s->num_free_in_buffers)
            pthread_cond_wait(&s->input_cond, &s->input_mutex);
        buffer = s->free_in_buffers[--s->num_free_in_buffers];
        pthread_mutex_unlock(&s->input_mutex);

        memcpy(buffer->pBuffer, s->extradata, s->extradata_size);
        buffer->nFilledLen = s->extradata_size;
        buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;
        buffer->nOffset = 0;
        buffer->nTimeStamp = 0;
        OMX_EmptyThisBuffer(s->handle, buffer);
    }

    if (omx_update_out_def(avctx))
        return AVERROR_INVALIDDATA;

    return 0;
fail:
    cleanup(s);
    return ret;
}

static int omx_reconfigure_out(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params = { 0 };
    OMX_ERRORTYPE err;
    int i;

    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->out_port, NULL);
    CHECK(err);

    pthread_mutex_lock(&s->output_mutex);
    for (i = 0; i < s->num_out_buffers; i++) {
        OMX_BUFFERHEADERTYPE *buffer;
        while (!s->num_done_out_buffers)
            pthread_cond_wait(&s->output_cond, &s->output_mutex);
        buffer = s->done_out_buffers[s->num_done_out_buffers - 1];
        s->num_done_out_buffers--;
        pthread_mutex_unlock(&s->output_mutex);
        OMX_FreeBuffer(s->handle, s->out_port, buffer);
        pthread_mutex_lock(&s->output_mutex);
    }
    pthread_mutex_unlock(&s->output_mutex);

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

    if (strstr(s->component_name, "OMX.TI.DUCATI1."))
        out_port_params.nBufferSize *= 2;

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
    AVPacket pkt;
    int orig_size = avpkt ? avpkt->size : 0;

    if (avpkt && avpkt->data && s->bsfc) {
        pkt = *avpkt;
        pkt.data = NULL;
        av_bitstream_filter_filter(s->bsfc, avctx, NULL, &pkt.data, &pkt.size, avpkt->data, avpkt->size, avpkt->flags & AV_PKT_FLAG_KEY);
        avpkt = &pkt;
    }

start:
    if (avpkt && avpkt->data) {
        // TODO: Check num_done_out_buffers too, unless we've returned data already
        pthread_mutex_lock(&s->input_mutex);
        while (!s->num_free_in_buffers && !s->reconfigure_out && !s->update_out_def)
            pthread_cond_wait(&s->input_cond, &s->input_mutex);
        if (s->reconfigure_out) {
            s->reconfigure_out = 0;
            pthread_mutex_unlock(&s->input_mutex);
            if (omx_reconfigure_out(avctx))
                return AVERROR_INVALIDDATA;
            goto start;
        }
        if (s->update_out_def) {
            s->update_out_def = 0;
            pthread_mutex_unlock(&s->input_mutex);
            if (omx_update_out_def(avctx))
                return AVERROR_INVALIDDATA;
            goto start;
        }
        buffer = s->free_in_buffers[--s->num_free_in_buffers];
        pthread_mutex_unlock(&s->input_mutex);

        memcpy(buffer->pBuffer, avpkt->data, avpkt->size);
        buffer->nFilledLen = avpkt->size;
        buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
        buffer->nOffset = 0;
        buffer->nTimeStamp = avpkt->pts;
        OMX_EmptyThisBuffer(s->handle, buffer);
        s->num_in_frames++;
    }

    pthread_mutex_lock(&s->output_mutex);
    if (!avpkt && s->num_out_frames < s->num_in_frames) {
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

    if (buffer) {
        const uint8_t *ptr, *chroma;
        AVFrame *frame = data;
        int i, stride = s->stride, width = avctx->width;
        if (ff_get_buffer(avctx, frame, 0) < 0) {
            av_log(avctx, AV_LOG_ERROR, "reget_buffer() failed\n");
            return -1;
        }
        s->num_out_frames++;
        ptr = buffer->pBuffer + buffer->nOffset;
        for (i = 0; i < avctx->height; i++) {
            memcpy(frame->data[0] + frame->linesize[0]*i, ptr, width);
            ptr += stride;
        }
        ptr = chroma = buffer->pBuffer + buffer->nOffset + stride*s->plane_size;
        if (frame->data[2]) {
            width /= 2;
            stride /= 2;
        }
        for (i = 0; i < avctx->height/2; i++) {
            memcpy(frame->data[1] + frame->linesize[1]*i, ptr, width);
            ptr += stride;
        }
        if (frame->data[2]) {
            ptr = chroma + stride*s->plane_size/2;
            for (i = 0; i < avctx->height/2; i++) {
                memcpy(frame->data[2] + frame->linesize[2]*i, ptr, width);
                ptr += stride;
            }
        }

        frame->pts = frame->pkt_pts = buffer->nTimeStamp;
        frame->pkt_dts = AV_NOPTS_VALUE;
        *got_frame = 1;
        OMX_FillThisBuffer(s->handle, buffer);
    }
    if (avpkt && avpkt->data && s->bsfc)
        av_free(avpkt->data);
    return orig_size;
}

static av_cold int omx_decode_end(AVCodecContext *avctx)
{
    OMXCodecContext *s = avctx->priv_data;

    cleanup(s);
    return 0;
}

#define OFFSET(x) offsetof(OMXCodecContext, x)
#define VDE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "omx_libname", "OpenMAX library name", OFFSET(libname), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VDE },
    { "omx_libprefix", "OpenMAX library prefix", OFFSET(libprefix), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VDE },
    { NULL }
};

#if CONFIG_OMX_MPEG4_ENCODER
static const AVClass omx_mpeg4enc_class = {
    .class_name = "OpenMAX MPEG4 encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_omx_mpeg4_encoder = {
    .name             = "omx_mpeg4",
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_MPEG4,
    .priv_data_size   = sizeof(OMXCodecContext),
    .init             = omx_encode_init,
    .encode2          = omx_encode_frame,
    .close            = omx_encode_end,
    .pix_fmts         = omx_encoder_pix_fmts,
    .long_name        = NULL_IF_CONFIG_SMALL("OpenMAX MPEG4 video encoder"),
    .capabilities     = CODEC_CAP_DELAY,
    .init_static_data = omx_encode_init_static,
    .priv_class       = &omx_mpeg4enc_class,
};
#endif

#if CONFIG_OMX_H264_ENCODER
static const AVClass omx_h264enc_class = {
    .class_name = "OpenMAX H264 encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_omx_h264_encoder = {
    .name             = "omx_h264",
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_H264,
    .priv_data_size   = sizeof(OMXCodecContext),
    .init             = omx_encode_init,
    .encode2          = omx_encode_frame,
    .close            = omx_encode_end,
    .pix_fmts         = omx_encoder_pix_fmts,
    .long_name        = NULL_IF_CONFIG_SMALL("OpenMAX H264 video encoder"),
    .capabilities     = CODEC_CAP_DELAY,
    .init_static_data = omx_encode_init_static,
    .priv_class       = &omx_h264enc_class,
};
#endif

#if CONFIG_OMX_H264_DECODER
static const AVClass omx_h264dec_class = {
    .class_name = "OpenMAX H264 decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_omx_h264_decoder = {
    .name           = "omx_h264",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(OMXCodecContext),
    .init           = omx_decode_init,
    .decode         = omx_decode_frame,
    .close          = omx_decode_end,
    .pix_fmts       = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX H264 video decoder"),
    .capabilities   = CODEC_CAP_DELAY,
    .priv_class     = &omx_h264dec_class,
};
#endif

#if CONFIG_OMX_MPEG4_DECODER
static const AVClass omx_mpeg4dec_class = {
    .class_name = "OpenMAX MPEG4 decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_omx_mpeg4_decoder = {
    .name           = "omx_mpeg4",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MPEG4,
    .priv_data_size = sizeof(OMXCodecContext),
    .init           = omx_decode_init,
    .decode         = omx_decode_frame,
    .close          = omx_decode_end,
    .pix_fmts       = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX MPEG4 video decoder"),
    .capabilities   = CODEC_CAP_DELAY,
    .priv_class     = &omx_mpeg4dec_class,
};
#endif

#if CONFIG_OMX_VC1_DECODER
static const AVClass omx_vc1dec_class = {
    .class_name = "OpenMAX VC1 decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_omx_vc1_decoder = {
    .name           = "omx_vc1",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VC1,
    .priv_data_size = sizeof(OMXCodecContext),
    .init           = omx_decode_init,
    .decode         = omx_decode_frame,
    .close          = omx_decode_end,
    .pix_fmts       = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX VC1 video decoder"),
    .capabilities   = CODEC_CAP_DELAY,
    .priv_class     = &omx_vc1dec_class,
};
#endif

#if CONFIG_OMX_WMV3_DECODER
static const AVClass omx_wmv3dec_class = {
    .class_name = "OpenMAX WMV3 decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_omx_wmv3_decoder = {
    .name           = "omx_wmv3",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_WMV3,
    .priv_data_size = sizeof(OMXCodecContext),
    .init           = omx_decode_init,
    .decode         = omx_decode_frame,
    .close          = omx_decode_end,
    .pix_fmts       = (const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE},
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX WMV3 video decoder"),
    .capabilities   = CODEC_CAP_DELAY,
    .priv_class     = &omx_wmv3dec_class,
};
#endif
