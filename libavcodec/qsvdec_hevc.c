/*
 * Intel MediaSDK QSV based HEVC decoder
 *
 * copyright (c) 2014 Yukinori Yamazoe
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

#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "internal.h"
#include "qsv.h"
#include "qsvdec.h"

typedef struct QSVDecHevcContext {
    AVClass *class;
    QSVDecContext qsv;
} QSVDecHevcContext;

static av_cold int qsv_dec_init(AVCodecContext *avctx)
{
    QSVDecHevcContext *q = avctx->priv_data;
    mfxBitstream *bs     = &q->qsv.bs;
    uint8_t *tmp         = NULL;
    int ret;

    avctx->pix_fmt = AV_PIX_FMT_NV12;

    tmp = av_malloc(avctx->extradata_size);
    if (!tmp)
        return AVERROR(ENOMEM);
    bs->Data       = tmp;
    bs->DataLength = avctx->extradata_size;
    bs->MaxLength  = avctx->extradata_size;
    memcpy(bs->Data, avctx->extradata, avctx->extradata_size);

    ret = ff_qsv_dec_init(avctx, &q->qsv);
    if (ret < 0) {
        av_freep(&bs->Data);
    }

    return ret;
}

static int qsv_dec_frame(AVCodecContext *avctx, void *data,
                         int *got_frame, AVPacket *avpkt)
{
    QSVDecHevcContext *q = avctx->priv_data;
    AVFrame *frame       = data;

    return ff_qsv_dec_frame(avctx, &q->qsv, frame, got_frame, avpkt);
}

static int qsv_dec_close(AVCodecContext *avctx)
{
    QSVDecHevcContext *q = avctx->priv_data;
    int ret              = ff_qsv_dec_close(&q->qsv);

    av_freep(&q->qsv.bs.Data);

    return ret;
}

static void qsv_dec_flush(AVCodecContext *avctx)
{
    QSVDecHevcContext *q = avctx->priv_data;

    ff_qsv_dec_flush(&q->qsv);
}

#define OFFSET(x) offsetof(QSVDecHevcContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "async_depth", "Number which limits internal frame buffering", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VD },
    { "timeout", "Maximum timeout in milliseconds when the device has been busy", OFFSET(qsv.timeout), AV_OPT_TYPE_INT, { .i64 = TIMEOUT_DEFAULT }, 0, INT_MAX, VD },
    { NULL },
};

static const AVClass class = {
    .class_name = "hevc_qsv",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_qsv_decoder = {
    .name           = "hevc_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("HEVC (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVDecHevcContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = qsv_dec_init,
    .decode         = qsv_dec_frame,
    .flush          = qsv_dec_flush,
    .close          = qsv_dec_close,
    .capabilities   = CODEC_CAP_DELAY | CODEC_CAP_PKT_TS | CODEC_CAP_DR1,
    .priv_class     = &class,
};
