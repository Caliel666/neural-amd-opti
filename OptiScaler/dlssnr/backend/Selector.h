#pragma once
#include "Kind.h"
#include <string>

namespace DlssNr::Backend
{
Kind RequestedKind();
Kind ActiveKindFromConfig();
// Read-only host metadata queries; never calls the private HostStart entry.
bool NeuralHubHostDetected();
bool NeuralHubHostAvailable();
std::string NeuralHubHostStatus();
std::string NeuralHubRuntimeLabel();
// True while lmxxf (with LmxxfWired()) or mochizuki is active. Mutual exclusion vs graphics tracker.
bool SubmissionHooksWanted();
// The runtimes NR without upscaling runs on: danielblnc, and mochizuki between two lists of its own.
bool PresentRuntimeSupported();
} // namespace DlssNr::Backend
