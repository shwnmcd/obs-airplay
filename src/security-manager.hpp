#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace obs_airplay {

enum class SecurityMode : int {
	None = 0,               // No PIN (optional PC approval / allowlist still applies)
	PairingPinTrusted = 1,  // Apple SRP-6a + Ed25519 one-time pairing PIN, remembers trusted iPhones
	StaticPassword = 2,     // Fixed AirPlay password (RFC2617 Digest)
	OneTimePinEvery = 3,    // Fresh random 4-digit 2FA PIN on every connection (RFC2617 Digest)
};

struct TrustedDevice {
	std::string device_id;
	std::string name;
	std::string model;
	std::string public_key_b64; // Optional Ed25519 key for PairingPinTrusted mode
};

enum class ApprovalDecision {
	Deny = 0,
	AllowOnce = 1,
	AllowAndTrust = 2,
};

struct SecurityPolicy {
	SecurityMode mode = SecurityMode::OneTimePinEvery;
	std::string static_password;
	bool require_pc_approval = true;
	bool strict_trusted_only = false;
	bool hide_popups_from_capture = true;
	bool interactive_prompts_enabled = true; // Set false in headless automated tests
};

class SecurityManager {
public:
	explicit SecurityManager(const std::string &config_dir = "");
	~SecurityManager();

	SecurityManager(const SecurityManager &) = delete;
	SecurityManager &operator=(const SecurityManager &) = delete;

	void set_policy(const SecurityPolicy &policy);
	SecurityPolicy policy() const;

	// Persistent Ed25519 server key path for UxPlay raop_init2()
	std::string server_key_path() const;

	// Trusted device store operations
	bool is_device_trusted(const std::string &device_id) const;
	bool is_public_key_registered(const std::string &pk_b64) const;
	void trust_device(const std::string &device_id,
			  const std::string &name,
			  const std::string &model,
			  const std::string &pk_b64 = "");
	void revoke_all_trusted();
	std::vector<TrustedDevice> list_trusted_devices() const;
	std::string trusted_devices_summary() const;

	// Brute-force / spam protection for repeated rejected attempts
	bool is_device_rate_limited(const std::string &device_id) const;
	void record_denied_attempt(const std::string &device_id);
	void clear_denied_attempts(const std::string &device_id);

	// Called from UxPlay's cb_report_client_request()
	// Returns true to admit the client, false to reject with RTSP 401 Unauthorized.
	bool evaluate_client_request(const std::string &device_id,
				     const std::string &model,
				     const std::string &name,
				     std::string &out_reason);

	// Non-blocking stream-safe 2FA PIN overlay window (hidden from OBS Display Capture via WDA_EXCLUDEFROMCAPTURE)
	void show_pin_popup(const std::string &pin, const std::string &server_name);
	void dismiss_pin_popup();
	std::string current_active_pin() const;

	static std::string default_config_directory();

private:
	void load_trusted_devices_locked();
	void save_trusted_devices_locked() const;
	ApprovalDecision prompt_user_approval(const std::string &device_id,
					      const std::string &model,
					      const std::string &name) const;

	std::string config_dir_;
	std::string trusted_file_path_;
	std::string server_key_path_;

	mutable std::mutex mutex_;
	SecurityPolicy policy_;
	std::vector<TrustedDevice> trusted_devices_;

	struct RateLimitState {
		int denied_count = 0;
		std::chrono::steady_clock::time_point window_start;
		std::chrono::steady_clock::time_point blocked_until;
	};
	mutable std::unordered_map<std::string, RateLimitState> rate_limits_;

	// Non-blocking PIN popup window state
	mutable std::mutex pin_mutex_;
	std::string active_pin_;
	std::atomic<bool> pin_cancel_{false};
	std::atomic<void *> pin_hwnd_{nullptr};
	std::thread pin_thread_;
};

} // namespace obs_airplay
