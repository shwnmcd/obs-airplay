#include <obs-module.h>
#include <util/base.h>

#include "airplay-frame-filter.hpp"
#include "airplay-source.hpp"

// Declare module manually with LIBOBS minimum compatible API version 30.0.0 so a single
// self-contained obs-airplay.dll loads cleanly across OBS Studio 30, 31, 32, 33, and newer.
static obs_module_t *obs_module_pointer = nullptr;

MODULE_EXPORT void obs_module_set_pointer(obs_module_t *module)
{
	obs_module_pointer = module;
}

obs_module_t *obs_current_module(void)
{
	return obs_module_pointer;
}

MODULE_EXPORT uint32_t obs_module_ver(void)
{
	return MAKE_SEMANTIC_VERSION(30, 0, 0);
}

OBS_MODULE_USE_DEFAULT_LOCALE("obs-airplay", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "iOS AirPlay Screen Cast";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Native zero-app iPhone and iPad AirPlay screen mirroring receiver for OBS Studio.";
}

bool obs_module_load(void)
{
	obs_airplay::register_airplay_source();
	obs_airplay::register_airplay_frame_filter();
	blog(LOG_INFO, "[obs-airplay] Plugin loaded successfully (v1.3.0, universal OBS 30-33+)");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-airplay] Plugin unloaded");
}
