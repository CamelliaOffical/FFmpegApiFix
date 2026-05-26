#include "recorder.hpp"
#include "utils.hpp"

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/dict.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/mem.h>
    #include <libavutil/opt.h>
    #include <libavutil/pixdesc.h>
    #include <libswscale/swscale.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersrc.h>
    #include <libavfilter/buffersink.h>
}

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <utility>

BEGIN_FFMPEG_NAMESPACE_V

namespace {
    std::string trim(std::string value) {
        auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return "";
        auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    std::string appendFilter(std::string base, std::string extra) {
        base = trim(std::move(base));
        extra = trim(std::move(extra));
        if (extra.empty())
            return base;
        if (base.empty())
            return extra;
        return base + "," + extra;
    }

    std::string normalizeFilters(std::string filters) {
        filters = trim(std::move(filters));
        if (filters.empty())
            return "";

        auto eq = filters.find('=');
        auto comma = filters.find(',');
        if (eq != std::string::npos && (comma == std::string::npos || eq < comma)) {
            auto name = filters.substr(0, eq);
            if (name == "all" || name == "iall" || name == "space" ||
                name == "ispace" || name == "range" || name == "irange" ||
                name == "primaries" || name == "iprimaries" ||
                name == "trc" || name == "itrc" || name == "format" ||
                name == "fast" || name == "dither") {
                filters = "colorspace=" + filters;
            }
        }

        return filters;
    }

    std::string lowerCopy(std::string value) {
        for (auto& ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    }

    std::string stripOptionPrefix(std::string key) {
        while (!key.empty() && key.front() == '-')
            key.erase(key.begin());
        return key;
    }

    std::string stripStreamSuffix(std::string key) {
        if (auto colon = key.find(':'); colon != std::string::npos)
            key.erase(colon);
        return key;
    }

    bool isOptionToken(const std::string& token) {
        if (token.size() < 2 || token.front() != '-')
            return false;
        return !std::isdigit(static_cast<unsigned char>(token[1])) && token[1] != '.';
    }

    std::vector<std::string> splitArgs(const std::string& args) {
        std::vector<std::string> tokens;
        std::string token;
        char quote = '\0';
        bool escaped = false;

        for (char ch : args) {
            if (escaped) {
                token.push_back(ch);
                escaped = false;
                continue;
            }

            if (quote != '\0' && ch == '\\') {
                escaped = true;
                continue;
            }

            if (quote != '\0') {
                if (ch == quote)
                    quote = '\0';
                else
                    token.push_back(ch);
                continue;
            }

            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }

            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!token.empty()) {
                    tokens.push_back(std::move(token));
                    token.clear();
                }
                continue;
            }

            token.push_back(ch);
        }

        if (!token.empty())
            tokens.push_back(std::move(token));

        return tokens;
    }

    std::optional<int64_t> parseBitrate(std::string value) {
        value = lowerCopy(trim(std::move(value)));
        if (value.empty())
            return std::nullopt;

        double multiplier = 1.0;
        if (auto last = value.back(); last == 'k' || last == 'm' || last == 'g') {
            if (last == 'k')
                multiplier = 1000.0;
            else if (last == 'm')
                multiplier = 1000.0 * 1000.0;
            else
                multiplier = 1000.0 * 1000.0 * 1000.0;
            value.pop_back();
        }

        try {
            return static_cast<int64_t>(std::stod(value) * multiplier);
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
    }

    struct ParsedFFmpegArgs {
        AVDictionary* codecOptions = nullptr;
        AVDictionary* formatOptions = nullptr;
        std::string codecName;
        std::string filters;
        std::optional<int64_t> bitrate;
        AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
        std::vector<FFmpegOption> colorOptions;

        ParsedFFmpegArgs() = default;
        ParsedFFmpegArgs(const ParsedFFmpegArgs&) = delete;
        ParsedFFmpegArgs& operator=(const ParsedFFmpegArgs&) = delete;

        ~ParsedFFmpegArgs() {
            av_dict_free(&codecOptions);
            av_dict_free(&formatOptions);
        }
    };

    void setDictOption(AVDictionary** dict, std::string key, const std::string& value) {
        key = stripStreamSuffix(stripOptionPrefix(trim(std::move(key))));
        if (!key.empty())
            av_dict_set(dict, key.c_str(), value.c_str(), 0);
    }

    void appendExplicitOptions(AVDictionary** dict, const std::vector<FFmpegOption>& options) {
        for (auto const& option : options)
            setDictOption(dict, option.key, option.value);
    }

    bool isFormatOption(const std::string& key) {
        return key == "movflags" || key == "fflags" || key == "flush_packets" ||
               key == "max_interleave_delta" || key == "brand" ||
               key == "frag_duration" || key == "frag_size" || key == "write_tmcd";
    }

    bool isIgnoredOption(const std::string& key) {
        return key == "y" || key == "n" || key == "hide_banner" ||
               key == "nostdin" || key == "stats" || key == "nostats" ||
               key == "loglevel" || key == "i" || key == "f" || key == "r" ||
               key == "s" || key == "ss" || key == "t" || key == "to" ||
               key == "frames" || key == "vframes" || key == "an" ||
               key == "vn" || key == "sn" || key == "dn";
    }

    std::optional<std::string> parseCliArgs(const std::string& args, ParsedFFmpegArgs& parsed, bool formatOnly) {
        auto tokens = splitArgs(args);

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (!isOptionToken(tokens[i]))
                continue;

            auto fullKey = lowerCopy(stripOptionPrefix(tokens[i]));
            auto key = stripStreamSuffix(fullKey);
            std::string value;
            if (i + 1 < tokens.size() && !isOptionToken(tokens[i + 1]))
                value = tokens[++i];

            if (auto colon = fullKey.find(':'); colon != std::string::npos &&
                colon + 1 < fullKey.size() && fullKey[colon + 1] != 'v') {
                continue;
            }

            if (isIgnoredOption(key))
                continue;

            if (fullKey == "c:v" || fullKey == "codec:v" || key == "vcodec" || fullKey == "c" || fullKey == "codec") {
                if (!value.empty())
                    parsed.codecName = value;
                continue;
            }

            if (fullKey == "b:v" || key == "vb" || fullKey == "b") {
                auto bitrate = parseBitrate(value);
                if (!bitrate)
                    return "Could not parse bitrate from '" + value + "'.";
                parsed.bitrate = *bitrate;
                continue;
            }

            if (key == "pix_fmt" || key == "pixel_format") {
                auto pixFmt = av_get_pix_fmt(value.c_str());
                if (pixFmt == AV_PIX_FMT_NONE)
                    return "Unknown pixel format '" + value + "'.";
                parsed.pixelFormat = pixFmt;
                continue;
            }

            if (key == "vf" || fullKey == "filter:v" || key == "filter_complex") {
                parsed.filters = appendFilter(parsed.filters, value);
                continue;
            }

            if (key == "color_range" || key == "colorspace" ||
                key == "color_primaries" || key == "color_trc") {
                parsed.colorOptions.push_back({key, value});
                continue;
            }

            if (formatOnly || isFormatOption(key))
                setDictOption(&parsed.formatOptions, key, value);
            else
                setDictOption(&parsed.codecOptions, key, value);
        }

        return std::nullopt;
    }

    void applyColorOptions(AVCodecContext* context, const ParsedFFmpegArgs& args) {
        for (auto const& option : args.colorOptions) {
            auto value = lowerCopy(option.value);
            if (option.key == "color_range") {
                auto range = static_cast<AVColorRange>(av_color_range_from_name(value.c_str()));
                if (value == "pc" || value == "full" || value == "jpeg")
                    range = AVCOL_RANGE_JPEG;
                else if (value == "tv" || value == "limited" || value == "mpeg")
                    range = AVCOL_RANGE_MPEG;
                if (range != AVCOL_RANGE_UNSPECIFIED || value == "unknown" || value == "unspecified")
                    context->color_range = range;
            }
            else if (option.key == "colorspace") {
                auto space = static_cast<AVColorSpace>(av_color_space_from_name(value.c_str()));
                if (space != AVCOL_SPC_UNSPECIFIED || value == "unknown" || value == "unspecified")
                    context->colorspace = space;
            }
            else if (option.key == "color_primaries") {
                auto primaries = static_cast<AVColorPrimaries>(av_color_primaries_from_name(value.c_str()));
                if (primaries != AVCOL_PRI_UNSPECIFIED || value == "unknown" || value == "unspecified")
                    context->color_primaries = primaries;
            }
            else if (option.key == "color_trc") {
                auto trc = static_cast<AVColorTransferCharacteristic>(av_color_transfer_from_name(value.c_str()));
                if (trc != AVCOL_TRC_UNSPECIFIED || value == "unknown" || value == "unspecified")
                    context->color_trc = trc;
            }
        }
    }

    std::string pixelFormatName(AVPixelFormat format) {
        if (auto name = av_get_pix_fmt_name(format))
            return name;
        return std::to_string(static_cast<int>(format));
    }
}

std::vector<std::string> Recorder::getAvailableCodecs() {
    std::vector<std::string> vec;

    void* iter = nullptr;
    const AVCodec * codec;

    while ((codec = av_codec_iterate(&iter))) {
        if(codec->type == AVMEDIA_TYPE_VIDEO &&
                (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC || codec->id == AV_CODEC_ID_VP8 || codec->id == AV_CODEC_ID_VP9 || codec->id == AV_CODEC_ID_AV1 || codec->id == AV_CODEC_ID_MPEG4) &&
                avcodec_find_encoder_by_name(codec->name) != nullptr && codec->pix_fmts && std::ranges::find(vec, std::string(codec->name)) == vec.end())
            vec.emplace_back(codec->name);
    }
    
    return vec;
}

const AVCodec* getCodecByName(const std::string& name) {
    void* iter = nullptr;
    const AVCodec * codec;
    while ((codec = av_codec_iterate(&iter))) {
        if(codec->type == AVMEDIA_TYPE_VIDEO && std::string(codec->name) == name)
            return codec;
    }
    return nullptr;
}

geode::Result<> Recorder::Impl::init(const RenderSettings& settings) {
    RenderSettingsCml extended;
    static_cast<RenderSettings&>(extended) = settings;
    return init(extended);
}

geode::Result<> Recorder::Impl::init(const RenderSettingsCml& settings) {
    ParsedFFmpegArgs ffArgs;
    if (auto err = parseCliArgs(settings.m_encoderArgs, ffArgs, false))
        return geode::Err(*err);
    if (auto err = parseCliArgs(settings.m_formatArgs, ffArgs, true))
        return geode::Err(*err);

    appendExplicitOptions(&ffArgs.codecOptions, settings.m_codecOptions);
    appendExplicitOptions(&ffArgs.formatOptions, settings.m_formatOptions);

    auto codecName = trim(ffArgs.codecName.empty() ? settings.m_codec : ffArgs.codecName);
    if (codecName.empty())
        return geode::Err("Codec is empty.");

    int ret = avformat_alloc_output_context2(&m_formatContext, NULL, NULL, settings.m_outputFile.string().c_str());
    if (!m_formatContext)
        return geode::Err("Could not create output context: " + utils::getErrorString(ret));

    m_codec = getCodecByName(codecName);
    if (!m_codec)
        return geode::Err("Could not find encoder '" + codecName + "'.");

    m_videoStream = avformat_new_stream(m_formatContext, m_codec);
    if (!m_videoStream)
        return geode::Err("Could not create video stream.");

    m_codecContext = avcodec_alloc_context3(m_codec);
    if (!m_codecContext)
        return geode::Err("Could not allocate video codec context.");

    if(settings.m_hardwareAccelerationType != HardwareAccelerationType::NONE && (ret = av_hwdevice_ctx_create(&m_hwDevice, (AVHWDeviceType)settings.m_hardwareAccelerationType, NULL, NULL, 0)); ret < 0)
        return geode::Err("Could not create hardware device context: " + utils::getErrorString(ret));

    m_codecContext->hw_device_ctx = m_hwDevice ? av_buffer_ref(m_hwDevice) : nullptr;
    m_codecContext->codec_id = m_codec->id;
    m_codecContext->bit_rate = ffArgs.bitrate.value_or(settings.m_bitrate);
    m_codecContext->width = settings.m_width;
    m_codecContext->height = settings.m_height;
    m_codecContext->time_base = AVRational{1, settings.m_fps};
    m_codecContext->pix_fmt = AV_PIX_FMT_NONE;
    m_videoStream->time_base = m_codecContext->time_base;

    if(!m_codec->pix_fmts)
        return geode::Err("Codec does not have any supported pixel formats.");

    auto inputPixelFormat = static_cast<AVPixelFormat>(settings.m_pixelFormat);
    auto requestedPixelFormat = ffArgs.pixelFormat != AV_PIX_FMT_NONE ? ffArgs.pixelFormat : inputPixelFormat;
    bool explicitPixelFormat = ffArgs.pixelFormat != AV_PIX_FMT_NONE;

    if (const AVPixelFormat *pix_fmt = m_codec->pix_fmts) {
        while (*pix_fmt != AV_PIX_FMT_NONE) {
            if(*pix_fmt == AV_PIX_FMT_MEDIACODEC && (!explicitPixelFormat || requestedPixelFormat == AV_PIX_FMT_NV12)) {
                m_codecContext->pix_fmt = AV_PIX_FMT_NV12; 
                break;
            }
            if(*pix_fmt == requestedPixelFormat)
                m_codecContext->pix_fmt = *pix_fmt;
            ++pix_fmt;
        }
    }
    if(m_codecContext->pix_fmt == AV_PIX_FMT_NONE) {
        if (explicitPixelFormat)
            return geode::Err("Codec '" + codecName + "' does not support pixel format '" + pixelFormatName(requestedPixelFormat) + "'.");
        geode::log::info("Codec {} does not support pixel format {}, defaulting to codec's format", codecName, pixelFormatName(requestedPixelFormat));
        m_codecContext->pix_fmt = m_codec->pix_fmts[0];
    }
    else
        geode::log::info("Codec {} supports pixel format {}.", codecName, pixelFormatName(m_codecContext->pix_fmt));

    applyColorOptions(m_codecContext, ffArgs);

    if (m_formatContext->oformat->flags & AVFMT_GLOBALHEADER)
        m_codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (ret = avcodec_open2(m_codecContext, m_codec, &ffArgs.codecOptions); ret < 0)
        return geode::Err("Could not open codec: " + utils::getErrorString(ret));

    if (ret = avcodec_parameters_from_context(m_videoStream->codecpar, m_codecContext); ret < 0)
        return geode::Err("Could not copy codec parameters: " + utils::getErrorString(ret));

    if (!(m_formatContext->oformat->flags & AVFMT_NOFILE)) {
        if (ret = avio_open(&m_formatContext->pb, settings.m_outputFile.string().c_str(), AVIO_FLAG_WRITE); ret < 0)
            return geode::Err("Could not open output file: " + utils::getErrorString(ret));
    }

    if (ret = avformat_write_header(m_formatContext, &ffArgs.formatOptions); ret < 0)
        return geode::Err("Could not write header: " + utils::getErrorString(ret));
    m_headerWritten = true;

    m_frame = av_frame_alloc();
    if(!m_frame)
        return geode::Err("Could not allocate raw frame.");
    m_frame->format = inputPixelFormat;
    m_frame->width = m_codecContext->width;
    m_frame->height = m_codecContext->height;

    m_convertedFrame = av_frame_alloc();
    if(!m_convertedFrame)
        return geode::Err("Could not allocate converted frame.");
    m_convertedFrame->format = m_codecContext->pix_fmt;
    m_convertedFrame->width = m_codecContext->width;
    m_convertedFrame->height = m_codecContext->height;
    if(ret = av_image_alloc(m_convertedFrame->data, m_convertedFrame->linesize, m_convertedFrame->width, m_convertedFrame->height, m_codecContext->pix_fmt, 32); ret < 0)
        return geode::Err("Could not allocate raw picture buffer: " + utils::getErrorString(ret));

    m_filteredFrame = av_frame_alloc();
    if(!m_filteredFrame)
        return geode::Err("Could not allocate filtered frame.");

    m_packet = av_packet_alloc();
    if(!m_packet)
        return geode::Err("Could not allocate packet.");

    m_packet->data = nullptr;
    m_packet->size = 0;

    std::string filterChain = normalizeFilters(settings.m_colorspaceFilters);
    filterChain = appendFilter(filterChain, settings.m_videoFilters);
    filterChain = appendFilter(filterChain, ffArgs.filters);
    if(settings.m_doVerticalFlip)
        filterChain = appendFilter("vflip", filterChain);

    if(!filterChain.empty()) {
        m_filterGraph = avfilter_graph_alloc();
        if (!m_filterGraph)
            return geode::Err("Could not allocate filter graph.");

        const AVFilter* buffersrc = avfilter_get_by_name("buffer");
        const AVFilter* buffersink = avfilter_get_by_name("buffersink");

        auto aspectRatio = m_codecContext->sample_aspect_ratio;
        if (aspectRatio.num == 0 || aspectRatio.den == 0)
            aspectRatio = AVRational{1, 1};

        char args[512];
        snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            m_codecContext->width, m_codecContext->height, m_codecContext->pix_fmt,
            m_codecContext->time_base.num, m_codecContext->time_base.den,
            aspectRatio.num, aspectRatio.den);

        if(ret = avfilter_graph_create_filter(&m_buffersrcCtx, buffersrc, "in", args, nullptr, m_filterGraph); ret < 0) {
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Could not create input for filter graph: " + utils::getErrorString(ret));
        }

        if(ret = avfilter_graph_create_filter(&m_buffersinkCtx, buffersink, "out", nullptr, nullptr, m_filterGraph); ret < 0) {
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Could not create output for filter graph: " + utils::getErrorString(ret));
        }

        AVPixelFormat pixelFormats[] = {m_codecContext->pix_fmt, AV_PIX_FMT_NONE};
        if (ret = av_opt_set_int_list(m_buffersinkCtx, "pix_fmts", pixelFormats, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN); ret < 0) {
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Could not configure filter output format: " + utils::getErrorString(ret));
        }

        AVFilterInOut* graphInputs = avfilter_inout_alloc();
        AVFilterInOut* graphOutputs = avfilter_inout_alloc();
        if (!graphInputs || !graphOutputs) {
            avfilter_inout_free(&graphInputs);
            avfilter_inout_free(&graphOutputs);
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Could not allocate filter pads.");
        }

        graphOutputs->name = av_strdup("in");
        graphOutputs->filter_ctx = m_buffersrcCtx;
        graphOutputs->pad_idx = 0;
        graphOutputs->next = nullptr;

        graphInputs->name = av_strdup("out");
        graphInputs->filter_ctx = m_buffersinkCtx;
        graphInputs->pad_idx = 0;
        graphInputs->next = nullptr;
        if (!graphOutputs->name || !graphInputs->name) {
            avfilter_inout_free(&graphInputs);
            avfilter_inout_free(&graphOutputs);
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Could not allocate filter pad names.");
        }

        ret = avfilter_graph_parse_ptr(m_filterGraph, filterChain.c_str(),
                                       &graphInputs, &graphOutputs, nullptr);
        avfilter_inout_free(&graphInputs);
        avfilter_inout_free(&graphOutputs);
        if(ret < 0) {
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Could not parse filter graph '" + filterChain + "': " + utils::getErrorString(ret));
        }

        if (ret = avfilter_graph_config(m_filterGraph, nullptr); ret < 0) {
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Could not configure filter graph '" + filterChain + "': " + utils::getErrorString(ret));
        }

        if (av_buffersink_get_w(m_buffersinkCtx) != m_codecContext->width ||
            av_buffersink_get_h(m_buffersinkCtx) != m_codecContext->height) {
            avfilter_graph_free(&m_filterGraph);
            return geode::Err("Filter graph output size must match encoder size.");
        }
    }

    if(inputPixelFormat != m_codecContext->pix_fmt) {
        m_swsCtx = sws_getContext(m_codecContext->width, m_codecContext->height, inputPixelFormat, m_codecContext->width,
            m_codecContext->height, m_codecContext->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        if (!m_swsCtx)
            return geode::Err("Could not create sws context.");
    }

    m_frameCount = 0;
    auto expectedSize = av_image_get_buffer_size(inputPixelFormat, m_frame->width, m_frame->height, 1);
    if (expectedSize < 0)
        return geode::Err("Could not calculate raw frame size: " + utils::getErrorString(expectedSize));
    m_expectedSize = static_cast<size_t>(expectedSize);

    m_init = true;

    return geode::Ok();
}

geode::Result<> Recorder::Impl::writeFrame(std::span<uint8_t const> frameData) {
    if (!m_init || !m_frame)
        return geode::Err("Recorder is not initialized.");

    if(frameData.size() != m_expectedSize)
        return geode::Err("Frame data size does not match expected dimensions.");

    int ret = av_image_fill_arrays(
        m_frame->data,
        m_frame->linesize,
        frameData.data(),
        (AVPixelFormat)m_frame->format,
        m_frame->width,
        m_frame->height,
        1
    );

    if (ret < 0)
        return geode::Err("Failed to fill image arrays: " + utils::getErrorString(ret));

    if(m_swsCtx) {
        sws_scale(
            m_swsCtx, m_frame->data, m_frame->linesize, 0, m_frame->height,
            m_convertedFrame->data, m_convertedFrame->linesize);
    }
    else {
        av_frame_copy(m_convertedFrame, m_frame);
        av_frame_copy_props(m_convertedFrame, m_frame);
    }

    AVFrame* frameToEncode = m_convertedFrame;

    if(m_buffersrcCtx) {
        geode::Result<> res = filterFrame(m_convertedFrame, m_filteredFrame);

        if(res.isErr())
            return res;

        frameToEncode = m_filteredFrame;
    }

    frameToEncode->pts = m_frameCount++;

    ret = avcodec_send_frame(m_codecContext, frameToEncode);
    if (ret < 0)
        return geode::Err("Error while sending frame: " + utils::getErrorString(ret));

    while (ret >= 0) {
        ret = avcodec_receive_packet(m_codecContext, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return geode::Err("Error while receiving packet: " + utils::getErrorString(ret));

        av_packet_rescale_ts(m_packet, m_codecContext->time_base, m_videoStream->time_base);
        m_packet->stream_index = m_videoStream->index;

        av_interleaved_write_frame(m_formatContext, m_packet);
        av_packet_unref(m_packet);
    }

    av_frame_unref(m_filteredFrame);

    return geode::Ok();
}

geode::Result<> Recorder::Impl::filterFrame(AVFrame* inputFrame, AVFrame* outputFrame) {
    int ret = 0;
    if (ret = av_buffersrc_add_frame_flags(m_buffersrcCtx, inputFrame, AV_BUFFERSRC_FLAG_KEEP_REF); ret < 0) {
        return geode::Err("Could not feed frame to filter graph: " + utils::getErrorString(ret));
    }

    if (ret = av_buffersink_get_frame(m_buffersinkCtx, outputFrame); ret < 0) {
        av_frame_unref(outputFrame);
        return geode::Err("Could not retrieve frame from filter graph: " + utils::getErrorString(ret));
    }

    return geode::Ok();
}

void Recorder::Impl::stop() {
    if(m_codecContext && m_videoStream && m_formatContext && m_packet && m_headerWritten) {
        avcodec_send_frame(m_codecContext, nullptr);
        while (avcodec_receive_packet(m_codecContext, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_codecContext->time_base, m_videoStream->time_base);
            m_packet->stream_index = m_videoStream->index;
            av_interleaved_write_frame(m_formatContext, m_packet);
            av_packet_unref(m_packet);
        }
    }

    if(m_formatContext && m_headerWritten)
        av_write_trailer(m_formatContext);

    if(m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    if(m_filterGraph) {
        avfilter_graph_free(&m_filterGraph);
        m_buffersrcCtx = nullptr;
        m_buffersinkCtx = nullptr;
    }

    if(m_codecContext)
        avcodec_free_context(&m_codecContext);

    if(m_frame)
        av_frame_free(&m_frame);
    if(m_convertedFrame) {
        if (m_convertedFrame->data[0])
            av_freep(&m_convertedFrame->data[0]);
        av_frame_free(&m_convertedFrame);
    }

    if(m_formatContext) {
        if (!(m_formatContext->oformat->flags & AVFMT_NOFILE) && m_formatContext->pb)
            avio_closep(&m_formatContext->pb);
        avformat_free_context(m_formatContext);
        m_formatContext = nullptr;
    }

    if(m_filteredFrame)
        av_frame_free(&m_filteredFrame);

    if (m_hwDevice)
        av_buffer_unref(&m_hwDevice);

    if(m_packet)
        av_packet_free(&m_packet);

    m_codec = nullptr;
    m_videoStream = nullptr;
    m_frameCount = 0;
    m_expectedSize = 0;
    m_headerWritten = false;
    m_init = false;
}

END_FFMPEG_NAMESPACE_V
