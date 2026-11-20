#include "airplay-receiver.hpp"
#include "media-decoder.hpp"
#include "security-manager.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

static int g_failures = 0;

#define CHECK(cond, msg)                                                           \
	do {                                                                       \
		if (!(cond)) {                                                     \
			std::cerr << "[FAIL] " << (msg) << " (" << #cond << ")\n"; \
			++g_failures;                                              \
		} else {                                                           \
			std::cout << "[PASS] " << (msg) << "\n";                   \
		}                                                                  \
	} while (0)

static void test_mac_derivation()
{
	std::string mac1 = obs_airplay::AirPlayReceiver::derive_mac_address("OBS iPhone Cast");
	std::string mac2 = obs_airplay::AirPlayReceiver::derive_mac_address("OBS iPhone Cast");
	std::string mac3 = obs_airplay::AirPlayReceiver::derive_mac_address("OBS iPad Cast");

	CHECK(mac1 == mac2, "MAC derivation is deterministic for identical receiver names");
	CHECK(mac1 != mac3, "MAC derivation differs for distinct receiver names");

	std::vector<char> bytes;
	CHECK(obs_airplay::AirPlayReceiver::parse_mac_bytes(mac1, bytes) && bytes.size() == 6,
	      "Derived MAC parses into 6 raw bytes");
	// Check locally-administered unicast bits (bit 1 = 1, bit 0 = 0)
	uint8_t first = static_cast<uint8_t>(bytes[0]);
	CHECK((first & 0x03) == 0x02, "Derived MAC is locally-administered unicast");
}

static void test_audio_decoder_pcm_and_volume()
{
	std::atomic<int> callback_count{0};
	uint32_t last_frames = 0;
	float sample_peak = 0.0f;

	obs_airplay::MediaDecoder decoder(
		nullptr,
		[&](const struct obs_source_audio *audio) {
			callback_count.fetch_add(1);
			last_frames = audio->frames;
			const auto *left = reinterpret_cast<const float *>(audio->data[0]);
			for (uint32_t i = 0; i < audio->frames; ++i) {
				sample_peak = std::max(sample_peak, std::fabs(left[i]));
			}
		});

	obs_airplay::DecoderConfig cfg;
	cfg.enable_audio = true;
	cfg.sync_device_volume = true;
	decoder.configure(cfg);

	// 480 stereo S16LE samples at half full scale (16384 -> ~0.5f)
	std::vector<int16_t> pcm(480 * 2, 16384);
	decoder.set_volume_gain(0.5f); // Expect peak ~0.25f

	bool ok = decoder.decode_audio(reinterpret_cast<const uint8_t *>(pcm.data()),
				       static_cast<int>(pcm.size() * sizeof(int16_t)),
				       1, 1000000ULL);
	CHECK(ok && callback_count.load() == 1, "PCM audio packet decoded and delivered to OBS callback");
	CHECK(last_frames == 480, "Decoded audio frame count matches 480 stereo samples");
	CHECK(std::fabs(sample_peak - 0.25f) < 0.01f, "Volume gain scaling applied accurately (0.5 * 0.5 = 0.25)");
}

static void test_video_decoder_h264_roundtrip()
{
	// Verify FFmpeg H.264 decoder (linked against OBS Studio 33's avcodec-62.dll) initializes
	// and decodes a real Annex-B H.264 bitstream if an encoder is available, or initializes cleanly.
	const AVCodec *h264_dec = avcodec_find_decoder(AV_CODEC_ID_H264);
	const AVCodec *hevc_dec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
	const AVCodec *aac_dec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	const AVCodec *alac_dec = avcodec_find_decoder(AV_CODEC_ID_ALAC);

	CHECK(h264_dec != nullptr, "OBS FFmpeg avcodec-62 provides H.264 decoder");
	CHECK(hevc_dec != nullptr, "OBS FFmpeg avcodec-62 provides HEVC/H.265 decoder");
	CHECK(aac_dec != nullptr, "OBS FFmpeg avcodec-62 provides AAC/AAC-ELD decoder");
	CHECK(alac_dec != nullptr, "OBS FFmpeg avcodec-62 provides ALAC decoder");

	// Try encoding a 160x96 solid YUV420P frame with libx264 (bundled in OBS FFmpeg) -> decode with MediaDecoder!
	const AVCodec *h264_enc = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (h264_enc) {
		AVCodecContext *enc_ctx = avcodec_alloc_context3(h264_enc);
		enc_ctx->width = 160;
		enc_ctx->height = 96;
		enc_ctx->time_base = AVRational{1, 30};
		enc_ctx->framerate = AVRational{30, 1};
		enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
		enc_ctx->gop_size = 1;
		av_opt_set(enc_ctx->priv_data, "preset", "ultrafast", 0);
		av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);

		if (avcodec_open2(enc_ctx, h264_enc, nullptr) == 0) {
			AVFrame *src_frame = av_frame_alloc();
			src_frame->format = AV_PIX_FMT_YUV420P;
			src_frame->width = 160;
			src_frame->height = 96;
			av_frame_get_buffer(src_frame, 32);
			std::memset(src_frame->data[0], 128, static_cast<size_t>(src_frame->linesize[0]) * 96);
			std::memset(src_frame->data[1], 128, static_cast<size_t>(src_frame->linesize[1]) * 48);
			std::memset(src_frame->data[2], 128, static_cast<size_t>(src_frame->linesize[2]) * 48);
			src_frame->pts = 0;

			std::atomic<int> decoded_frames{0};
			uint32_t out_w = 0;
			uint32_t out_h = 0;
			enum video_format out_fmt = VIDEO_FORMAT_NONE;

			obs_airplay::MediaDecoder decoder(
				[&](const struct obs_source_frame *frame) {
					decoded_frames.fetch_add(1);
					out_w = frame->width;
					out_h = frame->height;
					out_fmt = frame->format;
				},
				nullptr);

			obs_airplay::DecoderConfig cfg;
			cfg.hardware_decode = false; // Software path deterministic in headless test
			decoder.configure(cfg);

			AVPacket *pkt = av_packet_alloc();
			avcodec_send_frame(enc_ctx, src_frame);
			avcodec_send_frame(enc_ctx, nullptr);
			while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
				decoder.decode_video(pkt->data, pkt->size, false, 123456789ULL);
				av_packet_unref(pkt);
			}
			av_packet_free(&pkt);
			av_frame_free(&src_frame);

			CHECK(decoded_frames.load() >= 1, "End-to-end H.264 Annex-B decode produced obs_source_frame");
			CHECK(out_w == 160 && out_h == 96, "Decoded obs_source_frame matches 160x96 geometry");
			CHECK(out_fmt == VIDEO_FORMAT_I420 || out_fmt == VIDEO_FORMAT_NV12,
			      "Decoded obs_source_frame uses native YUV OBS format");
		}
		avcodec_free_context(&enc_ctx);
	} else {
		std::cout << "[INFO] Skipping H.264 encode roundtrip (no H.264 encoder present)\n";
	}
}

static void test_live_airplay_receiver_lifecycle()
{
	std::string latest_status;
	obs_airplay::AirPlayReceiver receiver(
		nullptr,
		nullptr,
		nullptr,
		[&](const std::string &s) { latest_status = s; });

	obs_airplay::ReceiverConfig cfg;
	cfg.server_name = "OBS AirPlay SelfTest";
	cfg.fixed_ports = true;
	cfg.port_offset = 1230; // Use high test ports so we don't collide with running OBS
	cfg.enable_hevc = true;

	bool started = receiver.start(cfg);
	CHECK(started && receiver.is_running(), "AirPlayReceiver starts RAOP HTTP/RTSP server and mDNS broadcast");
	std::string active_ip = receiver.active_ipv4();
	std::cout << "[INFO] Auto-selected mDNS IPv4 address: " << active_ip << "\n";
	CHECK(active_ip != "unknown" && active_ip.rfind("26.", 0) != 0,
	      "Auto mDNS interface selection skipped VPN (26.x) and picked real local LAN IP (" + active_ip + ")");
	uint16_t port = receiver.airplay_port();
	CHECK(port == 8230 || port > 0, "AirPlayReceiver bound listening TCP port (" + std::to_string(port) + ")");

#ifdef _WIN32
	// Give httpd thread a moment to enter accept loop, then test real RTSP GET /info handshake
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	int conn_res = connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	if (conn_res != 0) {
		int wsa_err = WSAGetLastError();
		std::cout << "[INFO] connect(127.0.0.1:" << port << ") failed with WSA error " << wsa_err << "\n";
	}
	CHECK(conn_res == 0, "TCP connection succeeded to live AirPlay RTSP server port");
	if (conn_res == 0) {
		const char *req = "GET /info RTSP/1.0\r\nCSeq: 1\r\nX-Apple-Device-ID: 0x001122334455\r\n\r\n";
		send(sock, req, static_cast<int>(std::strlen(req)), 0);
		char resp[512] = {};
		int n = recv(sock, resp, static_cast<int>(sizeof(resp) - 1), 0);
		CHECK(n > 0 && std::strstr(resp, "RTSP/1.0 200 OK") != nullptr,
		      "Live AirPlay RTSP server responded with RTSP/1.0 200 OK to GET /info");
	}
	closesocket(sock);
#endif

	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	receiver.stop();
	CHECK(!receiver.is_running(), "AirPlayReceiver stopped cleanly without hanging");
}

#ifdef _WIN32
static void test_plugin_dll_exports(const char *dll_path, const char *obs_bin_dir)
{
	SetDllDirectoryA(obs_bin_dir);
	HMODULE mod = LoadLibraryA(dll_path);
	CHECK(mod != nullptr, std::string("LoadLibraryA succeeded for ") + dll_path);
	if (!mod) {
		std::cerr << "       Win32 GetLastError = " << GetLastError() << "\n";
		return;
	}

	auto fn_ver = reinterpret_cast<uint32_t (*)()>(GetProcAddress(mod, "obs_module_ver"));
	auto fn_set_ptr = GetProcAddress(mod, "obs_module_set_pointer");
	auto fn_load = GetProcAddress(mod, "obs_module_load");
	auto fn_unload = GetProcAddress(mod, "obs_module_unload");
	auto fn_name = reinterpret_cast<const char *(*)()>(GetProcAddress(mod, "obs_module_name"));
	auto fn_desc = reinterpret_cast<const char *(*)()>(GetProcAddress(mod, "obs_module_description"));

	CHECK(fn_ver != nullptr, "Export present: obs_module_ver");
	CHECK(fn_set_ptr != nullptr, "Export present: obs_module_set_pointer");
	CHECK(fn_load != nullptr, "Export present: obs_module_load");
	CHECK(fn_unload != nullptr, "Export present: obs_module_unload");
	CHECK(fn_name != nullptr, "Export present: obs_module_name");
	CHECK(fn_desc != nullptr, "Export present: obs_module_description");

	if (fn_ver) {
		uint32_t ver = fn_ver();
		uint32_t major = (ver >> 24) & 0xFF;
		uint32_t minor = (ver >> 16) & 0xFF;
		uint32_t patch = ver & 0xFFFF;
		std::cout << "[INFO] obs_module_ver() = " << major << "." << minor << "." << patch << "\n";
		CHECK(major == 33, "Plugin reports LIBOBS_API_MAJOR_VER == 33");
	}
	if (fn_name) {
		std::cout << "[INFO] obs_module_name() = " << fn_name() << "\n";
	}

	FreeLibrary(mod);
}
#endif

static void test_security_manager_2fa_and_allowlist()
{
	std::filesystem::path tmp_dir =
		std::filesystem::temp_directory_path() / "obs-airplay-sec-test";
	std::error_code ec;
	std::filesystem::remove_all(tmp_dir, ec);

	{
		obs_airplay::SecurityManager sec(tmp_dir.string());
		obs_airplay::SecurityPolicy pol;
		pol.mode = obs_airplay::SecurityMode::OneTimePinEvery;
		pol.require_pc_approval = true;
		pol.strict_trusted_only = false;
		pol.interactive_prompts_enabled = false; // Headless test: auto-denies unapproved prompts
		sec.set_policy(pol);

		// Unknown rando should be denied when PC approval is required and unapproved
		std::string reason;
		bool rando_ok = sec.evaluate_client_request("AA:BB:CC:DD:EE:01", "iPhone15,2", "Rando Phone", reason);
		CHECK(!rando_ok, "Untrusted device denied when PC approval is not granted");

		// Repeated denials trigger brute-force rate-limit cooldown after 3 attempts
		sec.evaluate_client_request("AA:BB:CC:DD:EE:01", "iPhone15,2", "Rando Phone", reason);
		sec.evaluate_client_request("AA:BB:CC:DD:EE:01", "iPhone15,2", "Rando Phone", reason);
		CHECK(sec.is_device_rate_limited("AA:BB:CC:DD:EE:01"),
		      "Brute-force rate limiter engages cooldown after 3 denied attempts");

		// Trusting owner's iPhone + Ed25519 key
		sec.trust_device("5E:C1:3B:DD:39:F5", "Owner iPhone", "iPhone16,1", "BASE64ED25519KEY==");
		CHECK(sec.is_device_trusted("5E:C1:3B:DD:39:F5"), "Trusted device recorded in store");
		CHECK(sec.is_public_key_registered("BASE64ED25519KEY=="), "Ed25519 public key recorded in store");

		// Enable strict allowlist: trusted device passes, unknown device silently blocked
		pol.strict_trusted_only = true;
		sec.set_policy(pol);
		bool owner_ok = sec.evaluate_client_request("5E:C1:3B:DD:39:F5", "iPhone16,1", "Owner iPhone", reason);
		bool unknown_ok = sec.evaluate_client_request("11:22:33:44:55:66", "iPhone14,2", "Stranger", reason);
		CHECK(owner_ok, "Trusted owner iPhone admitted in Strict Allowlist mode");
		CHECK(!unknown_ok, "Unknown device blocked in Strict Allowlist mode");
	}

	// Reload from disk to verify persistence across OBS restarts
	{
		obs_airplay::SecurityManager reloaded(tmp_dir.string());
		CHECK(reloaded.is_device_trusted("5E:C1:3B:DD:39:F5"),
		      "Trusted device persists across SecurityManager restarts");
		CHECK(reloaded.is_public_key_registered("BASE64ED25519KEY=="),
		      "Ed25519 public key persists across SecurityManager restarts");

		reloaded.revoke_all_trusted();
		CHECK(!reloaded.is_device_trusted("5E:C1:3B:DD:39:F5"),
		      "revoke_all_trusted() clears all trusted devices");
		CHECK(!reloaded.is_public_key_registered("BASE64ED25519KEY=="),
		      "revoke_all_trusted() clears all registered Ed25519 keys");
	}

	std::filesystem::remove_all(tmp_dir, ec);
}

int main(int argc, char **argv)
{
	std::cout << "=== obs-airplay Native Verification Suite ===\n";
	test_mac_derivation();
	test_audio_decoder_pcm_and_volume();
	test_video_decoder_h264_roundtrip();
	test_security_manager_2fa_and_allowlist();
	test_live_airplay_receiver_lifecycle();

#ifdef _WIN32
	if (argc >= 3) {
		test_plugin_dll_exports(argv[1], argv[2]);
	}
#endif

	if (g_failures > 0) {
		std::cerr << "\nFAILED with " << g_failures << " failure(s).\n";
		return 1;
	}
	std::cout << "\nALL TESTS PASSED!\n";
	return 0;
}
