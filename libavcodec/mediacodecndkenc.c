/*
 * Android MediaCodec NDK encoder
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

#include <assert.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include "encode.h"

#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "internal.h"
#include "mediacodecndk.h"

typedef struct MediaCodecNDKEncoderContext
{
    AVClass *avclass;
    AMediaCodec *encoder;
    AVFrame *frame;
    int saw_output_eos;
    int64_t last_dts;
    char *mediacodec_name;
    int rc_mode;
    int width;
    int height;
    uint8_t *new_extradata;
    int new_extradata_size;
    int is_rtk;
    ssize_t waiting_buffer;
} MediaCodecNDKEncoderContext;

#define LOCAL_BUFFER_FLAG_SYNCFRAME 1
#define LOCAL_BUFFER_FLAG_CODECCONFIG 2

#define OFFSET(x) offsetof(MediaCodecNDKEncoderContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

#define RC_MODE_CQ  0 // Unimplemented
#define RC_MODE_VBR 1
#define RC_MODE_CBR 2

static const AVOption options[] = {
    { "mediacodec_name", "Name of the MediaCodec codec to use", OFFSET(mediacodec_name), AV_OPT_TYPE_STRING, { .str = NULL },  CHAR_MIN, CHAR_MAX, VE },
    { "rc-mode", "The bitrate mode to use", OFFSET(rc_mode), AV_OPT_TYPE_INT, {.i64 = RC_MODE_VBR }, RC_MODE_VBR, RC_MODE_CBR, VE, "rc_mode"},
    //    { "cq", "Constant quality", 0, AV_OPT_TYPE_CONST, {.i64 = RC_MODE_CQ}, INT_MIN, INT_MAX, VE, "rc_mode" },
        { "vbr", "Variable bitrate", 0, AV_OPT_TYPE_CONST, {.i64 = RC_MODE_VBR}, INT_MIN, INT_MAX, VE, "rc_mode" },
        { "cbr", "Constant bitrate", 0, AV_OPT_TYPE_CONST, {.i64 = RC_MODE_CBR}, INT_MIN, INT_MAX, VE, "rc_mode" },
    { "mediacodec_output_size", "Temporary hack to support scaling on output", OFFSET(width), AV_OPT_TYPE_IMAGE_SIZE, {.i64 = 0} , 48, 3840, VE },
    { "is-rtk", "Whether the encoder is Realtek's", OFFSET(is_rtk), AV_OPT_TYPE_BOOL, {.i64 = -1 }, -1, 1, VE },
    { NULL },
};

static av_cold int mediacodecndk_encode_init(AVCodecContext *avctx)
{
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;
    AMediaFormat* format = NULL;
    int pixelFormat;
    const char* mime = ff_mediacodecndk_get_mime(avctx->codec_id);
    int encoderStatus = 0;
    AMediaCodecBufferInfo bufferInfo;
    int ret = ff_mediacodecndk_init_binder();
    AVRational sar = avctx->sample_aspect_ratio;
    media_status_t status;
    int32_t level;
    int32_t profile;

    av_log(avctx, AV_LOG_DEBUG, "NdkEnc: mediacodecndk_encode_init: Start\n");

    if (ret < 0)
        return ret;

    if (ctx->is_rtk < 0)
        ctx->is_rtk = !!getenv("PLEX_MEDIA_SERVER_IS_KAMINO");

    pixelFormat = ff_mediacodecndk_get_color_format(avctx->pix_fmt);

    ctx->frame = av_frame_alloc();
    if (!ctx->frame)
        return AVERROR(ENOMEM);

    if (!(format = AMediaFormat_new()))
        return AVERROR(ENOMEM);

    av_log(avctx, AV_LOG_INFO, "NdkEnc: is_rtk: %i pix_fmt: %i pixelFormat: %i\n",
        ctx->is_rtk, avctx->pix_fmt, pixelFormat);

    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, avctx->height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, avctx->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_WIDTH, avctx->width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_HEIGHT, avctx->height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, pixelFormat);

    AMediaFormat_setInt32(format, "bitrate-mode", ctx->rc_mode); // AMEDIAFORMAT_KEY_BITRATE_MODE - SDK 28
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, avctx->bit_rate);

    switch (ctx->rc_mode)
    {
        case RC_MODE_VBR:
        case RC_MODE_CBR:
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "NdkEnc: Unsupported bitrate mode: %d\n", ctx->rc_mode);
            return AVERROR_EXTERNAL;
    }

    if (avctx->rc_max_rate && avctx->rc_buffer_size) {
        AMediaFormat_setInt32(format, "max-bitrate", avctx->rc_max_rate);
        AMediaFormat_setInt32(format, "virtualbuffersize", avctx->rc_buffer_size);
    }

    av_log(avctx, AV_LOG_INFO, "NdkEnc: Mime: %s Size: %ix%i rc_mode: %i rc_max_rate: %"PRId64" rc_buffer_size: %i bit_rate: %"PRId64"\n",
        mime, avctx->width, avctx->height, ctx->rc_mode, avctx->rc_max_rate, avctx->rc_buffer_size, avctx->bit_rate);

    if (avctx->framerate.num && avctx->framerate.den) {
        av_log(avctx, AV_LOG_INFO, "NdkEnc: Framerate: %i / %i\n", avctx->framerate.num, avctx->framerate.den);
        AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, av_q2d(avctx->framerate));
    }
    else {
        av_log(avctx, AV_LOG_INFO, "NdkEnc: Timebase: %i / %i\n", avctx->time_base.num, avctx->time_base.den);
        AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_FRAME_RATE, av_q2d(av_inv_q(avctx->time_base)));
    }

    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);//FIXME
    //AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_STRIDE, avctx->width);
    AMediaFormat_setInt32(format, "priority", 1); // AMEDIAFORMAT_KEY_PRIORITY - SDK 28

    if (avctx->codec_id == AV_CODEC_ID_HEVC) {
        level = 0x1000; //Level41
        profile = 0x01; //Main
    } else {
        level = 0x200; //Level31
        profile = 0x08; //High
    }

    if (avctx->level > 0) {
        level = avctx->level;
    }

    if (avctx->profile > 0) {
        profile = avctx->profile;
    }

    av_log(avctx, AV_LOG_INFO, "NdkEnc: Profile: %d Level: %d\n", profile, level);
    AMediaFormat_setInt32(format, "profile", profile); // AMEDIAFORMAT_KEY_PROFILE - SDK 28
    AMediaFormat_setInt32(format, "level", level); // AMEDIAFORMAT_KEY_LEVEL - SDK 28

    if (!sar.num || !sar.den)
        sar.num = sar.den = 1;

    if (ctx->width && ctx->height) {
        AMediaFormat_setInt32(format, "output_width", ctx->width);
        AMediaFormat_setInt32(format, "output_height", ctx->height);

        av_log(avctx, AV_LOG_INFO, "NdkEnc: OutputSize: %ix%i\n", ctx->width, ctx->height);

        sar = av_mul_q((AVRational) {
            ctx->height * avctx->width, ctx->width * avctx->height
        }, sar);
    }

    av_reduce(&sar.num, &sar.den, sar.num, sar.den, 4096);

    AMediaFormat_setInt32(format, "aspect-width", sar.num);
    AMediaFormat_setInt32(format, "aspect-height", sar.den);

    av_log(avctx, AV_LOG_INFO, "NdkEnc: aspect: %i / %i\n", sar.num, sar.den);

    av_log(avctx, AV_LOG_INFO, "NdkEnc: mediacodec_name: %s\n", ctx->mediacodec_name);

    if (ctx->mediacodec_name) {
        if (!(ctx->encoder = AMediaCodec_createCodecByName(ctx->mediacodec_name))) {
            av_log(avctx, AV_LOG_ERROR, "NdkEnc: Encoder could not be created for name: %s\n", ctx->mediacodec_name);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    } else {
        if (!(ctx->encoder = AMediaCodec_createEncoderByType(mime))) {
            av_log(avctx, AV_LOG_ERROR, "NdkEnc: Encoder could not be created for mime type: %s\n", mime);
            ret = AVERROR_EXTERNAL;
            goto fail;
        }
    }

    if (AMediaCodec_configure(ctx->encoder, format, NULL, 0,
        AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK) {
        av_log(avctx, AV_LOG_ERROR, "NdkEnc: Failed to configure encoder; check parameters\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    status = AMediaCodec_start(ctx->encoder);

    av_log(avctx, AV_LOG_DEBUG, "NdkEnc: AMediaCodec_start returned %d\n", status);

    AMediaFormat_delete(format);
    format = NULL;

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        av_log(avctx, AV_LOG_DEBUG, "NdkEnc: Try read extradata (current size: %d)\n", avctx->extradata_size);
        int prev_size = avctx->extradata_size;
        while ((encoderStatus = AMediaCodec_dequeueOutputBuffer(ctx->encoder, &bufferInfo, 0)) >= 0) {
            size_t outSize;
            uint8_t *outBuffer = NULL;
            outBuffer = AMediaCodec_getOutputBuffer(ctx->encoder, encoderStatus, &outSize);

            av_assert0(outBuffer);
            av_log(avctx, AV_LOG_VERBOSE, "NdkEnc: Got codec specific data of size %d\n", bufferInfo.size);
            if ((ret = av_reallocp(&avctx->extradata,
                avctx->extradata_size + bufferInfo.size +
                AV_INPUT_BUFFER_PADDING_SIZE)) < 0) {
                goto fail;
            }
            memcpy(avctx->extradata + avctx->extradata_size, outBuffer, bufferInfo.size);
            avctx->extradata_size += bufferInfo.size;
            memset(avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
        }

        if (prev_size == avctx->extradata_size)
            av_log(avctx, AV_LOG_WARNING, "NdkEnc: AV_CODEC_FLAG_GLOBAL_HEADER is set but not extradata was retrieved!\n");
    } else {
        av_log(avctx, AV_LOG_VERBOSE, "NdkEnc: extradata size: %d\n", avctx->extradata_size);
    }

    av_log(avctx, AV_LOG_DEBUG, "NdkEnc: Finished init: %i\n", encoderStatus);

    return 0;

fail:
    if (format)
        AMediaFormat_delete(format);
    if (ctx->encoder)
        AMediaCodec_delete(ctx->encoder);
    if (ctx->frame)
        av_frame_free(&ctx->frame);
    return ret;
}


static int mediacodecndk_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;
    ssize_t bufferIndex = ctx->waiting_buffer;

    ctx->waiting_buffer = -1;
    if (bufferIndex < 0)
        bufferIndex = AMediaCodec_dequeueInputBuffer(ctx->encoder, 1000000);

    av_log(avctx, AV_LOG_TRACE, "NdkEnc: mediacodecndk_send_frame: Start\n");

    if (bufferIndex >= 0) {
        int ret = 0;
        if (!frame) {
            AMediaCodec_queueInputBuffer(ctx->encoder, bufferIndex, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            av_log(avctx, AV_LOG_DEBUG, "NdkEnc: Queued EOS buffer %zi\n", bufferIndex);
        }
        else {
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
            int i, nb_planes = 0, linesize[4], out_linesize[4];
            int align = ctx->is_rtk ? 16 : 1;
            uint32_t flags = 0;
            size_t bufferSize = 0;
            uint8_t *buffer = AMediaCodec_getInputBuffer(ctx->encoder, bufferIndex, &bufferSize);
            uint8_t *bufferEnd = buffer + bufferSize;

            if (!desc) {
                av_log(avctx, AV_LOG_ERROR, "NdkEnc: Cannot get input pixdesc!\n");
                ret = AVERROR(EINVAL);
                goto fail;
            }

            if (!buffer) {
                av_log(avctx, AV_LOG_ERROR, "NdkEnc: Cannot get input buffer!\n");
                return AVERROR_EXTERNAL;
            }

            for (i = 0; i < desc->nb_components; i++)
                nb_planes = FFMAX(desc->comp[i].plane + 1, nb_planes);

            ret = av_image_fill_linesizes(linesize, frame->format, frame->width);
            av_assert0(ret >= 0);
            ret = av_image_fill_linesizes(out_linesize, frame->format, FFALIGN(frame->width, align));
            av_assert0(ret >= 0);

            for (i = 0; i < nb_planes; i++) {
                int j;
                int shift_h = (i == 1 || i == 2) ? desc->log2_chroma_h : 0;
                const uint8_t *src = frame->data[i];
                int h = AV_CEIL_RSHIFT(frame->height, shift_h);

                if (buffer + (out_linesize[i] * (h - 1)) + linesize[i] > bufferEnd) {
                    av_log(avctx, AV_LOG_ERROR, "NdkEnc: Buffer not large enough for input (%ix%i %s %zu)\n",
                        frame->width, frame->height, desc->name, bufferSize);
                    ret = AVERROR(EINVAL);
                    break;
                }

                for (j = 0; j < h; j++) {
                    memcpy(buffer, src, linesize[i]);
                    buffer += out_linesize[i];
                    src += frame->linesize[i];
                }

                buffer += out_linesize[i] * (FFALIGN(h, FFMAX(align >> shift_h, 1)) - h);
            }

            if (frame->pict_type == AV_PICTURE_TYPE_I)
                flags |= LOCAL_BUFFER_FLAG_SYNCFRAME;

        fail:
            AMediaCodec_queueInputBuffer(ctx->encoder, bufferIndex, 0, bufferSize, av_rescale_q(frame->pts, avctx->time_base, AV_TIME_BASE_Q), flags);
            av_log(avctx, AV_LOG_DEBUG, "NdkEnc: Queued input buffer %zi (flags=%i)\n", bufferIndex, flags);
        }
        return ret;
    }
    else if (bufferIndex == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        av_log(avctx, AV_LOG_WARNING, "NdkEnc: No input buffers available\n");
        return AVERROR(EAGAIN);
    }
    else {
        av_log(avctx, AV_LOG_ERROR, "NdkEnc: Unknown error in dequeueInputBuffer: %zi\n", bufferIndex);
        return AVERROR_EXTERNAL;
    }
}

static int mediacodecndk_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    int64_t timeout = avctx->internal->draining ? 1000000 : 0;
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;
    AVFrame *frame = ctx->frame;
    int ret;

    av_log(avctx, AV_LOG_TRACE, "NdkEnc: receive_packet: Start\n");

    if (!frame->buf[0]) {
        ret = ff_encode_get_frame(avctx, frame);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    ret = mediacodecndk_send_frame(avctx, frame);
    if (ret != AVERROR(EAGAIN))
        av_frame_unref(ctx->frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return ret;


    while (!ctx->saw_output_eos) {
        AMediaCodecBufferInfo bufferInfo;
        int encoderStatus = AMediaCodec_dequeueOutputBuffer(ctx->encoder, &bufferInfo, timeout);
        if (encoderStatus == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            // no output available yet
            av_log(avctx, AV_LOG_DEBUG, "NdkEnc: No packets available yet\n");
            // This can mean either that the codec is starved and we need to send more
            // packets (EAGAIN), or that it's still working and we need to wait on it.
            // We can't tell which case it is, but if there are no input buffers
            // available, we at least know it shouldn't be starved, so try again
            // with a larger timeout in that case.
            if (ctx->waiting_buffer < 0 && !timeout) {
                ctx->waiting_buffer = AMediaCodec_dequeueInputBuffer(ctx->encoder, 0);
                if (ctx->waiting_buffer < 0) {
                    av_log(avctx, AV_LOG_DEBUG, "NdkEnc: Out of input buffers; waiting for output\n");
                    timeout = 1000000;
                    continue;
                }
            }
            return AVERROR(EAGAIN);
        }
        else if (encoderStatus == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // should happen before receiving buffers, and should only happen once
            AMediaFormat *format = AMediaCodec_getOutputFormat(ctx->encoder);
            av_assert0(format);
            av_log(avctx, AV_LOG_VERBOSE, "NdkEnc: MediaCodec output format changed: %s\n",
                AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        }
        else if (encoderStatus == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            av_log(avctx, AV_LOG_VERBOSE, "NdkEnc: MediaCodec output buffers changed\n");
        }
        else if (encoderStatus < 0) {
            av_log(avctx, AV_LOG_WARNING, "NdkEnc: Unknown MediaCodec status: %i\n", encoderStatus);
        }
        else {
            size_t outSize;
            uint8_t *outBuffer = AMediaCodec_getOutputBuffer(ctx->encoder, encoderStatus, &outSize);
            if (bufferInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                av_log(avctx, AV_LOG_VERBOSE, "NdkEnc: Got EOS at output\n");
                AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
                ctx->saw_output_eos = true;
                break;
            }

            av_assert0(outBuffer);
            if (bufferInfo.flags & LOCAL_BUFFER_FLAG_CODECCONFIG) {
                av_log(avctx, AV_LOG_VERBOSE, "NdkEnc: Got extradata of size %d\n", bufferInfo.size);
                if (ctx->new_extradata)
                    av_free(ctx->new_extradata);
                ctx->new_extradata = av_mallocz(bufferInfo.size + AV_INPUT_BUFFER_PADDING_SIZE);
                ctx->new_extradata_size = bufferInfo.size;
                if (!ctx->new_extradata) {
                    AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
                    av_log(avctx, AV_LOG_ERROR, "NdkEnc: Failed to allocate extradata\n");
                    return AVERROR(ENOMEM);
                }
                memcpy(ctx->new_extradata, outBuffer, bufferInfo.size);

                av_log(avctx, AV_LOG_DEBUG, "NdkEnc: extradata:\n");
                for (int i = 0; i < ctx->new_extradata_size; i++)
                    av_log(avctx, AV_LOG_DEBUG, "%02X ", ctx->new_extradata[i]);
                av_log(avctx, AV_LOG_DEBUG, "\n");

                AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);

                if (!avctx->extradata_size) {
                    av_log(avctx, AV_LOG_DEBUG, "NdkEnc: setting avctx->extradata, size: %d:\n", ctx->new_extradata_size);
                    avctx->extradata = ctx->new_extradata;
                    avctx->extradata_size = ctx->new_extradata_size;
                }
                continue;
            }

            if ((ret = ff_get_encode_buffer(avctx, pkt, bufferInfo.size, 0) < 0)) {
                AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);
                av_log(avctx, AV_LOG_ERROR, "NdkEnc: Failed to allocate packet: %i\n", ret);
                return ret;
            }

            memcpy(pkt->data, outBuffer, bufferInfo.size);
            pkt->pts = av_rescale_q(bufferInfo.presentationTimeUs, AV_TIME_BASE_Q, avctx->time_base);
            pkt->dts = pkt->pts; // AV_NOPTS_VALUE;
            if (bufferInfo.flags & LOCAL_BUFFER_FLAG_SYNCFRAME)
                pkt->flags |= AV_PKT_FLAG_KEY;

            AMediaCodec_releaseOutputBuffer(ctx->encoder, encoderStatus, false);

            if (ctx->new_extradata) {

                uint8_t *side_data = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, ctx->new_extradata_size);
                if (!side_data)
                    return AVERROR(ENOMEM);

                memcpy(side_data, ctx->new_extradata, ctx->new_extradata_size);

                if (ctx->new_extradata != avctx->extradata)
                    av_free(ctx->new_extradata);

                av_log(avctx, AV_LOG_VERBOSE, "NdkEnc: Added extradata of size %d\n", ctx->new_extradata_size);

                ctx->new_extradata = NULL;
                ctx->new_extradata_size = 0;
            }

            av_log(avctx, AV_LOG_DEBUG, "NdkEnc: receive_packet: Success - Data size: %d  Framesize:%dx%d\n", pkt->size, avctx->width, avctx->height);

            return 0;
        }
    }
    return AVERROR_EOF;
}

static av_cold int mediacodecndk_encode_close(AVCodecContext *avctx)
{
    MediaCodecNDKEncoderContext *ctx = avctx->priv_data;

    if (ctx->encoder) {
        AMediaCodec_stop(ctx->encoder);
        AMediaCodec_flush(ctx->encoder);
        AMediaCodec_delete(ctx->encoder);
    }

    av_frame_free(&ctx->frame);

    return 0;
}

static const AVClass mediacodecndk_h264_enc_class = {
    .class_name = "mediacodecndk_h264_enc",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_mediacodecndk_encoder = {
    .p.name = "h264_mediacodecndk",
    .p.long_name = NULL_IF_CONFIG_SMALL("H.264 MediaCodec NDK"),
    .p.type = AVMEDIA_TYPE_VIDEO,
    .p.id = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(MediaCodecNDKEncoderContext),
    .init = mediacodecndk_encode_init,
    //.send_frame = mediacodecndk_send_frame,
    //.receive_packet = mediacodecndk_receive_packet,
    FF_CODEC_RECEIVE_PACKET_CB(mediacodecndk_receive_packet),
    .close = mediacodecndk_encode_close,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .p.priv_class = &mediacodecndk_h264_enc_class,
    .p.wrapper_name  = "mediacodecndk",
    .p.pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};

static const AVClass mediacodecndk_hevc_enc_class = {
    .class_name = "mediacodecndk_hevc_enc",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_hevc_mediacodecndk_encoder = {
    .p.name = "hevc_mediacodecndk",
    .p.long_name = NULL_IF_CONFIG_SMALL("HEVC (H.265) MediaCodec NDK"),
    .p.type = AVMEDIA_TYPE_VIDEO,
    .p.id = AV_CODEC_ID_HEVC,
    .priv_data_size = sizeof(MediaCodecNDKEncoderContext),
    .init = mediacodecndk_encode_init,
    //.send_frame = mediacodecndk_send_frame,
    //.receive_packet = mediacodecndk_receive_packet,
    FF_CODEC_RECEIVE_PACKET_CB(mediacodecndk_receive_packet),
    .close = mediacodecndk_encode_close,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY,
    .p.priv_class = &mediacodecndk_hevc_enc_class,
    .p.wrapper_name  = "mediacodecndk",
    .p.pix_fmts = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    },
};
