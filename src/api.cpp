#include "api.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {
    constexpr const char* API_TEMP_PATH = "/api-commands.tmp";
    constexpr const char* COMMANDS_PATH = "/commands";
    constexpr const char* BACKUP_PATH = "/commands.previous";
    constexpr size_t MAX_COMMAND_LINE_LENGTH = 128;

    struct UploadContext {
        File file;
        size_t bytesWritten = 0;
        size_t totalBytes = 0;
        bool rejected = false;
        bool responseSent = false;
        bool ownsUploadSlot = false;
        bool hasFile = false;
    };

    Movement* apiMovement;
    Runner* apiRunner;
    PhaseManager* apiPhaseManager;
    volatile bool apiUploadInProgress = false;

    void addCorsHeaders(AsyncWebServerResponse* response) {
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        response->addHeader("Access-Control-Allow-Headers", "Content-Type");
    }

    void sendJson(AsyncWebServerRequest* request, int code, const String& body) {
        AsyncWebServerResponse* response = request->beginResponse(code, "application/json", body);
        addCorsHeaders(response);
        request->send(response);
    }

    void sendError(AsyncWebServerRequest* request, int code, const char* message) {
        sendJson(request, code, String("{\"error\":\"") + message + "\"}");
    }

    const char* getStatus() {
        if (apiRunner->hasError()) {
            return "error";
        }
        if (apiRunner->isRunning()) {
            return "printing";
        }
        if (apiMovement->isMoving() &&
            strcmp(apiPhaseManager->getCurrentPhase()->getName(), "ExtendToHome") == 0) {
            return "homing";
        }
        return "idle";
    }

    bool isBusy() {
        return apiUploadInProgress || apiRunner->isRunning() || apiRunner->hasError() || apiMovement->isMoving();
    }

    bool getQueryBool(AsyncWebServerRequest* request, const char* name, bool defaultValue, bool& valid) {
        valid = true;
        if (!request->hasParam(name, false)) {
            return defaultValue;
        }
        String value = request->getParam(name, false)->value();
        value.toLowerCase();
        if (value == "true") {
            return true;
        }
        if (value == "false") {
            return false;
        }
        valid = false;
        return defaultValue;
    }

    bool parseFiniteNumber(const String& text, double& value) {
        String trimmed = text;
        trimmed.trim();
        if (trimmed.length() == 0) {
            return false;
        }

        char* end = nullptr;
        const char* start = trimmed.c_str();
        value = strtod(start, &end);
        return end != start && *end == '\0' && isfinite(value);
    }

    bool readLine(File& file, String& line) {
        line = "";
        bool gotCharacter = false;
        while (file.available()) {
            int value = file.read();
            if (value < 0) {
                break;
            }
            if (value == '\n') {
                return true;
            }
            gotCharacter = true;
            if (line.length() >= MAX_COMMAND_LINE_LENGTH) {
                return false;
            }
            line += static_cast<char>(value);
        }
        return gotCharacter;
    }

    bool validateCommandsFile(File& file) {
        String line;
        double value;
        if (!readLine(file, line) || line.length() < 2) {
            return false;
        }
        line.trim();
        if (!line.startsWith("d") || !parseFiniteNumber(line.substring(1), value) || value <= 0) {
            return false;
        }

        if (!readLine(file, line) || line.length() < 2) {
            return false;
        }
        line.trim();
        if (!line.startsWith("h") || !parseFiniteNumber(line.substring(1), value) || value <= 0) {
            return false;
        }

        while (file.available()) {
            if (!readLine(file, line)) {
                return false;
            }
            line.trim();
            if (line.length() == 0) {
                return false;
            }
            if (line == "p0" || line == "p1") {
                continue;
            }

            const int separator = line.indexOf(' ');
            if (separator <= 0 || separator >= static_cast<int>(line.length()) - 1) {
                return false;
            }
            double x;
            double y;
            if (!parseFiniteNumber(line.substring(0, separator), x) ||
                !parseFiniteNumber(line.substring(separator + 1), y) ||
                x < 0 || y < 0 ||
                (apiMovement->getTopDistance() > 0 && x > apiMovement->getWidth() + 1)) {
                return false;
            }
        }
        return true;
    }

    void cleanupUpload(UploadContext* context) {
        if (context->hasFile) {
            context->file.close();
            context->hasFile = false;
        }
        if (SD.exists(API_TEMP_PATH)) {
            SD.remove(API_TEMP_PATH);
        }
        if (context->ownsUploadSlot) {
            apiUploadInProgress = false;
            context->ownsUploadSlot = false;
        }
    }

    void rejectUpload(AsyncWebServerRequest* request, UploadContext* context, int code, const char* message) {
        context->rejected = true;
        cleanupUpload(context);
        if (!context->responseSent) {
            sendError(request, code, message);
            context->responseSent = true;
        }
    }

    bool hasSupportedContentType(AsyncWebServerRequest* request) {
        String contentType = request->contentType();
        contentType.toLowerCase();
        return contentType.startsWith("text/plain") ||
               contentType.startsWith("application/octet-stream");
    }

    void handleApiUpload(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
        UploadContext* context = static_cast<UploadContext*>(request->_tempObject);
        if (index == 0 && context == nullptr) {
            context = new UploadContext();
            request->_tempObject = context;
            request->onDisconnect([request]() {
                UploadContext* disconnectedContext = static_cast<UploadContext*>(request->_tempObject);
                if (disconnectedContext != nullptr) {
                    cleanupUpload(disconnectedContext);
                    delete disconnectedContext;
                    request->_tempObject = nullptr;
                }
            });

            if (isBusy()) {
                rejectUpload(request, context, 409, "Plotter is busy");
                return;
            }
            context->ownsUploadSlot = true;
            apiUploadInProgress = true;

            bool validRequireHome;
            const bool requireHome = getQueryBool(request, "require_home", true, validRequireHome);
            if (!validRequireHome) {
                rejectUpload(request, context, 400, "Invalid require_home value");
                return;
            }
            if (requireHome && !apiMovement->isHomed()) {
                rejectUpload(request, context, 412, "Plotter is not homed");
                return;
            }
            if (!hasSupportedContentType(request)) {
                rejectUpload(request, context, 415, "Content-Type must be text/plain or application/octet-stream");
                return;
            }

            const size_t totalSpace = SD.totalBytes();
            const size_t usedSpace = SD.usedBytes();
            const size_t freeBytes = usedSpace < totalSpace ? totalSpace - usedSpace : 0;
            if (total == 0) {
                rejectUpload(request, context, 400, "Command upload is empty");
                return;
            }
            if (freeBytes < total) {
                rejectUpload(request, context, 507, "Not enough space on SD card");
                return;
            }

            if (SD.exists(API_TEMP_PATH) && !SD.remove(API_TEMP_PATH)) {
                rejectUpload(request, context, 500, "Could not prepare command file");
                return;
            }
            context->file = SD.open(API_TEMP_PATH, FILE_WRITE);
            if (!context->file) {
                rejectUpload(request, context, 500, "Could not open command file");
                return;
            }
            context->hasFile = true;
            context->totalBytes = total;
        }

        if (context == nullptr || context->rejected) {
            return;
        }
        if (index != context->bytesWritten || len > context->totalBytes - context->bytesWritten) {
            rejectUpload(request, context, 400, "Invalid command upload stream");
            return;
        }
        if (len > 0) {
            const size_t written = context->file.write(data, len);
            if (written != len) {
                rejectUpload(request, context, 507, "Failed to write command file");
                return;
            }
            context->bytesWritten += written;
        }
    }

    bool installCommandsFile() {
        const bool hadPrevious = SD.exists(COMMANDS_PATH);
        if (SD.exists(BACKUP_PATH) && !SD.remove(BACKUP_PATH)) {
            return false;
        }
        if (hadPrevious && !SD.rename(COMMANDS_PATH, BACKUP_PATH)) {
            return false;
        }
        if (!SD.rename(API_TEMP_PATH, COMMANDS_PATH)) {
            if (hadPrevious) {
                SD.rename(BACKUP_PATH, COMMANDS_PATH);
            }
            return false;
        }
        if (hadPrevious) {
            SD.remove(BACKUP_PATH);
        }
        return true;
    }

    void handleApiPrintComplete(AsyncWebServerRequest* request) {
        UploadContext* context = static_cast<UploadContext*>(request->_tempObject);
        request->_tempObject = nullptr;
        if (context == nullptr) {
            if (isBusy()) {
                sendError(request, 409, "Plotter is busy");
            } else {
                bool validRequireHome;
                const bool requireHome = getQueryBool(request, "require_home", true, validRequireHome);
                if (!validRequireHome) {
                    sendError(request, 400, "Invalid require_home value");
                } else if (requireHome && !apiMovement->isHomed()) {
                    sendError(request, 412, "Plotter is not homed");
                } else {
                    sendError(request, 400, "Command upload is empty");
                }
            }
            return;
        }

        if (context->rejected) {
            delete context;
            return;
        }
        if (context->bytesWritten != context->totalBytes) {
            rejectUpload(request, context, 400, "Command upload was incomplete");
            delete context;
            return;
        }

        context->file.flush();
        context->file.close();
        context->hasFile = false;
        File validationFile = SD.open(API_TEMP_PATH, FILE_READ);
        if (!validationFile || !validateCommandsFile(validationFile)) {
            validationFile.close();
            rejectUpload(request, context, 400, "Invalid command file format");
            delete context;
            return;
        }
        validationFile.close();

        bool validAutostart;
        const bool autostart = getQueryBool(request, "autostart", true, validAutostart);
        if (!validAutostart) {
            rejectUpload(request, context, 400, "Invalid autostart value");
            delete context;
            return;
        }
        if (!installCommandsFile()) {
            rejectUpload(request, context, 500, "Could not install command file");
            delete context;
            return;
        }

        if (autostart) {
            apiRunner->requestStart(650, false);
            apiPhaseManager->setPhase(PhaseManager::BeginDrawing);
        }

        const size_t bytesWritten = context->bytesWritten;
        apiUploadInProgress = false;
        context->ownsUploadSlot = false;

        const String body = String("{\"bytesWritten\":") + String(bytesWritten) +
                            ",\"status\":\"" + getStatus() + "\"}";
        delete context;
        sendJson(request, 202, body);
    }

    void handleCanvas(AsyncWebServerRequest* request) {
        const int dPins = apiMovement->getTopDistance();
        const bool calibrated = dPins > 0;
        const double canvasWidth = calibrated ? apiMovement->getWidth() : -1;
        const Movement::Point home = apiMovement->getHomeCoordinates();

        AsyncResponseStream* response = request->beginResponseStream("application/json");
        addCorsHeaders(response);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& root = jsonBuffer.createObject();
        root["d_pins"] = dPins;
        root["homeX"] = home.x;
        root["homeY"] = home.y;
        root["canvasWidth"] = canvasWidth;
        root["canvasHeight"] = -1;
        root["canvasHeightKnown"] = false;
        root["homed"] = apiMovement->isHomed();
        root["status"] = getStatus();
        root["safeXFraction"] = safeXFraction;
        root["safeYFraction"] = safeYFraction;
        root["leftRightMarginMm"] = calibrated ? dPins * safeXFraction : -1;
        root["topMarginMm"] = calibrated ? dPins * safeYFraction : -1;
        JsonObject& bounds = root.createNestedObject("printableBounds");
        bounds["xMin"] = 0;
        bounds["xMax"] = canvasWidth;
        bounds["yMin"] = 0;
        bounds["yMax"] = -1;
        bounds["heightLimited"] = false;
        root.printTo(*response);
        request->send(response);
    }

    void handleStatus(AsyncWebServerRequest* request) {
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        addCorsHeaders(response);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& root = jsonBuffer.createObject();
        root["status"] = getStatus();
        root["homed"] = apiMovement->isHomed();
        root["bytesRead"] = apiRunner->bytesRead();
        root["totalBytes"] = apiRunner->totalBytes();
        root["uploadInProgress"] = apiUploadInProgress;
        root.printTo(*response);
        request->send(response);
    }

    void handleStop(AsyncWebServerRequest* request) {
        if (apiUploadInProgress) {
            sendError(request, 409, "Command upload is in progress");
            return;
        }
        if (apiRunner->isRunning() || apiRunner->hasError()) {
            apiRunner->requestStop();
            const String body = String("{\"status\":\"") + getStatus() + "\",\"stopRequested\":true}";
            sendJson(request, 202, body);
            return;
        }
        sendJson(request, 200, String("{\"status\":\"") + getStatus() + "\"}");
    }

    void handleOptions(AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(204, "text/plain", "");
        addCorsHeaders(response);
        request->send(response);
    }

    void handleNotFound(AsyncWebServerRequest* request) {
        if (request->url().startsWith("/api/")) {
            sendError(request, 404, "API endpoint not found");
            return;
        }
        request->send(404, "text/plain", "Not found");
    }
}

void registerApiRoutes(AsyncWebServer& server, Movement* movement, Runner* runner, PhaseManager* phaseManager) {
    apiMovement = movement;
    apiRunner = runner;
    apiPhaseManager = phaseManager;

    server.on("/api/canvas", HTTP_GET, handleCanvas);
    server.on("/api/canvas", HTTP_OPTIONS, handleOptions);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/status", HTTP_OPTIONS, handleOptions);
    server.on("/api/print", HTTP_POST, handleApiPrintComplete, nullptr, handleApiUpload);
    server.on("/api/print", HTTP_OPTIONS, handleOptions);
    server.on("/api/stop", HTTP_POST, handleStop);
    server.on("/api/stop", HTTP_OPTIONS, handleOptions);
    server.onNotFound(handleNotFound);
}
