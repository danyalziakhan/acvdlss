#pragma once

#include <cstdint>

struct ID3D12Device;

// Diagnostics for hangs and conflicts. Always: device removal and a line at a normal exit. With Debug=1 also
// start-up stages with their times, DLLs loaded later on and a watchdog. The watchdog logs the call stack of a
// thread stuck in a stage, or of the game's threads when frames stop.

// From DllMain after log_start. Starts the watchdog and the DLL load log with Debug=1.
void diag_start();
// From DllMain at detach. exiting is true when the whole game is closing.
void diag_stop(bool exiting);
// From init_device, before DLSS starts.
void diag_device_created();
// From destroy_device. Ends the watchdog, which can't outlive the device's frames.
void diag_device_destroyed();
// Once a frame from the present event.
void diag_present(ID3D12Device *device);

// A start-up step. With Debug=1 its start and its time go to the log, and the watchdog reports it when it
// takes too long. Not nested.
class diag_stage
{
public:
    explicit diag_stage(const char *name);
    ~diag_stage();
    diag_stage(const diag_stage &) = delete;
    diag_stage &operator=(const diag_stage &) = delete;

private:
    uint64_t _start = 0;
};
