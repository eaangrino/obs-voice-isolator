#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info voice_isolator_filter;

bool obs_module_load(void)
{
    obs_register_source(&voice_isolator_filter);
    obs_log(LOG_INFO, "loaded successfully (version %s)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "unloaded");
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Aggressive voice isolation filter with RNNoise, breath gating, and optional auxiliary-microphone cancellation.";
}
