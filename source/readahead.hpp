#pragma once
#include <mpv/client.h>

#include <cstdint>
#include <string>

// Local files are played through an "smx://" stream that SwitchMX reads itself: a dedicated
// high-priority thread does large sequential reads into a RAM ring buffer far ahead of mpv.
// On the Switch, mpv's own demuxer thread competes with decoding for CPU and was starved,
// so it fetched one packet at a time and playback kept stopping to buffer.

void registerReadAhead(mpv_handle* mpv, size_t capacityBytes);
std::string readAheadUrl(const std::string& path);   // "smx://" + path
std::string stripReadAheadUrl(const std::string& s);  // inverse; other strings unchanged

struct ReadAheadStats {
    double readMBps = 0;      // recent SD throughput while reading
    double bufferedMB = 0;    // data ready ahead of playback
    double slowestReadMs = 0;  // slowest single read in the last few seconds
    bool active = false;
};
ReadAheadStats readAheadStats();
// What went wrong opening the last file, or what had to give (once; empty if nothing).
std::string takeReadAheadNote();
