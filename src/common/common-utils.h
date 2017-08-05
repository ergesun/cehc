/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */

#ifndef CEHC_COMMON_UTILS_H
#define CEHC_COMMON_UTILS_H

#include "common-def.h"

namespace cehc {
    namespace common {
        class CommonUtils {
        public:
            /**
             * 获取当前系统时间(unix epoch到现在的秒+纳秒数)。
             * @return
             */
            static uctime_t GetCurrentTime();
        }; // class CommonUtils
    }
}

#endif //CEHC_COMMON_UTILS_H
