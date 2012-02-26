/*
 * Generic segmenter
 * Copyright (c) 2011, Luca Barbato
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

#include <strings.h>
#include <float.h>

#include "avformat.h"
#include "internal.h"

#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/parseutils.h"
#include "libavutil/mathematics.h"

typedef struct {
    const AVClass *class;  /**< Class for private options. */
    int number;
    AVFormatContext *avf;
    char *format;          /**< Set by a private option. */
    char *list;            /**< Set by a private option. */
    int   hls_list;        /**< Set by a private option. */
    float time;            /**< Set by a private option. */
    int  size;             /**< Set by a private option. */
    int  wrap;             /**< Set by a private option. */
    int  skip_header_trailer; /**< Set by a private option. */
    int64_t offset_time;
    int64_t recording_time;
    int has_video;
    AVIOContext *pb;
} SegmentContext;

static int segment_start(AVFormatContext *s)
{
    SegmentContext *c = s->priv_data;
    AVFormatContext *oc = c->avf;
    int err = 0;

    if (c->wrap)
        c->number %= c->wrap;

    if (av_get_frame_filename(oc->filename, sizeof(oc->filename),
                              s->filename, c->number++) < 0)
        return AVERROR(EINVAL);

    if ((err = avio_open2(&oc->pb, oc->filename, AVIO_FLAG_WRITE,
                          &s->interrupt_callback, NULL)) < 0)
        return err;

    if (oc->oformat->priv_class)
        av_opt_set(oc->priv_data, "resend_headers", "1", 0);

    return 0;
}

static int segment_end(AVFormatContext *oc)
{
    int ret = 0;

    av_write_frame(oc, NULL); /* Flush any buffered data */
    avio_close(oc->pb);

    return ret;
}

static int open_null_ctx(AVIOContext **ctx)
{
    int buf_size = 32768;
    uint8_t *buf = av_malloc(buf_size);
    if (!buf)
        return AVERROR(ENOMEM);
    *ctx = avio_alloc_context(buf, buf_size, AVIO_FLAG_WRITE, NULL, NULL, NULL, NULL);
    if (!*ctx) {
        av_free(buf);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static void close_null_ctx(AVIOContext *pb)
{
    av_free(pb->buffer);
    av_free(pb);
}

static int seg_write_header(AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc;
    int ret, i;

    seg->number = 0;
    seg->offset_time = 0;
    seg->recording_time = seg->time * 1000000;

    if (seg->list) {
        if ((ret = avio_open2(&seg->pb, seg->list, AVIO_FLAG_WRITE,
                              &s->interrupt_callback, NULL)) < 0)
            return ret;
        if (seg->hls_list) {
            avio_printf(seg->pb, "#EXTM3U\n");
            avio_printf(seg->pb, "#EXT-X-TARGETDURATION:%d\n", (int)(seg->time + 0.5));
            avio_printf(seg->pb, "#EXT-X-MEDIA-SEQUENCE:0\n");
            avio_flush(seg->pb);
        }
    }

    for (i = 0; i< s->nb_streams; i++)
        seg->has_video +=
            (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO);

    if (seg->has_video > 1)
        av_log(s, AV_LOG_WARNING,
               "More than a single video stream present, "
               "expect issues decoding it.\n");

    oc = avformat_alloc_context();

    if (!oc) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    oc->oformat = av_guess_format(seg->format, s->filename, NULL);

    if (!oc->oformat) {
        ret = AVERROR_MUXER_NOT_FOUND;
        goto fail;
    }
    if (oc->oformat->flags & AVFMT_NOFILE) {
        av_log(s, AV_LOG_ERROR, "format %s not supported.\n",
               oc->oformat->name);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    oc->interrupt_callback = s->interrupt_callback;
    seg->avf = oc;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st;
        if (!(st = avformat_new_stream(oc, NULL))) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        avcodec_copy_context(st->codec, s->streams[i]->codec);
        st->codec->codec_tag = 0; /* HACK, this fixes writing to ismv */
    }

    if (av_get_frame_filename(oc->filename, sizeof(oc->filename),
                              s->filename, seg->number++) < 0) {
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (!seg->skip_header_trailer) {
    if ((ret = avio_open2(&oc->pb, oc->filename, AVIO_FLAG_WRITE,
                          &s->interrupt_callback, NULL)) < 0)
        goto fail;
    } else {
        if ((ret = open_null_ctx(&oc->pb)) < 0)
            goto fail;
    }

    if ((ret = avformat_write_header(oc, NULL)) < 0) {
        avio_close(oc->pb);
        goto fail;
    }

    if (seg->skip_header_trailer) {
        close_null_ctx(oc->pb);
        if ((ret = avio_open2(&oc->pb, oc->filename, AVIO_FLAG_WRITE,
                              &s->interrupt_callback, NULL)) < 0)
            goto fail;
    }

    if (seg->list) {
        /* HLS TODO:
         * - Add the entries to the playlist only once they're finished
         * - Write the actual segment length, not the target length
         */
        if (seg->hls_list)
            avio_printf(seg->pb, "#EXTINF:%d, no desc\n", (int)(seg->time + 0.5));
        avio_printf(seg->pb, "%s\n", oc->filename);
        avio_flush(seg->pb);
    }

fail:
    if (ret) {
        if (seg->list)
            avio_close(seg->pb);
        avformat_free_context(oc);
    }
    return ret;
}

static int seg_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;
    AVStream *st = s->streams[pkt->stream_index];
    int64_t end_pts = seg->recording_time * seg->number;
    int ret;

    if ((seg->has_video && st->codec->codec_type == AVMEDIA_TYPE_VIDEO) &&
        av_compare_ts(pkt->pts, st->time_base,
                      end_pts, AV_TIME_BASE_Q) >= 0 &&
        pkt->flags & AV_PKT_FLAG_KEY) {

        av_log(s, AV_LOG_DEBUG, "Next segment starts at %d %"PRId64"\n",
               pkt->stream_index, pkt->pts);

        ret = segment_end(oc);

        if (!ret)
            ret = segment_start(s);

        if (ret)
            goto fail;

        if (seg->list) {
            if (seg->hls_list)
                avio_printf(seg->pb, "#EXTINF:%d, no desc\n", (int)(seg->time + 0.5));
            avio_printf(seg->pb, "%s\n", oc->filename);
            avio_flush(seg->pb);
            if (seg->size && !(seg->number % seg->size)) {
                avio_close(seg->pb);
                if ((ret = avio_open2(&seg->pb, seg->list, AVIO_FLAG_WRITE,
                                      &s->interrupt_callback, NULL)) < 0)
                    goto fail;
            }
        }
    }

    ret = ff_write_chained(oc, pkt->stream_index, pkt, s);

fail:
    if (ret < 0) {
        if (seg->list)
            avio_close(seg->pb);
        avformat_free_context(oc);
    }

    return ret;
}

static int seg_write_trailer(struct AVFormatContext *s)
{
    SegmentContext *seg = s->priv_data;
    AVFormatContext *oc = seg->avf;
    int ret;
    if (seg->skip_header_trailer) {
        ret = segment_end(oc);
        open_null_ctx(&oc->pb);
        av_write_trailer(oc);
        close_null_ctx(oc->pb);
    } else {
        av_write_trailer(oc);
        ret = segment_end(oc);
    }
    if (seg->list) {
        if (seg->hls_list)
            avio_printf(seg->pb, "#EXT-X-ENDLIST\n");
        avio_flush(seg->pb);
        avio_close(seg->pb);
    }
    avformat_free_context(oc);
    return ret;
}

#define OFFSET(x) offsetof(SegmentContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "segment_format",    "container format used for the segments",  OFFSET(format),  AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,       E },
    { "segment_time",      "segment length in seconds",               OFFSET(time),    AV_OPT_TYPE_FLOAT,  {.dbl = 2},     0, FLT_MAX, E },
    { "segment_list",      "output the segment list",                 OFFSET(list),    AV_OPT_TYPE_STRING, {.str = NULL},  0, 0,       E },
    { "segment_hls_list",  "output a HLS segment playlist",           OFFSET(hls_list),AV_OPT_TYPE_INT,    {.dbl = 0},     0, 1,       E },
    { "segment_list_size", "maximum number of playlist entries",      OFFSET(size),    AV_OPT_TYPE_INT,    {.dbl = 5},     0, INT_MAX, E },
    { "segment_wrap",      "number after which the index wraps",      OFFSET(wrap),    AV_OPT_TYPE_INT,    {.dbl = 0},     0, INT_MAX, E },
    { "skip_header_trailer", "don't write header/trailer to the segments", OFFSET(skip_header_trailer), AV_OPT_TYPE_INT, {.dbl = 0}, 0, 1, E },
    { NULL },
};

static const AVClass seg_class = {
    .class_name = "segment muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


AVOutputFormat ff_segment_muxer = {
    .name           = "segment",
    .long_name      = NULL_IF_CONFIG_SMALL("segment muxer"),
    .priv_data_size = sizeof(SegmentContext),
    .flags          = AVFMT_GLOBALHEADER | AVFMT_NOFILE,
    .write_header   = seg_write_header,
    .write_packet   = seg_write_packet,
    .write_trailer  = seg_write_trailer,
    .priv_class     = &seg_class,
};
