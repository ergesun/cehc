/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */

#ifndef cehttpclient__h
#define cehttpclient__h

#include <curl/multi.h>
#include <stdbool.h>

#include "../common/common-def.h"
#include "../common/timer.h"
#include "../common/spin-lock.h"

using namespace cehc::common;

#ifndef __cplusplus
extern "C" {
#endif

/**
 * cehc <==> curl epoll http client service
 */

/**
 * 每个http service
 */
typedef struct cehc_http_service_s {
    int epfd;
    spin_lock_t ep_sl;
    int ep_timeout_ms;
    int ep_once_ev_cnt;
    CURLM *multi;
    int running_count;
    volatile bool stop;
    // TODO(sunchao): 考虑优化或者放弃boost timer而是自己通过rbtree实现以优化性能。
    Timer *timer;
    Timer::TimerCallback timer_cb;
    pthread_t tid;
    std::mutex multi_handles_mtx;
} cehc_http_service_t;


/**
 * 每一个easy handle关联的连接上下文。
 * TODO(sunchao):增加连接对象的池子以单纯减少内存碎片和提高性能(因为http连接方面的池子curl本身是有的)
 */
typedef struct cehc_connection_s {
    CURL *easy;
    curl_socket_t fd;
    char *url;
    bool is_in_ep;
    cehc_http_service_t *http_service;

    /**
     * 此回调表示可发送数据。
     * https://curl.haxx.se/libcurl/c/CURLOPT_READFUNCTION.html
     * @param ptr 需要发送的数据buffer指针
     * @param 可发送的buffer大小
     * @Return 有效的大小，如果size是0，表示告知curl数据发送完毕
     */
    size_t (*send_cb)(struct cehc_connection_s *, void *ptr, size_t size, size_t nmemb);

    /**
     * 此回调表示缓冲区有数据需要处理。
     * https://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
     * @param ptr 接收到的buffer的指针
     * @param 可处理的buffer大小
     * @Return 接收了的大小
     */
    size_t (*recv_cb)(struct cehc_connection_s *, void *ptr, size_t size, size_t nmemb);

    /**
     * https://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html
     * @param ptr 接收到的buffer的指针
     * @param 可处理的buffer大小
     * @Return 接收了的大小
     */
    size_t (*header_cb)(struct cehc_connection_s *, void *ptr, size_t size, size_t nmemb);

    /**
     * 此回调表示此请求已完成。
     */
    void (*complete_cb)(struct cehc_connection_s *);

    // user在检查错误的时候，以下几个错误应该顺次检查，只要有一个有错误，那么就是失败了。
    int err_no;         // errno, 成功为0。
    int ep_code;        // epoll code，成功为0。
    CURLMcode cm_code;  // curl multi code，成功为CURLM_OK
    CURLcode ce_code;   // curl easy code，成功为CURLE_OK
    long http_code;     // http code,按照http规范或者实际业务检查。

    char errormsg[CURL_ERROR_SIZE];
    /**
     * user可以传递的上下文。
     */
    void *user_ctx;
} cehc_connection_t, *cehc_connection_ptr;


/**
 * 创建连接的参数。
 */
typedef struct cehc_newconn_params_s {
    /**
     * 目标url(uri)
     */
    const char *url;
    /**
     * 目标池子
     */
    cehc_http_service_t *hs;
    /**
     * 此回调表示可发送数据。
     * https://curl.haxx.se/libcurl/c/CURLOPT_READFUNCTION.html
     * @param ptr 需要发送的数据buffer指针
     * @param size 可发送的buffer大小
     * @Return 有效的大小，如果size是0，表示告知curl数据发送完毕
     */
    size_t (*send_cb)(struct cehc_connection_s *, void *ptr, size_t size, size_t nmemb);
    /**
     * 此回调表示缓冲区有数据需要处理。
     * https://curl.haxx.se/libcurl/c/CURLOPT_WRITEFUNCTION.html
     * @param ptr 接收到的buffer的指针
     * @param size 可处理的buffer大小
     * @Return 接收了的大小
     */
    size_t (*recv_cb)(struct cehc_connection_s *, void *ptr, size_t size, size_t nmemb);
    /**
     * https://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html
     * @param ptr 接收到的buffer的指针
     * @param 可处理的buffer大小
     * @Return 接收了的大小
     */
    size_t (*header_cb)(struct cehc_connection_s *, void *ptr, size_t size, size_t nmemb);
    /**
     * 此回调表示此请求已完成。
     */
    void (*complete_cb)(struct cehc_connection_s *);
    /**
     * user可以传递的上下文。
     */
    void *user_ctx;
} cehc_newconn_params_t, *cehc_newconn_params_ptr;


/**
 * 初始化一个easy handle对应一个请求，之后交给multi handle托管。
 * 注意：本curl封装并不完全，以下curl easy设置为本封装保留，user切不可使用，否则会造成功能性错误。
 *    ->CURLOPT_WRITEDATA、CURLOPT_WRITEFUNCTION、CURLOPT_READDATA、
 *    ->CURLOPT_READFUNCTION、CURLOPT_PRIVATE、CURLOPT_URL、CURLOPT_NOSIGNAL
 * @param url
 * @param hs 如果是c++，此参数实际应用时应当隐藏在http service之中不需要用户传入
 * @return 失败返回NULL
 */
cehc_connection_t*
cehc_new_conn(cehc_newconn_params_ptr params);


/**
 * user使用完conn需要释放掉。
 * 注：目前只支持收到complete事件之后调用，其他时候调用可能会有内存泄漏。
 * @param conn, new_conn得到的conn的地址的地址
 */
void
cehc_delete_conn(cehc_connection_t **conn);


/**
 * 加入到http service中跑，动作为non blocking。
 * @param conn
 * @param errmsg 长度上限为CURL_ERROR_SIZE
 * @return 成功为true, errmsg的strlen为0;失败为false并对输入参数errmsg赋值。
 */
bool
cehc_run_conn(cehc_connection_ptr conn, char *errmsg);


/**
 * 检查conn除了http code之外有无错误。
 */
bool
cehc_conn_ok_except_httpcode(cehc_connection_ptr conn);


/**
 * 初始化curl服务，无论创建多少个http client service，此函数需要全局仅且只有一次调用。
 * @return 成功true，失败false
 */
bool
cehc_init_curl_global_service();


/**
 * 释放全局的curl服务资源。
 */
void
cehc_uninit_curl_global_service();


/**
 * 创建一个新的连接服务(一个自管理连接池(curl multi + epoll))。
 * 此处需改进，因为如果并发非常高的话应该支持使用多个http service实例才能更好的调度事件。
 * @param ep_ev_cnt epoll处理的事件上限
 * @param ep_once_ev_cnt epoll_wait得到的事件个数上限
 * @param ep_timeout_ms epoll_wait的timeout时间
 * @return 失败NULL
 */
cehc_http_service_t *
cehc_new_http_service(int ep_ev_cnt, int ep_once_ev_cnt, int ep_timeout_ms);


/**
 * 创建一个新的连接服务(一个自管理连接池(curl multi + epoll)),
 * 相当于new_http_service(512, 512, -1);
 * @return 失败为NULL
 */
cehc_http_service_t *
cehc_new_http_service_by_default_params();


/**
 * 启动http service.
 * @param hs new_http_service得到的http service
 * @param pthread 执行的线程
 * @return 成功true，失败false
 */
bool cehc_run_http_serivce(cehc_http_service_t *hs);


/**
 * 释放一个http service。
 * @param hs
 */
void
cehc_delete_http_serivce(cehc_http_service_t **hs);


#ifndef __cplusplus
}
#endif
#endif //cehttpclient__h
