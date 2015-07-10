/*
 * Copyright (c) 2003-2013 Loren Merritt
 * Copyright (c) 2015 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* Computes the Structural Similarity Metric between two video streams.
 * original algorithm:
 * Z. Wang, A. C. Bovik, H. R. Sheikh and E. P. Simoncelli,
 *   "Image quality assessment: From error visibility to structural similarity,"
 *   IEEE Transactions on Image Processing, vol. 13, no. 4, pp. 600-612, Apr. 2004.
 *
 * To improve speed, this implementation uses the standard approximation of
 * overlapped 8x8 block sums, rather than the original gaussian weights.
 */

/*
 * @file
 * Caculate the SSIM between two input videos.
 */

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "dualinput.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct SSIMContext {
    const AVClass *class;
    FFDualInputContext dinput;
    FILE *stats_file;
    char *stats_file_str;
    int nb_components;
    uint64_t nb_frames;
    double ssim[4];
    char comps[4];
    int *coefs;
    uint8_t rgba_map[4];
    int planewidth[4];
    int planeheight[4];
    int *temp;
} SSIMContext;

#define OFFSET(x) offsetof(SSIMContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption ssim_options[] = {
    {"stats_file", "Set file where to store per-frame difference information", OFFSET(stats_file_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    {"f",          "Set file where to store per-frame difference information", OFFSET(stats_file_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(ssim);

static const int rgb_coefs[4]  = { 1, 1, 1, 3};
static const int yuv_coefs[4]  = { 4, 1, 1, 6};
static const int gray_coefs[4] = { 1, 0, 0, 1};

static void set_meta(AVDictionary **metadata, const char *key, char comp, float d)
{
    char value[128];
    snprintf(value, sizeof(value), "%0.2f", d);
    if (comp) {
        char key2[128];
        snprintf(key2, sizeof(key2), "%s%c", key, comp);
        av_dict_set(metadata, key2, value, 0);
    } else {
        av_dict_set(metadata, key, value, 0);
    }
}

static void ssim_4x4x2_core(const uint8_t *main, int main_stride,
                            const uint8_t *ref, int ref_stride,
                            int sums[2][4])
{
    int x, y, z;

    for (z = 0; z < 2; z++) {
        uint32_t s1 = 0, s2 = 0, ss = 0, s12 = 0;

        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                int a = main[x + y * main_stride];
                int b = ref[x + y * ref_stride];

                s1  += a;
                s2  += b;
                ss  += a*a;
                ss  += b*b;
                s12 += a*b;
            }
        }

        sums[z][0] = s1;
        sums[z][1] = s2;
        sums[z][2] = ss;
        sums[z][3] = s12;
        main += 4;
        ref += 4;
    }
}

static float ssim_end1(int s1, int s2, int ss, int s12)
{
    static const int ssim_c1 = (int)(.01*.01*255*255*64 + .5);
    static const int ssim_c2 = (int)(.03*.03*255*255*64*63 + .5);

    int fs1 = s1;
    int fs2 = s2;
    int fss = ss;
    int fs12 = s12;
    int vars = fss * 64 - fs1 * fs1 - fs2 * fs2;
    int covar = fs12 * 64 - fs1 * fs2;

    return (float)(2 * fs1 * fs2 + ssim_c1) * (float)(2 * covar + ssim_c2)
         / ((float)(fs1 * fs1 + fs2 * fs2 + ssim_c1) * (float)(vars + ssim_c2));
}

static float ssim_end4(int sum0[5][4], int sum1[5][4], int width)
{
    float ssim = 0.0;
    int i;

    for (i = 0; i < width; i++)
        ssim += ssim_end1(sum0[i][0] + sum0[i + 1][0] + sum1[i][0] + sum1[i + 1][0],
                          sum0[i][1] + sum0[i + 1][1] + sum1[i][1] + sum1[i + 1][1],
                          sum0[i][2] + sum0[i + 1][2] + sum1[i][2] + sum1[i + 1][2],
                          sum0[i][3] + sum0[i + 1][3] + sum1[i][3] + sum1[i + 1][3]);
    return ssim;
}

static float ssim_plane(uint8_t *main, int main_stride,
                        uint8_t *ref, int ref_stride,
                        int width, int height, void *temp)
{
    int z = 0;
    int x, y;
    float ssim = 0.0;
    int (*sum0)[4] = temp;
    int (*sum1)[4] = sum0 + (width >> 2) + 3;

    width >>= 2;
    height >>= 2;

    for (y = 1; y < height; y++) {
        for (; z <= y; z++) {
            FFSWAP(void*, sum0, sum1);
            for (x = 0; x < width; x+=2)
                ssim_4x4x2_core(&main[4 * (x + z * main_stride)], main_stride,
                                &ref[4 * (x + z * ref_stride)], ref_stride,
                                &sum0[x]);
        }

        for (x = 0; x < width - 1; x += 4)
            ssim += ssim_end4(sum0 + x, sum1 + x, FFMIN(4, width - x - 1));
    }

    return ssim / ((height - 1) * (width - 1));
}

static double ssim_db(double ssim, double weight)
{
    return 10 * (log(weight) / log(10) - log(weight - ssim) / log(10));
}

static AVFrame *do_ssim(AVFilterContext *ctx, AVFrame *main,
                        const AVFrame *ref)
{
    AVDictionary **metadata = avpriv_frame_get_metadatap(main);
    SSIMContext *s = ctx->priv;
    float c[4], ssimv;
    int i;

    s->nb_frames++;

    for (i = 0; i < s->nb_components; i++)
        c[i] = ssim_plane(main->data[i], main->linesize[i],
                          ref->data[i], ref->linesize[i],
                          s->planewidth[i], s->planeheight[i], s->temp);

    ssimv = (c[0] * s->coefs[0] + c[1] * s->coefs[1] + c[2] * s->coefs[2]) / s->coefs[3];

    for (i = 0; i < s->nb_components; i++)
        set_meta(metadata, "lavfi.ssim.", s->comps[i], c[i]);

    set_meta(metadata, "lavfi.ssim.All", 0, ssimv);
    set_meta(metadata, "lavfi.ssim.dB", 0, ssim_db(c[0] * s->coefs[0] + c[1] * s->coefs[1] + c[2] * s->coefs[2], s->coefs[3]));

    if (s->stats_file) {
        fprintf(s->stats_file, "n:%"PRId64" ", s->nb_frames);

        for (i = 0; i < s->nb_components; i++)
            fprintf(s->stats_file, "%c:%f ", s->comps[i], c[i]);

        fprintf(s->stats_file, "All:%f (%f)\n", ssimv, ssim_db(c[0] * s->coefs[0] + c[1] * s->coefs[1] + c[2] * s->coefs[2], s->coefs[3]));
    }

    s->ssim[0] += c[0];
    s->ssim[1] += c[1];
    s->ssim[2] += c[2];

    return main;
}

static av_cold int init(AVFilterContext *ctx)
{
    SSIMContext *s = ctx->priv;

    if (s->stats_file_str) {
        s->stats_file = fopen(s->stats_file_str, "w");
        if (!s->stats_file) {
            int err = AVERROR(errno);
            char buf[128];
            av_strerror(err, buf, sizeof(buf));
            av_log(ctx, AV_LOG_ERROR, "Could not open stats file %s: %s\n",
                   s->stats_file_str, buf);
            return err;
        }
    }

    s->dinput.process = do_ssim;
    s->dinput.shortest = 1;
    s->dinput.repeatlast = 0;
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_GBRP,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_input_ref(AVFilterLink *inlink)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    AVFilterContext *ctx  = inlink->dst;
    SSIMContext *s = ctx->priv;
    int is_rgb;

    s->nb_components = desc->nb_components;

    if (ctx->inputs[0]->w != ctx->inputs[1]->w ||
        ctx->inputs[0]->h != ctx->inputs[1]->h) {
        av_log(ctx, AV_LOG_ERROR, "Width and height of input videos must be same.\n");
        return AVERROR(EINVAL);
    }
    if (ctx->inputs[0]->format != ctx->inputs[1]->format) {
        av_log(ctx, AV_LOG_ERROR, "Inputs must be of same pixel format.\n");
        return AVERROR(EINVAL);
    }

    is_rgb = ff_fill_rgba_map(s->rgba_map, inlink->format) >= 0;
    s->comps[0] = is_rgb ? 'R' : 'Y';
    s->comps[1] = is_rgb ? 'G' : 'U';
    s->comps[2] = is_rgb ? 'B' : 'V';
    s->comps[3] = 'A';

    if (is_rgb) {
        s->coefs = rgb_coefs;
    } else if (s->nb_components == 1) {
        s->coefs = gray_coefs;
    } else {
        s->coefs = yuv_coefs;
    }

    s->planeheight[1] = s->planeheight[2] = FF_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;
    s->planewidth[1]  = s->planewidth[2]  = FF_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0]  = s->planewidth[3]  = inlink->w;

    s->temp = av_malloc((2 * inlink->w + 12) * sizeof(*s->temp));
    if (!s->temp)
        return AVERROR(ENOMEM);

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    SSIMContext *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[0];
    int ret;

    outlink->w = mainlink->w;
    outlink->h = mainlink->h;
    outlink->time_base = mainlink->time_base;
    outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
    outlink->frame_rate = mainlink->frame_rate;

    if ((ret = ff_dualinput_init(ctx, &s->dinput)) < 0)
        return ret;

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *buf)
{
    SSIMContext *s = inlink->dst->priv;
    return ff_dualinput_filter_frame(&s->dinput, inlink, buf);
}

static int request_frame(AVFilterLink *outlink)
{
    SSIMContext *s = outlink->src->priv;
    return ff_dualinput_request_frame(&s->dinput, outlink);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SSIMContext *s = ctx->priv;

    if (s->nb_frames > 0) {
        if (s->nb_components == 3) {
            av_log(ctx, AV_LOG_INFO, "SSIM %c:%f %c:%f %c:%f All:%f (%f)\n",
               s->comps[0], s->ssim[0] / s->nb_frames,
               s->comps[1], s->ssim[1] / s->nb_frames,
               s->comps[2], s->ssim[2] / s->nb_frames,
               (s->ssim[0] * 4 + s->ssim[1] + s->ssim[2]) / (s->nb_frames * 6),
               ssim_db(s->ssim[0] * 4 + s->ssim[1] + s->ssim[2], s->nb_frames * 6));
        } else if (s->nb_components == 1) {
            av_log(ctx, AV_LOG_INFO, "SSIM All:%f (%f)\n",
               s->ssim[0] / s->nb_frames, ssim_db(s->ssim[0], s->nb_frames));
        }
    }

    ff_dualinput_uninit(&s->dinput);

    if (s->stats_file)
        fclose(s->stats_file);

    av_freep(&s->temp);
}

static const AVFilterPad ssim_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },{
        .name         = "reference",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input_ref,
    },
    { NULL }
};

static const AVFilterPad ssim_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_ssim = {
    .name          = "ssim",
    .description   = NULL_IF_CONFIG_SMALL("Calculate the SSIM between two video streams."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(SSIMContext),
    .priv_class    = &ssim_class,
    .inputs        = ssim_inputs,
    .outputs       = ssim_outputs,
};
