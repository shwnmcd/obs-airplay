#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <obs-module.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace obs_airplay {

using VideoFrameCallback = std::function<void(const struct obs_source_frame *frame)>;
using AudioFrameCallback = std::function<void(const struct obs_source_audio *audio)>;

struct DecoderConfig {
	bool hardware_decode = true;
	bool enable_audio = true;
	bool sync_device_volume = true;
};

class MediaDecoder {
public:
	MediaDecoder(VideoFrameCallback video_cb, AudioFrameCallback audio_cb);
	~MediaDecoder();

	MediaDecoder(const MediaDecoder &) = delete;
	MediaDecoder &operator=(const MediaDecoder &) = delete;

	void configure(const DecoderConfig &config);
	void set_volume_gain(float linear_gain);

	// Called from UxPlay's mirroring video thread
	bool decode_video(const uint8_t *data, int data_len, bool is_h265,
			  uint64_t obs_timestamp_ns);

	// Called from UxPlay's RAOP audio thread
	// ct: 1 = PCM S16LE, 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD
	bool decode_audio(const uint8_t *data, int data_len, uint8_t ct,
			  uint64_t obs_timestamp_ns);

	void flush_video();
	void flush_audio();
	void reset_all();

	uint32_t last_video_width() const { return last_width_.load(std::memory_order_relaxed); }
	uint32_t last_video_height() const { return last_height_.load(std::memory_order_relaxed); }

private:
	bool ensure_video_decoder(bool is_h265);
	void destroy_video_decoder_locked();
	void emit_video_frame_locked(AVFrame *frame, uint64_t obs_timestamp_ns);

	bool ensure_audio_decoder(uint8_t ct);
	void destroy_audio_decoder_locked();
	void emit_audio_frame_locked(AVFrame *frame, uint64_t obs_timestamp_ns);
	void emit_pcm_s16le_locked(const uint8_t *data, int data_len, uint64_t obs_timestamp_ns);

	static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);

	VideoFrameCallback video_cb_;
	AudioFrameCallback audio_cb_;

	std::atomic<bool> hw_decode_enabled_{true};
	std::atomic<bool> audio_enabled_{true};
	std::atomic<bool> sync_volume_{true};
	std::atomic<float> volume_gain_{1.0f};
	std::atomic<uint32_t> last_width_{0};
	std::atomic<uint32_t> last_height_{0};

	// Video decoder state
	std::mutex video_mutex_;
	AVCodecContext *video_ctx_ = nullptr;
	AVBufferRef *hw_device_ctx_ = nullptr;
	enum AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
	AVPacket *video_pkt_ = nullptr;
	AVFrame *video_frame_ = nullptr;
	AVFrame *sw_frame_ = nullptr;
	SwsContext *sws_ctx_ = nullptr;
	int sws_src_w_ = 0;
	int sws_src_h_ = 0;
	enum AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;
	std::vector<uint8_t> sws_buffer_;
	uint8_t *sws_data_[4] = {nullptr, nullptr, nullptr, nullptr};
	int sws_linesize_[4] = {0, 0, 0, 0};
	bool video_is_h265_ = false;
	bool hw_decode_active_ = false;

	// Audio decoder state
	std::mutex audio_mutex_;
	AVCodecContext *audio_ctx_ = nullptr;
	AVPacket *audio_pkt_ = nullptr;
	AVFrame *audio_frame_ = nullptr;
	SwrContext *swr_ctx_ = nullptr;
	int swr_in_rate_ = 0;
	enum AVSampleFormat swr_in_fmt_ = AV_SAMPLE_FMT_NONE;
	int swr_in_channels_ = 0;
	uint8_t active_audio_ct_ = 0;
	std::vector<float> audio_left_;
	std::vector<float> audio_right_;
};

} // namespace obs_airplay
