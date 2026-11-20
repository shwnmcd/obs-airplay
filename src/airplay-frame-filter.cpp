#include "airplay-frame-filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/base.h>

namespace obs_airplay {

namespace {

// Embedded fallback copy of data/airplay_frame.effect so the filter still works even if
// only obs-airplay.dll is installed without the data directory.
constexpr const char *kEmbeddedEffectSource = R"EFFECT(
uniform float4x4 ViewProj;
uniform texture2d image;

uniform float2 src_size;
uniform float2 out_size;
uniform float2 pad_origin;

uniform float corner_radius;
uniform float border_width;
uniform float4 border_color;

uniform float shadow_enabled;
uniform float4 shadow_color;
uniform float shadow_blur;
uniform float shadow_spread;
uniform float2 shadow_offset;

sampler_state def_sampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertInOut {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertInOut VSDefault(VertInOut vert_in)
{
	VertInOut vert_out;
	vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = vert_in.uv;
	return vert_out;
}

float sdRoundedBox(float2 p, float2 half_size, float r)
{
	float rr = min(r, min(half_size.x, half_size.y));
	float2 q = abs(p) - half_size + float2(rr, rr);
	return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - rr;
}

float approxTanh(float x)
{
	float xc = clamp(x, -8.0, 8.0);
	float e2x = exp(2.0 * xc);
	return (e2x - 1.0) / (e2x + 1.0);
}

float4 PSAirPlayFrame(VertInOut vert_in) : TARGET
{
	float2 px = vert_in.uv * out_size;
	float2 local_px = px - pad_origin;
	float2 half_size = src_size * 0.5;
	float2 p = local_px - half_size;

	float r = clamp(corner_radius, 0.0, min(half_size.x, half_size.y));
	float d_fg = sdRoundedBox(p, half_size, r);
	float fg_mask = 1.0 - smoothstep(-0.75, 0.75, d_fg);

	float2 src_uv = clamp(local_px / max(src_size, float2(1.0, 1.0)), float2(0.0, 0.0), float2(1.0, 1.0));
	float4 fg = image.Sample(def_sampler, src_uv);

	if (border_width > 0.01 && border_color.a > 0.001) {
		float bw = min(border_width, min(half_size.x, half_size.y));
		float border_mask = smoothstep(-bw - 0.75, -bw + 0.75, d_fg);
		float b_alpha = border_mask * border_color.a;
		fg.rgb = lerp(fg.rgb, border_color.rgb, b_alpha);
		fg.a = max(fg.a, b_alpha);
	}
	fg.a *= fg_mask;

	float4 sh = float4(0.0, 0.0, 0.0, 0.0);
	if (shadow_enabled > 0.5 && shadow_color.a > 0.001) {
		float2 sh_half = max(half_size + float2(shadow_spread, shadow_spread), float2(1.0, 1.0));
		float sh_r = clamp(r + shadow_spread, 0.0, min(sh_half.x, sh_half.y));
		float d_sh = sdRoundedBox(p - shadow_offset, sh_half, sh_r);

		float sh_mask;
		if (shadow_blur <= 0.5) {
			sh_mask = 1.0 - smoothstep(-0.75, 0.75, d_sh);
		} else {
			float sigma = max(shadow_blur * 0.45, 0.5);
			sh_mask = 0.5 * (1.0 - approxTanh(0.79788456 * (d_sh / sigma)));
			float edge_fade = 1.0 - smoothstep(shadow_blur * 1.15, shadow_blur * 1.48, d_sh);
			sh_mask *= edge_fade;
		}
		sh = float4(shadow_color.rgb, shadow_color.a * sh_mask);
	}

	float out_a = fg.a + sh.a * (1.0 - fg.a);
	float3 out_rgb = float3(0.0, 0.0, 0.0);
	if (out_a > 0.0001) {
		out_rgb = (fg.rgb * fg.a + sh.rgb * sh.a * (1.0 - fg.a)) / out_a;
	}
	return float4(out_rgb, out_a);
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(vert_in);
		pixel_shader  = PSAirPlayFrame(vert_in);
	}
}
)EFFECT";

struct FrameFilterContext {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;

	gs_eparam_t *p_src_size = nullptr;
	gs_eparam_t *p_out_size = nullptr;
	gs_eparam_t *p_pad_origin = nullptr;
	gs_eparam_t *p_corner_radius = nullptr;
	gs_eparam_t *p_border_width = nullptr;
	gs_eparam_t *p_border_color = nullptr;
	gs_eparam_t *p_shadow_enabled = nullptr;
	gs_eparam_t *p_shadow_color = nullptr;
	gs_eparam_t *p_shadow_blur = nullptr;
	gs_eparam_t *p_shadow_spread = nullptr;
	gs_eparam_t *p_shadow_offset = nullptr;

	bool frame_enabled = false;
	float corner_radius = 38.0f;
	float border_width = 0.0f;
	struct vec4 border_color = {1.0f, 1.0f, 1.0f, 1.0f};

	bool shadow_enabled = false;
	struct vec4 shadow_color = {0.0f, 0.0f, 0.0f, 0.65f};
	float shadow_blur = 32.0f;
	float shadow_spread = 0.0f;
	float shadow_offset_x = 0.0f;
	float shadow_offset_y = 14.0f;

	uint32_t pad_left = 0;
	uint32_t pad_right = 0;
	uint32_t pad_top = 0;
	uint32_t pad_bottom = 0;
};

void compute_shadow_padding(FrameFilterContext *ctx)
{
	if (!ctx->shadow_enabled) {
		ctx->pad_left = 0;
		ctx->pad_right = 0;
		ctx->pad_top = 0;
		ctx->pad_bottom = 0;
		return;
	}

	const float reach = std::ceil(ctx->shadow_blur * 1.5f + std::max(0.0f, ctx->shadow_spread)) + 2.0f;
	ctx->pad_left = static_cast<uint32_t>(std::max(0.0f, std::ceil(reach - ctx->shadow_offset_x)));
	ctx->pad_right = static_cast<uint32_t>(std::max(0.0f, std::ceil(reach + ctx->shadow_offset_x)));
	ctx->pad_top = static_cast<uint32_t>(std::max(0.0f, std::ceil(reach - ctx->shadow_offset_y)));
	ctx->pad_bottom = static_cast<uint32_t>(std::max(0.0f, std::ceil(reach + ctx->shadow_offset_y)));
}

const char *filter_get_name(void *type_data)
{
	(void)type_data;
	return obs_module_text("FrameFilterName");
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<FrameFilterContext *>(data);
	if (!ctx)
		return;

	ctx->frame_enabled = obs_data_get_bool(settings, kPropFrameEnabled);
	ctx->corner_radius = static_cast<float>(obs_data_get_double(settings, kPropCornerRadius));
	ctx->border_width = static_cast<float>(obs_data_get_double(settings, kPropBorderWidth));
	vec4_from_rgba(&ctx->border_color, static_cast<uint32_t>(obs_data_get_int(settings, kPropBorderColor)));

	ctx->shadow_enabled = obs_data_get_bool(settings, kPropShadowEnabled);
	vec4_from_rgba(&ctx->shadow_color, static_cast<uint32_t>(obs_data_get_int(settings, kPropShadowColor)));
	ctx->shadow_blur = static_cast<float>(obs_data_get_double(settings, kPropShadowBlur));
	ctx->shadow_spread = static_cast<float>(obs_data_get_double(settings, kPropShadowSpread));
	ctx->shadow_offset_x = static_cast<float>(obs_data_get_double(settings, kPropShadowOffsetX));
	ctx->shadow_offset_y = static_cast<float>(obs_data_get_double(settings, kPropShadowOffsetY));

	compute_shadow_padding(ctx);
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new FrameFilterContext();
	ctx->context = source;

	obs_enter_graphics();
	char *effect_path = obs_module_file("airplay_frame.effect");
	char *err_str = nullptr;
	if (effect_path) {
		ctx->effect = gs_effect_create_from_file(effect_path, &err_str);
		bfree(effect_path);
	}
	if (!ctx->effect) {
		if (err_str) {
			blog(LOG_WARNING, "[obs-airplay] File effect load warning (%s); using embedded shader",
			     err_str);
			bfree(err_str);
			err_str = nullptr;
		}
		ctx->effect = gs_effect_create(kEmbeddedEffectSource, "airplay_frame_embedded", &err_str);
	}
	if (!ctx->effect) {
		blog(LOG_ERROR, "[obs-airplay] Failed to compile airplay_frame.effect: %s",
		     err_str ? err_str : "unknown error");
	}
	if (err_str) {
		bfree(err_str);
	}

	if (ctx->effect) {
		ctx->p_src_size = gs_effect_get_param_by_name(ctx->effect, "src_size");
		ctx->p_out_size = gs_effect_get_param_by_name(ctx->effect, "out_size");
		ctx->p_pad_origin = gs_effect_get_param_by_name(ctx->effect, "pad_origin");
		ctx->p_corner_radius = gs_effect_get_param_by_name(ctx->effect, "corner_radius");
		ctx->p_border_width = gs_effect_get_param_by_name(ctx->effect, "border_width");
		ctx->p_border_color = gs_effect_get_param_by_name(ctx->effect, "border_color");
		ctx->p_shadow_enabled = gs_effect_get_param_by_name(ctx->effect, "shadow_enabled");
		ctx->p_shadow_color = gs_effect_get_param_by_name(ctx->effect, "shadow_color");
		ctx->p_shadow_blur = gs_effect_get_param_by_name(ctx->effect, "shadow_blur");
		ctx->p_shadow_spread = gs_effect_get_param_by_name(ctx->effect, "shadow_spread");
		ctx->p_shadow_offset = gs_effect_get_param_by_name(ctx->effect, "shadow_offset");
	}
	obs_leave_graphics();

	filter_update(ctx, settings);
	return ctx;
}

void filter_destroy(void *data)
{
	auto *ctx = static_cast<FrameFilterContext *>(data);
	if (!ctx)
		return;
	if (ctx->effect) {
		obs_enter_graphics();
		gs_effect_destroy(ctx->effect);
		obs_leave_graphics();
	}
	delete ctx;
}

uint32_t filter_get_width(void *data)
{
	auto *ctx = static_cast<FrameFilterContext *>(data);
	if (!ctx)
		return 0;
	obs_source_t *target = obs_filter_get_target(ctx->context);
	uint32_t base_w = target ? obs_source_get_base_width(target) : 0;
	if (base_w == 0)
		return 0;
	if (!ctx->frame_enabled && !ctx->shadow_enabled)
		return base_w;
	return base_w + ctx->pad_left + ctx->pad_right;
}

uint32_t filter_get_height(void *data)
{
	auto *ctx = static_cast<FrameFilterContext *>(data);
	if (!ctx)
		return 0;
	obs_source_t *target = obs_filter_get_target(ctx->context);
	uint32_t base_h = target ? obs_source_get_base_height(target) : 0;
	if (base_h == 0)
		return 0;
	if (!ctx->frame_enabled && !ctx->shadow_enabled)
		return base_h;
	return base_h + ctx->pad_top + ctx->pad_bottom;
}

void filter_video_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	auto *ctx = static_cast<FrameFilterContext *>(data);
	if (!ctx || !ctx->effect || (!ctx->frame_enabled && !ctx->shadow_enabled)) {
		obs_source_skip_video_filter(ctx ? ctx->context : nullptr);
		return;
	}

	obs_source_t *target = obs_filter_get_target(ctx->context);
	if (!target) {
		obs_source_skip_video_filter(ctx->context);
		return;
	}

	const uint32_t src_w = obs_source_get_base_width(target);
	const uint32_t src_h = obs_source_get_base_height(target);
	if (src_w == 0 || src_h == 0) {
		obs_source_skip_video_filter(ctx->context);
		return;
	}

	const uint32_t out_w = src_w + ctx->pad_left + ctx->pad_right;
	const uint32_t out_h = src_h + ctx->pad_top + ctx->pad_bottom;

	if (!obs_source_process_filter_begin(ctx->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		return;
	}

	struct vec2 v_src = {static_cast<float>(src_w), static_cast<float>(src_h)};
	struct vec2 v_out = {static_cast<float>(out_w), static_cast<float>(out_h)};
	struct vec2 v_pad = {static_cast<float>(ctx->pad_left), static_cast<float>(ctx->pad_top)};
	struct vec2 v_off = {ctx->shadow_offset_x, ctx->shadow_offset_y};

	gs_effect_set_vec2(ctx->p_src_size, &v_src);
	gs_effect_set_vec2(ctx->p_out_size, &v_out);
	gs_effect_set_vec2(ctx->p_pad_origin, &v_pad);

	gs_effect_set_float(ctx->p_corner_radius, ctx->frame_enabled ? ctx->corner_radius : 0.0f);
	gs_effect_set_float(ctx->p_border_width, ctx->frame_enabled ? ctx->border_width : 0.0f);
	gs_effect_set_vec4(ctx->p_border_color, &ctx->border_color);

	gs_effect_set_float(ctx->p_shadow_enabled, ctx->shadow_enabled ? 1.0f : 0.0f);
	gs_effect_set_vec4(ctx->p_shadow_color, &ctx->shadow_color);
	gs_effect_set_float(ctx->p_shadow_blur, ctx->shadow_blur);
	gs_effect_set_float(ctx->p_shadow_spread, ctx->shadow_spread);
	gs_effect_set_vec2(ctx->p_shadow_offset, &v_off);

	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA,
				   GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	obs_source_process_filter_end(ctx->context, ctx->effect, out_w, out_h);

	gs_blend_state_pop();
}

obs_properties_t *filter_get_properties(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();
	add_frame_style_properties(props);
	return props;
}

} // namespace

void set_frame_style_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, kPropFrameEnabled, false);
	obs_data_set_default_double(settings, kPropCornerRadius, 38.0);
	obs_data_set_default_double(settings, kPropBorderWidth, 0.0);
	obs_data_set_default_int(settings, kPropBorderColor, 0xFFFFFFFF);

	obs_data_set_default_bool(settings, kPropShadowEnabled, false);
	obs_data_set_default_int(settings, kPropShadowColor, 0xA6000000);
	obs_data_set_default_double(settings, kPropShadowBlur, 32.0);
	obs_data_set_default_double(settings, kPropShadowSpread, 0.0);
	obs_data_set_default_double(settings, kPropShadowOffsetX, 0.0);
	obs_data_set_default_double(settings, kPropShadowOffsetY, 14.0);
}

void add_frame_style_properties(obs_properties_t *props)
{
	obs_properties_t *frame_group = obs_properties_create();
	obs_property_t *p = obs_properties_add_float_slider(
		frame_group, kPropCornerRadius, obs_module_text("CornerRadius"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	p = obs_properties_add_float_slider(
		frame_group, kPropBorderWidth, obs_module_text("BorderWidth"), 0.0, 30.0, 0.5);
	obs_property_float_set_suffix(p, " px");

	obs_properties_add_color_alpha(frame_group, kPropBorderColor, obs_module_text("BorderColor"));

	obs_properties_add_group(props, kPropFrameEnabled, obs_module_text("RoundedCornersGroup"),
				 OBS_GROUP_CHECKABLE, frame_group);

	obs_properties_t *shadow_group = obs_properties_create();
	obs_properties_add_color_alpha(shadow_group, kPropShadowColor, obs_module_text("ShadowColor"));

	p = obs_properties_add_float_slider(
		shadow_group, kPropShadowBlur, obs_module_text("ShadowBlur"), 0.0, 120.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	p = obs_properties_add_float_slider(
		shadow_group, kPropShadowSpread, obs_module_text("ShadowSpread"), -20.0, 50.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	p = obs_properties_add_float_slider(
		shadow_group, kPropShadowOffsetX, obs_module_text("ShadowOffsetX"), -100.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	p = obs_properties_add_float_slider(
		shadow_group, kPropShadowOffsetY, obs_module_text("ShadowOffsetY"), -100.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " px");

	obs_properties_add_group(props, kPropShadowEnabled, obs_module_text("BackShadowGroup"),
				 OBS_GROUP_CHECKABLE, shadow_group);
}

void register_airplay_frame_filter()
{
	struct obs_source_info info = {};
	info.id = kFrameFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = filter_get_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.get_width = filter_get_width;
	info.get_height = filter_get_height;
	info.get_defaults = set_frame_style_defaults;
	info.get_properties = filter_get_properties;
	info.update = filter_update;
	info.video_render = filter_video_render;

	obs_register_source(&info);
}

} // namespace obs_airplay
