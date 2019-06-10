/*
 * Copyright (c) 2013 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * libavformat/libavcodec demuxing and muxing API example.
 *
 * Remux streams from one container format to another.
 * @example remuxing.c
 */

#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS

#include <signal.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
}

#include <string>

volatile int DIE = 0;

void sigma(int x) {
	if (++DIE >= 20) abort();
}

// FFMPEG's convenience macro causes GCC to complain when compiled as C++11
static std::string ffmpeg_averrtostring(const int ret) {
    char err[AV_ERROR_MAX_STRING_SIZE] = {0};

    av_make_error_string(err, AV_ERROR_MAX_STRING_SIZE, ret);
    return std::string(err);
}

static std::string cpp_av_ts2str(int64_t ts) {
    char str[AV_TS_MAX_STRING_SIZE];

    av_ts_make_string(str, ts);
    return std::string(str);
}

static std::string cpp_av_ts2timestr(int64_t ts, AVRational *tb) {
    char str[AV_TS_MAX_STRING_SIZE];

    if (ts == AV_NOPTS_VALUE) snprintf(str, AV_TS_MAX_STRING_SIZE, "NOPTS");
    else                      snprintf(str, AV_TS_MAX_STRING_SIZE, "%.6g", av_q2d(*tb) * ts);

    return std::string(str);
}

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag)
{
#if 0
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           tag,
           cpp_av_ts2str(pkt->pts).c_str(),         cpp_av_ts2timestr(pkt->pts, time_base).c_str(),
           cpp_av_ts2str(pkt->dts).c_str(),         cpp_av_ts2timestr(pkt->dts, time_base).c_str(),
           cpp_av_ts2str(pkt->duration).c_str(),    cpp_av_ts2timestr(pkt->duration, time_base).c_str(),
           pkt->stream_index);
#endif
}

int main(int argc, char **argv)
{
    AVOutputFormat *ofmt = NULL;
    AVDictionary *mp4_dict = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket pkt;
    const char *in_filename, *out_filename;
    int ret, i;

    if (argc < 3) {
        printf("usage: %s input output\n"
               "API example program to remux a media file with libavformat and libavcodec.\n"
               "The output format is guessed according to the file extension.\n"
               "\n", argv[0]);
        return 1;
    }

    in_filename  = argv[1];
    out_filename = argv[2];

    av_register_all();

    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        fprintf(stderr, "Could not open input file '%s'", in_filename);
        goto end;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        goto end;
    }

    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
            goto end;
        }
        out_stream->codec->codec_tag = 0;
        if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            goto end;
        }
    }

    av_dict_set(&mp4_dict, "movflags", "faststart", 0);
    av_dict_set(&mp4_dict, "chunk_duration", "30", 0);

    ret = avformat_write_header(ofmt_ctx, &mp4_dict);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
    }

	/* soft break on CTRL+C */
	signal(SIGINT,sigma);
	signal(SIGHUP,sigma);
	signal(SIGQUIT,sigma);
	signal(SIGTERM,sigma);

    int64_t pts_prev[ifmt_ctx->nb_streams];
    int64_t pts_adjust[ifmt_ctx->nb_streams];

    for (size_t i=0;i < ifmt_ctx->nb_streams;i++) {
        pts_prev[i] = AV_NOPTS_VALUE;
        pts_adjust[i] = 0;
    }

    while (!DIE) {
        AVStream *in_stream, *out_stream;

        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0)
            break;

        in_stream  = ifmt_ctx->streams[pkt.stream_index];
        out_stream = ofmt_ctx->streams[pkt.stream_index];

        int64_t ts = AV_NOPTS_VALUE;

        if (pkt.dts != AV_NOPTS_VALUE)
            ts = pkt.dts;
        if (pkt.pts != AV_NOPTS_VALUE) {
            if (ts != AV_NOPTS_VALUE)
                ts = std::min(pkt.pts,ts); // DTS should not exceed PTS
            else
                ts = pkt.pts;
        }

        log_packet(ifmt_ctx, &pkt, "in");

        /* adjust time */
        if (pkt.pts != AV_NOPTS_VALUE)
            pkt.pts += pts_adjust[pkt.stream_index];
        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts += pts_adjust[pkt.stream_index];

        /* copy packet */
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;

        log_packet(ofmt_ctx, &pkt, "out");

        ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt_ctx);
end:

    avformat_close_input(&ifmt_ctx);

    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", ffmpeg_averrtostring(ret).c_str());
        return 1;
    }

    return 0;
}
