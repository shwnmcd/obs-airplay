#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media-decoder.hpp"
#include "security-manager.hpp"

extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"
}

namespace obs_airplay {

struct ReceiverConfig {
	std::string server_name = "OBS iPhone Cast";
	uint16_t width = 1920;
	uint16_t height = 1080;
	uint16_t refresh_rate = 60;
	uint16_t max_fps = 60;
	bool enable_hevc = true;
	bool hardware_decode = true;
	bool enable_audio = true;
	bool sync_device_volume = true;
	bool clear_on_disconnect = true;
	bool fixed_ports = true;
	uint16_t port_offset = 0;
	std::string mac_address; // Empty -> deterministic MAC derived from server_name
	std::string password;    // Used only when security.mode == SecurityMode::StaticPassword
	std::string bind_ipv4;   // Empty or "auto" -> smart best local Wi-Fi/LAN IPv4
	SecurityPolicy security;
	bool verbose_logging = false;
};

struct NetworkAdapterInfo {
	std::string name;
	std::string ipv4;
};

using ClearVideoCallback = std::function<void()>;
using StatusCallback = std::function<void(const std::string &status)>;

class AirPlayReceiver {
public:
	AirPlayReceiver(VideoFrameCallback video_cb,
			AudioFrameCallback audio_cb,
			ClearVideoCallback clear_cb,
			StatusCallback status_cb);
	~AirPlayReceiver();

	AirPlayReceiver(const AirPlayReceiver &) = delete;
	AirPlayReceiver &operator=(const AirPlayReceiver &) = delete;

	bool start(const ReceiverConfig &config);
	void stop();
	bool is_running() const { return running_.load(std::memory_order_acquire); }

	std::string status_summary() const;
	uint16_t airplay_port() const { return airplay_port_.load(std::memory_order_relaxed); }
	std::string active_ipv4() const;

	// Live security helpers
	void update_security_policy(const SecurityPolicy &policy);
	std::string trusted_devices_summary() const;
	void revoke_all_trusted_devices();

	static std::vector<NetworkAdapterInfo> enumerate_ipv4_adapters();
	static std::string derive_mac_address(const std::string &seed);
	static bool parse_mac_bytes(const std::string &mac_str, std::vector<char> &out_bytes);

private:
	uint64_t map_remote_timestamp(uint64_t remote_ntp_ns);
	void reset_clock_anchor();
	void set_status(const std::string &status);

	// UxPlay RAOP static C trampolines
	static void cb_log(void *cls, int level, const char *msg);
	static void cb_conn_init(void *cls);
	static void cb_conn_destroy(void *cls);
	static void cb_conn_reset(void *cls, int reason);
	static void cb_conn_feedback(void *cls);
	static void cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit);
	static int cb_video_set_codec_and_size(void *cls, video_codec_t codec,
					       unsigned short width_source, unsigned short height_source,
					       unsigned short x_offset, unsigned short y_offset,
					       unsigned short width, unsigned short height);
	static void cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data);
	static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data);
	static void cb_video_flush(void *cls);
	static void cb_audio_flush(void *cls);
	static void cb_video_reset(void *cls, reset_type_t type);
	static void cb_video_pause(void *cls);
	static void cb_video_resume(void *cls);
	static void cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf,
					bool *usingScreen, bool *isMedia, uint64_t *audioFormat);
	static void cb_audio_set_volume(void *cls, float volume);
	static double cb_audio_set_client_volume(void *cls);
	static const char *cb_passwd(void *cls, int *len);
	static void cb_display_pin(void *cls, char *pin);
	static void cb_register_client(void *cls, const char *device_id, const char *pk_str, const char *name);
	static bool cb_check_register(void *cls, const char *pk_str);

	VideoFrameCallback video_cb_;
	AudioFrameCallback audio_cb_;
	ClearVideoCallback clear_cb_;
	StatusCallback status_cb_;

	std::unique_ptr<MediaDecoder> decoder_;
	std::unique_ptr<SecurityManager> security_;

	mutable std::mutex lifecycle_mutex_;
	ReceiverConfig config_;
	dnssd_t *dnssd_ = nullptr;
	raop_t *raop_ = nullptr;
	std::atomic<bool> running_{false};
	std::atomic<bool> stopping_{false};
	std::atomic<uint16_t> airplay_port_{0};
	std::atomic<int> open_connections_{0};
	std::atomic<double> current_volume_db_{0.0};

	// Timestamp sync anchor (maps AirPlay remote NTP ns -> OBS monotonic ns)
	std::mutex clock_mutex_;
	bool clock_anchored_ = false;
	uint64_t remote_base_ns_ = 0;
	uint64_t obs_base_ns_ = 0;

	mutable std::mutex status_mutex_;
	std::string status_text_ = "Idle";
	std::string connected_device_;
};

} // namespace obs_airplay
