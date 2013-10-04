/*
 * Intel MediaSDK QSV based MPEG-2 enccoder
 *
 * copyright (c) 2013 Yukinori Yamazoe
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


#include <stdint.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "avcodec.h"
#include "internal.h"
#include "mpegvideo.h"
#include "qsv.h"
#include "qsvenc.h"

typedef struct QSVMPEGEncContext {
    AVClass *class;
    QSVEncContext qsv;
} QSVMPEGEncContext;

static av_cold int qsv_enc_init(AVCodecContext *avctx)
{
    QSVMPEGEncContext *q = avctx->priv_data;

    return ff_qsv_enc_init(avctx, &q->qsv);
}

static int qsv_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                         const AVFrame *frame, int *got_packet)
{
    QSVMPEGEncContext *q = avctx->priv_data;

    return ff_qsv_enc_frame(avctx, &q->qsv, pkt, frame, got_packet);
}

static av_cold int qsv_enc_close(AVCodecContext *avctx)
{
    QSVMPEGEncContext *q = avctx->priv_data;

    return ff_qsv_enc_close(avctx, &q->qsv);
}

#define OFFSET(x) offsetof(QSVMPEGEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "async_depth", "Number which limits internal frame buffering", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VE },
    { "timeout", "Maximum timeout in milliseconds when the device has been busy", OFFSET(qsv.timeout), AV_OPT_TYPE_INT, { .i64 = TIMEOUT_DEFAULT }, 0, INT_MAX, VE },
    { "qpi", NULL, OFFSET(qsv.qpi), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 51, VE },
    { "qpp", NULL, OFFSET(qsv.qpp), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 51, VE },
    { "qpb", NULL, OFFSET(qsv.qpb), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 51, VE },
    { "profile", NULL, OFFSET(qsv.profile), AV_OPT_TYPE_INT, { .i64 = MFX_PROFILE_UNKNOWN }, 0, INT_MAX, VE, "profile" },
    { "unknown", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_UNKNOWN      }, INT_MIN, INT_MAX, VE, "profile" },
    { "simple" , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_MPEG2_SIMPLE }, INT_MIN, INT_MAX, VE, "profile" },
    { "main"   , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_MPEG2_MAIN   }, INT_MIN, INT_MAX, VE, "profile" },
    { "high"   , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_PROFILE_MPEG2_HIGH   }, INT_MIN, INT_MAX, VE, "profile" },
    { "level", NULL, OFFSET(qsv.level), AV_OPT_TYPE_INT, { .i64 = MFX_LEVEL_UNKNOWN }, 0, INT_MAX, VE, "level" },
    { "unknown" , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LEVEL_UNKNOWN        }, INT_MIN, INT_MAX, VE, "level" },
    { "low"     , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LEVEL_MPEG2_LOW      }, INT_MIN, INT_MAX, VE, "level" },
    { "main"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LEVEL_MPEG2_MAIN     }, INT_MIN, INT_MAX, VE, "level" },
    { "high1440", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LEVEL_MPEG2_HIGH1440 }, INT_MIN, INT_MAX, VE, "level" },
    { "high"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_LEVEL_MPEG2_HIGH     }, INT_MIN, INT_MAX, VE, "level" },
    { NULL },
};

static const AVClass class = {
    .class_name = "mpeg2_qsv encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault qsv_enc_defaults[] = {
    { "i_qfactor", "-0.96" },
    { "i_qoffset", "-1.0" },
    { "b_qfactor", "1.04" },
    { "b_qoffset", "1.0" },
    { "coder",     "-1" },
    { "b",         "0" },
    { "g",         "-1" },
    { "bf",        "-1" },
    { "refs",      "-1" },
    { "flags",     "+cgop" },
    { NULL },
};

AVCodec ff_mpeg2_qsv_encoder = {
    .name           = "mpeg2_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-2 video (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVMPEGEncContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MPEG2VIDEO,
    .init           = qsv_enc_init,
    .encode2        = qsv_enc_frame,
    .close          = qsv_enc_close,
    .capabilities   = CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_NV12, AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = qsv_enc_defaults,
};
