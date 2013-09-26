/*
 * Intel MediaSDK QSV utility functions
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

#include <string.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "internal.h"
#include "avcodec.h"
#include "qsv.h"

int ff_qsv_error(int mfx_err)
{
    switch (mfx_err) {
    case MFX_ERR_NONE:
        return 0;
    case MFX_ERR_MEMORY_ALLOC:
    case MFX_ERR_NOT_ENOUGH_BUFFER:
        return AVERROR(ENOMEM);
    case MFX_ERR_INVALID_HANDLE:
        return AVERROR(EINVAL);
    case MFX_ERR_DEVICE_FAILED:
    case MFX_ERR_DEVICE_LOST:
    case MFX_ERR_LOCK_MEMORY:
        return AVERROR(EIO);
    case MFX_ERR_NULL_PTR:
    case MFX_ERR_UNDEFINED_BEHAVIOR:
    case MFX_ERR_NOT_INITIALIZED:
        return AVERROR_BUG;
    case MFX_ERR_UNSUPPORTED:
    case MFX_ERR_NOT_FOUND:
        return AVERROR(ENOSYS);
    case MFX_ERR_MORE_DATA:
    case MFX_ERR_MORE_SURFACE:
    case MFX_ERR_MORE_BITSTREAM:
        return AVERROR(EAGAIN);
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
    case MFX_ERR_INVALID_VIDEO_PARAM:
        return AVERROR(EINVAL);
    case MFX_ERR_ABORTED:
    case MFX_ERR_UNKNOWN:
    default:
        return AVERROR_UNKNOWN;
    }
}

int ff_qsv_codec_id_to_mfx(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return MFX_CODEC_AVC;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
        return MFX_CODEC_MPEG2;
    case AV_CODEC_ID_VC1:
        return MFX_CODEC_VC1;
    default:
        break;
    }

    return AVERROR(ENOSYS);
}

static void reset_timestamps(QSVContext *q)
{
    for (int i = 0; i < q->nb_timestamps; i++)
        q->timestamps[i].pts = q->timestamps[i].dts = AV_NOPTS_VALUE;
}

int ff_qsv_init(AVCodecContext *c, QSVContext *q)
{
    int ret;
    mfxIMPL impl             = MFX_IMPL_AUTO_ANY;
    mfxVersion ver           = { { QSV_VERSION_MINOR, QSV_VERSION_MAJOR } };
    mfxBitstream *bs         = &q->bs;
    mfxFrameAllocRequest req = { { 0 } };

    if ((ret = ff_qsv_codec_id_to_mfx(c->codec_id)) < 0)
        return ret;

    q->param.mfx.CodecId = ret;

    if ((ret = MFXInit(impl, &ver, &q->session)) < 0)
        return ff_qsv_error(ret);

    MFXQueryIMPL(q->session, &impl);

    if (impl & MFX_IMPL_SOFTWARE)
        av_log(c, AV_LOG_INFO,
               "Using Intel QuickSync decoder software implementation.\n");
    else if (impl & MFX_IMPL_HARDWARE)
        av_log(c, AV_LOG_INFO,
               "Using Intel QuickSync decoder hardware accelerated implementation.\n");
    else
        av_log(c, AV_LOG_INFO,
               "Unknown Intel QuickSync decoder implementation %d.\n", impl);

    q->param.IOPattern  = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    q->param.AsyncDepth = q->async_depth;

    if ((ret = MFXVideoDECODE_DecodeHeader(q->session, bs, &q->param)) < 0)
        return ff_qsv_error(ret);

    c->width         = q->param.mfx.FrameInfo.CropW;
    c->height        = q->param.mfx.FrameInfo.CropH;
    c->coded_width   = q->param.mfx.FrameInfo.Width;
    c->coded_height  = q->param.mfx.FrameInfo.Height;
    c->time_base.den = q->param.mfx.FrameInfo.FrameRateExtN;
    c->time_base.num = q->param.mfx.FrameInfo.FrameRateExtD / c->ticks_per_frame;

    if (!q->need_reinit)
        bs->DataLength = bs->DataOffset = 0;

    bs->DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;

    ret = MFXVideoDECODE_QueryIOSurf(q->session, &q->param, &req);
    if (ret < 0)
        return ff_qsv_error(ret);

    q->nb_timestamps = req.NumFrameSuggested + q->param.AsyncDepth;
    q->put_dts_cnt   = 0;
    q->decoded_cnt   = 0;
    q->last_ret      = MFX_ERR_MORE_DATA;

    if (!(q->timestamps = av_mallocz_array(q->nb_timestamps, sizeof(*q->timestamps))))
        return AVERROR(ENOMEM);

    reset_timestamps(q);

    if ((ret = MFXVideoDECODE_Init(q->session, &q->param)))
        ret = ff_qsv_error(ret);

    return ret;
}

static int bitstream_realloc(mfxBitstream *bs, int size)
{
    uint8_t *tmp;

    if (bs->MaxLength >= size)
        return 0;

    tmp = av_realloc(bs->Data, size);
    if (!tmp) {
        av_freep(&bs->Data);
        return AVERROR(ENOMEM);
    }

    bs->Data      = tmp;
    bs->MaxLength = size;

    return 0;
}

static int bitstream_enqueue(mfxBitstream *bs, uint8_t *data, int size)
{
    int bs_size = bs->DataLength + size;
    int ret;

    if ((ret = bitstream_realloc(bs, bs_size)) < 0)
        return ret;

    if (bs_size > bs->MaxLength - bs->DataOffset) {
        memmove(bs->Data, bs->Data + bs->DataOffset, bs->DataLength);
        bs->DataOffset = 0;
    }

    memcpy(bs->Data + bs->DataOffset + bs->DataLength, data, size);

    bs->DataLength += size;

    return 0;
}

static void free_surface_pool(QSVContext *q)
{
    QSVDecBufferPool **next = &q->buf_pool;
    QSVDecBufferPool *pool;

    while (*next) {
        pool = *next;
        *next = pool->next;
        av_frame_free((AVFrame **)&pool->buf.surface.Data.MemId);
        av_freep(&pool);
    }
}

static QSVDecBufferPool *alloc_surface(AVCodecContext *avctx, QSVContext *q)
{
    QSVDecBufferPool *pool = NULL;
    AVFrame *frame         = NULL;

    if (!(pool = av_mallocz(sizeof(*pool)))) {
        av_log(avctx, AV_LOG_ERROR, "av_mallocz() failed\n");
        goto fail;
    }
    if (!(frame = av_frame_alloc())) {
        av_log(avctx, AV_LOG_ERROR, "av_frame_alloc() failed\n");
        goto fail;
    }
    if (ff_get_buffer(avctx, frame, 0) < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        goto fail;
    }

    pool->buf.surface.Data.MemId = frame;
    pool->buf.surface.Data.Y     = frame->data[0];
    pool->buf.surface.Data.UV    = frame->data[1];
    pool->buf.surface.Data.Pitch = frame->linesize[0];
    pool->buf.surface.Info       = q->param.mfx.FrameInfo;

    return pool;

fail:
    av_freep(&pool);
    av_frame_free(&frame);

    return NULL;
}

static mfxFrameSurface1 *get_surface(AVCodecContext *avctx, QSVContext *q)
{
    QSVDecBufferPool **next = &q->buf_pool;
    QSVDecBufferPool *pool;
    AVFrame *frame;

    while (*next) {
        pool = *next;
        next = &pool->next;
        if (!pool->buf.surface.Data.Locked && !pool->buf.sync) {
            frame = pool->buf.surface.Data.MemId;
            if (!frame->data[0]) {
                if (ff_get_buffer(avctx, frame, 0) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
                    return NULL;
                }
                pool->buf.surface.Data.Y     = frame->data[0];
                pool->buf.surface.Data.UV    = frame->data[1];
                pool->buf.surface.Data.Pitch = frame->linesize[1];
                pool->buf.surface.Info       = q->param.mfx.FrameInfo;
            }
            return &pool->buf.surface;
        }
    }

    if (!(pool = alloc_surface(avctx, q)))
        return NULL;

    *next = pool;

    return &pool->buf.surface;
}

static void release_surface(QSVContext *q, mfxFrameSurface1 *surf)
{
    QSVDecBufferPool *pool = q->buf_pool;

    while (pool) {
        if (&pool->buf.surface == surf) {
            pool->buf.sync = 0;
            break;
        }
        pool = pool->next;
    }
}

static void put_sync(QSVContext *q, mfxFrameSurface1 *surf, mfxSyncPoint sync)
{
    QSVDecBufferPool *pool = q->buf_pool;

    while (pool) {
        if (&pool->buf.surface == surf) {
            QSVDecBuffer *buf = &pool->buf;

            buf->sync = sync;

            if (q->pending_sync_end)
                q->pending_sync_end->next = buf;
            else
                q->pending_sync = buf;

            q->pending_sync_end = buf;

            q->nb_sync++;

            break;
        }
        pool = pool->next;
    }
}

static void get_sync(QSVContext *q, mfxFrameSurface1 **surf, mfxSyncPoint *sync)
{
    QSVDecBuffer *buf = q->pending_sync;

    *surf = &buf->surface;
    *sync = buf->sync;

    q->pending_sync = buf->next;

    if (!q->pending_sync)
        q->pending_sync_end = NULL;

    q->nb_sync--;
}

static int realloc_timestamps(QSVContext *q, int old_nmemb, int new_nmemb)
{
    QSVTimeStamp *tmp = av_realloc_array(q->timestamps, new_nmemb, sizeof(*q->timestamps));
    if (!tmp)
        return AVERROR(ENOMEM);

    q->timestamps = tmp;
    q->nb_timestamps = new_nmemb;

    for (int i = old_nmemb; i < q->nb_timestamps; i++)
        q->timestamps[i].pts = q->timestamps[i].dts = AV_NOPTS_VALUE;

    return 0;
}

static int get_dts(QSVContext *q, int64_t pts, int64_t *dts)
{
    int i;

    if (pts == AV_NOPTS_VALUE) {
        *dts = AV_NOPTS_VALUE;
        return 0;
    }

    for (i = 0; i < q->nb_timestamps; i++) {
        if (q->timestamps[i].pts == pts)
            break;
    }
    if (i == q->nb_timestamps) {
        av_log(q, AV_LOG_ERROR,
               "Requested pts %"PRId64" does not match any dts\n",
               pts);
        return AVERROR_BUG;
    }
    *dts = q->timestamps[i].dts;

    q->timestamps[i].pts = AV_NOPTS_VALUE;

    return 0;
}

static int put_dts(QSVContext *q, int64_t pts, int64_t dts)
{
    int ret, i;

    if (!q->decoded_cnt && q->nb_timestamps == q->put_dts_cnt) {
        // For decoder delay
        if ((ret = realloc_timestamps(q, q->nb_timestamps, q->nb_timestamps * 2)) < 0)
            return ret;
    } else if (q->decoded_cnt == 1 && q->nb_timestamps < (q->put_dts_cnt + 32)) {
        // For frame reordering
        // I[31]P[30]B[29]B[28] ... B[1]B[0] (Number in [] is display order)
        if ((ret = realloc_timestamps(q, q->nb_timestamps, q->put_dts_cnt + 32)) < 0)
            return ret;
    }

    i = q->put_dts_cnt % q->nb_timestamps;
    q->timestamps[i].pts = pts;
    q->timestamps[i].dts = dts;
    q->put_dts_cnt++;

    return 0;
}

int ff_qsv_decode(AVCodecContext *avctx, QSVContext *q,
                  AVFrame *frame, int *got_frame,
                  AVPacket *avpkt)
{
    mfxFrameSurface1 *insurf;
    mfxFrameSurface1 *outsurf;
    mfxSyncPoint sync = 0;
    mfxBitstream *bs  = &q->bs;
    int size          = avpkt->size;
    int busymsec      = 0;
    int ret;

    *got_frame = 0;

    if (size)
        ff_packet_list_put(&q->pending, &q->pending_end, avpkt);

    // (2) Flush cached frames before reinit
    if (q->need_reinit)
        bs = NULL;

    ret = q->last_ret;
    do {
        if (ret == MFX_ERR_MORE_DATA) {
            if (!bs) {
                break;
            } else if (q->pending) {
                AVPacket pkt = { 0 };

                ff_packet_list_get(&q->pending, &q->pending_end, &pkt);

                if (!(ret = put_dts(q, pkt.pts, pkt.dts))) {
                    q->bs.TimeStamp = pkt.pts;
                    // QSV calculates TimeStamp of output based on the first
                    //  TimeStamp when q->bs.TimeStamp = MFX_TIMESTAMP_UNKNOWN
                    if (q->ts_by_qsv)
                        if (q->put_dts_cnt > 1 || pkt.pts == AV_NOPTS_VALUE)
                            q->bs.TimeStamp = MFX_TIMESTAMP_UNKNOWN;

                    ret = bitstream_enqueue(&q->bs, pkt.data, pkt.size);
                }

                av_packet_unref(&pkt);

                if (ret < 0)
                    return ret;
            } else if (!size) {
                // Flush cached frames when EOF
                bs = NULL;
            } else {
                break;
            }
        } else if (ret == MFX_WRN_VIDEO_PARAM_CHANGED) {
            // Detected new seaquence header has compatible video parameter
            // Automatically bitstream move forward next time
        } else if (ret == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM) {
            // Detected new seaquence header has incompatible video parameter
            if (bs) {
                // (1) Flush cached frames before reinit
                bs = NULL;
                q->need_reinit = 1;
            } else {
                return AVERROR_BUG;
            }
        }

        if (!(insurf = get_surface(avctx, q)))
            break;

        ret = MFXVideoDECODE_DecodeFrameAsync(q->session, bs,
                                              insurf, &outsurf, &sync);

        if (ret == MFX_WRN_DEVICE_BUSY) {
            if (busymsec > q->timeout) {
                av_log(avctx, AV_LOG_WARNING, "Timeout, device is so busy\n");
                return AVERROR(EIO);
            } else {
                av_usleep(1000);
                busymsec++;
            }
        } else {
            busymsec = 0;
        }
    } while (ret == MFX_ERR_MORE_SURFACE || ret == MFX_ERR_MORE_DATA ||
             ret == MFX_WRN_DEVICE_BUSY  || ret == MFX_WRN_VIDEO_PARAM_CHANGED ||
             ret == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);

    q->last_ret = ret;

    if (ret == MFX_ERR_MORE_DATA)
        ret = 0;

    if (sync)
        put_sync(q, outsurf, sync);

    if (q->pending_sync && !q->pending_sync->surface.Data.Locked &&
        (q->nb_sync >= q->async_depth || !size || q->need_reinit)) {
        int64_t pts, dts;
        get_sync(q, &outsurf, &sync);

        MFXVideoCORE_SyncOperation(q->session, sync, 60000);

        pts = outsurf->Data.TimeStamp;
        if (q->ts_by_qsv) {
            dts = pts;
        } else {
            if ((ret = get_dts(q, outsurf->Data.TimeStamp, &dts)) < 0)
                return ret;
        }

        av_frame_move_ref(frame, outsurf->Data.MemId);

        frame->pkt_pts = frame->pts = pts;
        frame->pkt_dts = dts;

        frame->repeat_pict =
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FRAME_TRIPLING ? 4 :
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FRAME_DOUBLING ? 2 :
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED ? 1 : 0;
        frame->top_field_first =
            !!(outsurf->Info.PicStruct & MFX_PICSTRUCT_FIELD_TFF);
        frame->interlaced_frame =
            !(outsurf->Info.PicStruct & MFX_PICSTRUCT_PROGRESSIVE);

        release_surface(q, outsurf);

        q->decoded_cnt++;
        *got_frame = 1;
    }

    if (ret < 0)
        return ff_qsv_error(ret);

    return size;
}

int ff_qsv_flush(QSVContext *q)
{
    int ret;

    if ((ret = MFXVideoDECODE_Reset(q->session, &q->param)));
        ret = ff_qsv_error(ret);

    q->bs.DataOffset = q->bs.DataLength = 0;

    free_surface_pool(q);

    reset_timestamps(q);

    ff_packet_list_free(&q->pending, &q->pending_end);

    return ret;
}

int ff_qsv_close(QSVContext *q)
{
    int ret = MFXClose(q->session);

    free_surface_pool(q);

    av_freep(&q->timestamps);

    ff_packet_list_free(&q->pending, &q->pending_end);

    return ff_qsv_error(ret);
}

int ff_qsv_reinit(AVCodecContext *avctx, QSVContext *q)
{
    int ret;

    MFXClose(q->session);

    free_surface_pool(q);

    av_freep(&q->timestamps);

    ret = ff_qsv_init(avctx, q);

    q->need_reinit = 0;

    return ret;
}
