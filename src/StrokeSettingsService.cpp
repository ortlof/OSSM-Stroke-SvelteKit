#include <StrokeSettingsService.h>

StrokeSettingsService::StrokeSettingsService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit) : _httpEndpoint(
                                                                                                         StrokeSettings::read,
                                                                                                         StrokeSettings::update,
                                                                                                         this,
                                                                                                         server,
                                                                                                         STROKE_SETTINGS_ENDPOINT_PATH,
                                                                                                         sveltekit->getSecurityManager(),
                                                                                                         AuthenticationPredicates::IS_AUTHENTICATED),
                                                                                                     _fsPersistence(
                                                                                                         StrokeSettings::read,
                                                                                                         StrokeSettings::update,
                                                                                                         this,
                                                                                                         sveltekit->getFS(),
                                                                                                         STROKE_SETTINGS_FILE_PATH)
{
}

void StrokeSettingsService::begin()
{
    // Load the configuration from LittleFS. If the file does not exist the
    // values compiled in above stay in place.
    _fsPersistence.readFromFS();
    _httpEndpoint.begin();
}
