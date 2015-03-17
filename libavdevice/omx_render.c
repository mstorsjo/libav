/*
 * OMX Render
 * Copyright (C) 2015 Martin Storsjo
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
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/time.h"
#include "libavcodec/omx_core.h"
#include "libavformat/avformat.h"
#include <pthread.h>
#include <sys/time.h>
#include <OMX_Broadcom.h>

typedef struct OMXRenderContext {
    const AVClass *class;
    char *libname;
    char *libprefix;

    AVFormatContext *ctx;

    char *video_size;

    char component_name[OMX_MAX_STRINGNAME_SIZE];
    OMX_VERSIONTYPE version;
    OMX_HANDLETYPE handle;
    int in_port;
    int disabled, enabled;
    OMX_COLOR_FORMATTYPE color_format;
    int stride, plane_size;

    int num_in_buffers;
    OMX_BUFFERHEADERTYPE **in_buffer_headers;
    int num_free_in_buffers;
    OMX_BUFFERHEADERTYPE **free_in_buffers;
    pthread_mutex_t input_mutex;
    pthread_cond_t input_cond;

    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    OMX_STATETYPE state;
    OMX_ERRORTYPE error;

    int num_in_frames;
} OMXRenderContext;

static OMX_ERRORTYPE event_handler(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2, OMX_PTR event_data)
{
    OMXRenderContext *s = app_data;
    switch (event) {
    case OMX_EventError:
        pthread_mutex_lock(&s->state_mutex);
        av_log(s->ctx, AV_LOG_ERROR, "OMX error %x\n", (uint32_t) data1);
        s->error = data1;
        pthread_cond_broadcast(&s->state_cond);
        pthread_mutex_unlock(&s->state_mutex);
        break;
    case OMX_EventCmdComplete:
        if (data1 == OMX_CommandStateSet) {
            pthread_mutex_lock(&s->state_mutex);
            s->state = data2;
            av_log(s->ctx, AV_LOG_INFO, "OMX state changed to %d\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortDisable) {
            pthread_mutex_lock(&s->state_mutex);
            s->disabled = 1;
            av_log(s->ctx, AV_LOG_INFO, "OMX port %d disabled\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else if (data1 == OMX_CommandPortEnable) {
            pthread_mutex_lock(&s->state_mutex);
            s->enabled = 1;
            av_log(s->ctx, AV_LOG_INFO, "OMX port %d enabled\n", (uint32_t) data2);
            pthread_cond_broadcast(&s->state_cond);
            pthread_mutex_unlock(&s->state_mutex);
        } else {
            av_log(s->ctx, AV_LOG_INFO, "OMX command complete, command %d, value %d\n", (uint32_t) data1, (uint32_t) data2);
        }
        break;
    case OMX_EventPortSettingsChanged:
        av_log(s->ctx, AV_LOG_INFO, "OMX port %d settings changed\n", (uint32_t) data1);
        pthread_mutex_lock(&s->input_mutex);
        pthread_mutex_unlock(&s->input_mutex);
        break;
    case OMX_EventParamOrConfigChanged:
        switch (data2) {
        case OMX_IndexParamCameraDeviceNumber:
            av_log(s->ctx, AV_LOG_INFO, "OMX_IndexParamCameraDeviceNumber changed\n");
            break;
        }
        break;
    default:
        av_log(s->ctx, AV_LOG_INFO, "OMX event %x %x %x\n", event, (uint32_t) data1, (uint32_t) data2);
        break;
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE empty_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_BUFFERHEADERTYPE *buffer)
{
    OMXRenderContext *s = app_data;
    if (buffer->pAppPrivate) {
        av_buffer_unref((AVBufferRef**)&buffer->pAppPrivate);
        buffer->pAppPrivate = NULL;
    }

    append_buffer(&s->input_mutex, &s->input_cond,
                  &s->num_free_in_buffers, s->free_in_buffers, buffer);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_BUFFERHEADERTYPE *buffer)
{
    return OMX_ErrorNone;
}

static const OMX_CALLBACKTYPE callbacks = {
    event_handler,
    empty_buffer_done,
    fill_buffer_done
};

static av_cold int wait_for_state(OMXRenderContext *s, OMX_STATETYPE state)
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

#if 0
static av_cold int wait_for_port_event(OMXRenderContext *s, int enabled)
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
#endif

static av_cold int omx_component_init(AVFormatContext *s1)
{
    OMXRenderContext *s = s1->priv_data;
    AVCodecParameters *codecpar = s1->streams[0]->codecpar;
    OMX_PORT_PARAM_TYPE video_port_params = { 0 };
    OMX_PARAM_PORTDEFINITIONTYPE in_port_params = { 0 };
    OMX_VIDEO_PARAM_PORTFORMATTYPE video_port_format = { 0 };
    OMX_CONFIG_DISPLAYREGIONTYPE config_display = { 0 };
    OMX_ERRORTYPE err;
    int i;

    s->version.s.nVersionMajor = 1;
    s->version.s.nVersionMinor = 1;
    s->version.s.nRevision     = 2;

    err = ff_omx_context->ptr_GetHandle(&s->handle, s->component_name, s, (OMX_CALLBACKTYPE*) &callbacks);
    if (err != OMX_ErrorNone) {
        av_log(s1, AV_LOG_ERROR, "OMX_GetHandle(%s) failed: %x\n", s->component_name, err);
        return AVERROR_ENCODER_NOT_FOUND;
    }

#define CHECK(x) do { if (x != OMX_ErrorNone) { av_log(s1, AV_LOG_ERROR, "err %x (%d) on line %d\n", x, x, __LINE__); return AVERROR_ENCODER_NOT_FOUND; } } while (0)
    INIT_STRUCT(video_port_params);
    err = OMX_GetParameter(s->handle, OMX_IndexParamVideoInit, &video_port_params);
    CHECK(err);

    s->in_port = -1;
    for (i = 0; i < video_port_params.nPorts; i++) {
        int port = video_port_params.nStartPortNumber + i;
        OMX_PARAM_PORTDEFINITIONTYPE port_params = { 0 };
        INIT_STRUCT(port_params);
        port_params.nPortIndex = port;
        err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &port_params);
        if (err != OMX_ErrorNone) {
            av_log(s1, AV_LOG_WARNING, "port %d error %x\n", port, err);
            break;
        }
        if (port_params.eDir == OMX_DirInput && s->in_port < 0) {
            in_port_params = port_params;
            s->in_port = port;
        }
    }
    if (s->in_port < 0) {
        av_log(s1, AV_LOG_ERROR, "No in port found\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    INIT_STRUCT(video_port_format);
    video_port_format.nIndex = 0;
    video_port_format.nPortIndex = s->in_port;
    OMX_GetParameter(s->handle, OMX_IndexParamVideoPortFormat, &video_port_format);
    s->color_format = video_port_format.eColorFormat;

    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    in_port_params.format.video.nFrameWidth = codecpar->width;
    in_port_params.format.video.nFrameHeight = codecpar->height;
    in_port_params.format.video.nStride = 0;
    in_port_params.format.video.nSliceHeight = FFALIGN(codecpar->height, 16);
//    in_port_params.format.video.xFramerate = 30 << 16;
    s->num_in_buffers = in_port_params.nBufferCountActual;

    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &in_port_params);
    CHECK(err);
    s->stride = FFMAX(in_port_params.format.video.nStride, in_port_params.format.video.nFrameWidth);
    s->plane_size = FFMAX(in_port_params.format.video.nSliceHeight, in_port_params.format.video.nFrameHeight);

    err = OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
    CHECK(err);

    s->in_buffer_headers = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_in_buffers);
    s->free_in_buffers   = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_in_buffers);
    for (i = 0; i < s->num_in_buffers && err == OMX_ErrorNone; i++) {
        err = OMX_UseBuffer(s->handle, &s->in_buffer_headers[i], s->in_port, s, in_port_params.nBufferSize, NULL);
        if (err == OMX_ErrorNone)
            s->in_buffer_headers[i]->pAppPrivate = NULL;
    }
    CHECK(err);
    s->num_in_buffers = i;

    if (wait_for_state(s, OMX_StateIdle) < 0) {
        av_log(s1, AV_LOG_ERROR, "Didn't get OMX_StateIdle\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    err = OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    CHECK(err);
    if (wait_for_state(s, OMX_StateExecuting) < 0) {
        av_log(s1, AV_LOG_ERROR, "Didn't get OMX_StateExecuting\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }

    INIT_STRUCT(config_display);
    config_display.nPortIndex = s->in_port;

    config_display.set = OMX_DISPLAY_SET_SRC_RECT;
    config_display.src_rect.width = codecpar->width;
    config_display.src_rect.height = codecpar->height;
    OMX_SetConfig(s->handle, OMX_IndexConfigDisplayRegion, &config_display);
    config_display.set = OMX_DISPLAY_SET_FULLSCREEN;
    config_display.fullscreen = OMX_TRUE;
    OMX_SetConfig(s->handle, OMX_IndexConfigDisplayRegion, &config_display);

    for (i = 0; i < s->num_in_buffers; i++)
        s->free_in_buffers[s->num_free_in_buffers++] = s->in_buffer_headers[i];

    return 0;
}

static av_cold void cleanup(OMXRenderContext *s)
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
            OMX_FreeBuffer(s->handle, s->in_port, buffer);
        }
        wait_for_state(s, OMX_StateLoaded);
    }
    if (s->handle) {
        ff_omx_context->ptr_FreeHandle(s->handle);
        s->handle = NULL;
    }

    ff_omx_deinit();
    pthread_cond_destroy(&s->state_cond);
    pthread_mutex_destroy(&s->state_mutex);
    pthread_cond_destroy(&s->input_cond);
    pthread_mutex_destroy(&s->input_mutex);
    av_freep(&s->in_buffer_headers);
    av_freep(&s->free_in_buffers);
}

#undef CHECK
#define CHECK(x) do { if (x != OMX_ErrorNone) { av_log(s1, AV_LOG_ERROR, "err %x (%d) on line %d\n", x, x, __LINE__); return AVERROR_INVALIDDATA; } } while (0)

static av_cold int omx_render_init(AVFormatContext *s1)
{
    OMXRenderContext *s = s1->priv_data;
    int ret = AVERROR_ENCODER_NOT_FOUND;
    AVStream *st;

    if (s1->nb_streams != 1) {
        av_log(s1, AV_LOG_ERROR, "Incorrect number of streams\n");
        return AVERROR(EINVAL);
    }
    st = s1->streams[0];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || st->codecpar->codec_id != AV_CODEC_ID_RAWVIDEO) {
        av_log(s1, AV_LOG_ERROR, "Invalid stream parameters\n");
        return AVERROR(EINVAL);
    }

    if ((ret = ff_omx_init(s1, s->libname, s->libprefix)) < 0)
        return ret;

    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->state_cond, NULL);
    pthread_mutex_init(&s->input_mutex, NULL);
    pthread_cond_init(&s->input_cond, NULL);
    s->ctx = s1;
    s->state = OMX_StateLoaded;
    s->error = OMX_ErrorNone;

    snprintf(s->component_name, sizeof(s->component_name), "OMX.broadcom.video_render");

    av_log(s1, AV_LOG_INFO, "Using %s\n", s->component_name);

    if ((ret = omx_component_init(s1)) < 0)
        goto fail;

    return 0;
fail:
    cleanup(s);
    return ret;
}

static int omx_render_frame(AVFormatContext *s1, AVPacket *pkt)
{
    OMXRenderContext *s = s1->priv_data;
    AVStream *st = s1->streams[0];
    OMX_BUFFERHEADERTYPE *buffer;
    uint8_t *src[4], *dst[4];
    int src_linesize[4], dst_linesize[4];

    buffer = get_buffer(&s->input_mutex, &s->input_cond,
                        &s->num_free_in_buffers, s->free_in_buffers, 1);

    s->num_in_frames++;


    if (pkt->buf) {
        AVBufferRef *buf = av_buffer_ref(pkt->buf);
        buffer->pAppPrivate = buf;
        buffer->pBuffer = buf->data;
        buffer->nFilledLen = pkt->size;
    } else {
        AVBufferRef *buf = av_buffer_alloc(buffer->nAllocLen);
        buffer->pAppPrivate = buf;
        buffer->pBuffer = buf->data;

    av_image_fill_arrays(src, src_linesize, NULL, st->codecpar->format, st->codecpar->width, st->codecpar->height, 1);
    buffer->nFilledLen = av_image_fill_arrays(dst, dst_linesize, buffer->pBuffer + buffer->nOffset, st->codecpar->format, s->stride, s->plane_size, 1);
    av_image_copy(dst, dst_linesize, (const uint8_t**) src, src_linesize, st->codecpar->format, st->codecpar->width, st->codecpar->height);
    }
    buffer->nOffset = 0;
    buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
    buffer->nTimeStamp = to_omx_ticks(0);
    OMX_EmptyThisBuffer(s->handle, buffer);

    return 0;
}

static av_cold int omx_render_end(AVFormatContext *s1)
{
    OMXRenderContext *s = s1->priv_data;

    cleanup(s);
    return 0;
}

#define OFFSET(x) offsetof(OMXRenderContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "omx_libname", "OpenMAX library name", OFFSET(libname), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VD },
    { "omx_libprefix", "OpenMAX library prefix", OFFSET(libprefix), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VD },
    { NULL }
};

static const AVClass omx_render_class = {
    .class_name = "OpenMAX render device",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVOutputFormat ff_omx_render_muxer = {
    .name           = "omx_render",
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX render device"),
    .priv_data_size = sizeof(OMXRenderContext),
    .video_codec    = AV_CODEC_ID_RAWVIDEO,
    .write_header   = omx_render_init,
    .write_packet   = omx_render_frame,
    .write_trailer  = omx_render_end,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &omx_render_class,
};
