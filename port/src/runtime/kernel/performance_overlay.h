#pragma once

namespace performance_overlay
{
// Starts a low-rate, click-through overlay when the launcher selected at least one metric.
void Start(void* gameWindow);
// Called after each successfully presented game frame.
void NotifyPresent();
}
