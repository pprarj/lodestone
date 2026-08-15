#pragma once

// Precompiled header, mandatory for CommonLibSSE-NG. CMake force-includes it
// in every file of the target, the auto-generated version file included.
//
// That is an ordering requirement rather than a convenience: the CommonLib
// headers have to be parsed before anything reaches REL/Relocation.h, which
// is what defines the stl namespace the rest of the library assumes.

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

using namespace std::literals;
