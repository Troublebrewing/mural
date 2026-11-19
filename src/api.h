#ifndef Api_h
#define Api_h

#include <ESPAsyncWebServer.h>
#include "movement.h"
#include "phases/phasemanager.h"
#include "runner.h"

void registerApiRoutes(AsyncWebServer& server, Movement* movement, Runner* runner, PhaseManager* phaseManager);

#endif
