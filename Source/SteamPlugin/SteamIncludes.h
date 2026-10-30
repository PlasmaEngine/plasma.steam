#pragma once

#include <Foundation/Basics.h>

// The Steamworks headers are third-party code compiled at the engine's warning level.
#if PL_ENABLED(PL_COMPILER_MSVC)
#  pragma warning(push)
#  pragma warning(disable : 4996) // deprecated CRT calls inside the SDK
#  pragma warning(disable : 4127) // conditional expression is constant
#endif

#include <steam/steam_api.h>

#if PL_ENABLED(PL_COMPILER_MSVC)
#  pragma warning(pop)
#endif

#ifndef PL_STEAM_HAS_INIT_EX
#  define PL_STEAM_HAS_INIT_EX 0
#endif

#ifndef PL_STEAM_HAS_REQUEST_CURRENT_STATS
#  define PL_STEAM_HAS_REQUEST_CURRENT_STATS 1
#endif
