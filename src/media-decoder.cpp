#include "media-decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <util/base.h>

namespace obs_airplay {

namespace {

// Exact AirPlay audio codec extradata / magic cookies (44100 Hz stereo)
constexpr uint8_t kAacEldExtradata[4] = {0xF8, 0xE8, 0x50, 0x00};
constexpr uint8_t kAacLcExtradata[2] = {0x12, 0x10};
constexpr uint8_t kAlacMagicCookie[36] = {
	0x00, 0x00, 0x00, 0x24, 'a',  'l',  'a',  'c',  0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x60, 0x00, 0x10, 0x28, 0x0A, 0x0E, 0x02, 0x00, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAC, 0x44,
};

enum video_colorspace map_colorspace(enum AVColorSpace av_cs)
{
	switch (av_cs) {
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		return VIDEO_CS_709;
	default:
		return VIDEO_CS_709;
	}
}

enum video_format map_direct_video_format(enum AVPixelFormat pix_fmt)
{
	switch (pix_fmt) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUVJ422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUVJ444P:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

} // namespace

MediaDecoder::MediaDecoder(VideoFrameCallback video_cb, AudioFrameCallback audio_cb)
	: video_cb_(std::move(video_cb)), audio_cb_(std::move(audio_cb))
{
	video_pkt_ = av_packet_alloc();
	video_frame_ = av_frame_alloc();
	sw_frame_ = av_frame_alloc();
	audio_pkt_ = av_packet_alloc();
	audio_frame_ = av_frame_alloc();
}

MediaDecoder::~MediaDecoder()
{
	reset_all();
	if (video_pkt_)
		av_packet_free(&video_pkt_);
	if (video_frame_)
		av_frame_free(&video_frame_);
	if (sw_frame_)
		av_frame_free(&sw_frame_);
	if (audio_pkt_)
		av_packet_free(&audio_pkt_);
	if (audio_frame_)
		av_frame_free(&audio_frame_);
}

void MediaDecoder::configure(const DecoderConfig &config)
{
	bool prev_hw = hw_decode_enabled_.exchange(config.hardware_decode, std::memory_order_relaxed);
	audio_enabled_.store(config.enable_audio, std::memory_order_relaxed);
	sync_volume_.store(config.sync_device_volume, std::memory_order_relaxed);

	if (prev_hw != config.hardware_decode) {
		std::lock_guard<std::mutex> lock(video_mutex_);
		destroy_video_decoder_locked();
	}
}

void MediaDecoder::set_volume_gain(float linear_gain)
{
	if (linear_gain < 0.0f)
		linear_gain = 0.0f;
	if (linear_gain > 1.0f)
		linear_gain = 1.0f;
	volume_gain_.store(linear_gain, std::memory_order_relaxed);
}

enum AVPixelFormat MediaDecoder::get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
	auto *self = static_cast<MediaDecoder *>(ctx->opaque);
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
		if (self && *p == self->hw_pix_fmt_) {
			return *p;
		}
	}
	blog(LOG_WARNING, "[obs-airplay] Hardware pixel format unavailable; falling back to software decode.");
	if (self) {
		self->hw_decode_active_ = false;
	}
	return pix_fmts[0];
}

void MediaDecoder::destroy_video_decoder_locked()
{
	if (video_ctx_) {
		avcodec_free_context(&video_ctx_);
	}
	if (hw_device_ctx_) {
		av_buffer_unref(&hw_device_ctx_);
	}
	if (sws_ctx_) {
		sws_freeContext(sws_ctx_);
		sws_ctx_ = nullptr;
	}
	sws_src_w_ = 0;
	sws_src_h_ = 0;
	sws_src_fmt_ = AV_PIX_FMT_NONE;
	hw_pix_fmt_ = AV_PIX_FMT_NONE;
	hw_decode_active_ = false;
}

bool MediaDecoder::ensure_video_decoder(bool is_h265)
{
	if (video_ctx_ && video_is_h265_ == is_h265) {
		return true;
	}

	destroy_video_decoder_locked();

	const enum AVCodecID codec_id = is_h265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec) {
		blog(LOG_ERROR, "[obs-airplay] FFmpeg decoder not found for %s", is_h265 ? "HEVC" : "H.264");
		return false;
	}

	video_ctx_ = avcodec_alloc_context3(codec);
	if (!video_ctx_) {
		blog(LOG_ERROR, "[obs-airplay] Failed to allocate video AVCodecContext");
		return false;
	}

	video_ctx_->opaque = this;
	video_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	video_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

	// Slice threading keeps latency near-zero (avoid multi-frame reordering delay)
	video_ctx_->thread_count = 4;
	video_ctx_->thread_type = FF_THREAD_SLICE;

	// Optional hardware acceleration on Windows (D3D11VA)
#ifdef _WIN32
	if (hw_decode_enabled_.load(std::memory_order_relaxed)) {
		enum AVPixelFormat found_hw_fmt = AV_PIX_FMT_NONE;
		for (int i = 0;; ++i) {
			const AVCodecHWConfig *hw_cfg = avcodec_get_hw_config(codec, i);
			if (!hw_cfg)
				break;
			if ((hw_cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
			    hw_cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
				found_hw_fmt = hw_cfg->pix_fmt;
				break;
			}
		}

		if (found_hw_fmt != AV_PIX_FMT_NONE) {
			int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_D3D11VA,
							 nullptr, nullptr, 0);
			if (err >= 0 && hw_device_ctx_) {
				hw_pix_fmt_ = found_hw_fmt;
				video_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
				video_ctx_->get_format = &MediaDecoder::get_hw_format;
				hw_decode_active_ = true;
				blog(LOG_INFO, "[obs-airplay] Using D3D11VA hardware decoder for %s",
				     is_h265 ? "HEVC" : "H.264");
			} else {
				blog(LOG_WARNING,
				     "[obs-airplay] D3D11VA init failed (err=%d); using CPU software decoder",
				     err);
			}
		}
	}
#endif

	int ret = avcodec_open2(video_ctx_, codec, nullptr);
	if (ret < 0) {
		blog(LOG_ERROR, "[obs-airplay] avcodec_open2 failed for video (%s): %d",
		     is_h265 ? "HEVC" : "H.264", ret);
		destroy_video_decoder_locked();
		return false;
	}

	video_is_h265_ = is_h265;
	blog(LOG_INFO, "[obs-airplay] Video decoder initialized (%s, hw=%s)",
	     is_h265 ? "HEVC/H.265" : "AVC/H.264",
	     hw_decode_active_ ? "D3D11VA" : "Software");
	return true;
}

bool MediaDecoder::decode_video(const uint8_t *data, int data_len, bool is_h265, uint64_t obs_timestamp_ns)
{
	if (!data || data_len <= 0)
		return false;

	std::lock_guard<std::mutex> lock(video_mutex_);
	if (!ensure_video_decoder(is_h265)) {
		return false;
	}

	av_packet_unref(video_pkt_);
	if (av_new_packet(video_pkt_, data_len) < 0) {
		return false;
	}
	std::memcpy(video_pkt_->data, data, static_cast<size_t>(data_len));
	video_pkt_->pts = static_cast<int64_t>(obs_timestamp_ns);
	video_pkt_->dts = video_pkt_->pts;

	int ret = avcodec_send_packet(video_ctx_, video_pkt_);
	av_packet_unref(video_pkt_);
	if (ret < 0) {
		// If hardware decode rejected a stream profile, fall back transparently to software
		if (hw_decode_active_) {
			blog(LOG_WARNING, "[obs-airplay] D3D11VA send_packet failed (%d); switching to software decode", ret);
			hw_decode_enabled_.store(false, std::memory_order_relaxed);
			destroy_video_decoder_locked();
		}
		return false;
	}

	bool emitted = false;
	while (true) {
		av_frame_unref(video_frame_);
		ret = avcodec_receive_frame(video_ctx_, video_frame_);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		}
		if (ret < 0) {
			break;
		}

		AVFrame *output_frame = video_frame_;
		if (hw_decode_active_ && video_frame_->format == hw_pix_fmt_) {
			av_frame_unref(sw_frame_);
			if (av_hwframe_transfer_data(sw_frame_, video_frame_, 0) < 0) {
				blog(LOG_WARNING, "[obs-airplay] Hardware frame transfer failed; switching to software decode");
				hw_decode_enabled_.store(false, std::memory_order_relaxed);
				destroy_video_decoder_locked();
				break;
			}
			sw_frame_->color_range = video_frame_->color_range;
			sw_frame_->colorspace = video_frame_->colorspace;
			sw_frame_->pts = video_frame_->pts;
			output_frame = sw_frame_;
		}

		uint64_t frame_ts = (output_frame->pts != AV_NOPTS_VALUE && output_frame->pts >= 0)
					    ? static_cast<uint64_t>(output_frame->pts)
					    : obs_timestamp_ns;
		emit_video_frame_locked(output_frame, frame_ts);
		emitted = true;
	}

	return emitted;
}

void MediaDecoder::emit_video_frame_locked(AVFrame *frame, uint64_t obs_timestamp_ns)
{
	if (!video_cb_ || !frame || frame->width <= 0 || frame->height <= 0)
		return;

	last_width_.store(static_cast<uint32_t>(frame->width), std::memory_order_relaxed);
	last_height_.store(static_cast<uint32_t>(frame->height), std::memory_order_relaxed);

	const auto av_fmt = static_cast<enum AVPixelFormat>(frame->format);
	enum video_format obs_fmt = map_direct_video_format(av_fmt);

	struct obs_source_frame obs_frame = {};
	obs_frame.width = static_cast<uint32_t>(frame->width);
	obs_frame.height = static_cast<uint32_t>(frame->height);
	obs_frame.timestamp = obs_timestamp_ns;

	const bool is_full_range =
		(frame->color_range == AVCOL_RANGE_JPEG) ||
		(av_fmt == AV_PIX_FMT_YUVJ420P) ||
		(av_fmt == AV_PIX_FMT_YUVJ422P) ||
		(av_fmt == AV_PIX_FMT_YUVJ444P);
	const enum video_range_type range = is_full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	const enum video_colorspace cs = map_colorspace(frame->colorspace);

	if (obs_fmt != VIDEO_FORMAT_NONE) {
		obs_frame.format = obs_fmt;
		for (int i = 0; i < MAX_AV_PLANES && i < 4; ++i) {
			obs_frame.data[i] = frame->data[i];
			obs_frame.linesize[i] = static_cast<uint32_t>(frame->linesize[i]);
		}
	} else {
		// Fallback conversion (e.g., 10-bit HEVC HDR -> NV12) via swscale
		constexpr enum AVPixelFormat dst_av_fmt = AV_PIX_FMT_NV12;
		obs_frame.format = VIDEO_FORMAT_NV12;

		if (!sws_ctx_ || sws_src_w_ != frame->width || sws_src_h_ != frame->height ||
		    sws_src_fmt_ != av_fmt) {
			if (sws_ctx_)
				sws_freeContext(sws_ctx_);

			sws_ctx_ = sws_getContext(frame->width, frame->height, av_fmt,
						  frame->width, frame->height, dst_av_fmt,
						  SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (!sws_ctx_)
				return;

			int buf_size = av_image_get_buffer_size(dst_av_fmt, frame->width, frame->height, 32);
			if (buf_size <= 0)
				return;

			sws_buffer_.resize(static_cast<size_t>(buf_size));
			av_image_fill_arrays(sws_data_, sws_linesize_, sws_buffer_.data(),
					     dst_av_fmt, frame->width, frame->height, 32);
			sws_src_w_ = frame->width;
			sws_src_h_ = frame->height;
			sws_src_fmt_ = av_fmt;
		}

		sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height,
			  sws_data_, sws_linesize_);

		for (int i = 0; i < 4; ++i) {
			obs_frame.data[i] = sws_data_[i];
			obs_frame.linesize[i] = static_cast<uint32_t>(sws_linesize_[i]);
		}
	}

	obs_frame.full_range = is_full_range;
	video_format_get_parameters_for_format(cs, range, obs_frame.format,
					       obs_frame.color_matrix,
					       obs_frame.color_range_min,
					       obs_frame.color_range_max);

	video_cb_(&obs_frame);
}

void MediaDecoder::destroy_audio_decoder_locked()
{
	if (audio_ctx_) {
		avcodec_free_context(&audio_ctx_);
	}
	if (swr_ctx_) {
		swr_free(&swr_ctx_);
	}
	swr_in_rate_ = 0;
	swr_in_fmt_ = AV_SAMPLE_FMT_NONE;
	swr_in_channels_ = 0;
	active_audio_ct_ = 0;
}

bool MediaDecoder::ensure_audio_decoder(uint8_t ct)
{
	if (ct == 1) {
		// Uncompressed PCM S16LE needs no AVCodecContext
		if (active_audio_ct_ != 1) {
			destroy_audio_decoder_locked();
			active_audio_ct_ = 1;
		}
		return true;
	}

	if (audio_ctx_ && active_audio_ct_ == ct) {
		return true;
	}

	destroy_audio_decoder_locked();

	enum AVCodecID codec_id = AV_CODEC_ID_NONE;
	const uint8_t *extradata = nullptr;
	int extradata_size = 0;
	const char *codec_label = "Unknown";

	switch (ct) {
	case 8:
		codec_id = AV_CODEC_ID_AAC;
		extradata = kAacEldExtradata;
		extradata_size = static_cast<int>(sizeof(kAacEldExtradata));
		codec_label = "AAC-ELD";
		break;
	case 4:
		codec_id = AV_CODEC_ID_AAC;
		extradata = kAacLcExtradata;
		extradata_size = static_cast<int>(sizeof(kAacLcExtradata));
		codec_label = "AAC-LC";
		break;
	case 2:
		codec_id = AV_CODEC_ID_ALAC;
		extradata = kAlacMagicCookie;
		extradata_size = static_cast<int>(sizeof(kAlacMagicCookie));
		codec_label = "ALAC";
		break;
	default:
		blog(LOG_WARNING, "[obs-airplay] Unsupported AirPlay audio compression type ct=%u", ct);
		return false;
	}

	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec) {
		blog(LOG_ERROR, "[obs-airplay] FFmpeg audio decoder not found for %s", codec_label);
		return false;
	}

	audio_ctx_ = avcodec_alloc_context3(codec);
	if (!audio_ctx_) {
		return false;
	}

	audio_ctx_->sample_rate = 44100;
	av_channel_layout_default(&audio_ctx_->ch_layout, 2);

	if (extradata && extradata_size > 0) {
		audio_ctx_->extradata = static_cast<uint8_t *>(
			av_mallocz(static_cast<size_t>(extradata_size) + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!audio_ctx_->extradata) {
			destroy_audio_decoder_locked();
			return false;
		}
		std::memcpy(audio_ctx_->extradata, extradata, static_cast<size_t>(extradata_size));
		audio_ctx_->extradata_size = extradata_size;
	}

	int ret = avcodec_open2(audio_ctx_, codec, nullptr);
	if (ret < 0) {
		blog(LOG_ERROR, "[obs-airplay] avcodec_open2 failed for audio (%s): %d", codec_label, ret);
		destroy_audio_decoder_locked();
		return false;
	}

	active_audio_ct_ = ct;
	blog(LOG_INFO, "[obs-airplay] Audio decoder initialized (%s, 44100 Hz stereo)", codec_label);
	return true;
}

bool MediaDecoder::decode_audio(const uint8_t *data, int data_len, uint8_t ct, uint64_t obs_timestamp_ns)
{
	if (!audio_enabled_.load(std::memory_order_relaxed))
		return false;
	if (!data || data_len <= 0)
		return false;

	std::lock_guard<std::mutex> lock(audio_mutex_);
	if (!ensure_audio_decoder(ct)) {
		return false;
	}

	if (ct == 1) {
		emit_pcm_s16le_locked(data, data_len, obs_timestamp_ns);
		return true;
	}

	av_packet_unref(audio_pkt_);
	if (av_new_packet(audio_pkt_, data_len) < 0) {
		return false;
	}
	std::memcpy(audio_pkt_->data, data, static_cast<size_t>(data_len));
	audio_pkt_->pts = static_cast<int64_t>(obs_timestamp_ns);

	int ret = avcodec_send_packet(audio_ctx_, audio_pkt_);
	av_packet_unref(audio_pkt_);
	if (ret < 0) {
		return false;
	}

	bool emitted = false;
	while (true) {
		av_frame_unref(audio_frame_);
		ret = avcodec_receive_frame(audio_ctx_, audio_frame_);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		}
		if (ret < 0) {
			break;
		}

		emit_audio_frame_locked(audio_frame_, obs_timestamp_ns);
		emitted = true;
	}

	return emitted;
}

void MediaDecoder::emit_pcm_s16le_locked(const uint8_t *data, int data_len, uint64_t obs_timestamp_ns)
{
	if (!audio_cb_)
		return;
	const int samples = data_len / 4; // 2 channels * 2 bytes
	if (samples <= 0)
		return;

	audio_left_.resize(static_cast<size_t>(samples));
	audio_right_.resize(static_cast<size_t>(samples));

	const float gain = sync_volume_.load(std::memory_order_relaxed)
				   ? volume_gain_.load(std::memory_order_relaxed)
				   : 1.0f;
	const float scale = gain / 32768.0f;

	const auto *src = reinterpret_cast<const int16_t *>(data);
	for (int i = 0; i < samples; ++i) {
		audio_left_[i] = static_cast<float>(src[i * 2]) * scale;
		audio_right_[i] = static_cast<float>(src[i * 2 + 1]) * scale;
	}

	struct obs_source_audio obs_audio = {};
	obs_audio.data[0] = reinterpret_cast<const uint8_t *>(audio_left_.data());
	obs_audio.data[1] = reinterpret_cast<const uint8_t *>(audio_right_.data());
	obs_audio.frames = static_cast<uint32_t>(samples);
	obs_audio.speakers = SPEAKERS_STEREO;
	obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs_audio.samples_per_sec = 44100;
	obs_audio.timestamp = obs_timestamp_ns;
	audio_cb_(&obs_audio);
}

void MediaDecoder::emit_audio_frame_locked(AVFrame *frame, uint64_t obs_timestamp_ns)
{
	if (!audio_cb_ || !frame || frame->nb_samples <= 0)
		return;

	const int in_rate = frame->sample_rate > 0 ? frame->sample_rate : 44100;
	const auto in_fmt = static_cast<enum AVSampleFormat>(frame->format);
	const int in_channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 2;

	if (!swr_ctx_ || swr_in_rate_ != in_rate || swr_in_fmt_ != in_fmt ||
	    swr_in_channels_ != in_channels) {
		if (swr_ctx_)
			swr_free(&swr_ctx_);

		AVChannelLayout out_layout = {};
		av_channel_layout_default(&out_layout, 2);

		AVChannelLayout in_layout = {};
		if (frame->ch_layout.nb_channels > 0) {
			av_channel_layout_copy(&in_layout, &frame->ch_layout);
		} else {
			av_channel_layout_default(&in_layout, 2);
		}

		int err = swr_alloc_set_opts2(&swr_ctx_, &out_layout, AV_SAMPLE_FMT_FLTP, in_rate,
					      &in_layout, in_fmt, in_rate, 0, nullptr);
		av_channel_layout_uninit(&out_layout);
		av_channel_layout_uninit(&in_layout);

		if (err < 0 || !swr_ctx_ || swr_init(swr_ctx_) < 0) {
			if (swr_ctx_)
				swr_free(&swr_ctx_);
			return;
		}

		swr_in_rate_ = in_rate;
		swr_in_fmt_ = in_fmt;
		swr_in_channels_ = in_channels;
	}

	const int max_out_samples = frame->nb_samples + 64;
	audio_left_.resize(static_cast<size_t>(max_out_samples));
	audio_right_.resize(static_cast<size_t>(max_out_samples));

	uint8_t *out_planes[2] = {
		reinterpret_cast<uint8_t *>(audio_left_.data()),
		reinterpret_cast<uint8_t *>(audio_right_.data()),
	};

	int out_samples = swr_convert(swr_ctx_, out_planes, max_out_samples,
				      const_cast<const uint8_t **>(frame->extended_data),
				      frame->nb_samples);
	if (out_samples <= 0)
		return;

	if (sync_volume_.load(std::memory_order_relaxed)) {
		const float gain = volume_gain_.load(std::memory_order_relaxed);
		if (std::fabs(gain - 1.0f) > 0.001f) {
			for (int i = 0; i < out_samples; ++i) {
				audio_left_[i] *= gain;
				audio_right_[i] *= gain;
			}
		}
	}

	struct obs_source_audio obs_audio = {};
	obs_audio.data[0] = reinterpret_cast<const uint8_t *>(audio_left_.data());
	obs_audio.data[1] = reinterpret_cast<const uint8_t *>(audio_right_.data());
	obs_audio.frames = static_cast<uint32_t>(out_samples);
	obs_audio.speakers = SPEAKERS_STEREO;
	obs_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs_audio.samples_per_sec = static_cast<uint32_t>(in_rate);
	obs_audio.timestamp = obs_timestamp_ns;
	audio_cb_(&obs_audio);
}

void MediaDecoder::flush_video()
{
	std::lock_guard<std::mutex> lock(video_mutex_);
	if (video_ctx_) {
		avcodec_flush_buffers(video_ctx_);
	}
}

void MediaDecoder::flush_audio()
{
	std::lock_guard<std::mutex> lock(audio_mutex_);
	if (audio_ctx_) {
		avcodec_flush_buffers(audio_ctx_);
	}
}

void MediaDecoder::reset_all()
{
	{
		std::lock_guard<std::mutex> lock(video_mutex_);
		destroy_video_decoder_locked();
	}
	{
		std::lock_guard<std::mutex> lock(audio_mutex_);
		destroy_audio_decoder_locked();
	}
	last_width_.store(0, std::memory_order_relaxed);
	last_height_.store(0, std::memory_order_relaxed);
}

} // namespace obs_airplay
