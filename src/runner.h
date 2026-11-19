#ifndef Runner_h
#define Runner_h
#include "movement.h"
#include "tasks/task.h"
#include "pen.h"
#include "display.h"
#include "SD.h"
class Runner {
    private:
    Movement *movement;
    Pen *pen;
    Display *display;
    void initTaskProvider();
    Task* getNextTask();
    void failCurrentJob(const char* message);
    Task* currentTask;
    volatile bool stopped;
    File openedFile;
    double totalDistance;
    double distanceSoFar;
    Movement::Point startPosition;
    Movement::Point targetPosition;
    int progress;
    int speed;
    Task *finishingSequence[1] = {NULL};
    int sequenceIx = 0;
    volatile bool startRequested;
    volatile bool stopRequested;
    int requestedSpeed;
    bool requestedRestartOnFinish;
    bool restartOnFinish;
    volatile bool error;
    size_t fileSize;
    size_t bytesConsumed;
    public:
    Runner(Movement *movement, Pen *pen, Display *display);
    void start(int speed, bool restartOnFinish = true);
    void requestStart(int speed, bool restartOnFinish = true);
    void requestStop();
    void run();
    void dryRun();
    bool isRunning();
    bool hasError();
    size_t bytesRead();
    size_t totalBytes();
};
#endif