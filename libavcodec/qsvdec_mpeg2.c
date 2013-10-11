/*
 * Intel MediaSDK QSV based MPEG-2 decoder
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
#include "qsvdec.h"

typedef struct QSVDecMpegContext {
    AVClass *class;
    QSVDecOptions options;
    QSVDecContext *qsv;
} QSVDecMpegContext;

static const uint8_t fake_ipic[] = { 0x00, 0x00, 0x01, 0x00, 0x00, 0x0F, 0xFF, 0xF8 };

static av_cold int qsv_dec_init(AVCodecContext *avctx)
{
    QSVDecMpegContext *q = avctx->priv_data;
    int ret              = AVERROR(ENOMEM);
    mfxBitstream bs;

    avctx->pix_fmt = AV_PIX_FMT_NV12;

    memset(&bs, 0, sizeof(bs));

    if (!(q->qsv = av_mallocz(sizeof(*q->qsv))))
        goto fail;

    q->qsv->options   = q->options;
    q->qsv->ts_by_qsv = 1;

    //FIXME feed it a fake I-picture directly
    if (!(bs.Data = av_malloc(avctx->extradata_size + sizeof(fake_ipic))))
        goto fail;

    memcpy(bs.Data, avctx->extradata, avctx->extradata_size);
    bs.DataLength += avctx->extradata_size;
    memcpy(bs.Data + bs.DataLength, fake_ipic, sizeof(fake_ipic));
    bs.DataLength += sizeof(fake_ipic);
    bs.MaxLength = bs.DataLength;

    q->qsv->bs = &bs;

    ret = ff_qsv_dec_init(avctx, q->qsv);
    if (ret < 0)
        goto fail;

    q->qsv->bs = NULL;

    av_freep(&bs.Data);

    return ret;

fail:
    av_freep(&q->qsv);
    av_freep(&bs.Data);

    return ret;
}

static int qsv_dec_frame(AVCodecContext *avctx, void *data,
                         int *got_frame, AVPacket *avpkt)
{
    QSVDecMpegContext *q = avctx->priv_data;
    AVFrame *frame       = data;
    int ret;

    // Reinit so finished flushing old video parameter cached frames
    if (q->qsv->need_reinit && q->qsv->last_ret == MFX_ERR_MORE_DATA &&
        !q->qsv->nb_sync) {
        ret = ff_qsv_dec_reinit(avctx, q->qsv);
        if (ret < 0)
            return ret;
    }

    return ff_qsv_dec_frame(avctx, q->qsv, frame, got_frame, avpkt);
}

static int qsv_dec_close(AVCodecContext *avctx)
{
    QSVDecMpegContext *q = avctx->priv_data;
    int ret              = 0;

    if (!avctx->internal->is_copy) {
        ret = ff_qsv_dec_close(q->qsv);
        av_freep(&q->qsv);
    }

    return ret;
}

static void qsv_dec_flush(AVCodecContext *avctx)
{
    QSVDecMpegContext *q = avctx->priv_data;

    ff_qsv_dec_flush(q->qsv);
}

#define OFFSET(x) offsetof(QSVDecMpegContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "async_depth", "Number which limits internal frame buffering", OFFSET(options.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VD },
    { "timeout", "Maximum timeout in milliseconds when the device has been busy", OFFSET(options.timeout), AV_OPT_TYPE_INT, { .i64 = TIMEOUT_DEFAULT }, 0, INT_MAX, VD },
    { NULL },
};

static const AVClass class = {
    .class_name = "mpeg2_qsv decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_mpeg2_qsv_decoder = {
    .name           = "mpeg2_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-2 video (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVDecMpegContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_MPEG2VIDEO,
    .init           = qsv_dec_init,
    .decode         = qsv_dec_frame,
    .flush          = qsv_dec_flush,
    .close          = qsv_dec_close,
    .capabilities   = CODEC_CAP_DELAY | CODEC_CAP_PKT_TS | CODEC_CAP_DR1 | CODEC_CAP_FRAME_THREADS,
    .priv_class     = &class,
};
