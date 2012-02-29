/*
 * RTP packetization for H.263 video
 * Copyright (c) 2012 Martin Storsjo
 * Copyright (c) 2000,2001 Fabrice Bellard
 * Copyright (c) 2002-2004 Michael Niedermayer
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

#include "avformat.h"
#include "rtpenc.h"
#include "libavcodec/put_bits.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/h263.h"
#include "libavcodec/mathops.h"

#define MV_VLC_BITS 9

extern const uint16_t avpriv_inter_vlc[103][2];
extern const int8_t avpriv_inter_level[102];
extern const int8_t avpriv_inter_run[102];

struct RTPFormatSpecificContext {
    VLC_TYPE intra_MCBPC_vlc_table[72][2];
    VLC_TYPE inter_MCBPC_vlc_table[198][2];
    VLC_TYPE cbpy_vlc_table[64][2];
    VLC_TYPE mv_vlc_table[538][2];
    VLC intra_MCBPC_vlc;
    VLC inter_MCBPC_vlc;
    VLC cbpy_vlc;
    VLC mv_vlc;
    uint8_t rl_table_store[2][2*MAX_RUN + MAX_LEVEL + 3];
    RLTable rl_inter;
};

struct H263Info {
    int src;
    int i;
    int u;
    int s;
    int a;
    int pb;
    int tr;
    int mb_rows_per_gob;
    int mb_per_row;
    int mb_per_gob;
    int cpm;
};

struct H263State {
    int gobn;
    int mba;
    int hmv1, vmv1, hmv2, vmv2;
    int quant;
    int mv[353][2];
    int first_row;
};

static void send_mode_a(AVFormatContext *s1, const struct H263Info *info,
                        const uint8_t *buf, int len, int ebits, int m)
{
    RTPMuxContext *s = s1->priv_data;
    PutBitContext pb;

    init_put_bits(&pb, s->buf, 32);
    put_bits(&pb, 1, 0); /* F - 0, mode A */
    put_bits(&pb, 1, 0); /* P - 0, normal I/P */
    put_bits(&pb, 3, 0); /* SBIT - 0 bits */
    put_bits(&pb, 3, ebits); /* EBIT */
    put_bits(&pb, 3, info->src); /* SRC - source format */
    put_bits(&pb, 1, info->i); /* I - inter/intra */
    put_bits(&pb, 1, info->u); /* U - unrestricted motion vector */
    put_bits(&pb, 1, info->s); /* S - syntax-baesd arithmetic coding */
    put_bits(&pb, 1, info->a); /* A - advanced prediction */
    put_bits(&pb, 4, 0); /* R - reserved */
    put_bits(&pb, 2, 0); /* DBQ - 0 */
    put_bits(&pb, 3, 0); /* TRB - 0 */
    put_bits(&pb, 8, info->tr); /* TR */
    flush_put_bits(&pb);
    memcpy(s->buf + 4, buf, len);

    ff_rtp_send_data(s1, s->buf, len + 4, m);
}

static void send_mode_b(AVFormatContext *s1, const struct H263Info *info,
                        const struct H263State *state, const uint8_t *buf,
                        int len, int sbits, int ebits, int m)
{
    RTPMuxContext *s = s1->priv_data;
    PutBitContext pb;

    init_put_bits(&pb, s->buf, 64);
    put_bits(&pb, 1, 1); /* F - 1, mode B */
    put_bits(&pb, 1, 0); /* P - 0, mode B */
    put_bits(&pb, 3, sbits); /* SBIT - 0 bits */
    put_bits(&pb, 3, ebits); /* EBIT - 0 bits */
    put_bits(&pb, 3, info->src); /* SRC - source format */
    put_bits(&pb, 5, state->quant); /* QUANT - quantizer for the first MB */
    put_bits(&pb, 5, state->gobn); /* GOBN - GOB number */
    put_bits(&pb, 9, state->mba); /* MBA - MB address */
    put_bits(&pb, 2, 0); /* R - reserved */
    put_bits(&pb, 1, info->i); /* I - inter/intra */
    put_bits(&pb, 1, info->u); /* U - unrestricted motion vector */
    put_bits(&pb, 1, info->s); /* S - syntax-baesd arithmetic coding */
    put_bits(&pb, 1, info->a); /* A - advanced prediction */
    put_bits(&pb, 7, state->hmv1); /* HVM1 - horizontal motion vector 1 */
    put_bits(&pb, 7, state->vmv1); /* VMV1 - vertical motion vector 1 */
    put_bits(&pb, 7, state->hmv2); /* HVM2 - horizontal motion vector 2 */
    put_bits(&pb, 7, state->vmv2); /* VMV2 - vertical motion vector 2 */
    flush_put_bits(&pb);
    memcpy(s->buf + 8, buf, len);

    ff_rtp_send_data(s1, s->buf, len + 8, m);
}

static int parse_gob_header(GetBitContext *gb, const struct H263Info *info,
                            struct H263State *state)
{
    int bits = 5 + 2 + 5 + info->cpm ? 2 : 0;
    int left;
    skip_bits(gb, 16); /* Zeros */
    for (left = get_bits_left(gb); left > bits; left--)
        if (get_bits1(gb))
            break;
    if (left <= bits)
        return -1;
    state->gobn = get_bits(gb, 5);
    if (info->cpm)
        skip_bits(gb, 2); /* GSBI */
    skip_bits(gb, 2); /* GFID */
    state->quant = get_bits(gb, 5);
    state->mba = 0;
    state->first_row = 1;
    memset(state->mv, 0, sizeof(state->mv));
    return 0;
}

static void parse_dquant(GetBitContext *gb, int *quant)
{
    static const int8_t quant_tab[4] = { -1, -2, 1, 2 };
    *quant += quant_tab[get_bits(gb, 2)];
    *quant = av_clip(*quant, 1, 31);
}

static void decode_motion(struct RTPFormatSpecificContext *ctx,
                          GetBitContext *gb, int *val)
{
    int sign;
    int code = get_vlc2(gb, ctx->mv_vlc.table, MV_VLC_BITS, 2);
    if (code <= 0)
        return;
    sign = get_bits1(gb);
    if (sign)
        code = -code;
    *val = *val + code;
    *val = sign_extend(*val, 6);
}

static void decode_motion2(struct RTPFormatSpecificContext *ctx,
                           GetBitContext *gb, int *x, int *y)
{
    decode_motion(ctx, gb, x);
    decode_motion(ctx, gb, y);
}

static void calc_mv_predictor(const struct H263Info *info,
                              const struct H263State *state,
                              int *pred_x, int *pred_y)
{
    int mb_x = state->mba % info->mb_per_row;
    if (state->first_row) {
        if (mb_x == 0) {
            *pred_x = *pred_y = 0;
        } else {
            *pred_x = state->mv[mb_x - 1][0];
            *pred_y = state->mv[mb_x - 1][1];
        }
    } else if (mb_x == 0) {
        *pred_x = mid_pred(0, state->mv[mb_x][0], state->mv[mb_x + 1][0]);
        *pred_y = mid_pred(0, state->mv[mb_x][1], state->mv[mb_x + 1][1]);
    } else {
        *pred_x = mid_pred(state->mv[mb_x - 1][0], state->mv[mb_x][0],
                           state->mv[mb_x + 1][0]);
        *pred_y = mid_pred(state->mv[mb_x - 1][1], state->mv[mb_x][1],
                           state->mv[mb_x + 1][1]);
    }
}

static void skip_block(struct RTPFormatSpecificContext *ctx,
                       const struct H263Info *info, GetBitContext *gb,
                       int intra, int coded)
{
    RLTable *rl = &ctx->rl_inter;
    if (intra) {
        skip_bits(gb, 8); /* level */
    } else {
    }
    if (!coded)
        return;
    /* The data isn't necessarily zero padded (when checking a subsequence
     * of the bitstream), so check the number of bits left to avoid
     * infinte loops. */
    while (get_bits_left(gb) > 0) {
        int last = 0;
        int code = get_vlc2(gb, rl->vlc.table, TEX_VLC_BITS, 2);
        if (code < 0) {
            av_log(NULL, AV_LOG_ERROR, "illegal ac vlc code\n");
            return;
        }
        if (code == rl->n) {
            int level;
            last = get_bits1(gb);
            skip_bits(gb, 6);
            level = get_bits(gb, 8);
            if (level == -128)
                skip_bits(gb, 11);
        } else {
            last = code >= rl->last;
            skip_bits(gb, 1);
        }
        if (last)
            break;
    }
}

static int parse_mb(struct RTPFormatSpecificContext *ctx, GetBitContext *gb,
                    const struct H263Info *info, struct H263State *state)
{
    int cbpc, cbpy, cbp, dquant = 0, intra = 0;
    int quant = state->quant;
    int pred_x, pred_y;
    int mb_x = state->mba % info->mb_per_row;
    int i;
    calc_mv_predictor(info, state, &pred_x, &pred_y);
    if (get_bits_left(gb) <= 0)
        return -1;
    if (info->i) {
        do {
            if (get_bits(gb, 1)) {
                if (get_bits_left(gb) <= 0)
                    return -1;
                state->mv[mb_x][0] = state->mv[mb_x][1] = 0;
                return 0;
            }
            cbpc = get_vlc2(gb, ctx->inter_MCBPC_vlc.table,
                            INTER_MCBPC_VLC_BITS, 2);
            if (cbpc < 0)
                return -1;
        } while (cbpc == 20);
        dquant = cbpc & 8;
        if (cbpc & 4)
            goto intra;
        cbpy = get_vlc2(gb, ctx->cbpy_vlc.table, CBPY_VLC_BITS, 1);
        cbpy ^= 0xF;
        cbp = (cbpc & 3) | (cbpy << 2);
        if (dquant)
            parse_dquant(gb, &quant);
        if (!(cbpc & 16)) {
            decode_motion2(ctx, gb, &pred_x, &pred_y);
        } else {
            /* Not implemented */
            av_log(NULL, AV_LOG_ERROR, "4MV not supported\n");
        }
    } else {
        do {
            cbpc = get_vlc2(gb, ctx->intra_MCBPC_vlc.table,
                            INTRA_MCBPC_VLC_BITS, 2);
            if (cbpc < 0)
                return -1;
        } while (cbpc == 8);
        dquant = cbpc & 4;
intra:
        cbpy = get_vlc2(gb, ctx->cbpy_vlc.table, CBPY_VLC_BITS, 1);
        cbp = (cbpc & 3) | (cbpy << 2);
        if (dquant)
            parse_dquant(gb, &quant);
        pred_x = pred_y = 0;
        intra = 1;
    }
    for (i = 0; i < 6; i++) {
        skip_block(ctx, info, gb, intra, cbp & 32);
        cbp += cbp;
    }
    if (get_bits_left(gb) > 0) {
        state->quant = quant;
        state->mv[mb_x][0] = pred_x;
        state->mv[mb_x][1] = pred_y;
    } else {
        return -1;
    }
    return 0;
}

static void find_mb_boundary(struct RTPFormatSpecificContext *ctx,
                             const uint8_t *buf, int sbits,
                             const struct H263Info *info,
                             struct H263State *state, int *len, int *ebits,
                             int first)
{
    GetBitContext gb;
    init_get_bits(&gb, buf, (*len)*8);
    skip_bits(&gb, sbits);
    if (first) {
        /* Parse PSC */
        skip_bits(&gb, 22); /* PSC */
        skip_bits(&gb, 8); /* TR */
        skip_bits(&gb, 2 + 3 + 3 + 5); /* PTYPE */
        skip_bits(&gb, 5); /* PQUANT */
        skip_bits(&gb, 1); /* CPM */
        if (info->cpm)
            skip_bits(&gb, 2); /* PSBI */
        /* Not handling PB frames - skipping TRB and DBQUANT */
        while (get_bits1(&gb)) /* PEI */
            skip_bits(&gb, 8); /* PSUPP */
        state->first_row = 1;
    } else {
        if (!show_bits(&gb, 16))
            parse_gob_header(&gb, info, state);
    }
    /* Start parsing MBs */
    while (1) {
        if (!show_bits(&gb, 16)) {
            /* Improbable, should have been found by the GBSC finder */
            if (parse_gob_header(&gb, info, state) < 0)
                return;
        } else {
            if (state->mba == info->mb_per_gob) {
                state->mba = 0;
                state->gobn++;
            }
        }
        if (parse_mb(ctx, &gb, info, state) < 0)
            break;
        state->mba++;
        if (state->mba == info->mb_per_row)
            state->first_row = 0;
        /* Allow restarting parsing from after this MB */
        *len = (get_bits_count(&gb) + 7)/8;
        *ebits = (8 - (get_bits_count(&gb) & 7)) & 7;
    }
    /* Set predictor according to the chosen MB */
    calc_mv_predictor(info, state, &state->hmv1, &state->vmv1);
}

#define INIT_VLC_PREALLOC(vlc, bits, a, b, c, d, e, f, g) do { \
        vlc.table = vlc ## _table; \
        vlc.table_allocated = sizeof(vlc ## _table)/2/sizeof(vlc ## _table[0][0]); \
        init_vlc(&vlc, bits, a, b, c, d, e, f, g, INIT_VLC_USE_NEW_STATIC); \
    } while (0)

void ff_rtp_send_h263_rfc2190(AVFormatContext *s1, const uint8_t *buf, int size,
                              const uint8_t *mb_info, int mb_info_size)
{
    RTPMuxContext *s = s1->priv_data;
    int len, first = 1, sbits = 0, ebits = 0;
    GetBitContext gb;
    struct H263Info info = { 0 };
    struct H263State state = { 0 };
    int mb_info_pos = 0, mb_info_count = mb_info_size / 12;
    const uint8_t *buf_base = buf;

    s->timestamp = s->cur_timestamp;

    if (!s->priv_data) {
        static const RLTable rl_inter = {
            102,
            58,
            avpriv_inter_vlc,
            avpriv_inter_run,
            avpriv_inter_level,
        };
        s->priv_data = av_mallocz(sizeof(struct RTPFormatSpecificContext));
        if (!s->priv_data)
            return;
        INIT_VLC_PREALLOC(s->priv_data->intra_MCBPC_vlc, INTRA_MCBPC_VLC_BITS, 9,
                          avpriv_h263_intra_MCBPC_bits, 1, 1,
                          avpriv_h263_intra_MCBPC_code, 1, 1);
        INIT_VLC_PREALLOC(s->priv_data->inter_MCBPC_vlc, INTER_MCBPC_VLC_BITS, 28,
                          avpriv_h263_inter_MCBPC_bits, 1, 1,
                          avpriv_h263_inter_MCBPC_code, 1, 1);
        INIT_VLC_PREALLOC(s->priv_data->cbpy_vlc, CBPY_VLC_BITS, 16,
                          &avpriv_h263_cbpy_tab[0][1], 2, 1,
                          &avpriv_h263_cbpy_tab[0][0], 2, 1);
        INIT_VLC_PREALLOC(s->priv_data->mv_vlc, MV_VLC_BITS, 33,
                          &avpriv_mvtab[0][1], 2, 1, &avpriv_mvtab[0][0], 2, 1);
        s->priv_data->rl_inter = rl_inter;
        avpriv_init_rl(&s->priv_data->rl_inter, s->priv_data->rl_table_store);
        INIT_VLC_RL(s->priv_data->rl_inter, 554);
    }

    init_get_bits(&gb, buf, size*8);
    if (get_bits(&gb, 22) == 0x20) { /* Picture Start Code */
        info.tr  = get_bits(&gb, 8);
        skip_bits(&gb, 2); /* PTYPE start, H261 disambiguation */
        skip_bits(&gb, 3); /* Split screen, document camera, freeze picture release */
        info.src = get_bits(&gb, 3);
        info.i   = get_bits(&gb, 1);
        info.u   = get_bits(&gb, 1);
        info.s   = get_bits(&gb, 1);
        info.a   = get_bits(&gb, 1);
        info.pb  = get_bits(&gb, 1);
        state.quant = get_bits(&gb, 5); /* PQUANT */
        info.cpm   = get_bits(&gb, 1); /* CPM */
    }
    if (info.a)
        av_log(s1, AV_LOG_ERROR,
               "Advanced prediction in RFC 2190 not supported currently\n");
    if (s1->streams[0]->codec->height <= 400)
        info.mb_rows_per_gob = 1;
    else if (s1->streams[0]->codec->height <= 800)
        info.mb_rows_per_gob = 2;
    else
        info.mb_rows_per_gob = 4;
    info.mb_per_row = s1->streams[0]->codec->width/16;
    info.mb_per_gob = info.mb_per_row * info.mb_rows_per_gob;
    if (info.mb_per_row > sizeof(state.mv)/sizeof(state.mv[0][0])/2) {
        av_log(s1, AV_LOG_ERROR, "Bad H263 frame size\n");
        return;
    }

    while (size > 0) {
        struct H263State packet_start_state = state;
        len = FFMIN(s->max_payload_size - 8, size);

        /* Look for a better place to split the frame into packets. */
        if (len < size) {
            const uint8_t *end = ff_h263_find_resync_marker_reverse(buf,
                                                                    buf + len);
            len = end - buf;
            if (len == s->max_payload_size - 8 && !mb_info_count) {
                find_mb_boundary(s->priv_data, buf, sbits, &info, &state,
                                 &len, &ebits, first);
            } else if (len == s->max_payload_size - 8) {
                /* Skip mb info prior to the start of the current ptr */
                while (mb_info_pos < mb_info_count) {
                    uint32_t pos = AV_RL32(&mb_info[12*mb_info_pos])/8;
                    if (pos >= buf - buf_base)
                        break;
                    mb_info_pos++;
                }
                /* Find the first mb info past the end pointer */
                while (mb_info_pos + 1 < mb_info_count) {
                    uint32_t pos = AV_RL32(&mb_info[12*(mb_info_pos + 1)])/8;
                    if (pos >= end - buf_base)
                        break;
                    mb_info_pos++;
                }
                if (mb_info_pos < mb_info_count) {
                    const uint8_t *ptr = &mb_info[12*mb_info_pos];
                    uint32_t bit_pos = AV_RL32(ptr);
                    uint32_t pos = (bit_pos + 7)/8;
                    if (pos <= end - buf_base) {
                        state.quant = ptr[4];
                        state.gobn  = ptr[5];
                        state.mba   = AV_RL16(&ptr[6]);
                        state.hmv1  = (int8_t) ptr[8];
                        state.vmv1  = (int8_t) ptr[9];
                        state.hmv2  = (int8_t) ptr[10];
                        state.vmv2  = (int8_t) ptr[11];
                        ebits = 8 * pos - bit_pos;
                        len   = pos - (buf - buf_base);
                        mb_info_pos++;
                    } else {
                        av_log(s1, AV_LOG_ERROR,
                               "Unable to split H263 packet, use -mb_info %d "
                               "or lower.\n", s->max_payload_size - 8);
                    }
                } else {
                    /* The parser only works fully correctly if it has parsed
                     * the bitstream from the last GBSC (to get MV prediction
                     * right.) Therefore, we don't use it here.
                     */
                    av_log(s1, AV_LOG_ERROR, "Unable to split H263 packet, "
                           "use -mb_info %d or -ps 1.\n",
                           s->max_payload_size - 8);
                }
            }
        }

        if (size > 2 && !buf[0] && !buf[1])
            send_mode_a(s1, &info, buf, len, ebits, len == size);
        else
            send_mode_b(s1, &info, &packet_start_state, buf, len, sbits,
                        ebits, len == size);

        if (ebits) {
            sbits = 8 - ebits;
            len--;
        } else {
            sbits = 0;
        }
        buf  += len;
        size -= len;
        first = 0;
        ebits = 0;
    }
}
