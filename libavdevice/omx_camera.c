/*
 * OMX Camera
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

typedef struct OMXCameraContext {
    const AVClass *class;
    char *libname;
    char *libprefix;

    AVFormatContext *ctx;

    char *video_size;
    int rotation;

    char component_name[OMX_MAX_STRINGNAME_SIZE];
    OMX_VERSIONTYPE version;
    OMX_HANDLETYPE handle;
    int out_port;
    int reconfigure_out, update_out_def;
    int disabled, enabled;
    OMX_COLOR_FORMATTYPE color_format;
    int stride, plane_size;

    int num_out_buffers;
    OMX_BUFFERHEADERTYPE **out_buffer_headers;
    int num_done_out_buffers;
    OMX_BUFFERHEADERTYPE **done_out_buffers;
    pthread_mutex_t output_mutex;
    pthread_cond_t output_cond;

    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    OMX_STATETYPE state;
    OMX_ERRORTYPE error;

    int num_out_frames;
} OMXCameraContext;

static OMX_ERRORTYPE event_handler(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_EVENTTYPE event, OMX_U32 data1, OMX_U32 data2, OMX_PTR event_data)
{
    OMXCameraContext *s = app_data;
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
        pthread_mutex_lock(&s->output_mutex);
        if (s->out_port == data1 && (data2 == 0 || data2 == OMX_IndexParamPortDefinition)) {
            s->reconfigure_out = 1;
            pthread_cond_broadcast(&s->output_cond);
        } else if (s->out_port == data1 && data2 == OMX_IndexConfigCommonOutputCrop) {
            s->update_out_def = 1;
            pthread_cond_broadcast(&s->output_cond);
        }
        pthread_mutex_unlock(&s->output_mutex);
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
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE fill_buffer_done(OMX_HANDLETYPE component, OMX_PTR app_data, OMX_BUFFERHEADERTYPE *buffer)
{
    OMXCameraContext *s = app_data;
    append_buffer(&s->output_mutex, &s->output_cond,
                  &s->num_done_out_buffers, s->done_out_buffers, buffer);
    return OMX_ErrorNone;
}

static const OMX_CALLBACKTYPE callbacks = {
    event_handler,
    empty_buffer_done,
    fill_buffer_done
};

static av_cold int wait_for_state(OMXCameraContext *s, OMX_STATETYPE state)
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

static av_cold int wait_for_port_event(OMXCameraContext *s, int enabled)
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

#define CHECK(x) do { if (x != OMX_ErrorNone) { av_log(s1, AV_LOG_ERROR, "err %x (%d) on line %d\n", x, x, __LINE__); return AVERROR_ENCODER_NOT_FOUND; } } while (0)

static int fill_buffer(AVFormatContext *s1, OMX_BUFFERHEADERTYPE *buf)
{
    OMXCameraContext *s = s1->priv_data;
    AVPacket *pkt;
    int ret;
    OMX_ERRORTYPE err;

    pkt = av_malloc(sizeof(*pkt));
    if (!pkt)
        return AVERROR(ENOMEM);
    ret = av_new_packet(pkt, buf->nAllocLen);
    if (ret < 0) {
        av_free(pkt);
        return ret;
    }
    buf->pAppPrivate = pkt;
    buf->pBuffer = pkt->data;
    err = OMX_FillThisBuffer(s->handle, buf);
    CHECK(err);
    return 0;
}

static av_cold int omx_component_init(AVFormatContext *s1)
{
    OMXCameraContext *s = s1->priv_data;
    OMX_PORT_PARAM_TYPE video_port_params = { 0 };
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params = { 0 };
    OMX_VIDEO_PARAM_PORTFORMATTYPE video_port_format = { 0 };
    OMX_ERRORTYPE err;
    int i, width = 640, height = 480, ret;
//    OMX_CONFIG_REQUESTCALLBACKTYPE request_callback;
    OMX_PARAM_U32TYPE device;
    OMX_CONFIG_ROTATIONTYPE rotation;

    if (s->video_size &&
        (ret = av_parse_video_size(&width, &height, s->video_size)) < 0) {
        av_log(s1, AV_LOG_ERROR, "Could not parse video size '%s'.\n",
               s->video_size);
        return ret;
    }

    s->version.s.nVersionMajor = 1;
    s->version.s.nVersionMinor = 1;
    s->version.s.nRevision     = 2;

    err = ff_omx_context->ptr_GetHandle(&s->handle, s->component_name, s, (OMX_CALLBACKTYPE*) &callbacks);
    if (err != OMX_ErrorNone) {
        av_log(s1, AV_LOG_ERROR, "OMX_GetHandle(%s) failed: %x\n", s->component_name, err);
        return AVERROR_ENCODER_NOT_FOUND;
    }

/*
    INIT_STRUCT(request_callback);
    request_callback.nPortIndex = OMX_ALL;
    request_callback.nIndex = OMX_IndexParamCameraDeviceNumber;
    request_callback.bEnable = OMX_TRUE;
    err = OMX_SetConfig(s->handle, OMX_IndexConfigRequestCallback, &request_callback);
    CHECK(err);
*/
    INIT_STRUCT(device);
    device.nPortIndex = OMX_ALL;
    device.nU32 = 0;
    OMX_SetParameter(s->handle, OMX_IndexParamCameraDeviceNumber, &device);

    INIT_STRUCT(video_port_params);
    err = OMX_GetParameter(s->handle, OMX_IndexParamVideoInit, &video_port_params);
    CHECK(err);

    s->out_port = -1;
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
        // port 70 is preview, 71 is capture
        if (port_params.eDir == OMX_DirOutput && s->out_port < 0) {
            out_port_params = port_params;
            s->out_port = port;
        }
    }
    if (s->out_port < 0) {
        av_log(s1, AV_LOG_ERROR, "No out port found\n");
        return AVERROR_UNKNOWN;
    }

    INIT_STRUCT(video_port_format);
    video_port_format.nIndex = 0;
    video_port_format.nPortIndex = s->out_port;
    OMX_GetParameter(s->handle, OMX_IndexParamVideoPortFormat, &video_port_format);
    s->color_format = video_port_format.eColorFormat;

    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, 73, NULL);
    if (wait_for_port_event(s, 0))
        return AVERROR_INVALIDDATA;
    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, 71, NULL);
    if (wait_for_port_event(s, 0))
        return AVERROR_INVALIDDATA;
    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, 72, NULL);
    if (wait_for_port_event(s, 0))
        return AVERROR_INVALIDDATA;

    INIT_STRUCT(rotation);
    rotation.nPortIndex = s->out_port;
    rotation.nRotation = s->rotation;
    err = OMX_SetConfig(s->handle, OMX_IndexConfigCommonRotate, &rotation);
    CHECK(err);

    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    out_port_params.format.video.nFrameWidth = width;
    out_port_params.format.video.nFrameHeight = height;
    out_port_params.format.video.nStride = width;
    out_port_params.format.video.nSliceHeight = height;
    out_port_params.format.video.xFramerate = 30 << 16;
    s->num_out_buffers = out_port_params.nBufferCountActual;

    err = OMX_SetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    err = OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
    CHECK(err);

    s->out_buffer_headers = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_out_buffers);
    s->done_out_buffers   = av_mallocz(sizeof(OMX_BUFFERHEADERTYPE*) * s->num_out_buffers);
    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++) {
        err = OMX_UseBuffer(s->handle, &s->out_buffer_headers[i], s->out_port, s, out_port_params.nBufferSize, NULL);
        if (err == OMX_ErrorNone) {
            s->out_buffer_headers[i]->pAppPrivate = NULL;
        }
    }
    CHECK(err);
    s->num_out_buffers = i;

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

    for (i = 0; i < s->num_out_buffers; i++) {
        ret = fill_buffer(s1, s->out_buffer_headers[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static av_cold void cleanup(OMXCameraContext *s)
{
    int i, executing;

    pthread_mutex_lock(&s->state_mutex);
    executing = s->state == OMX_StateExecuting;
    pthread_mutex_unlock(&s->state_mutex);

    if (executing) {
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
        wait_for_state(s, OMX_StateIdle);
        OMX_SendCommand(s->handle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
        for (i = 0; i < s->num_out_buffers; i++) {
            OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                                      &s->num_done_out_buffers, s->done_out_buffers, 1);
            if (buffer->pAppPrivate) {
                av_free_packet((AVPacket*) buffer->pAppPrivate);
                av_free(buffer->pAppPrivate);
                buffer->pBuffer = NULL;
            }
            OMX_FreeBuffer(s->handle, s->out_port, buffer);
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
    pthread_cond_destroy(&s->output_cond);
    pthread_mutex_destroy(&s->output_mutex);
    av_freep(&s->out_buffer_headers);
    av_freep(&s->done_out_buffers);
}

static int omx_update_out_def(AVFormatContext *s1)
{
    OMXCameraContext *s = s1->priv_data;
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params = { 0 };
    OMX_ERRORTYPE err;
    AVCodecParameters *codecpar = s1->streams[0]->codecpar;

    INIT_STRUCT(out_port_params);
    out_port_params.nPortIndex = s->out_port;
    err = OMX_GetParameter(s->handle, OMX_IndexParamPortDefinition, &out_port_params);
    CHECK(err);

    codecpar->width = out_port_params.format.video.nFrameWidth;
    codecpar->height = out_port_params.format.video.nFrameHeight;
    s->stride = out_port_params.format.video.nStride;
    s->plane_size = out_port_params.format.video.nSliceHeight;
    s->color_format = out_port_params.format.video.eColorFormat;

    if (s->plane_size < codecpar->height)
        s->plane_size = codecpar->height;
    if (s->stride < codecpar->width)
        s->stride = codecpar->width;

    codecpar->format = ff_omx_get_pix_fmt(s->color_format);
    return 0;
}

static av_cold int omx_camera_init(AVFormatContext *s1)
{
    OMXCameraContext *s = s1->priv_data;
    int ret = AVERROR_ENCODER_NOT_FOUND;
    AVStream *st;

    if ((ret = ff_omx_init(s1, s->libname, s->libprefix)) < 0)
        return ret;

    pthread_mutex_init(&s->state_mutex, NULL);
    pthread_cond_init(&s->state_cond, NULL);
    pthread_mutex_init(&s->output_mutex, NULL);
    pthread_cond_init(&s->output_cond, NULL);
    s->ctx = s1;
    s->state = OMX_StateLoaded;
    s->error = OMX_ErrorNone;

    snprintf(s->component_name, sizeof(s->component_name), "OMX.broadcom.camera");

    av_log(s1, AV_LOG_INFO, "Using %s\n", s->component_name);

    if ((ret = omx_component_init(s1)) < 0)
        goto fail;

    st = avformat_new_stream(s1, NULL);
    if (!st) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_RAWVIDEO;
    st->time_base = AV_TIME_BASE_Q;

    if (omx_update_out_def(s1))
        return AVERROR_INVALIDDATA;

    return 0;
fail:
    cleanup(s);
    return ret;
}

static int omx_reconfigure_out(AVFormatContext *s1)
{
    OMXCameraContext *s = s1->priv_data;
    OMX_PARAM_PORTDEFINITIONTYPE out_port_params = { 0 };
    OMX_ERRORTYPE err;
    int i, ret;

    err = OMX_SendCommand(s->handle, OMX_CommandPortDisable, s->out_port, NULL);
    CHECK(err);

    for (i = 0; i < s->num_out_buffers; i++) {
        OMX_BUFFERHEADERTYPE *buffer = get_buffer(&s->output_mutex, &s->output_cond,
                                                  &s->num_done_out_buffers, s->done_out_buffers, 1);
        if (buffer->pAppPrivate) {
            av_free_packet((AVPacket*) buffer->pAppPrivate);
            av_free(buffer->pAppPrivate);
            buffer->pBuffer = NULL;
        }
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

    for (i = 0; i < s->num_out_buffers && err == OMX_ErrorNone; i++) {
        err = OMX_UseBuffer(s->handle, &s->out_buffer_headers[i], s->out_port, s, out_port_params.nBufferSize, NULL);
        if (err == OMX_ErrorNone) {
            s->out_buffer_headers[i]->pAppPrivate = NULL;
        }
    }
    CHECK(err);
    s->num_out_buffers = i;

    if (wait_for_port_event(s, 1))
        return AVERROR_INVALIDDATA;

    for (i = 0; i < s->num_out_buffers; i++) {
        ret = fill_buffer(s1, s->out_buffer_headers[i]);
        if (ret < 0)
            return ret;
    }

    omx_update_out_def(s1);
    return 0;
}

static int omx_camera_frame(AVFormatContext *s1, AVPacket *pkt)
{
    OMXCameraContext *s = s1->priv_data;
    OMX_BUFFERHEADERTYPE *buffer;
    AVPacket *bufpkt;

start:
    pthread_mutex_lock(&s->output_mutex);
    while (!s->num_done_out_buffers && !s->reconfigure_out && !s->update_out_def)
        pthread_cond_wait(&s->output_cond, &s->output_mutex);
    if (s->reconfigure_out) {
        s->reconfigure_out = 0;
        pthread_mutex_unlock(&s->output_mutex);
        if (omx_reconfigure_out(s1))
            return AVERROR_INVALIDDATA;
        goto start;
    }
    if (s->update_out_def) {
        s->update_out_def = 0;
        pthread_mutex_unlock(&s->output_mutex);
        if (omx_update_out_def(s1))
            return AVERROR_INVALIDDATA;
        goto start;
    }
    if (s->num_done_out_buffers) {
        buffer = s->done_out_buffers[0];
        s->num_done_out_buffers--;
        memmove(&s->done_out_buffers[0], &s->done_out_buffers[1], s->num_done_out_buffers * sizeof(OMX_BUFFERHEADERTYPE*));
    } else {
        buffer = NULL;
    }
    pthread_mutex_unlock(&s->output_mutex);

    bufpkt = (AVPacket*) buffer->pAppPrivate;
    *pkt = *bufpkt;
    av_free(bufpkt);
    pkt->stream_index = 0;

//    pkt->pts = from_omx_ticks(buffer->nTimeStamp);
    pkt->dts = pkt->pts = av_gettime_relative();
    fill_buffer(s1, buffer);

    return 0;
}

static av_cold int omx_camera_end(AVFormatContext *s1)
{
    OMXCameraContext *s = s1->priv_data;

    cleanup(s);
    return 0;
}

#define OFFSET(x) offsetof(OMXCameraContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "omx_libname", "OpenMAX library name", OFFSET(libname), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VD },
    { "omx_libprefix", "OpenMAX library prefix", OFFSET(libprefix), AV_OPT_TYPE_STRING, { 0 }, 0, 0, VD },
    { "video_size",   "A string describing frame size, such as 640x480 or hd720.", OFFSET(video_size),   AV_OPT_TYPE_STRING, {.str = NULL},  0, 0, VD },
    { "rotation",     "Video rotation.", OFFSET(rotation),   AV_OPT_TYPE_INT, {.i64 = 0}, 0, 270, VD },
    { NULL }
};

static const AVClass omx_camera_class = {
    .class_name = "OpenMAX camera device",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVInputFormat ff_omx_camera_demuxer = {
    .name           = "omx_camera",
    .long_name      = NULL_IF_CONFIG_SMALL("OpenMAX camera device"),
    .priv_data_size = sizeof(OMXCameraContext),
    .read_header    = omx_camera_init,
    .read_packet    = omx_camera_frame,
    .read_close     = omx_camera_end,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &omx_camera_class,
};
