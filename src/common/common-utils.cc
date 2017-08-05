/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */

#include <time.h>

#include "common-utils.h"

namespace cehc {
    namespace common {
        uctime_t CommonUtils::GetCurrentTime() {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);

            return uctime_t(ts);
        }
    }
}
