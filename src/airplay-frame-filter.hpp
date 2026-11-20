#pragma once

#include <obs-module.h>

namespace obs_airplay {

constexpr const char *kFrameFilterId = "ios_airplay_frame_filter";
constexpr const char *kManagedFrameFilterName = "AirPlay Rounded Frame & Shadow";

// Shared property keys used both by the standalone filter and the AirPlay source UI
constexpr const char *kPropFrameEnabled = "frame_enabled";
constexpr const char *kPropCornerRadius = "corner_radius";
constexpr const char *kPropBorderWidth = "border_width";
constexpr const char *kPropBorderColor = "border_color";

constexpr const char *kPropShadowEnabled = "shadow_enabled";
constexpr const char *kPropShadowColor = "shadow_color";
constexpr const char *kPropShadowBlur = "shadow_blur";
constexpr const char *kPropShadowSpread = "shadow_spread";
constexpr const char *kPropShadowOffsetX = "shadow_offset_x";
constexpr const char *kPropShadowOffsetY = "shadow_offset_y";

void register_airplay_frame_filter();
void set_frame_style_defaults(obs_data_t *settings);
void add_frame_style_properties(obs_properties_t *props);

} // namespace obs_airplay
