#include "runner.h"
#include "tasks/movementtask.h"
#include "tasks/interpolatingmovementtask.h"
#include "tasks/pentask.h"
#include "pen.h"
#include "display.h"
#include "SD.h"
using namespace std;

Runner::Runner(Movement *movement, Pen *pen, Display *display) {
    stopped = true;
    currentTask = NULL;
    startRequested = false;
    stopRequested = false;
    requestedSpeed = printSpeedSteps;
    requestedRestartOnFinish = true;
    restartOnFinish = true;
    error = false;
    fileSize = 0;
    bytesConsumed = 0;
    this->movement = movement;
    this->pen = pen;
    this->display = display;
}

void Runner::initTaskProvider() {
    openedFile = SD.open("/commands");
    if (!openedFile || !openedFile.available()) {
        Serial.println("Failed to open file");
        throw std::invalid_argument("No File");
    }
    fileSize = openedFile.size();
    bytesConsumed = 0;

    auto line = openedFile.readStringUntil('\n');
    line.trim();
    if (line.charAt(0) == 'd') {
        totalDistance = line.substring(1).toDouble();
    } else {
        Serial.println("Bad file - no distance");
        throw std::invalid_argument("bad file");
    }

    auto heightLine = openedFile.readStringUntil('\n');
    heightLine.trim();
    if (heightLine.charAt(0) == 'h') {
        auto height = heightLine.substring(1).toDouble();
        // we actually dont need it, just validating
    } else {
        Serial.println("Bad file - no height");
        throw std::invalid_argument("bad file");
    }
    bytesConsumed = openedFile.position();

    Serial.println("Total distance to travel: " + String(totalDistance));

    distanceSoFar = 0;
    progress = -1; // so 0% appears right away
    sequenceIx = 0;
    startPosition = movement->getCoordinates();

    auto homeCoordinates = movement->getHomeCoordinates();
    finishingSequence[0] = new InterpolatingMovementTask(movement, homeCoordinates, speed);
}

void Runner::start(int speed, bool restartOnFinish) {
    this->speed = speed;
    this->restartOnFinish = restartOnFinish;
    error = false;
    currentTask = NULL;
    stopped = false;
    try {
        initTaskProvider();
        currentTask = getNextTask();
        if (currentTask != NULL) {
            currentTask->startRunning();
        } else {
            openedFile.close();
            stopped = true;
        }
    } catch (...) {
        if (currentTask != NULL) {
            delete currentTask;
        }
        currentTask = NULL;
        openedFile.close();
        stopped = true;
        error = true;
        throw;
    }
}

Task *Runner::getNextTask()
{
    if (openedFile.available())
    {
        auto line = openedFile.readStringUntil('\n');
        line.trim();
        bytesConsumed = openedFile.position();
        if (line.charAt(0) == 'p')
        {
            if (line.charAt(1) == '1')
            {
                //Serial.println("Pen down");
                return new PenTask(false, pen);
            }
            else
            {
                //Serial.println("Pen up");
                return new PenTask(true, pen);
            }
        }
        else
        {
            auto x = line.substring(0, line.indexOf(" ")).toDouble();
            auto y = line.substring(line.indexOf(" ") + 1).toDouble();
            targetPosition = Movement::Point(x, y);
            return new InterpolatingMovementTask(movement, targetPosition, speed);
        }
    }
    else
    {
        if (sequenceIx < (end(finishingSequence) - begin(finishingSequence))) {
            auto currentIx = sequenceIx;
            sequenceIx = sequenceIx + 1;
            return finishingSequence[currentIx];
        } else {
            if (restartOnFinish) {
                delay(200);
                ESP.restart();
            }
            return NULL;
        }
    }
}

void Runner::run()
{
    if (startRequested) {
        startRequested = false;
        if (stopRequested) {
            stopRequested = false;
            return;
        }
        try {
            start(requestedSpeed, requestedRestartOnFinish);
        } catch (const std::exception& error) {
            Serial.printf("Failed to start command file: %s\n", error.what());
        }
        return;
    }

    if (stopped)
    {
        if (stopRequested) {
            stopRequested = false;
            movement->stopMotion();
            try {
                pen->slowUp();
                error = false;
            } catch (const std::invalid_argument&) {
                error = true;
            }
        }
        return;
    }
    if (currentTask == NULL) {
        openedFile.close();
        stopped = true;
        return;
    }

    if (stopRequested) {
        stopRequested = false;
        movement->stopMotion();
        try {
            pen->slowUp();
            error = false;
        } catch (const std::invalid_argument&) {
            error = true;
        }
        if (currentTask != NULL) {
            delete currentTask;
            currentTask = NULL;
        }
        openedFile.close();
        stopped = true;
        return;
    }

    try {
        if (currentTask->isDone())
        {
            if (currentTask->name() == InterpolatingMovementTask::NAME) {
                auto distanceCovered = Movement::distanceBetweenPoints(startPosition, targetPosition);
                distanceSoFar += distanceCovered;
                startPosition = targetPosition;
                auto newProgress = int(floor(distanceSoFar / totalDistance * 100));
                if (newProgress > 100) {
                    newProgress = 100;
                }
                if (progress != newProgress) {
                    Serial.println("Progress: " + String(newProgress));
                    progress = newProgress;
                    display->showDrawing(progress);
                }
            }
            delete currentTask;
            currentTask = NULL;
            currentTask = getNextTask();
            if (currentTask != NULL)
            {
                currentTask->startRunning();
            }
            else
            {
                stopped = true;
                openedFile.close();
            }
        }
    } catch (const std::exception& error) {
        failCurrentJob(error.what());
    }
}

void Runner::failCurrentJob(const char* message) {
    Serial.printf("Print failed: %s\n", message);
    movement->stopMotion();
    try {
        pen->slowUp();
    } catch (const std::invalid_argument&) {
        Serial.println("Could not lift pen after print failure");
    }
    if (currentTask != NULL) {
        delete currentTask;
        currentTask = NULL;
    }
    openedFile.close();
    stopped = true;
    error = true;
}

void Runner::requestStop() {
    stopRequested = true;
}

void Runner::requestStart(int speed, bool restartOnFinish) {
    requestedSpeed = speed;
    requestedRestartOnFinish = restartOnFinish;
    stopRequested = false;
    startRequested = true;
}

bool Runner::isRunning() {
    return startRequested || !stopped;
}

bool Runner::hasError() {
    return error;
}

size_t Runner::bytesRead() {
    return bytesConsumed;
}

size_t Runner::totalBytes() {
    return fileSize;
}

void Runner::dryRun() {
    initTaskProvider();
    auto task = getNextTask();
    auto index = 1;
    while (task != NULL) {
        //Serial.println(String(index));
        index = index + 1;
        delete task;
        task = getNextTask();
    }
    Serial.println("All done");
}