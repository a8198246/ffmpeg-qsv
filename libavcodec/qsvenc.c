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

#include <string.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "libavutil/imgutils.h"
#include "internal.h"
#include "avcodec.h"
#include "qsv.h"
#include "qsvenc.h"

static int init_video_param(AVCodecContext *avctx, QSVEncContext *q)
{
    float quant;
    int ret;

    if ((ret = ff_qsv_codec_id_to_mfx(avctx->codec_id)) < 0)
        return ret;
    q->param.mfx.CodecId            = ret;
  //q->param.mfx.CodecProfile       = 0;
  //q->param.mfx.CodecLevel         = 0;
    q->param.mfx.TargetUsage        = MFX_TARGETUSAGE_BALANCED;
    q->param.mfx.GopPicSize         = avctx->gop_size < 0 ? 0 : avctx->gop_size;
    q->param.mfx.GopRefDist         = av_clip(avctx->max_b_frames, -1, 16) + 1;
    q->param.mfx.GopOptFlag         = MFX_GOP_CLOSED; // 0:open-gop
    q->param.mfx.IdrInterval        = 0;
    q->param.mfx.NumSlice           = avctx->slices;
    q->param.mfx.NumRefFrame        = avctx->refs < 0 ? 0 : avctx->refs;
    q->param.mfx.EncodedOrder       = 0;
    q->param.mfx.BufferSizeInKB     = 0; // MaxKbps/8;
  //q->param.mfx.TimeStampCalc      = 0; // API 1.3
  //q->param.mfx.ExtendedPicStruct  = 0; // API 1.3
  //q->param.mfx.BRCParamMultiplier = 0; // API 1.3
  //q->param.mfx.SliceGroupsPresent = 0; // API 1.6
    q->param.mfx.RateControlMethod = 
        avctx->flags & CODEC_FLAG_QSCALE ?      MFX_RATECONTROL_CQP :
        avctx->rc_max_rate &&
        avctx->rc_max_rate == avctx->bit_rate ? MFX_RATECONTROL_CBR :
                                                MFX_RATECONTROL_VBR;

    if (ret == MFX_CODEC_AVC)
        av_log(avctx, AV_LOG_INFO, "Codec:AVC\n");
    else if (ret == MFX_CODEC_MPEG2)
        av_log(avctx, AV_LOG_INFO, "Codec:MPEG2\n");
    if (q->param.mfx.GopPicSize)
        av_log(avctx, AV_LOG_INFO, "GopPicSize:%d\n", q->param.mfx.GopPicSize);
    if (q->param.mfx.GopRefDist)
        av_log(avctx, AV_LOG_INFO, "GopRefDist:%d\n", q->param.mfx.GopRefDist);
    if (q->param.mfx.NumSlice)
        av_log(avctx, AV_LOG_INFO, "NumSlice:%d\n", q->param.mfx.NumSlice);
    if (q->param.mfx.NumRefFrame)
        av_log(avctx, AV_LOG_INFO, "NumRefFrame:%d\n", q->param.mfx.NumRefFrame);

    switch (q->param.mfx.RateControlMethod) {
    case MFX_RATECONTROL_CBR: // API 1.0
        av_log(avctx, AV_LOG_INFO, "RateControlMethod:CBR\n");
      //q->param.mfx.InitialDelayInKB;
        q->param.mfx.TargetKbps = avctx->bit_rate / 1000;
        q->param.mfx.MaxKbps    = avctx->bit_rate / 1000;
        av_log(avctx, AV_LOG_INFO, "TargetKbps:%d\n", q->param.mfx.TargetKbps);
        break;
    case MFX_RATECONTROL_VBR: // API 1.0
        av_log(avctx, AV_LOG_INFO, "RateControlMethod:VBR\n");
      //q->param.mfx.InitialDelayInKB;
        q->param.mfx.TargetKbps = avctx->bit_rate / 1000; // >1072
        q->param.mfx.MaxKbps    = avctx->rc_max_rate / 1000;
        av_log(avctx, AV_LOG_INFO, "TargetKbps:%d\n", q->param.mfx.TargetKbps);
        if (q->param.mfx.MaxKbps)
            av_log(avctx, AV_LOG_INFO, "MaxKbps:%d\n", q->param.mfx.MaxKbps);
        break;
    case MFX_RATECONTROL_CQP: // API 1.1
        av_log(avctx, AV_LOG_INFO, "RateControlMethod:CQP\n");
        quant = avctx->global_quality / FF_QP2LAMBDA;
        if (avctx->i_quant_factor)
            quant *= fabs(avctx->i_quant_factor);
        quant += avctx->i_quant_offset;
        q->param.mfx.QPI = av_clip(quant, 0, 51);

        quant = avctx->global_quality / FF_QP2LAMBDA;
        q->param.mfx.QPP = av_clip(quant, 0, 51);

        quant = avctx->global_quality / FF_QP2LAMBDA;
        if (avctx->b_quant_factor)
            quant *= fabs(avctx->b_quant_factor);
        quant += avctx->b_quant_offset;
        q->param.mfx.QPB = av_clip(quant, 0, 51);

        av_log(avctx, AV_LOG_INFO, "QPI:%d, QPP:%d, QPB:%d\n",
               q->param.mfx.QPI, q->param.mfx.QPP, q->param.mfx.QPB);
        break;
    case MFX_RATECONTROL_AVBR: // API 1.3
        av_log(avctx, AV_LOG_ERROR,
               "RateControlMethod:AVBR is unimplemented.\n");
        /*
        q->param.mfx.TargetKbps;
        q->param.mfx.Accuracy;    // API 1.3
        q->param.mfx.Convergence; // API 1.3
        */
        return AVERROR(EINVAL);
    case MFX_RATECONTROL_LA: // API 1.7
        av_log(avctx, AV_LOG_ERROR,
               "RateControlMethod:LA is unimplemented.\n");
        /*
        q->param.mfx.InitialDelayInKB;
        q->param.mfx.TargetKbps;
        q->param.mfx.MaxKbps;
        q->extco2.LookAheadDepth; // API 1.7
        q->extco2.Trellis;        // API 1.6
        */
        return AVERROR(EINVAL);
    default:
        av_log(avctx, AV_LOG_ERROR,
               "RateControlMethod:%d is undefined.\n",
               q->param.mfx.RateControlMethod);
        return AVERROR(EINVAL);
    }

    q->param.mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
    q->param.mfx.FrameInfo.Width         = FFALIGN(avctx->width, 16);
    q->param.mfx.FrameInfo.Height        = FFALIGN(avctx->height, 32);
    q->param.mfx.FrameInfo.CropX         = 0;
    q->param.mfx.FrameInfo.CropY         = 0;
    q->param.mfx.FrameInfo.CropW         = avctx->width;
    q->param.mfx.FrameInfo.CropH         = avctx->height;
    q->param.mfx.FrameInfo.FrameRateExtN = avctx->time_base.den;
    q->param.mfx.FrameInfo.FrameRateExtD = avctx->time_base.num;
    q->param.mfx.FrameInfo.AspectRatioW  = avctx->sample_aspect_ratio.num;
    q->param.mfx.FrameInfo.AspectRatioH  = avctx->sample_aspect_ratio.den;
    q->param.mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_UNKNOWN;
    q->param.mfx.FrameInfo.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;

    av_log(avctx, AV_LOG_INFO, "FrameRate:%d/%d\n",
           q->param.mfx.FrameInfo.FrameRateExtN,
           q->param.mfx.FrameInfo.FrameRateExtD);

    q->extco.Header.BufferId      = MFX_EXTBUFF_CODING_OPTION;
    q->extco.Header.BufferSz      = sizeof(q->extco);
    q->extco.RateDistortionOpt    = MFX_CODINGOPTION_UNKNOWN;
    q->extco.EndOfSequence        = MFX_CODINGOPTION_UNKNOWN;
    q->extco.CAVLC                = avctx->coder_type == FF_CODER_TYPE_VLC ?
                                    MFX_CODINGOPTION_ON :
                                    MFX_CODINGOPTION_UNKNOWN;
  //q->extco.NalHrdConformance    = MFX_CODINGOPTION_UNKNOWN; // API 1.3
  //q->extco.SingleSeiNalUnit     = MFX_CODINGOPTION_UNKNOWN; // API 1.3
  //q->extco.VuiVclHrdParameters  = MFX_CODINGOPTION_UNKNOWN; // API 1.3
    q->extco.ResetRefList         = MFX_CODINGOPTION_UNKNOWN;
  //q->extco.RefPicMarkRep        = MFX_CODINGOPTION_UNKNOWN; // API 1.3
  //q->extco.FieldOutput          = MFX_CODINGOPTION_UNKNOWN; // API 1.3
  //q->extco.ViewOutput           = MFX_CODINGOPTION_UNKNOWN; // API 1.4
    q->extco.MaxDecFrameBuffering = MFX_CODINGOPTION_UNKNOWN;
    q->extco.AUDelimiter          = MFX_CODINGOPTION_UNKNOWN; // or OFF
    q->extco.EndOfStream          = MFX_CODINGOPTION_UNKNOWN;
    q->extco.PicTimingSEI         = MFX_CODINGOPTION_UNKNOWN; // or OFF
    q->extco.VuiNalHrdParameters  = MFX_CODINGOPTION_UNKNOWN;
    q->extco.FramePicture         = MFX_CODINGOPTION_ON;
  //q->extco.RecoveryPointSEI     = MFX_CODINGOPTION_UNKNOWN; // API 1.6

    if (q->extco.CAVLC == MFX_CODINGOPTION_ON)
        av_log(avctx, AV_LOG_INFO, "CAVLC:ON\n");

    q->extparam[q->param.NumExtParam] = (mfxExtBuffer *)&q->extco;
    q->param.ExtParam = q->extparam;
    q->param.NumExtParam++;

/*
    q->extcospspps.Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
    q->extcospspps.Header.BufferSz = sizeof(q->extcospspps);
    q->extcospspps.SPSBuffer       = q->spspps[0];
    q->extcospspps.SPSBufSize      = sizeof(q->spspps[0]);
    q->extcospspps.PPSBuffer       = q->spspps[1];
    q->extcospspps.PPSBufSize      = sizeof(q->spspps[1]);

    q->extparam[q->param.NumExtParam] = (mfxExtBuffer *)&q->extcospspps;
    q->param.ExtParam = q->extparam;
    q->param.NumExtParam++;
*/

    return 0;
}

int ff_qsv_enc_init(AVCodecContext *avctx, QSVEncContext *q)
{
    mfxIMPL impl             = MFX_IMPL_AUTO_ANY;
    mfxVersion ver           = { { QSV_VERSION_MINOR, QSV_VERSION_MAJOR } };
    mfxFrameAllocRequest req = { { 0 } };
    int ret;

    if ((ret = MFXInit(impl, &ver, &q->session)) < 0) {
        av_log(avctx, AV_LOG_DEBUG, "MFXInit():%d\n", ret);
        return ff_qsv_error(ret);
    }

    MFXQueryIMPL(q->session, &impl);

    if (impl & MFX_IMPL_SOFTWARE)
        av_log(avctx, AV_LOG_INFO,
               "Using Intel QuickSync encoder software implementation.\n");
    else if (impl & MFX_IMPL_HARDWARE)
        av_log(avctx, AV_LOG_INFO,
               "Using Intel QuickSync encoder hardware accelerated implementation.\n");
    else
        av_log(avctx, AV_LOG_INFO,
               "Unknown Intel QuickSync encoder implementation %d.\n", impl);

    q->param.IOPattern  = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    q->param.AsyncDepth = q->async_depth;

    if ((ret = init_video_param(avctx, q)) < 0)
        return ret;

    if ((ret = MFXVideoENCODE_QueryIOSurf(q->session, &q->param, &req) < 0)) {
        av_log(avctx, AV_LOG_DEBUG, "MFXVideoENCODE_QueryIOSurf():%d\n", ret);
        return ff_qsv_error(ret);
    }

    if (ret = MFXVideoENCODE_Init(q->session, &q->param)) {
        av_log(avctx, AV_LOG_DEBUG, "MFXVideoENCODE_Init():%d\n", ret);
        return ff_qsv_error(ret);
    }

    // for q->param.mfx.BufferSizeInKB
    MFXVideoENCODE_GetVideoParam(q->session, &q->param);

    /*
    av_log(q, AV_LOG_DEBUG, "sps");
    for (int i = 0; i < 64; i+=4)
        av_log(q, AV_LOG_DEBUG, ":%02x%02x%02x%02x", q->spspps[0][i], q->spspps[0][i+1], q->spspps[0][i+2], q->spspps[0][i+3]);
    av_log(q, AV_LOG_DEBUG, "\n");

    av_log(q, AV_LOG_DEBUG, "pps");
    for (int j = 0; j < 64; j+=4)
        av_log(q, AV_LOG_DEBUG, ":%02x%02x%02x%02x", q->spspps[1][j], q->spspps[1][j+1], q->spspps[1][j+2], q->spspps[1][j+3]);
    av_log(q, AV_LOG_DEBUG, "\n");
    */

    /*
    avctx->extradata = av_malloc(sizeof(q->spspps));
    memcpy(avctx->extradata, q->spspps, sizeof(q->spspps));
    avctx->extradata_size = sizeof(q->spspps);
    */

    q->first_pts = AV_NOPTS_VALUE;
    q->pts_delay = AV_NOPTS_VALUE;

    return ret;
}

static int put_frame(QSVEncContext *q, AVFrame *frame)
{
    QSVFrameList *list = av_mallocz(sizeof(*list));
    if (!list)
        return AVERROR(ENOMEM);

    list->frame = frame;

    if (q->pending_end)
        q->pending_end->next = list;
    else
        q->pending = list;

    q->pending_end = list;

    return 0;
}

static AVFrame *get_frame(QSVEncContext *q)
{
    QSVFrameList *list = q->pending;
    AVFrame *frame     = list->frame;

    q->pending = list->next;

    if (!q->pending)
        q->pending_end = NULL;

    av_free(list);

    return frame;
}

static void free_frame_list(QSVEncContext *q)
{
    QSVFrameList **next = &q->pending;
    QSVFrameList *list;

    while (*next) {
        list = *next;
        *next = list->next;
        av_frame_free(&list->frame);
        av_freep(&list);
    }
}

static int align_frame(AVCodecContext *avctx, AVFrame *frame)
{
    int width  = frame->linesize[0];
    int height = FFALIGN(frame->height, 32);
    int size   = width * height;
    int ret;
    AVFrame tmp;

    // check AVFrame buffer align for QSV
    if (!(width % 16) && frame->buf[0] && (frame->buf[0]->size >= size))
        return 0;

    av_frame_move_ref(&tmp, frame);

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
        av_frame_unref(&tmp);
        return ret;
    }

    if ((ret = av_frame_copy_props(frame, &tmp)) < 0) {
        av_frame_unref(&tmp);
        return ret;
    }

    av_image_copy(frame->data, frame->linesize, tmp.data, tmp.linesize,
                  frame->format, frame->width, frame->height);

    av_frame_unref(&tmp);

    return 0;
}

static mfxFrameSurface1 *get_surface(QSVEncContext *q)
{
    QSVSurfaceList **next = &q->surflist;
    QSVSurfaceList *list;

    while (*next) {
        list = *next;
        next = &list->next;
        if (!list->surface.Data.Locked) {
            av_frame_free((AVFrame **)(&list->surface.Data.MemId));
            return &list->surface;
        }
    }

    if (!(list = av_mallocz(sizeof(*list)))) {
        av_log(q, AV_LOG_ERROR, "av_mallocz() failed\n");
        return NULL;
    }

    *next = list;

    return &list->surface;
}

static void free_surface_list(QSVEncContext *q)
{
    QSVSurfaceList **next = &q->surflist;
    QSVSurfaceList *list;

    while (*next) {
        list = *next;
        *next = list->next;
        av_frame_free((AVFrame **)(&list->surface.Data.MemId));
        av_freep(&list);
    }
}

static void set_surface_param(QSVEncContext *q, mfxFrameSurface1 *surf,
                              AVFrame *frame)
{
    surf->Info = q->param.mfx.FrameInfo;

    surf->Info.PicStruct = 
        !frame->interlaced_frame ? MFX_PICSTRUCT_PROGRESSIVE :
        frame->top_field_first   ? MFX_PICSTRUCT_FIELD_TFF :
                                   MFX_PICSTRUCT_FIELD_BFF;
    if (frame->repeat_pict == 1)
        surf->Info.PicStruct |= MFX_PICSTRUCT_FIELD_REPEATED;
    else if (frame->repeat_pict == 2)
        surf->Info.PicStruct |= MFX_PICSTRUCT_FRAME_DOUBLING;
    else if (frame->repeat_pict == 4)
        surf->Info.PicStruct |= MFX_PICSTRUCT_FRAME_TRIPLING;

    surf->Data.MemId     = frame;
    surf->Data.Y         = frame->data[0];
    surf->Data.UV        = frame->data[1];
    surf->Data.Pitch     = frame->linesize[0];
    surf->Data.TimeStamp = frame->pts;
}

static QSVBitstreamList *alloc_bitstream_list_entry(QSVEncContext *q)
{
    QSVBitstreamList *list = NULL;
    uint8_t *data          = NULL;
    int size               = q->param.mfx.BufferSizeInKB * 1000;

    if (!(list = av_mallocz(sizeof(*list)))) {
        av_log(q, AV_LOG_ERROR, "av_mallocz() failed\n");
        goto fail;
    }
    if (!(data = av_mallocz(size))) {
        av_log(q, AV_LOG_ERROR, "av_mallocz() failed\n");
        goto fail;
    }
    list->bs.Data      = data;
    list->bs.MaxLength = size;

    return list;

fail:
    av_freep(&list);
    av_freep(&data);

    return NULL;
}

static mfxBitstream *get_bitstream(QSVEncContext *q)
{
    QSVBitstreamList **next = &q->bslist;
    QSVBitstreamList *list;

    while (*next) {
        list = *next;
        next = &list->next;
        if (!list->locked) {
            list->locked = 1;
            return &list->bs;
        }
    }

    if (!(list = alloc_bitstream_list_entry(q))) {
        av_log(q, AV_LOG_ERROR, "No bitstream!\n");
        return NULL;
    }

    list->locked = 1;
    *next = list;

    return &list->bs;
}

static void release_bitstream(QSVEncContext *q, mfxBitstream *bs)
{
    QSVBitstreamList **next = &q->bslist;
    QSVBitstreamList *list;

    while (*next) {
        list = *next;
        next = &list->next;
        if (&list->bs == bs) {
            list->locked = 0;
            list->bs.DataOffset = 0;
            list->bs.DataLength = 0;
            break;
        }
    }
}

static void free_bitstream_list(QSVEncContext *q)
{
    QSVBitstreamList **next = &q->bslist;
    QSVBitstreamList *list;

    while (*next) {
        list = *next;
        *next = list->next;
        av_freep(&list->bs.Data);
        av_freep(&list);
    }
}

static int put_encoded_data(QSVEncContext *q, mfxBitstream *bs, int64_t dts)
{
    QSVEncodedDataList *list = av_mallocz(sizeof(*list));
    if (!list)
        return AVERROR(ENOMEM);

    list->bs   = bs;
    list->dts  = dts;
    list->prev = q->edlist_tail;

    if (q->edlist_tail)
        q->edlist_tail->next = list;
    else
        q->edlist_head = list;

    q->edlist_tail = list;

    return 0;
}

static void get_encoded_data(QSVEncContext *q, mfxBitstream **bs, int64_t *dts)
{
    QSVEncodedDataList *list = q->edlist_head;

    *bs  = list->bs;
    *dts = list->dts;

    q->edlist_head = list->next;

    if (q->edlist_head)
        q->edlist_head->prev = NULL;
    else
        q->edlist_tail = NULL;

    av_free(list);
}

static void free_encoded_data_list(QSVEncContext *q)
{
    QSVEncodedDataList **next = &q->edlist_head;
    QSVEncodedDataList *list;

    while (*next) {
        list = *next;
        *next = list->next;
        av_freep(&list);
    }
}

static void fill_encoded_data_dts(QSVEncContext *q, int64_t base_dts)
{
    QSVEncodedDataList *list = q->edlist_tail;
    int cnt                  = 1;

    while (list) {
        if (list->dts == AV_NOPTS_VALUE)
            list->dts = base_dts - (q->pts_delay * cnt);
        else
            break;
        list = list->prev;
        cnt++;
    }
}

static void print_frametype(AVCodecContext *avctx, QSVEncContext *q,
                            mfxBitstream *bs, int indent)
{
    if (av_log_get_level() >= AV_LOG_DEBUG) {
        char buf[1024];

        buf[0] = '\0';

        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
                 "TimeStamp:%"PRId64", ", bs->TimeStamp);
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "FrameType:");

        if (bs->FrameType & MFX_FRAMETYPE_I)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " I");
        if (bs->FrameType & MFX_FRAMETYPE_P)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " P");
        if (bs->FrameType & MFX_FRAMETYPE_B)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " B");
        if (bs->FrameType & MFX_FRAMETYPE_S)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " S");
        if (bs->FrameType & MFX_FRAMETYPE_REF)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " REF");
        if (bs->FrameType & MFX_FRAMETYPE_IDR)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " IDR");
        if (bs->FrameType & MFX_FRAMETYPE_xI)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " xI");
        if (bs->FrameType & MFX_FRAMETYPE_xP)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " xP");
        if (bs->FrameType & MFX_FRAMETYPE_xB)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " xB");
        if (bs->FrameType & MFX_FRAMETYPE_xS)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " xS");
        if (bs->FrameType & MFX_FRAMETYPE_xREF)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " xREF");
        if (bs->FrameType & MFX_FRAMETYPE_xIDR)
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " xIDR");

        av_log(q, AV_LOG_DEBUG, "%*s%s\n", 4 * indent, "", buf);
    }
}

int ff_qsv_enc_frame(AVCodecContext *avctx, QSVEncContext *q,
                     AVPacket *pkt, const AVFrame *frame, int *got_packet)
{
    mfxFrameSurface1 *surf = NULL;
    mfxBitstream *bs       = NULL;
    mfxSyncPoint sync      = 0;
    int busymsec           = 0;
    int ret;
    int64_t dts;
    AVFrame *tmp;

    *got_packet = 0;

    if (frame) {
        av_log(q, AV_LOG_DEBUG, "frame->pts:%"PRId64"\n", frame->pts);

        if (q->first_pts == AV_NOPTS_VALUE)
            q->first_pts = frame->pts;
        else if (q->pts_delay == AV_NOPTS_VALUE)
            q->pts_delay = frame->pts - q->first_pts;

        if (!(tmp = av_frame_clone(frame)))
            return AVERROR(ENOMEM);

        if ((ret = put_frame(q, tmp)) < 0) {
            av_frame_free(&tmp);
            return ret;
        }

        ret = MFX_ERR_MORE_DATA;
    } else {
        av_log(q, AV_LOG_DEBUG, "frame:NULL\n");

        ret = MFX_ERR_NONE;
    }

    do {
        if (ret == MFX_ERR_MORE_DATA) {
            if (q->pending) {
                tmp = get_frame(q);

                if ((ret = align_frame(avctx, tmp)) < 0) {
                    av_frame_free(&tmp);
                    return ret;
                }

                if (!(surf = get_surface(q))) {
                    av_frame_free(&tmp);
                    return AVERROR(ENOMEM);
                }

                set_surface_param(q, surf, tmp);
            } else {
                break;
            }
        }

        bs = get_bitstream(q);

        ret = MFXVideoENCODE_EncodeFrameAsync(q->session, NULL, surf, bs, &sync);
        av_log(avctx, AV_LOG_DEBUG, "MFXVideoENCODE_EncodeFrameAsync():%d\n", ret);

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
    } while (ret == MFX_ERR_MORE_DATA || ret == MFX_WRN_DEVICE_BUSY);

    ret = ret == MFX_ERR_MORE_DATA ? 0 : ff_qsv_error(ret);

    if (sync) {
        ret = MFXVideoCORE_SyncOperation(q->session, sync, SYNC_TIME_DEFAULT);
        av_log(avctx, AV_LOG_DEBUG, "MFXVideoCORE_SyncOperation():%d\n", ret);
        if ((ret = ff_qsv_error(ret)) < 0)
            return ret;

        print_frametype(avctx, q, bs, 6);

        if (bs->FrameType & MFX_FRAMETYPE_REF ||
            bs->FrameType & MFX_FRAMETYPE_xREF) {
            dts = AV_NOPTS_VALUE;
        } else {
            dts = bs->TimeStamp;
            fill_encoded_data_dts(q, dts);
        }

        if ((ret = put_encoded_data(q, bs, dts)) < 0) {
            release_bitstream(q, bs);
            return ret;
        }
    }

    if (q->edlist_head && q->edlist_head->dts != AV_NOPTS_VALUE) {
        get_encoded_data(q, &bs, &dts);

        print_frametype(avctx, q, bs, 12);

        if ((ret = ff_alloc_packet(pkt, bs->DataLength)) < 0) {
            release_bitstream(q, bs);
            return ret;
        }

        pkt->dts  = dts;
        pkt->pts  = bs->TimeStamp;
        pkt->size = bs->DataLength;

        if (bs->FrameType & MFX_FRAMETYPE_I ||
            bs->FrameType & MFX_FRAMETYPE_xI ||
            bs->FrameType & MFX_FRAMETYPE_IDR ||
            bs->FrameType & MFX_FRAMETYPE_xIDR)
            pkt->flags |= AV_PKT_FLAG_KEY;

        memcpy(pkt->data, bs->Data + bs->DataOffset, bs->DataLength);

        release_bitstream(q, bs);

        *got_packet = 1;
    }

    return ret;
}

int ff_qsv_enc_close(AVCodecContext *avctx, QSVEncContext *q)
{
    MFXVideoENCODE_Close(q->session);
    av_log(avctx, AV_LOG_DEBUG, "MFXVideoENCODE_Close()\n");

    MFXClose(q->session);
    av_log(avctx, AV_LOG_DEBUG, "MFXClose()\n");

    free_surface_list(q);
    av_log(avctx, AV_LOG_DEBUG, "free_surface_list()\n");

    free_bitstream_list(q);
    av_log(avctx, AV_LOG_DEBUG, "free_bitstream_list()\n");

    free_encoded_data_list(q);
    av_log(avctx, AV_LOG_DEBUG, "free_encoded_data_list()\n");

    free_frame_list(q);
    av_log(avctx, AV_LOG_DEBUG, "free_frame_list()\n");

    return 0;
}
