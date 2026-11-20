#include <obs-module.h>
#include <util/base.h>

#include "airplay-frame-filter.hpp"
#include "airplay-source.hpp"

OBS_DECLARE_MODULE()
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
	blog(LOG_INFO, "[obs-airplay] Plugin loaded successfully (v1.2.0)");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-airplay] Plugin unloaded");
}
