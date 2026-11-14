#pragma once
#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#  ifdef small
#    undef small
#  endif
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "recovered_types.hpp"
#include "recovered_globals.hpp"
#include "recovered_functions.hpp"
