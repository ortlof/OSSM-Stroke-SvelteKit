#include <StrokeStateService.h>

StrokeStateService::StrokeStateService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit) : _httpEndpoint(
                                                                                                   // reader, updater, service, server, path, security, predicate
                                                                                                   StrokeState::read,
                                                                                                   StrokeState::update,
                                                                                                   this,
                                                                                                   server,
                                                                                                   STROKE_STATE_ENDPOINT_PATH,
                                                                                                   sveltekit->getSecurityManager(),
                                                                                                   AuthenticationPredicates::IS_AUTHENTICATED),
                                                                                               _eventEndpoint(
                                                                                                   StrokeState::read,
                                                                                                   StrokeState::update,
                                                                                                   this,
                                                                                                   sveltekit->getSocket(),
                                                                                                   STROKE_STATE_EVENT)
{
    // The state is live data and is not written to the file system.
    // Motion commands are applied by the main program, which registers its own
    // update handler on this service.
}

void StrokeStateService::begin()
{
    _httpEndpoint.begin();
    _eventEndpoint.begin();
}
