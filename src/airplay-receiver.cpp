#include "airplay-receiver.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <util/base.h>
#include <util/platform.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

extern "C" {
#include "mdnsd/mdnsd.h"
}

namespace obs_airplay {

namespace {

#ifdef _WIN32
struct WinsockInitializer {
	WinsockInitializer()
	{
		WSADATA wsa_data = {};
		WSAStartup(MAKEWORD(2, 2), &wsa_data);
	}
	~WinsockInitializer() { WSACleanup(); }
};

void ensure_winsock_initialized()
{
	static WinsockInitializer instance;
	(void)instance;
}
#endif

uint64_t fnv1a_64(const std::string &str)
{
	uint64_t hash = 14695981039346656037ULL;
	for (unsigned char c : str) {
		hash ^= c;
		hash *= 1099511628211ULL;
	}
	return hash;
}

} // namespace

AirPlayReceiver::AirPlayReceiver(VideoFrameCallback video_cb,
				 AudioFrameCallback audio_cb,
				 ClearVideoCallback clear_cb,
				 StatusCallback status_cb)
	: video_cb_(std::move(video_cb)),
	  audio_cb_(std::move(audio_cb)),
	  clear_cb_(std::move(clear_cb)),
	  status_cb_(std::move(status_cb)),
	  decoder_(std::make_unique<MediaDecoder>(video_cb_, audio_cb_)),
	  security_(std::make_unique<SecurityManager>())
{
#ifdef _WIN32
	ensure_winsock_initialized();
#endif
	ntp_global_init();
}

AirPlayReceiver::~AirPlayReceiver()
{
	stop();
}

std::string AirPlayReceiver::active_ipv4() const
{
	uint32_t addr_net = mdnsd_get_active_ipv4();
	if (addr_net == 0) {
		return "unknown";
	}
	char buf[INET_ADDRSTRLEN] = {};
	struct in_addr in = {};
	in.s_addr = addr_net;
	if (!inet_ntop(AF_INET, &in, buf, sizeof(buf))) {
		return "unknown";
	}
	return std::string(buf);
}

std::vector<NetworkAdapterInfo> AirPlayReceiver::enumerate_ipv4_adapters()
{
	std::vector<NetworkAdapterInfo> result;
#ifdef _WIN32
	ensure_winsock_initialized();
	ULONG buflen = 32768;
	std::vector<uint8_t> buffer(buflen);
	auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
	DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, addrs, &buflen);
	if (ret == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(buflen);
		addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
		ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, addrs, &buflen);
	}
	if (ret == NO_ERROR) {
		for (auto *curr = addrs; curr != nullptr; curr = curr->Next) {
			if (curr->OperStatus != IfOperStatusUp ||
			    curr->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
			    !curr->FirstUnicastAddress) {
				continue;
			}
			auto *sin = reinterpret_cast<struct sockaddr_in *>(
				curr->FirstUnicastAddress->Address.lpSockaddr);
			if (!sin || sin->sin_family != AF_INET)
				continue;

			uint32_t ip_net = sin->sin_addr.s_addr;
			uint8_t b0 = static_cast<uint8_t>(ip_net & 0xFF);
			uint8_t b1 = static_cast<uint8_t>((ip_net >> 8) & 0xFF);
			if (ip_net == 0 || b0 == 127 || (b0 == 169 && b1 == 254))
				continue;

			char ip_str[INET_ADDRSTRLEN] = {};
			if (!inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str)))
				continue;

			std::string friendly = "Network Adapter";
			if (curr->FriendlyName && curr->FriendlyName[0] != L'\0') {
				int needed = WideCharToMultiByte(CP_UTF8, 0, curr->FriendlyName, -1,
								 nullptr, 0, nullptr, nullptr);
				if (needed > 1) {
					friendly.resize(static_cast<size_t>(needed - 1));
					WideCharToMultiByte(CP_UTF8, 0, curr->FriendlyName, -1,
							    &friendly[0], needed, nullptr, nullptr);
				}
			}

			result.push_back(NetworkAdapterInfo{friendly, std::string(ip_str)});
		}
	}
#endif
	return result;
}

std::string AirPlayReceiver::derive_mac_address(const std::string &seed)
{
	uint64_t h = fnv1a_64(seed.empty() ? "OBS-AirPlay-Default" : seed);
	// Locally-administered unicast MAC address (bit 1 = 1, bit 0 = 0)
	uint8_t b[6];
	b[0] = static_cast<uint8_t>(((h >> 0) & 0xFC) | 0x02);
	b[1] = static_cast<uint8_t>((h >> 8) & 0xFF);
	b[2] = static_cast<uint8_t>((h >> 16) & 0xFF);
	b[3] = static_cast<uint8_t>((h >> 24) & 0xFF);
	b[4] = static_cast<uint8_t>((h >> 32) & 0xFF);
	b[5] = static_cast<uint8_t>((h >> 40) & 0xFF);

	char buf[24];
	std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
		      b[0], b[1], b[2], b[3], b[4], b[5]);
	return std::string(buf);
}

bool AirPlayReceiver::parse_mac_bytes(const std::string &mac_str, std::vector<char> &out_bytes)
{
	unsigned int vals[6] = {};
	if (std::sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x",
			&vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
		return false;
	}
	out_bytes.resize(6);
	for (int i = 0; i < 6; ++i) {
		if (vals[i] > 0xFF)
			return false;
		out_bytes[i] = static_cast<char>(vals[i]);
	}
	return true;
}

void AirPlayReceiver::set_status(const std::string &status)
{
	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		status_text_ = status;
	}
	if (status_cb_) {
		status_cb_(status);
	}
}

std::string AirPlayReceiver::status_summary() const
{
	if (security_) {
		std::string pin = security_->current_active_pin();
		if (!pin.empty()) {
			return "2FA Code Active: " + pin + " - Enter on iPhone (hidden from OBS capture)";
		}
	}
	std::lock_guard<std::mutex> lock(status_mutex_);
	return status_text_;
}

void AirPlayReceiver::update_security_policy(const SecurityPolicy &policy)
{
	if (security_) {
		security_->set_policy(policy);
	}
	std::lock_guard<std::mutex> lock(lifecycle_mutex_);
	config_.security = policy;
}

std::string AirPlayReceiver::trusted_devices_summary() const
{
	return security_ ? security_->trusted_devices_summary() : "Trusted Devices: Unavailable";
}

void AirPlayReceiver::revoke_all_trusted_devices()
{
	if (security_) {
		security_->revoke_all_trusted();
	}
}

void AirPlayReceiver::reset_clock_anchor()
{
	std::lock_guard<std::mutex> lock(clock_mutex_);
	clock_anchored_ = false;
	remote_base_ns_ = 0;
	obs_base_ns_ = 0;
}

uint64_t AirPlayReceiver::map_remote_timestamp(uint64_t remote_ntp_ns)
{
	const uint64_t now_ns = os_gettime_ns();
	if (remote_ntp_ns == 0) {
		return now_ns;
	}

	std::lock_guard<std::mutex> lock(clock_mutex_);
	if (!clock_anchored_) {
		remote_base_ns_ = remote_ntp_ns;
		obs_base_ns_ = now_ns;
		clock_anchored_ = true;
		return now_ns;
	}

	const int64_t delta_ns = static_cast<int64_t>(remote_ntp_ns - remote_base_ns_);
	const int64_t mapped_signed = static_cast<int64_t>(obs_base_ns_) + delta_ns;

	// If clock drifted or jumped by > 1.5 seconds (e.g., stream restart), smoothly re-anchor
	const int64_t drift = mapped_signed - static_cast<int64_t>(now_ns);
	constexpr int64_t kMaxDriftNs = 1500000000LL;
	if (mapped_signed <= 0 || drift > kMaxDriftNs || drift < -kMaxDriftNs) {
		remote_base_ns_ = remote_ntp_ns;
		obs_base_ns_ = now_ns;
		return now_ns;
	}

	return static_cast<uint64_t>(mapped_signed);
}

bool AirPlayReceiver::start(const ReceiverConfig &config)
{
	std::lock_guard<std::mutex> lock(lifecycle_mutex_);
	if (running_.load(std::memory_order_acquire)) {
		return true;
	}

	stopping_.store(false, std::memory_order_release);
	config_ = config;
	if (config_.server_name.empty()) {
		config_.server_name = "OBS iPhone Cast";
	}

	DecoderConfig dec_cfg;
	dec_cfg.hardware_decode = config_.hardware_decode;
	dec_cfg.enable_audio = config_.enable_audio;
	dec_cfg.sync_device_volume = config_.sync_device_volume;
	decoder_->configure(dec_cfg);
	decoder_->set_volume_gain(1.0f);
	reset_clock_anchor();

	// Resolve MAC address
	std::string mac_str = config_.mac_address;
	std::vector<char> hw_addr;
	if (mac_str.empty() || !parse_mac_bytes(mac_str, hw_addr)) {
		mac_str = derive_mac_address(config_.server_name);
		parse_mac_bytes(mac_str, hw_addr);
	}

	if (security_) {
		security_->set_policy(config_.security);
		security_->dismiss_pin_popup();
	}

	// Configure preferred mDNS IPv4 interface (0 = smart auto best local Wi-Fi/LAN)
	uint32_t preferred_ip = 0;
	if (!config_.bind_ipv4.empty() && config_.bind_ipv4 != "auto") {
		struct in_addr in = {};
		if (inet_pton(AF_INET, config_.bind_ipv4.c_str(), &in) == 1) {
			preferred_ip = in.s_addr;
		}
	}
	mdnsd_set_preferred_ipv4(preferred_ip);

	unsigned char pin_pw = 0;
	int legacy_pairing_bit = 0;
	std::string keyfile_path;
	switch (config_.security.mode) {
	case SecurityMode::PairingPinTrusted:
		pin_pw = 1;
		legacy_pairing_bit = 1;
		keyfile_path = security_ ? security_->server_key_path() : "";
		break;
	case SecurityMode::StaticPassword:
		pin_pw = config_.security.static_password.empty() ? 0 : 2;
		break;
	case SecurityMode::OneTimePinEvery:
		pin_pw = 3;
		break;
	case SecurityMode::None:
	default:
		pin_pw = 0;
		break;
	}

	int dnssd_err = 0;
	dnssd_ = dnssd_init(config_.server_name.c_str(),
			    static_cast<int>(config_.server_name.size()),
			    hw_addr.data(),
			    static_cast<int>(hw_addr.size()),
			    pin_pw,
			    &dnssd_err);
	if (!dnssd_ || dnssd_err != 0) {
		blog(LOG_ERROR, "[obs-airplay] dnssd_init failed (err=%d)", dnssd_err);
		set_status("Error: mDNS discovery init failed");
		if (dnssd_) {
			dnssd_destroy(dnssd_);
			dnssd_ = nullptr;
		}
		return false;
	}

	// Configure AirPlay feature flags for iOS Screen Mirroring + Audio
	dnssd_set_airplay_features(dnssd_, 0, 0);  // AirPlay HLS video off (mirroring mode)
	dnssd_set_airplay_features(dnssd_, 1, 1);  // photo supported
	dnssd_set_airplay_features(dnssd_, 2, 1);  // FairPlay DRM
	dnssd_set_airplay_features(dnssd_, 3, 0);
	dnssd_set_airplay_features(dnssd_, 4, 0);  // HLS off
	dnssd_set_airplay_features(dnssd_, 5, 1);
	dnssd_set_airplay_features(dnssd_, 6, 1);
	dnssd_set_airplay_features(dnssd_, 7, 1);  // Screen Mirroring supported!
	dnssd_set_airplay_features(dnssd_, 8, 0);
	dnssd_set_airplay_features(dnssd_, 9, 1);  // Audio supported
	dnssd_set_airplay_features(dnssd_, 10, 1);
	dnssd_set_airplay_features(dnssd_, 11, 1); // Audio packet redundancy
	dnssd_set_airplay_features(dnssd_, 12, 1); // FairPlay secure auth
	dnssd_set_airplay_features(dnssd_, 13, 1);
	dnssd_set_airplay_features(dnssd_, 14, 1); // FairPlay auth
	dnssd_set_airplay_features(dnssd_, 15, 1);
	dnssd_set_airplay_features(dnssd_, 16, 1);
	dnssd_set_airplay_features(dnssd_, 17, 1);
	dnssd_set_airplay_features(dnssd_, 18, 1);
	dnssd_set_airplay_features(dnssd_, 19, 1);
	dnssd_set_airplay_features(dnssd_, 20, 1);
	dnssd_set_airplay_features(dnssd_, 21, 1);
	dnssd_set_airplay_features(dnssd_, 22, 1);
	dnssd_set_airplay_features(dnssd_, 23, 0);
	dnssd_set_airplay_features(dnssd_, 24, 0);
	dnssd_set_airplay_features(dnssd_, 25, 1);
	dnssd_set_airplay_features(dnssd_, 26, 0);
	dnssd_set_airplay_features(dnssd_, 27, legacy_pairing_bit); // 1 = SRP-6a Pair-Setup-PIN, 0 = standard
	dnssd_set_airplay_features(dnssd_, 28, 1);
	dnssd_set_airplay_features(dnssd_, 29, 0);
	dnssd_set_airplay_features(dnssd_, 30, 1); // Unified RAOP support
	dnssd_set_airplay_features(dnssd_, 31, 0);
	dnssd_set_airplay_features(dnssd_, 42, config_.enable_hevc ? 1 : 0); // H.265/HEVC screen mirroring

	// Wire RAOP callbacks with `this` as `cls`
	raop_callbacks_t cbs = {};
	cbs.cls = this;
	cbs.conn_init = &AirPlayReceiver::cb_conn_init;
	cbs.conn_destroy = &AirPlayReceiver::cb_conn_destroy;
	cbs.conn_reset = &AirPlayReceiver::cb_conn_reset;
	cbs.conn_feedback = &AirPlayReceiver::cb_conn_feedback;
	cbs.report_client_request = &AirPlayReceiver::cb_report_client_request;
	cbs.video_set_codec_and_size = &AirPlayReceiver::cb_video_set_codec_and_size;
	cbs.video_process = &AirPlayReceiver::cb_video_process;
	cbs.audio_process = &AirPlayReceiver::cb_audio_process;
	cbs.video_flush = &AirPlayReceiver::cb_video_flush;
	cbs.audio_flush = &AirPlayReceiver::cb_audio_flush;
	cbs.video_reset = &AirPlayReceiver::cb_video_reset;
	cbs.video_pause = &AirPlayReceiver::cb_video_pause;
	cbs.video_resume = &AirPlayReceiver::cb_video_resume;
	cbs.audio_get_format = &AirPlayReceiver::cb_audio_get_format;
	cbs.audio_set_volume = &AirPlayReceiver::cb_audio_set_volume;
	cbs.audio_set_client_volume = &AirPlayReceiver::cb_audio_set_client_volume;
	cbs.passwd = &AirPlayReceiver::cb_passwd;
	cbs.display_pin = &AirPlayReceiver::cb_display_pin;
	cbs.register_client = &AirPlayReceiver::cb_register_client;
	cbs.check_register = &AirPlayReceiver::cb_check_register;

	raop_ = raop_init(&cbs);
	if (!raop_) {
		blog(LOG_ERROR, "[obs-airplay] raop_init failed");
		dnssd_destroy(dnssd_);
		dnssd_ = nullptr;
		set_status("Error: RAOP init failed");
		return false;
	}

	raop_set_log_callback(raop_, &AirPlayReceiver::cb_log, this);
	raop_set_log_level(raop_, config_.verbose_logging ? LOGGER_DEBUG : LOGGER_INFO);

	// nohold = 1 allows a new iPhone to seamlessly take over if an old session hung
	if (raop_init2(raop_, 1, mac_str.c_str(), keyfile_path.c_str()) != 0) {
		blog(LOG_ERROR, "[obs-airplay] raop_init2 failed");
		raop_destroy(raop_);
		raop_ = nullptr;
		dnssd_destroy(dnssd_);
		dnssd_ = nullptr;
		set_status("Error: RAOP crypto/key init failed");
		return false;
	}

	if (pin_pw == 1) {
		// 0 tells UxPlay to generate a fresh random 4-digit pairing PIN per new device
		raop_set_plist(raop_, "pin", 0);
	}

	if (config_.width > 0)
		raop_set_plist(raop_, "width", config_.width);
	if (config_.height > 0)
		raop_set_plist(raop_, "height", config_.height);
	if (config_.refresh_rate > 0)
		raop_set_plist(raop_, "refreshRate", config_.refresh_rate);
	if (config_.max_fps > 0)
		raop_set_plist(raop_, "maxFPS", config_.max_fps);
	raop_set_plist(raop_, "overscanned", 0);

	unsigned short tcp_ports[3] = {0, 0, 0};
	unsigned short udp_ports[3] = {0, 0, 0};
	if (config_.fixed_ports) {
		const unsigned short off = config_.port_offset;
		tcp_ports[0] = static_cast<unsigned short>(7100 + off);
		tcp_ports[1] = static_cast<unsigned short>(7000 + off);
		tcp_ports[2] = static_cast<unsigned short>(7001 + off);
		udp_ports[0] = static_cast<unsigned short>(7011 + off);
		udp_ports[1] = static_cast<unsigned short>(6001 + off);
		udp_ports[2] = static_cast<unsigned short>(6000 + off);
	}
	raop_set_tcp_ports(raop_, tcp_ports);
	raop_set_udp_ports(raop_, udp_ports);

	unsigned short port = raop_get_port(raop_);
	if (raop_start_httpd(raop_, &port) < 0) {
		blog(LOG_ERROR, "[obs-airplay] Failed to start AirPlay HTTP/RTSP server on port %u", port);
		raop_destroy(raop_);
		raop_ = nullptr;
		dnssd_destroy(dnssd_);
		dnssd_ = nullptr;
		set_status("Error: Failed to bind AirPlay TCP port");
		return false;
	}

	raop_set_port(raop_, port);
	raop_set_dnssd(raop_, dnssd_);
	airplay_port_.store(port, std::memory_order_relaxed);

	if (dnssd_register_raop(dnssd_, port) != 0 || dnssd_register_airplay(dnssd_, port) != 0) {
		blog(LOG_ERROR, "[obs-airplay] Failed to register AirPlay mDNS services");
		raop_destroy(raop_);
		raop_ = nullptr;
		dnssd_unregister_raop(dnssd_);
		dnssd_unregister_airplay(dnssd_);
		dnssd_destroy(dnssd_);
		dnssd_ = nullptr;
		set_status("Error: mDNS broadcast failed (check UDP 5353 / Firewall)");
		return false;
	}

	running_.store(true, std::memory_order_release);

	std::ostringstream oss;
	oss << "Ready: \"" << config_.server_name << "\" on " << active_ipv4() << ":" << port
	    << " (MAC " << mac_str << ") - Open Control Center -> Screen Mirroring on iPhone";
	set_status(oss.str());
	blog(LOG_INFO, "[obs-airplay] %s", oss.str().c_str());
	return true;
}

void AirPlayReceiver::stop()
{
	std::lock_guard<std::mutex> lock(lifecycle_mutex_);
	if (!running_.load(std::memory_order_acquire) && !raop_ && !dnssd_) {
		return;
	}

	stopping_.store(true, std::memory_order_release);
	running_.store(false, std::memory_order_release);

	if (security_) {
		security_->dismiss_pin_popup();
	}
	if (dnssd_) {
		dnssd_unregister_raop(dnssd_);
		dnssd_unregister_airplay(dnssd_);
	}
	if (raop_) {
		raop_destroy(raop_);
		raop_ = nullptr;
	}
	if (dnssd_) {
		dnssd_destroy(dnssd_);
		dnssd_ = nullptr;
	}

	open_connections_.store(0, std::memory_order_relaxed);
	airplay_port_.store(0, std::memory_order_relaxed);
	reset_clock_anchor();
	if (decoder_) {
		decoder_->reset_all();
	}

	set_status("Stopped");
	blog(LOG_INFO, "[obs-airplay] AirPlay receiver stopped");
}

void AirPlayReceiver::cb_log(void *cls, int level, const char *msg)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!msg)
		return;
	if (self && !self->config_.verbose_logging && level == LOGGER_DEBUG)
		return;
	if (std::strstr(msg, "ENTER PIN =") != nullptr) {
		blog(LOG_INFO, "[obs-airplay:raop] Client challenged for 4-digit 2FA PIN (code redacted from log)");
		return;
	}

	int obs_level = LOG_INFO;
	switch (level) {
	case LOGGER_ERR:
		obs_level = LOG_ERROR;
		break;
	case LOGGER_WARNING:
		obs_level = LOG_WARNING;
		break;
	case LOGGER_DEBUG:
		obs_level = LOG_DEBUG;
		break;
	default:
		obs_level = LOG_INFO;
		break;
	}
	blog(obs_level, "[obs-airplay:raop] %s", msg);
}

void AirPlayReceiver::cb_conn_init(void *cls)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return;
	int count = self->open_connections_.fetch_add(1, std::memory_order_relaxed) + 1;
	blog(LOG_INFO, "[obs-airplay] Client connected (active connections=%d)", count);
}

void AirPlayReceiver::cb_conn_destroy(void *cls)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return;
	int remaining = self->open_connections_.fetch_sub(1, std::memory_order_relaxed) - 1;
	if (remaining <= 0) {
		self->open_connections_.store(0, std::memory_order_relaxed);
		self->reset_clock_anchor();
		if (self->decoder_) {
			self->decoder_->flush_video();
			self->decoder_->flush_audio();
		}
		if (self->config_.clear_on_disconnect && self->clear_cb_) {
			self->clear_cb_();
		}
		// Keep the 2FA PIN popup & status visible while the user is typing the PIN on iPhone!
		// (iOS closes the initial challenge socket right after PAIR-PIN-START / 401 and opens
		//  a second socket once the user finishes typing the 4-digit code.)
		const bool pin_waiting = self->security_ && !self->security_->current_active_pin().empty();
		if (!pin_waiting) {
			std::ostringstream oss;
			oss << "Ready: \"" << self->config_.server_name << "\" on " << self->active_ipv4()
			    << ":" << self->airplay_port_.load(std::memory_order_relaxed)
			    << " - Waiting for iPhone";
			self->set_status(oss.str());
		}
		blog(LOG_INFO, "[obs-airplay] Client disconnected");
	}
}

void AirPlayReceiver::cb_conn_reset(void *cls, int reason)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return;
	blog(LOG_WARNING, "[obs-airplay] Connection reset (reason=%d)", reason);
	self->reset_clock_anchor();
	if (self->decoder_) {
		self->decoder_->flush_video();
		self->decoder_->flush_audio();
	}
	if (self->config_.clear_on_disconnect && self->clear_cb_) {
		self->clear_cb_();
	}
}

void AirPlayReceiver::cb_conn_feedback(void *cls)
{
	(void)cls;
}

void AirPlayReceiver::cb_report_client_request(void *cls, char *deviceid, char *model, char *name, bool *admit)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (admit)
		*admit = false;
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return;

	if (self->security_) {
		self->security_->dismiss_pin_popup();
	}

	std::string dev_id = deviceid ? deviceid : "";
	std::string dev_name = name ? name : "Apple Device";
	std::string dev_model = model ? model : "iOS";
	blog(LOG_INFO, "[obs-airplay] Mirroring request from %s (%s, id=%s)",
	     dev_name.c_str(), dev_model.c_str(), dev_id.empty() ? "unknown" : dev_id.c_str());

	std::string reason;
	bool allowed = true;
	if (self->security_) {
		allowed = self->security_->evaluate_client_request(dev_id, dev_model, dev_name, reason);
	}

	if (admit)
		*admit = allowed;

	if (!allowed) {
		self->set_status("Blocked: " + dev_name + " (" + reason + ")");
		blog(LOG_WARNING, "[obs-airplay] REJECTED connection from %s (%s): %s",
		     dev_name.c_str(), dev_id.c_str(), reason.c_str());
		return;
	}

	{
		std::lock_guard<std::mutex> lock(self->status_mutex_);
		self->connected_device_ = dev_name + " (" + dev_model + ")";
	}
	self->set_status("Connected: " + dev_name + " (" + dev_model + ") [" + reason + "]");
}

int AirPlayReceiver::cb_video_set_codec_and_size(void *cls, video_codec_t codec,
						 unsigned short width_source, unsigned short height_source,
						 unsigned short x_offset, unsigned short y_offset,
						 unsigned short width, unsigned short height)
{
	(void)x_offset;
	(void)y_offset;
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return 0;

	blog(LOG_INFO, "[obs-airplay] Stream geometry: codec=%s source=%ux%u display=%ux%u",
	     codec == VIDEO_CODEC_H265 ? "H.265/HEVC" : "H.264/AVC",
	     width_source, height_source, width, height);
	return 0;
}

void AirPlayReceiver::cb_video_process(void *cls, raop_ntp_t *ntp, video_decode_struct *data)
{
	(void)ntp;
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire) || !data || !self->decoder_)
		return;

	const uint64_t obs_ts = self->map_remote_timestamp(data->ntp_time_remote);
	self->decoder_->decode_video(data->data, data->data_len, data->is_h265, obs_ts);
}

void AirPlayReceiver::cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *data)
{
	(void)ntp;
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire) || !self->config_.enable_audio ||
	    !data || !self->decoder_)
		return;

	const uint64_t obs_ts = self->map_remote_timestamp(data->ntp_time_remote);
	self->decoder_->decode_audio(data->data, data->data_len, data->ct, obs_ts);
}

void AirPlayReceiver::cb_video_flush(void *cls)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire) || !self->decoder_)
		return;
	self->decoder_->flush_video();
}

void AirPlayReceiver::cb_audio_flush(void *cls)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire) || !self->config_.enable_audio ||
	    !self->decoder_)
		return;
	self->decoder_->flush_audio();
}

void AirPlayReceiver::cb_video_reset(void *cls, reset_type_t type)
{
	(void)type;
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return;
	self->reset_clock_anchor();
	if (self->decoder_) {
		self->decoder_->flush_video();
	}
	if (self->config_.clear_on_disconnect && self->clear_cb_) {
		self->clear_cb_();
	}
}

void AirPlayReceiver::cb_video_pause(void *cls)
{
	(void)cls;
}

void AirPlayReceiver::cb_video_resume(void *cls)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return;
	self->reset_clock_anchor();
}

void AirPlayReceiver::cb_audio_get_format(void *cls, unsigned char *ct, unsigned short *spf,
					  bool *usingScreen, bool *isMedia, uint64_t *audioFormat)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire))
		return;

	blog(LOG_INFO, "[obs-airplay] Audio format negotiated: ct=%u spf=%u screen=%d media=%d fmt=0x%llx",
	     ct ? *ct : 0, spf ? *spf : 0,
	     usingScreen ? static_cast<int>(*usingScreen) : 0,
	     isMedia ? static_cast<int>(*isMedia) : 0,
	     audioFormat ? static_cast<unsigned long long>(*audioFormat) : 0ULL);
}

void AirPlayReceiver::cb_audio_set_volume(void *cls, float volume)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire) || !self->config_.enable_audio ||
	    !self->decoder_)
		return;

	float linear = 1.0f;
	if (volume <= -144.0f || volume <= -30.0f) {
		linear = 0.0f;
		self->current_volume_db_.store(-30.0, std::memory_order_relaxed);
	} else if (volume >= 0.0f) {
		linear = 1.0f;
		self->current_volume_db_.store(0.0, std::memory_order_relaxed);
	} else {
		self->current_volume_db_.store(static_cast<double>(volume), std::memory_order_relaxed);
		linear = static_cast<float>(std::pow(10.0, 0.05 * static_cast<double>(volume)));
	}
	self->decoder_->set_volume_gain(linear);
}

double AirPlayReceiver::cb_audio_set_client_volume(void *cls)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	return self ? self->current_volume_db_.load(std::memory_order_relaxed) : 0.0;
}

const char *AirPlayReceiver::cb_passwd(void *cls, int *len)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self) {
		if (len)
			*len = 0;
		return nullptr;
	}

	if (self->config_.security.mode == SecurityMode::OneTimePinEvery) {
		if (len)
			*len = -1; // Instructs UxPlay to generate & verify a fresh random 4-digit PIN
		return nullptr;
	}
	if (self->config_.security.mode == SecurityMode::StaticPassword &&
	    !self->config_.security.static_password.empty()) {
		if (len)
			*len = static_cast<int>(self->config_.security.static_password.size());
		return self->config_.security.static_password.c_str();
	}

	if (len)
		*len = 0;
	return nullptr;
}

void AirPlayReceiver::cb_display_pin(void *cls, char *pin)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire) || !pin || !pin[0])
		return;

	std::string pin_str(pin);
	blog(LOG_INFO, "[obs-airplay:security] 2FA PIN generated for \"%s\" (displayed in private PC popup)",
	     self->config_.server_name.c_str());

	self->set_status("2FA Code Active - Check private PC popup and enter 4-digit PIN on iPhone");
	if (self->security_) {
		self->security_->show_pin_popup(pin_str, self->config_.server_name);
	}
}

void AirPlayReceiver::cb_register_client(void *cls, const char *device_id, const char *pk_str, const char *name)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || self->stopping_.load(std::memory_order_acquire) || !self->security_)
		return;

	std::string id = device_id ? device_id : "";
	std::string pk = pk_str ? pk_str : "";
	std::string dev_name = name ? name : "Apple Device";
	self->security_->trust_device(id, dev_name, "", pk);
	self->security_->dismiss_pin_popup();
}

bool AirPlayReceiver::cb_check_register(void *cls, const char *pk_str)
{
	auto *self = static_cast<AirPlayReceiver *>(cls);
	if (!self || !self->security_)
		return false;

	if (self->config_.security.mode != SecurityMode::PairingPinTrusted) {
		return true;
	}
	return self->security_->is_public_key_registered(pk_str ? pk_str : "");
}

} // namespace obs_airplay
