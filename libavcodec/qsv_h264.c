/*
 * Intel MediaSDK QSV based H.264 decoder
 *
 * copyright (c) 2013 Luca Barbato
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
#include "h264.h"
#include "qsv.h"

typedef struct QSVH264Context {
    AVClass *class;
    QSVContext qsv;
    AVBitStreamFilterContext *bsf;
} QSVH264Context;

static const uint8_t fake_idr[] = { 0x00, 0x00, 0x01, 0x65 };

static av_cold int qsv_dec_init(AVCodecContext *avctx)
{
    QSVH264Context *q        = avctx->priv_data;
    mfxBitstream *bs         = &q->qsv.bs;
    int ret;

    avctx->pix_fmt = AV_PIX_FMT_NV12;

    if (!(q->bsf = av_bitstream_filter_init("h264_mp4toannexb")))
        return AVERROR(ENOMEM);

    // Data and DataLength passed as dummy pointers
    av_bitstream_filter_filter(q->bsf, avctx, NULL,
                               &bs->Data, &bs->DataLength,
                               NULL, 0, 0);

    //FIXME feed it a fake IDR directly
    bs->Data = av_malloc(avctx->extradata_size + sizeof(fake_idr));
    bs->DataLength = avctx->extradata_size;

    memcpy(bs->Data, avctx->extradata, avctx->extradata_size);
    memcpy(bs->Data + bs->DataLength, fake_idr, sizeof(fake_idr));

    bs->DataLength += sizeof(fake_idr);

    bs->MaxLength = bs->DataLength;

    ret = ff_qsv_init(avctx, &q->qsv);
    if (ret < 0) {
        av_freep(&bs->Data);
        av_bitstream_filter_close(q->bsf);
    }

    return ret;
}

static int qsv_dec_frame(AVCodecContext *avctx, void *data,
                         int *got_frame, AVPacket *avpkt)
{
    QSVH264Context *q = avctx->priv_data;
    AVFrame *frame    = data;
    uint8_t *p;
    int size;
    int ret;

    av_bitstream_filter_filter(q->bsf, avctx, NULL,
                               &p, &size,
                               avpkt->data, avpkt->size, 0);

    if (p != avpkt->data) {
        AVPacket pkt = { 0 };

        av_packet_copy_props(&pkt, avpkt);

        pkt.data = p;
        pkt.size = size;

        ret = ff_qsv_decode(avctx, &q->qsv, frame, got_frame, &pkt);

        av_free(p);
    } else
        ret = ff_qsv_decode(avctx, &q->qsv, frame, got_frame, avpkt);

    return ret;
}

static int qsv_dec_close (AVCodecContext *avctx)
{
    QSVH264Context *q = avctx->priv_data;
    int ret = ff_qsv_close(&q->qsv);

    av_bitstream_filter_close(q->bsf);
    av_freep(&q->qsv.bs.Data);
    return ret;
}

static void qsv_dec_flush(AVCodecContext *avctx)
{
    QSVH264Context *q = avctx->priv_data;

    ff_qsv_flush(&q->qsv);
}

#define OFFSET(x) offsetof(QSVH264Context, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "async_depth", "Internal parallelization depth, the higher the value the higher the latency.", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VD },
    { NULL },
};

static const AVClass class = {
    .class_name = "h264_qsv",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h264_qsv_decoder = {
    .name           = "h264_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVH264Context),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .init           = qsv_dec_init,
    .decode         = qsv_dec_frame,
    .flush          = qsv_dec_flush,
    .close          = qsv_dec_close,
    .capabilities   = CODEC_CAP_DELAY | CODEC_CAP_PKT_TS,
    .priv_class     = &class,
};
