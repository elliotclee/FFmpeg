/*
 * Android MediaCodec NDK decoder
 *
 * Copyright (c) 2015-2016, NVIDIA CORPORATION. All rights reserved.
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

// Just for intellisense symbol resolution
//#define __ANDROID_API__ 22

#include "libavutil/opt.h"
#include "libavutil/buffer_internal.h"
#include "libavutil/avassert.h"

#include <assert.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include "avcodec.h"
#include "codec_internal.h"
#include "internal.h"
#include "decode.h"
#include "h264.h"
#include "mediacodecndk.h"
#include "mediacodecndk_utils.h"

#define TIMEOUT 10000

#define OFFSET(x) offsetof(MediaCodecNDKDecoderContext, x)
static const AVOption options[] = {
    { "mediacodec_name", "Name of the MediaCodec codec to use", OFFSET(mediacodec_name), AV_OPT_TYPE_STRING, { .str = NULL },  CHAR_MIN, CHAR_MAX, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM },
    { "hwdeint_mode", "Used for setting deinterlace mode in MediaCodecNDKDecoder", OFFSET(deint_mode), AV_OPT_TYPE_INT,{.i64 = 1 } , 0, 2, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM },
    { "output_size", "Output width/height", OFFSET(out_width), AV_OPT_TYPE_IMAGE_SIZE, {.i64 = 0} , 0, 3840, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static void mediacodecndk_delete_decoder(void *opaque, uint8_t *data)
{
    AMediaCodec *decoder = opaque;
    av_log(NULL, AV_LOG_DEBUG, "NdkDec: Deleting decoder\n");
    AMediaCodec_delete(decoder);
}

static av_cold int mediacodecndk_decode_init(AVCodecContext *avctx)
{
    MediaCodecNDKDecoderContext *ctx = avctx->priv_data;
    AMediaFormat* format;
    const char *mime = ff_mediacodecndk_get_mime(avctx->codec_id);
    const char *formatString = NULL;
    int ret = ff_mediacodecndk_init_binder();

    if (ret < 0)
        return ret;

    ctx->waiting_buffer = -1;

    if (avctx->codec->type == AVMEDIA_TYPE_AUDIO)
        avctx->sample_fmt = AV_SAMPLE_FMT_S16;

    format = AMediaFormat_new();
    if (!format)
        return AVERROR(ENOMEM);

    switch (avctx->codec_id) {

    case AV_CODEC_ID_H264:
        if ((ret = h264_set_extradata(avctx, format)) < 0)
            return ret;
        break;
    case AV_CODEC_ID_HEVC:
        if ((ret = hevc_set_extradata(avctx, format)) < 0)
            return ret;
        break;
    default:
        if (avctx->extradata)
            AMediaFormat_setBuffer(format, "csd-0", (void*)avctx->extradata, avctx->extradata_size);
    }

    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, COLOR_FormatYUV420SemiPlanar);
    // Set these fields to output dimension when HW scaler in decoder is ready
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, avctx->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, avctx->height);
    AMediaFormat_setInt32(format, "deinterlace-method", ctx->deint_mode);

    if (ctx->out_width && ctx->out_height) {
        av_log(avctx, AV_LOG_INFO, "NdkDec: Setting output size to %dx%d\n", ctx->out_width, ctx->out_height);
        AMediaFormat_setInt32(format, "eWid", ctx->out_width);
        AMediaFormat_setInt32(format, "eHei", ctx->out_height);
        ff_set_dimensions(avctx, ctx->out_width, ctx->out_height);
    }

    av_log(avctx, AV_LOG_INFO, "NdkDec: mediacodec_name: %s\n", ctx->mediacodec_name);

    if (ctx->mediacodec_name) {
        if (!(ctx->decoder = AMediaCodec_createCodecByName(ctx->mediacodec_name))) {
            av_log(avctx, AV_LOG_ERROR, "NdkDec: Decoder could not be created for name: %s\n", ctx->mediacodec_name);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else {
        if (!(ctx->decoder = AMediaCodec_createDecoderByType(mime))) {
            av_log(avctx, AV_LOG_ERROR, "NdkDec: Decoder could not be created for mime type: %s\n", mime);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    if (!(ctx->decoder_ref = av_buffer_create(NULL, 0, mediacodecndk_delete_decoder, ctx->decoder, 0))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    formatString = AMediaFormat_toString(format);

    ret = AMediaCodec_configure(ctx->decoder, format, NULL, 0, 0);
    if (ret != AMEDIA_OK) {
        av_log(avctx, AV_LOG_ERROR, "NdkDec: Failed to configure decoder; (status = %d). Format: %s\n", ret, formatString);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ret = AMediaCodec_start(ctx->decoder);
    if (ret != AMEDIA_OK) {
        av_log(avctx, AV_LOG_ERROR, "NdkDec: Failed to start decoder; (status = %d). Format: %s\n", ret, formatString);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    AMediaFormat_delete(format);

    if (ctx->mediacodec_name && strcmp(ctx->mediacodec_name, "OMX.amlogic.mpeg2.decoder.awesome") == 0) {
        av_log(avctx, AV_LOG_INFO, "Enabling workaround for %s\n", ctx->mediacodec_name);
        ctx->amlogic_mpeg2_api23_workaround = 1;
    }

    return 0;

fail:
    if (format)
        AMediaFormat_delete(format);
    if (ctx->decoder_ref)
        av_buffer_unref(&ctx->decoder_ref);
    else if (ctx->decoder)
        AMediaCodec_delete(ctx->decoder);
    return ret;
}

static int mediacodecndk_send_packet(AVCodecContext *avctx, const AVPacket* avpkt)
{
    MediaCodecNDKDecoderContext *ctx = avctx->priv_data;
    int in_index, ret = 0;
    size_t in_size;
    uint8_t* in_buffer = NULL;
    AVPacket filtered_pkt = { 0 };

    //if (ctx->bsfc && avpkt) {
    //    AVPacket filter_pkt = { 0 };
    //    if ((ret = av_packet_ref(&filter_pkt, avpkt)) < 0)
    //        return ret;

    //    if ((ret = av_bsf_send_packet(ctx->bsfc, &filter_pkt)) < 0) {
    //        av_packet_unref(&filter_pkt);
    //        return ret;
    //    }

    //    if ((ret = av_bsf_receive_packet(ctx->bsfc, &filtered_pkt)) < 0)
    //        return ret;

    //    avpkt = &filtered_pkt;
    //}

    // receive_frame may have already dequeued a buffer
    in_index = ctx->waiting_buffer;
    ctx->waiting_buffer = -1;
    if (in_index < 0)
        in_index = AMediaCodec_dequeueInputBuffer(ctx->decoder, 1000000);
    if (in_index < 0) {
        av_log(avctx, AV_LOG_WARNING, "NdkDec: Failed to get input buffer! ret = %d\n", in_index);
        ret = AVERROR(EAGAIN);
        goto fail;
    }

    in_buffer = AMediaCodec_getInputBuffer(ctx->decoder, in_index, &in_size);
    if (!in_buffer) {
        av_log(avctx, AV_LOG_ERROR, "NdkDec: Cannot get input buffer (#%d)!\n", in_index);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (!avpkt) {
        AMediaCodec_queueInputBuffer(ctx->decoder, in_index, 0, 0, 0, BUFFER_FLAG_EOS);
        return AVERROR_EOF;
    }

    av_assert0(avpkt->size <= in_size);
    memcpy(in_buffer, avpkt->data, avpkt->size);
    AMediaCodec_queueInputBuffer(ctx->decoder, in_index, 0, avpkt->size, avpkt->pts, 0);

fail:
    av_packet_unref(&filtered_pkt);
    return ret;
}

static void mediacodecndk_free_buffer(void *opaque, uint8_t *data)
{
    AVBufferRef *decoder_ref = opaque;
    av_log(NULL, AV_LOG_DEBUG, "NdkDec: Releasing buffer: %" PRId32 "\n", (int32_t)data);
    AMediaCodec_releaseOutputBuffer(av_buffer_get_opaque(decoder_ref), (int32_t)data, false);
    av_buffer_unref(&decoder_ref);
}

static int mediacodecndk_receive_frame(AVCodecContext *avctx, AVFrame* frame)
{
    MediaCodecNDKDecoderContext *ctx = avctx->priv_data;
    AMediaCodecBufferInfo bufferInfo;
    size_t out_size;
    uint8_t* out_buffer = NULL;
    int32_t out_index = -2;
    int ret;
    AVBufferRef *ref;

    int64_t timeout = avctx->internal->draining ? 1000000 : 0;

    while (1) {
        out_index = AMediaCodec_dequeueOutputBuffer(ctx->decoder, &bufferInfo, timeout);
        if (out_index >= 0) {
            if (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                ret = AVERROR_EOF;
                goto fail;
            }
            break;
        }
        else if (out_index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            av_log(avctx, AV_LOG_DEBUG, "NdkDec: Mediacodec info output buffers changed\n");
        }
        else if (out_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = AMediaCodec_getOutputFormat(ctx->decoder);

            av_assert0(format);

            av_log(avctx, AV_LOG_INFO, "NdkDec: MediaCodec output format changed: %s\n",
                AMediaFormat_toString(format));

            if (avctx->codec->type == AVMEDIA_TYPE_AUDIO) {
                int32_t channels, sample_rate;
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sample_rate);
                AMediaFormat_delete(format);

                avctx->sample_rate = sample_rate;
                avctx->ch_layout.nb_channels = channels;
            }
            else {
                enum AVPixelFormat pix_fmt;
                int32_t width = 0, height = 0, crop_width = 0, crop_height = 0, stride = 0, slice_height = 0;

                AMediaFormat_getInt32(format, "crop-width", &crop_width);
                AMediaFormat_getInt32(format, "crop-height", &crop_height);
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride);
                AMediaFormat_getInt32(format, "slice-height", &slice_height);
                AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &ctx->color_format);

                AMediaFormat_getInt32(format, "crop-top", &ctx->crop_top);
                AMediaFormat_getInt32(format, "crop-bottom", &ctx->crop_bottom);
                AMediaFormat_getInt32(format, "crop-left", &ctx->crop_left);
                AMediaFormat_getInt32(format, "crop-right", &ctx->crop_right);
                AMediaFormat_getInt32(format, "display-width", &ctx->display_width);
                AMediaFormat_getInt32(format, "display-height", &ctx->display_height);

                AMediaFormat_delete(format);

                av_log(avctx, AV_LOG_DEBUG, "NdkDec: Output format: crop_width:%d crop_height:%d slice_height:%d crop_top:%d crop_bottom:%d crop_left:%d crop_right:%d display_width:%d display_height:%d\n",
                    crop_width, crop_height, slice_height, ctx->crop_top, ctx->crop_bottom, ctx->crop_left, ctx->crop_right, ctx->display_width, ctx->display_height);

                if (ctx->mediacodec_name && !strcmp(ctx->mediacodec_name, "OMX.k3.video.decoder.avc") && ctx->color_format == COLOR_FormatYCbYCr) {
                    ctx->color_format = COLOR_TI_FormatYUV420PackedSemiPlanar;
                }

                pix_fmt = ff_mediacodecndk_get_pix_fmt(ctx->color_format);
                av_log(avctx, AV_LOG_DEBUG, "NdkDec: Setting pix_fmt: %d for amc color format %d (previous value: %d)\n", pix_fmt, ctx->color_format, avctx->pix_fmt);

                if (pix_fmt == AV_PIX_FMT_NONE) {
                    av_log(avctx, AV_LOG_ERROR, "NdkDec: Unsupported output color format: 0x%x (value=%d) %i\n", ctx->color_format, ctx->color_format, ctx->color_format);
                    return AVERROR_EXTERNAL;
                }

                avctx->pix_fmt = pix_fmt;

                if (stride)
                    ctx->stride = stride;
                else
                    ctx->stride = width;

                if (slice_height) {
                    ctx->slice_height = slice_height;
                } else {
                    ctx->slice_height = height;

                    // Only apply this workaround if no slice_height is returned
                    if (ctx->mediacodec_name && strstr(ctx->mediacodec_name, "OMX.Nvidia.")) {
                        ctx->slice_height = FFALIGN(height, 16);
                    }
               }

                if (ctx->mediacodec_name) {
                    if (strstr(ctx->mediacodec_name, "OMX.SEC.avc.dec")) {
                        ctx->slice_height = avctx->height;
                        ctx->stride = avctx->width;
                    }
                }

                if (crop_width && crop_height && (!ctx->out_width || !ctx->out_height))
                    ff_set_dimensions(avctx, crop_width, crop_height);
                else if (width && height && (!ctx->out_width || !ctx->out_height))
                    ff_set_dimensions(avctx, width, height);

                // From normal mediacodec:
                ////width = crop_right + 1 - crop_left;
                ////height = crop_bottom + 1 - crop_top;
                //// Must be checked first with format.containsKey("crop-left")

                ////if (display_width && display_height) {
                ////    AVRational sar = av_div_q(
                ////        (AVRational){ display_width, display_height },
                ////        (AVRational){ width, height });
                ////    ff_set_sar(avctx, sar);
                ////}

                ////av_log(avctx, AV_LOG_INFO,
                ////    "Output crop parameters top=%d bottom=%d left=%d right=%d, "
                ////    "resulting dimensions width=%d height=%d\n",
                ////    crop_top, crop_bottom, crop_left, crop_right,
                ////    width, height);

                ////return ff_set_dimensions(avctx, width, height);

                av_assert0(ctx->slice_height >= avctx->height &&
                    ctx->stride >= avctx->width);
            }
        }
        else if (out_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            av_log(avctx, AV_LOG_DEBUG, "NdkDec: No frames available yet\n");
            // This can mean either that the codec is starved and we need to send more
            // frames (EAGAIN), or that it's still working and we need to wait on it.
            // We can't tell which case it is, but if there are no input buffers
            // available, we at least know it shouldn't be starved, so try again
            // with a larger timeout in that case.
            if (ctx->waiting_buffer < 0 && !timeout) {
                ctx->waiting_buffer = AMediaCodec_dequeueInputBuffer(ctx->decoder, 0);
                if (ctx->waiting_buffer < 0 && !ctx->amlogic_mpeg2_api23_workaround) {
                    av_log(avctx, AV_LOG_VERBOSE, "NdkDec: Out of input buffers; waiting for output\n");
                    timeout = 1000000;
                    continue;
                }
            }
            return AVERROR(EAGAIN);
        }
        else {
            av_log(avctx, AV_LOG_ERROR, "NdkDec: Unexpected info code: %d\n", out_index);
            return AVERROR_EXTERNAL;
        }
    }

    out_buffer = AMediaCodec_getOutputBuffer(ctx->decoder, out_index, &out_size);
    if (!out_buffer) {
        av_log(avctx, AV_LOG_ERROR, "NdkDec: Failed to get output buffer\n");
        return AVERROR_EXTERNAL;
    }

    // Apply color format decoding adapted from mediacodec_common.c (copies buffer data)
    // unless the format is NV12 or audio
    if (avctx->codec->type == AVMEDIA_TYPE_VIDEO 
        && (ctx->color_format != COLOR_FormatYUV420SemiPlanar || avctx->pix_fmt != AV_PIX_FMT_NV12)) {

        av_log(avctx, AV_LOG_DEBUG, "NdkDec: Returning buffer #%" PRId32 " (copy)\n", out_index);

        if ((ret = mediacodec_wrap_sw_buffer(avctx, ctx, out_buffer, out_size, out_index, &bufferInfo, frame)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "NdkDec: Failed to wrap MediaCodec buffer\n");
            goto fail;
        }

        AMediaCodec_releaseOutputBuffer(ctx->decoder, out_index, false);

        return 0;
    }

    av_log(avctx, AV_LOG_DEBUG, "NdkDec: Returning buffer #%" PRId32 " (nocopy)\n", out_index);

    // For pix_fmt NV12 and audio, use the supplied buffers without copying
    if (avctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        frame->nb_samples = bufferInfo.size / avctx->ch_layout.nb_channels / 2;
    }

    if ((ret = ff_decode_frame_props(avctx, frame)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "NdkDec: ff_decode_frame_props failed: %d\n", ret);
        goto fail;
    }

    frame->width = avctx->width;
    frame->height = avctx->height;

    if (!(ref = av_buffer_ref(ctx->decoder_ref))) {
        av_log(avctx, AV_LOG_ERROR, "NdkDec: av_buffer_ref failed\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    frame->buf[0] = av_buffer_create((void*)(intptr_t)out_index, out_size, mediacodecndk_free_buffer, ref, AV_BUFFER_FLAG_READONLY);

    if (!frame->buf[0]) {
        av_log(avctx, AV_LOG_ERROR, "NdkDec: av_buffer_create failed\n");
        av_buffer_unref(&ref);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    frame->data[0] = out_buffer;

    if (avctx->codec->type == AVMEDIA_TYPE_VIDEO) {

        frame->linesize[0] = ctx->stride;
        frame->data[1] = out_buffer + ctx->stride * ctx->slice_height;
        frame->linesize[1] = ctx->stride;
    }

    frame->pts = bufferInfo.presentationTimeUs;
    frame->pkt_dts = AV_NOPTS_VALUE;
    return 0;
fail:
    AMediaCodec_releaseOutputBuffer(ctx->decoder, out_index, false);
    return ret;
}

static av_cold int mediacodecndk_decode_close(AVCodecContext *avctx)
{
    MediaCodecNDKDecoderContext *ctx = avctx->priv_data;

    if (ctx->decoder) {
        AMediaCodec_flush(ctx->decoder);
        AMediaCodec_stop(ctx->decoder);
    }
    av_buffer_unref(&ctx->decoder_ref);
    //av_bsf_free(&ctx->bsfc);
    return 0;
}

static av_cold void mediacodecndk_decode_flush(AVCodecContext *avctx)
{
    MediaCodecNDKDecoderContext *ctx = avctx->priv_data;
    AMediaCodec_flush(ctx->decoder);
}

#define FFMC_DEC_CLASS(NAME, OPTIONS) \
    static const AVClass ffmediacodecndk_##NAME##_dec_class = { \
        .class_name = "mediacodecndk_" #NAME "_dec", \
        .item_name  = av_default_item_name, \
        .option     = OPTIONS, \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define FFMC_DEC(TYPE, NAME, NICENAME, ID, BSF) \
    const FFCodec ff_##NAME##_mediacodecndk_decoder = { \
        .p.name           = #NAME "_mediacodecndk", \
        .p.long_name      = NULL_IF_CONFIG_SMALL(NICENAME " MediaCodec NDK"), \
        .p.type           = AVMEDIA_TYPE_##TYPE, \
        .p.id             = ID, \
        .priv_data_size   = sizeof(MediaCodecNDKDecoderContext), \
        .init             = mediacodecndk_decode_init, \
        .close            = mediacodecndk_decode_close, \
        .send_packet      = mediacodecndk_send_packet, \
        .cb.receive_frame = mediacodecndk_receive_frame, \
        .cb_type          = FF_CODEC_CB_TYPE_RECEIVE_FRAME, \
        .flush            = mediacodecndk_decode_flush, \
        .bsfs             = BSF, \
        .p.priv_class     = &ffmediacodecndk_##NAME##_dec_class, \
        .p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
        .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP, \
        .p.wrapper_name  = "mediacodecndk", \
    };

#define FFMC_VDEC(NAME, NICENAME, ID, BSF) \
    FFMC_DEC_CLASS(NAME, options) \
    FFMC_DEC(VIDEO, NAME, NICENAME, ID, BSF)

#define FFMC_ADEC(NAME, NICENAME, ID) \
    FFMC_DEC_CLASS(NAME, NULL) \
    FFMC_DEC(AUDIO, NAME, NICENAME, ID, NULL)

FFMC_VDEC(h264, "H.264", AV_CODEC_ID_H264, "h264_mp4toannexb")
FFMC_VDEC(hevc, "H.265", AV_CODEC_ID_HEVC, "hevc_mp4toannexb")
FFMC_VDEC(mpeg2, "MPEG-2", AV_CODEC_ID_MPEG2VIDEO, NULL)
FFMC_VDEC(mpeg4, "MPEG-4", AV_CODEC_ID_MPEG4, NULL)
FFMC_VDEC(vc1, "VC1", AV_CODEC_ID_VC1, NULL)
FFMC_VDEC(vp8, "VP8", AV_CODEC_ID_VP8, NULL)
FFMC_VDEC(vp9, "VP9", AV_CODEC_ID_VP9, NULL)

FFMC_ADEC(mp1, "MP1", AV_CODEC_ID_MP1)
FFMC_ADEC(mp2, "MP2", AV_CODEC_ID_MP2)
FFMC_ADEC(mp3, "MP3", AV_CODEC_ID_MP3)
