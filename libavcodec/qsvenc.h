/*
 * Intel MediaSDK QSV encoder utility functions
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

#ifndef AVCODEC_QSVENC_H
#define AVCODEC_QSVENC_H

#include <stdint.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/avutil.h"


typedef struct QSVFrameList {
    AVFrame *frame;
    struct QSVFrameList *next;
} QSVFrameList;

typedef struct QSVEncBuffer {
    uint8_t *data;
    mfxBitstream bs;
    mfxSyncPoint sync;
    int64_t dts;
    struct QSVEncBuffer *prev;
    struct QSVEncBuffer *next;
} QSVEncBuffer;

typedef struct QSVEncBufferPool {
    QSVEncBuffer buf;
    struct QSVEncBufferPool *next;
} QSVEncBufferPool;

typedef struct QSVEncContext {
    AVClass *class;
    mfxSession session;
    mfxVideoParam param;
    mfxExtCodingOption extco;
    mfxExtCodingOption2 extco2;
    mfxExtCodingOptionSPSPPS extcospspps;
    mfxExtBuffer *extparam[3];
    uint8_t spspps[2][256];
    int64_t first_pts;
    int64_t pts_delay;
    int async_depth;
    int timeout;
    QSVFrameList *pending, *pending_end;
    QSVSurfaceList *surflist;
    QSVEncBufferPool *buf_pool;
    QSVEncBuffer *pending_sync, *pending_sync_end;
    int nb_sync;
    QSVEncBuffer *pending_dts, *pending_dts_end;
    int nb_dts;
} QSVEncContext;

int ff_qsv_enc_init(AVCodecContext *avctx, QSVEncContext *q);

int ff_qsv_enc_frame(AVCodecContext *avctx, QSVEncContext *q,
                     AVPacket *pkt, const AVFrame *frame, int *got_packet);

int ff_qsv_enc_close(AVCodecContext *avctx, QSVEncContext *q);

#endif /* AVCODEC_QSVENC_H */
