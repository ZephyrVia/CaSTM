#pragma once

#include <cstdio>

#include "WwSTM/Config.hpp"

#if STM_WW_ENABLE_LOGGING
#define WWSTM_DLOG(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define WWSTM_DLOG(...) ((void)0)
#endif

