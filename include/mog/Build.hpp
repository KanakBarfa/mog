// Stable gates over generated probe results; the only header that reads BuildFeatures.
#pragma once

#include <mog/BuildFeatures.hpp>

#if !defined(MOG_PROFILE_PORTABLE) && !defined(MOG_PROFILE_FRONTIER) && !defined(MOG_PROFILE_LAB)
#error "MOG profile macro missing; configure through CMake"
#endif

#define MOG_VERSION_STRING MOG_VERSION

#if defined(MOG_PROFILE_PORTABLE)
#define MOG_CONTRACTS_DEFAULT_MODE 2 // ignore
#elif defined(MOG_PROFILE_FRONTIER) || defined(MOG_PROFILE_LAB)
#define MOG_CONTRACTS_DEFAULT_MODE 0 // enforce
#endif
