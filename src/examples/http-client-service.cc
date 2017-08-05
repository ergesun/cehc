/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */

#include "http-client-service.h"

namespace cehc {
    namespace test {
        typedef tuple<pair<char*, int32_t>*, mutex*, condition_variable*, bool*, size_t*> easy_get_ctx_t, *easy_get_ctx_ptr;
        typedef tuple<bool*, mutex*, condition_variable*, bool*> easy_post_str_ctx_t, *easy_post_str_ctx_ptr;

        static size_t
        easy_get_recv_cb(cehc_connection_s *conn, void *ptr, size_t size, size_t nmemb) {
#define MEM_EXPAND_FACTOR 2
            easy_get_ctx_ptr ctx = static_cast<easy_get_ctx_ptr>(conn->user_ctx);
            auto res = std::get<0>(*ctx);
            auto left_size = std::get<4>(*ctx);
            // TODO(sunchao): 使用内存池
            size *= nmemb;
            size_t dst_size = (size_t)(res->second);
            IOUtils::CopyCumulativelyWithCharEnd(&res->first, &dst_size, left_size, ptr, size);
            res->second = (int32_t)dst_size;
            return size;
        }

        static void
        easy_get_complete_cb(cehc_connection_t *conn) {
            easy_get_ctx_ptr ctx = static_cast<easy_get_ctx_ptr>(conn->user_ctx);
            auto res = std::get<0>(*ctx);
            auto mtx = std::get<1>(*ctx);
            auto cv = std::get<2>(*ctx);
            auto complete = std::get<3>(*ctx);
            if (!cehc_conn_ok_except_httpcode(conn) || (200 != conn->http_code)) {
                DELETE_PTR(res->first);
                res->second = -1;
            }
            cehc_delete_conn(&conn);
            std::unique_lock<std::mutex> l(*mtx);
            *complete = true;
            l.unlock();
            cv->notify_one();
        }

        static void
        easy_post_str_complete_cb(cehc_connection_t *conn) {
            easy_post_str_ctx_ptr ctx = static_cast<easy_post_str_ctx_ptr>(conn->user_ctx);
            auto res = std::get<0>(*ctx);
            auto mtx = std::get<1>(*ctx);
            auto cv = std::get<2>(*ctx);
            auto complete = std::get<3>(*ctx);
            if (!cehc_conn_ok_except_httpcode(conn) || (200 != conn->http_code)) {
                *res = false;
            }
            *res = true;
            cehc_delete_conn(&conn);
            std::unique_lock<std::mutex> l(*mtx);
            *complete = true;
            l.unlock();
            cv->notify_one();
        }

        void HttpClientService::Start() {
            m_pCehcHttpClient = cehc_new_http_service(256,
                                                      128,
                                                      2000); // 此参数必须不能为-1,为的是能停止循环线程。
            if (!m_pCehcHttpClient) {
                throw std::runtime_error("cehc_new_http_service failed!");
            }

            if (!cehc_run_http_serivce(m_pCehcHttpClient)) {
                throw std::runtime_error("cehc_run_http_serivce failed!");
            }
        }

        void HttpClientService::Stop() {
            cehc_delete_http_serivce(&m_pCehcHttpClient);
        }

        void HttpClientService::Get(HttpGetParams &get_params) {
            cehc_connection_ptr conn = cehc_new_conn(static_cast<cehc_newconn_params_ptr>(&get_params));
            if (!conn) {
                throw std::runtime_error("new conn failed!");
            }

            curl_easy_setopt(conn->easy, CURLOPT_CONNECTTIMEOUT_MS, 2000);
        }

        pair<char*, size_t> HttpClientService::Get(string url) {
            pair<char*, int32_t> res;
            res.first = nullptr;
            res.second = 0;
            mutex mtx;
            condition_variable cv;
            bool complete = false;
            size_t leftSize = 0;
            easy_get_ctx_t ctx(&res, &mtx, &cv, &complete, &leftSize);
            // C-style的函数指针不能兼容lambda
            cehc_newconn_params_t conn_param = {
                .url = url.c_str(),
                .hs = m_pCehcHttpClient,
                .send_cb = nullptr,
                .recv_cb = easy_get_recv_cb,
                .header_cb = nullptr,
                .complete_cb = easy_get_complete_cb,
                .user_ctx = (void*)(&ctx)
            };

            auto conn = cehc_new_conn(&conn_param);
            if (!conn) {
                throw new std::runtime_error("cehc_new_conn failed!");
            }

            curl_easy_setopt(conn->easy, CURLOPT_TIMEOUT, 5L); // TODO(sunchao): 改为可配值
            curl_easy_setopt(conn->easy, CURLOPT_CONNECTTIMEOUT_MS, 2000);
            char errmsg[CURL_ERROR_SIZE];
            if (!cehc_run_conn(conn, errmsg)) {
                cehc_delete_conn(&conn);
                sprintf(stderr, errmsg);
                complete = true;
                res.first = nullptr;
                res.second = -1;
            }

            std::unique_lock<std::mutex> l(mtx);
            while (!complete) {
                cv.wait(l);
            }

            return std::move(res);
        }

        bool HttpClientService::Post(const string &url, const string &data) {
            bool res = false;
            mutex mtx;
            condition_variable cv;
            bool complete = false;
            easy_post_str_ctx_t ctx(&res, &mtx, &cv, &complete);
            // C-style的函数指针不能兼容lambda
            cehc_newconn_params_t conn_param = {
                .url = url.c_str(),
                .hs = m_pCehcHttpClient,
                .send_cb = nullptr,
                .recv_cb = nullptr,
                .header_cb = nullptr,
                .complete_cb = easy_post_str_complete_cb,
                .user_ctx = (void*)(&ctx)
            };

            auto conn = cehc_new_conn(&conn_param);
            if (!conn) {
                throw new std::runtime_error("cehc_new_conn failed!");
            }

            curl_easy_setopt(conn->easy, CURLOPT_TIMEOUT, 5L); // TODO(sunchao): 改为可配值
            curl_easy_setopt(conn->easy, CURLOPT_CONNECTTIMEOUT_MS, 2000);
            curl_easy_setopt(conn->easy, CURLOPT_POST, 1);
            curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDS, data.c_str());
            curl_easy_setopt(conn->easy, CURLOPT_POSTFIELDSIZE, data.size()); // <= 2GB，否则用CURLOPT_POSTFIELDSIZE_LARGE
            char errmsg[CURL_ERROR_SIZE];
            if (!cehc_run_conn(conn, errmsg)) {
                cehc_delete_conn(&conn);
                sprintf(stderr, errmsg);
                complete = true;
                res = false;
            }

            std::unique_lock<std::mutex> l(mtx);
            while (!complete) {
                cv.wait(l);
            }

            return res;
        }
    }
}

