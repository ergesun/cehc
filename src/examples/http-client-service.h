/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */


#ifndef CEHC_HTTP_CLIENT_SERVICE_H
#define CEHC_HTTP_CLIENT_SERVICE_H

#include <string>
#include <utility>

using namespace std;

#include "../cehc/cehttpclient.h"

namespace cehc {
    namespace test {
        typedef cehc_newconn_params_t HttpGetParams;

        /**
         * http client的全局服务
         * TODO(sunchao):
         *      1、添加retry机制
         *      2、封装掉connection，不暴露给用户
         *      4、扩展为可多个cehc http client services（目前只管理了一个）
         */
        class HttpClientService {
        public:
            void Start();
            void Stop();

            /**
             * 适用于数据量较大的请求。
             * 分次接收远端的数据，参考cehttpclient的README。
             * 注意：除发生异常之外，其他情况当前版本的api都由user负责释放(free)complete回调返回的conn。
             * @Exception std::runtime_error
             * @return
             */
            void Get(HttpGetParams &get_params);
            /**
             * 适用于数据量较小的请求，最大支持int32_t类型能表示的最大值减1。
             * 一次性完成并返回接收到的远端的所有的数据。
             * 注意： 1. 相比接收到的数据大小，多了一个1 byte存储了最后的\0。
             *       2. 如果pair.first不为空，你需要用完之后释放它。
             * @return 如果pair的second为-1时表示调用失败。
             */
            pair<char*, size_t> Get(string url);
            /**
             * 发送string，不接收响应数据。
             * @param url
             * @param data 小于等于2GB
             * @return 成功返回true，失败返回false。
             */
            bool Post(const string &url, const string &data);

        private:
            friend class ServiceManager;
            HttpClientService();

        private:
            cehc_http_service_t *m_pCehcHttpClient = nullptr;
        };
    }
}

#endif //CEHC_HTTP_CLIENT_SERVICE_H
