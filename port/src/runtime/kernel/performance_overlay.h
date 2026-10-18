#pragma once
#include <cstdint>

namespace performance_overlay
{
// Starts a low-rate, click-through overlay when the launcher selected at least one metric.
void Start(void* gameWindow);
// Called after each successfully presented game frame.
void NotifyPresent();
// Client-relative bottom edge while visible, 0 when hidden.
int VisibleBottom();
// Number of game frames successfully presented since the process started.
uint64_t PresentedFrameCount();
}
