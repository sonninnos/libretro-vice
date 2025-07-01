#ifdef __LIBRETRO__
/* Moved all inclusions of compiler libraries here to ensure defines for C++98-compliance are not
   applied to them. */

#ifndef SYSINCLUDES_H
#define SYSINCLUDES_H
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <sstream>
#include <limits>
#include <map>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>
#ifdef HAVE_MMINTRIN_H
#  include <mmintrin.h>
#endif
#endif

#ifdef __QNX__
#include <math.h>
#include <string.h>
#endif

#ifdef __PS3__
#include "PS3_include.h"
#endif

#endif
