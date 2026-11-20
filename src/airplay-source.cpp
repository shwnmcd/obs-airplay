#include "airplay-source.hpp"
#include "airplay-frame-filter.hpp"
#include "airplay-receiver.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <util/base.h>

namespace obs_airplay {

namespace {

constexpr const char *kPropStatus = "status_info";
constexpr const char *kPropServerName = "server_name";
constexpr const char *kPropNetworkIface = "bind_ipv4";
constexpr const char *kPropResolution = "resolution";
constexpr const char *kPropMaxFps = "max_fps";
constexpr const char *kPropEnableHevc = "enable_hevc";
constexpr const char *kPropHardwareDecode = "hardware_decode";
constexpr const char *kPropEnableAudio = "enable_audio";
constexpr const char *kPropSyncVolume = "sync_device_volume";
constexpr const char *kPropClearOnDisconnect = "clear_on_disconnect";
constexpr const char *kPropFixedPorts = "fixed_ports";
constexpr const char *kPropPortOffset = "port_offset";
constexpr const char *kPropMacAddress = "mac_address";
constexpr const char *kPropSecurityGroup = "security_group";
constexpr const char *kPropSecurityMode = "security_mode";
constexpr const char *kPropPassword = "password";
constexpr const char *kPropRequireApproval = "require_pc_approval";
constexpr const char *kPropStrictTrusted = "strict_trusted_only";
constexpr const char *kPropHideFromCapture = "hide_popups_from_capture";
constexpr const char *kPropTrustedInfo = "trusted_devices_info";
constexpr const char *kPropRevokeTrustedBtn = "revoke_trusted_devices";
constexpr const char *kPropVerboseLog = "verbose_logging";
constexpr const char *kPropRestartBtn = "restart_receiver";

struct AirPlaySourceContext {
	obs_source_t *source = nullptr;
	std::unique_ptr<AirPlayReceiver> receiver;
	ReceiverConfig active_config;
};

void parse_resolution(const char *res_str, uint16_t &width, uint16_t &height)
{
	unsigned int w = 1920;
	unsigned int h = 1080;
	if (res_str && std::sscanf(res_str, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240 &&
	    w <= 7680 && h <= 4320) {
		width = static_cast<uint16_t>(w);
		height = static_cast<uint16_t>(h);
		return;
	}
	width = 1920;
	height = 1080;
}

ReceiverConfig config_from_settings(obs_data_t *settings)
{
	ReceiverConfig cfg;
	const char *name = obs_data_get_string(settings, kPropServerName);
	cfg.server_name = (name && name[0] != '\0') ? name : "OBS iPhone Cast";

	parse_resolution(obs_data_get_string(settings, kPropResolution), cfg.width, cfg.height);

	long long fps = obs_data_get_int(settings, kPropMaxFps);
	if (fps < 15 || fps > 120)
		fps = 60;
	cfg.max_fps = static_cast<uint16_t>(fps);
	cfg.refresh_rate = static_cast<uint16_t>(fps);

	cfg.enable_hevc = obs_data_get_bool(settings, kPropEnableHevc);
	cfg.hardware_decode = obs_data_get_bool(settings, kPropHardwareDecode);
	cfg.enable_audio = obs_data_get_bool(settings, kPropEnableAudio);
	cfg.sync_device_volume = obs_data_get_bool(settings, kPropSyncVolume);
	cfg.clear_on_disconnect = obs_data_get_bool(settings, kPropClearOnDisconnect);
	cfg.fixed_ports = obs_data_get_bool(settings, kPropFixedPorts);

	long long offset = obs_data_get_int(settings, kPropPortOffset);
	if (offset < 0)
		offset = 0;
	if (offset > 2000)
		offset = 2000;
	cfg.port_offset = static_cast<uint16_t>(offset);

	const char *mac = obs_data_get_string(settings, kPropMacAddress);
	cfg.mac_address = mac ? mac : "";

	const char *pw = obs_data_get_string(settings, kPropPassword);
	cfg.password = pw ? pw : "";

	long long sec_mode = obs_data_get_int(settings, kPropSecurityMode);
	if (sec_mode < 0 || sec_mode > 3)
		sec_mode = static_cast<long long>(SecurityMode::OneTimePinEvery);
	cfg.security.mode = static_cast<SecurityMode>(sec_mode);
	cfg.security.static_password = cfg.password;
	cfg.security.require_pc_approval = obs_data_get_bool(settings, kPropRequireApproval);
	cfg.security.strict_trusted_only = obs_data_get_bool(settings, kPropStrictTrusted);
	cfg.security.hide_popups_from_capture = obs_data_get_bool(settings, kPropHideFromCapture);

	const char *bind_ip = obs_data_get_string(settings, kPropNetworkIface);
	cfg.bind_ipv4 = (bind_ip && bind_ip[0] != '\0') ? bind_ip : "auto";

	cfg.verbose_logging = obs_data_get_bool(settings, kPropVerboseLog);
	return cfg;
}

bool config_requires_restart(const ReceiverConfig &a, const ReceiverConfig &b)
{
	return a.server_name != b.server_name ||
	       a.bind_ipv4 != b.bind_ipv4 ||
	       a.width != b.width ||
	       a.height != b.height ||
	       a.max_fps != b.max_fps ||
	       a.enable_hevc != b.enable_hevc ||
	       a.hardware_decode != b.hardware_decode ||
	       a.enable_audio != b.enable_audio ||
	       a.sync_device_volume != b.sync_device_volume ||
	       a.clear_on_disconnect != b.clear_on_disconnect ||
	       a.fixed_ports != b.fixed_ports ||
	       a.port_offset != b.port_offset ||
	       a.mac_address != b.mac_address ||
	       a.password != b.password ||
	       a.security.mode != b.security.mode ||
	       a.security.static_password != b.security.static_password ||
	       a.verbose_logging != b.verbose_logging;
}

const char *airplay_get_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("AirPlaySource");
}

void airplay_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kPropServerName, "OBS iPhone Cast");
	obs_data_set_default_string(settings, kPropNetworkIface, "auto");
	obs_data_set_default_string(settings, kPropResolution, "1920x1080");
	obs_data_set_default_int(settings, kPropMaxFps, 60);
	obs_data_set_default_bool(settings, kPropEnableHevc, true);
	obs_data_set_default_bool(settings, kPropHardwareDecode, true);
	obs_data_set_default_bool(settings, kPropEnableAudio, true);
	obs_data_set_default_bool(settings, kPropSyncVolume, true);
	obs_data_set_default_bool(settings, kPropClearOnDisconnect, true);
	obs_data_set_default_bool(settings, kPropFixedPorts, true);
	obs_data_set_default_int(settings, kPropPortOffset, 0);
	obs_data_set_default_string(settings, kPropMacAddress, "");
	obs_data_set_default_int(settings, kPropSecurityMode, static_cast<long long>(SecurityMode::OneTimePinEvery));
	obs_data_set_default_string(settings, kPropPassword, "");
	obs_data_set_default_bool(settings, kPropRequireApproval, true);
	obs_data_set_default_bool(settings, kPropStrictTrusted, false);
	obs_data_set_default_bool(settings, kPropHideFromCapture, true);
	obs_data_set_default_bool(settings, kPropVerboseLog, false);
	set_frame_style_defaults(settings);
}

void sync_frame_filter(AirPlaySourceContext *ctx, obs_data_t *settings)
{
	if (!ctx || !ctx->source || !settings)
		return;

	const bool frame_on = obs_data_get_bool(settings, kPropFrameEnabled);
	const bool shadow_on = obs_data_get_bool(settings, kPropShadowEnabled);
	const bool want_filter = frame_on || shadow_on;

	obs_source_t *existing = obs_source_get_filter_by_name(ctx->source, kManagedFrameFilterName);

	if (want_filter) {
		obs_data_t *fset = obs_data_create();
		obs_data_set_bool(fset, kPropFrameEnabled, frame_on);
		obs_data_set_double(fset, kPropCornerRadius, obs_data_get_double(settings, kPropCornerRadius));
		obs_data_set_double(fset, kPropBorderWidth, obs_data_get_double(settings, kPropBorderWidth));
		obs_data_set_int(fset, kPropBorderColor, obs_data_get_int(settings, kPropBorderColor));

		obs_data_set_bool(fset, kPropShadowEnabled, shadow_on);
		obs_data_set_int(fset, kPropShadowColor, obs_data_get_int(settings, kPropShadowColor));
		obs_data_set_double(fset, kPropShadowBlur, obs_data_get_double(settings, kPropShadowBlur));
		obs_data_set_double(fset, kPropShadowSpread, obs_data_get_double(settings, kPropShadowSpread));
		obs_data_set_double(fset, kPropShadowOffsetX, obs_data_get_double(settings, kPropShadowOffsetX));
		obs_data_set_double(fset, kPropShadowOffsetY, obs_data_get_double(settings, kPropShadowOffsetY));

		if (!existing) {
			obs_source_t *flt = obs_source_create(kFrameFilterId, kManagedFrameFilterName,
							      fset, nullptr);
			if (flt) {
				obs_source_filter_add(ctx->source, flt);
				obs_source_release(flt);
			}
		} else {
			obs_source_update(existing, fset);
			obs_source_set_enabled(existing, true);
		}
		obs_data_release(fset);
	} else if (existing) {
		obs_source_set_enabled(existing, false);
	}

	if (existing) {
		obs_source_release(existing);
	}
}

bool on_restart_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)property;
	auto *ctx = static_cast<AirPlaySourceContext *>(data);
	if (!ctx || !ctx->receiver)
		return false;

	blog(LOG_INFO, "[obs-airplay] Manual receiver restart requested");
	ctx->receiver->stop();
	obs_source_output_video(ctx->source, nullptr);
	ctx->receiver->start(ctx->active_config);

	obs_property_t *status_prop = obs_properties_get(props, kPropStatus);
	if (status_prop) {
		obs_property_set_description(status_prop, ctx->receiver->status_summary().c_str());
	}
	return true;
}

bool on_fixed_ports_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)property;
	bool fixed = obs_data_get_bool(settings, kPropFixedPorts);
	obs_property_t *offset_prop = obs_properties_get(props, kPropPortOffset);
	if (offset_prop) {
		obs_property_set_visible(offset_prop, fixed);
	}
	return true;
}

bool on_security_mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)property;
	const long long mode = obs_data_get_int(settings, kPropSecurityMode);
	obs_property_t *pw_prop = obs_properties_get(props, kPropPassword);
	if (pw_prop) {
		obs_property_set_visible(pw_prop, mode == static_cast<long long>(SecurityMode::StaticPassword));
	}
	return true;
}

bool on_revoke_trusted_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)property;
	auto *ctx = static_cast<AirPlaySourceContext *>(data);
	if (!ctx || !ctx->receiver)
		return false;

	ctx->receiver->revoke_all_trusted_devices();
	if (ctx->active_config.security.mode == SecurityMode::PairingPinTrusted) {
		ctx->receiver->stop();
		obs_source_output_video(ctx->source, nullptr);
		ctx->receiver->start(ctx->active_config);
	}

	obs_property_t *trusted_prop = obs_properties_get(props, kPropTrustedInfo);
	if (trusted_prop) {
		obs_property_set_description(trusted_prop, ctx->receiver->trusted_devices_summary().c_str());
	}
	return true;
}

obs_properties_t *airplay_get_properties(void *data)
{
	auto *ctx = static_cast<AirPlaySourceContext *>(data);
	obs_properties_t *props = obs_properties_create();

	std::string status = (ctx && ctx->receiver) ? ctx->receiver->status_summary() : "Not initialized";
	obs_properties_add_text(props, kPropStatus, status.c_str(), OBS_TEXT_INFO);

	obs_property_t *p = obs_properties_add_text(props, kPropServerName,
						    obs_module_text("ServerName"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, obs_module_text("ServerName.Description"));

	obs_property_t *iface = obs_properties_add_list(props, kPropNetworkIface,
							obs_module_text("NetworkInterface"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(iface, obs_module_text("NetworkInterface.Description"));
	obs_property_list_add_string(iface, "Auto (Best Local Wi-Fi / LAN)", "auto");
	for (const auto &adapter : AirPlayReceiver::enumerate_ipv4_adapters()) {
		std::string label = adapter.name + " (" + adapter.ipv4 + ")";
		obs_property_list_add_string(iface, label.c_str(), adapter.ipv4.c_str());
	}

	obs_property_t *res = obs_properties_add_list(props, kPropResolution,
						      obs_module_text("Resolution"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(res, "1920x1080 (Full HD)", "1920x1080");
	obs_property_list_add_string(res, "2560x1440 (QHD)", "2560x1440");
	obs_property_list_add_string(res, "3840x2160 (4K UHD)", "3840x2160");
	obs_property_list_add_string(res, "1280x720 (HD)", "1280x720");

	obs_property_t *fps = obs_properties_add_list(props, kPropMaxFps,
						      obs_module_text("MaxFps"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "60 FPS", 60);
	obs_property_list_add_int(fps, "30 FPS", 30);
	obs_property_list_add_int(fps, "120 FPS (ProMotion)", 120);

	// Security & 2FA group (anti-rando protection)
	obs_properties_t *sec_group = obs_properties_create();

	obs_property_t *sec_mode = obs_properties_add_list(sec_group, kPropSecurityMode,
							   obs_module_text("SecurityMode"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sec_mode, obs_module_text("SecurityMode.OneTimePin"),
				  static_cast<long long>(SecurityMode::OneTimePinEvery));
	obs_property_list_add_int(sec_mode, obs_module_text("SecurityMode.PairingPinTrusted"),
				  static_cast<long long>(SecurityMode::PairingPinTrusted));
	obs_property_list_add_int(sec_mode, obs_module_text("SecurityMode.StaticPassword"),
				  static_cast<long long>(SecurityMode::StaticPassword));
	obs_property_list_add_int(sec_mode, obs_module_text("SecurityMode.None"),
				  static_cast<long long>(SecurityMode::None));
	obs_property_set_long_description(sec_mode, obs_module_text("SecurityMode.Description"));
	obs_property_set_modified_callback(sec_mode, on_security_mode_modified);

	p = obs_properties_add_text(sec_group, kPropPassword, obs_module_text("Password"), OBS_TEXT_PASSWORD);
	obs_property_set_long_description(p, obs_module_text("Password.Description"));

	p = obs_properties_add_bool(sec_group, kPropRequireApproval, obs_module_text("RequirePcApproval"));
	obs_property_set_long_description(p, obs_module_text("RequirePcApproval.Description"));

	p = obs_properties_add_bool(sec_group, kPropStrictTrusted, obs_module_text("StrictTrustedOnly"));
	obs_property_set_long_description(p, obs_module_text("StrictTrustedOnly.Description"));

	p = obs_properties_add_bool(sec_group, kPropHideFromCapture, obs_module_text("HidePopupsFromCapture"));
	obs_property_set_long_description(p, obs_module_text("HidePopupsFromCapture.Description"));

	std::string trusted_txt = (ctx && ctx->receiver) ? ctx->receiver->trusted_devices_summary()
							 : "Trusted Devices: None";
	obs_properties_add_text(sec_group, kPropTrustedInfo, trusted_txt.c_str(), OBS_TEXT_INFO);

	obs_properties_add_button2(sec_group, kPropRevokeTrustedBtn, obs_module_text("RevokeTrustedDevices"),
				   on_revoke_trusted_clicked, ctx);

	obs_properties_add_group(props, kPropSecurityGroup, obs_module_text("SecurityGroup"),
				 OBS_GROUP_NORMAL, sec_group);

	// GPU-accelerated Rounded Corners, Border, and Back Shadow controls
	add_frame_style_properties(props);

	p = obs_properties_add_bool(props, kPropEnableHevc, obs_module_text("EnableHevc"));
	obs_property_set_long_description(p, obs_module_text("EnableHevc.Description"));

	p = obs_properties_add_bool(props, kPropHardwareDecode, obs_module_text("HardwareDecode"));
	obs_property_set_long_description(p, obs_module_text("HardwareDecode.Description"));

	p = obs_properties_add_bool(props, kPropEnableAudio, obs_module_text("EnableAudio"));
	obs_property_set_long_description(p, obs_module_text("EnableAudio.Description"));

	p = obs_properties_add_bool(props, kPropSyncVolume, obs_module_text("SyncDeviceVolume"));
	obs_property_set_long_description(p, obs_module_text("SyncDeviceVolume.Description"));

	p = obs_properties_add_bool(props, kPropClearOnDisconnect, obs_module_text("ClearOnDisconnect"));
	obs_property_set_long_description(p, obs_module_text("ClearOnDisconnect.Description"));

	obs_property_t *fixed = obs_properties_add_bool(props, kPropFixedPorts, obs_module_text("FixedPorts"));
	obs_property_set_long_description(fixed, obs_module_text("FixedPorts.Description"));
	obs_property_set_modified_callback(fixed, on_fixed_ports_modified);

	obs_properties_add_int(props, kPropPortOffset, obs_module_text("PortOffset"), 0, 2000, 10);

	p = obs_properties_add_text(props, kPropMacAddress, obs_module_text("MacAddress"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, obs_module_text("MacAddress.Description"));

	obs_properties_add_bool(props, kPropVerboseLog, obs_module_text("VerboseLogging"));

	obs_properties_add_button2(props, kPropRestartBtn, obs_module_text("RestartReceiver"),
				   on_restart_clicked, ctx);

	return props;
}

void airplay_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<AirPlaySourceContext *>(data);
	if (!ctx || !ctx->receiver)
		return;

	sync_frame_filter(ctx, settings);

	ReceiverConfig next_cfg = config_from_settings(settings);
	if (!ctx->receiver->is_running() || config_requires_restart(ctx->active_config, next_cfg)) {
		ctx->active_config = next_cfg;
		ctx->receiver->stop();
		if (ctx->active_config.clear_on_disconnect) {
			obs_source_output_video(ctx->source, nullptr);
		}
		ctx->receiver->start(ctx->active_config);
	} else {
		// Apply non-restart security policy toggles (PC approval / strict allowlist / capture hide) live
		ctx->active_config.security = next_cfg.security;
		ctx->receiver->update_security_policy(next_cfg.security);
	}
}

void airplay_load(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<AirPlaySourceContext *>(data);
	sync_frame_filter(ctx, settings);
}

void *airplay_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new AirPlaySourceContext();
	ctx->source = source;

	// Use low-latency async video buffering in OBS
	obs_source_set_async_unbuffered(source, true);

	ctx->receiver = std::make_unique<AirPlayReceiver>(
		[source](const struct obs_source_frame *frame) {
			obs_source_output_video(source, frame);
		},
		[source](const struct obs_source_audio *audio) {
			obs_source_output_audio(source, audio);
		},
		[source]() {
			obs_source_output_video(source, nullptr);
		},
		[](const std::string &status) {
			blog(LOG_INFO, "[obs-airplay] Status: %s", status.c_str());
		});

	ctx->active_config = config_from_settings(settings);
	ctx->receiver->start(ctx->active_config);
	return ctx;
}

void airplay_destroy(void *data)
{
	auto *ctx = static_cast<AirPlaySourceContext *>(data);
	if (!ctx)
		return;
	if (ctx->receiver) {
		ctx->receiver->stop();
	}
	delete ctx;
}

} // namespace

void register_airplay_source()
{
	struct obs_source_info info = {};
	info.id = "ios_airplay_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = airplay_get_name;
	info.create = airplay_create;
	info.destroy = airplay_destroy;
	info.get_defaults = airplay_get_defaults;
	info.get_properties = airplay_get_properties;
	info.update = airplay_update;
	info.load = airplay_load;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	obs_register_source(&info);
}

} // namespace obs_airplay
