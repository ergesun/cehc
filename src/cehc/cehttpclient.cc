/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */

#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <errno.h>
#include <curl/curl.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "cehttpclient.h"

#define cehc_def_epoll_event struct epoll_event ee;                \
                             bzero(&ee, sizeof(struct epoll_event));

#define cehc_process_ep_add_err()  conn->err_no = errno;                                           \
                                   sprintf(conn->errormsg, "%s", strerror(conn->err_no));          \
                                   cehc_complete_conn_by_ep_err(conn);

static int
cehc_set_nonblocking(int fd, cehc_connection_t *conn) {
    int opts;
    if((opts = fcntl(fd, F_GETFL)) < 0) {
        conn->err_no = errno;
        //LOG(ERROR) << "get fd = " << fd << ", opts err = " << strerror(conn->err_no) << ", url = " << conn->url;
        fprintf(stderr, "get fd = %d, opts err = %s, url = %s", fd, strerror(conn->err_no), conn->url);

        return opts;
    }

    opts = opts|O_NONBLOCK;
    if((conn->err_no = fcntl(fd, F_SETFL, opts)) < 0) {
        conn->err_no = errno;
        //LOG(WARNING) << "set fd = " << fd << " O_NONBLOCK err = " << strerror(conn->err_no) << ", url = " << conn->url;
        fprintf(stderr, "set fd = %d, O_NONBLOCK err = %s, url = %s", fd, strerror(conn->err_no), conn->url);

        return conn->err_no;
    }

    return conn->err_no;
}

static void
cehc_set_timer(cehc_http_service_t *hs, long time_ms) {
    if (!hs) {
        return;
    }

    Timer::Event ev(nullptr, &hs->timer_cb);
    hs->timer->SubscribeEventAfter(uctime_t(0, time_ms * 1000 * 1000), ev);
}

/* Check for completed transfers, and remove their easy handles */
static void
cehc_check_multi_info(cehc_http_service_t *http_service) {
    //printf("[DEBUG] %s:.\n", __FUNCTION__);
    CURLMsg *msg = NULL;
    int msgs_left = 0;
    CURL *easy = NULL;
    while ((msg = curl_multi_info_read(http_service->multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            //printf("[INFO] %s: REMAINING=> %d\n", __FUNCTION__, http_service->running_count);
            easy = msg->easy_handle;
            if (!easy) {
                //LOG(ERROR) << "easy handle is null!";
                fprintf(stderr, "easy handle is null!");
                continue;
            }

            cehc_connection_ptr conn = NULL;
            CURLcode cc;
            if (((cc = curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn)) != CURLE_OK) || !conn) {
                //LOG(ERROR) << "conn is null! curl errmsg = " << curl_easy_strerror(cc) << ".";
                fprintf(stderr, "conn is null! curl errmsg = %s.", curl_easy_strerror(cc));
                curl_multi_remove_handle(http_service->multi, easy);
                curl_easy_cleanup(easy);
                continue;
            }

            if ((cc =  curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &(conn->http_code))) != CURLE_OK) {
                auto errmsg = curl_easy_strerror(cc);
                //LOG(ERROR) << "curl get http response code failed with errmsg = " << errmsg << ".";
                fprintf(stderr, "curl get http response code failed with errmsg =  %s.", errmsg);
                conn->ce_code = cc;
                sprintf(conn->errormsg, "%s", errmsg);
            }

            curl_multi_remove_handle(http_service->multi, easy);
            if (conn) {
                //printf("[DEBUG] %s: DONE %s => (fd = %d), (curl status = %s)\n",
                //       __FUNCTION__, conn->url, conn->fd, curl_easy_strerror(res));
                if (conn->complete_cb) {
                    conn->complete_cb(conn);
                }
            }
        }
    }
}

/**
 * 参考https://curl.haxx.se/libcurl/c/CURLMOPT_TIMERFUNCTION.html
 * @param sig
 * @param si
 * @param uc
 */
static void
cehc_timer_handler(void *userp) {
    cehc_http_service_t *hs = static_cast<cehc_http_service_t*>(userp);
    if (hs && !hs->stop) {
        std::unique_lock<std::mutex> l(hs->multi_handles_mtx);
        curl_multi_socket_action(hs->multi, CURL_SOCKET_TIMEOUT, 0, &(hs->running_count));
        //cehc_check_multi_info(hs);
    }
}

/**
 * http://www.cnblogs.com/chineseboy/p/3959852.html <- cjson
 * @param ptr
 * @param size
 * @param nmemb
 * @param ctx 可用作多线程应用时的request ctx。
 * @return
 */
static size_t
cehc_receive_data(void *ptr, size_t size, size_t nmemb, void *ctx) {
    cehc_connection_t *conn = (ctx ? (cehc_connection_t*)ctx : NULL);
    if (!conn) {
        fprintf(stderr, "conn ctx is null!");
        return 0;
    }

    if (conn->recv_cb) {
        return conn->recv_cb(conn, ptr, size , nmemb);
    }
    return size * nmemb;
}

/**
 * 返回0表示上传结束。
 * @param ptr
 * @param size
 * @param nmemb
 * @param ctx
 * @return
 */
static size_t
cehc_send_data(void *ptr, size_t size, size_t nmemb, void *ctx) {
    cehc_connection_t *conn = (ctx ? (cehc_connection_t*)ctx : NULL);
    if (!conn) {
        //LOG(ERROR) << "conn ctx is null!";
        fprintf(stderr, "conn ctx is null!");
        return 0;
    }

    if (conn->send_cb) {
        return conn->send_cb(conn, ptr, size, nmemb);
    }
    return size * nmemb;
}

/**
 * 返回0表示上传结束。
 * @param ptr
 * @param size
 * @param nmemb
 * @param ctx
 * @return
 */
static size_t
cehc_header_data(void *ptr, size_t size, size_t nmemb, void *ctx) {
    cehc_connection_t *conn = (ctx ? (cehc_connection_t*)ctx : NULL);
    if (!conn) {
        //LOG(ERROR) << "conn ctx is null!";
        fprintf(stderr, "conn ctx is null!");
        return 0;
    }

    if (conn->header_cb) {
        return conn->header_cb(conn, ptr, size, nmemb);
    }
    return size * nmemb;
}

static void
cehc_complete_conn_by_ep_err(cehc_connection_ptr conn) {
    if (conn) {
        if (conn->http_service->multi && conn->easy) {
            curl_multi_remove_handle(conn->http_service->multi, conn->easy);
        }

        if (conn->complete_cb) {
            conn->complete_cb(conn);
        }
    }
}

static void
cehc_ep_remove_conn(cehc_connection_t *conn) {
    if (LIKELY(conn->is_in_ep)) {
        SpinLock l(&conn->http_service->ep_sl);
        if (conn->is_in_ep) {
            conn->is_in_ep = false;
            cehc_def_epoll_event;
            ee.data.fd = conn->fd;
            ee.events = EPOLLIN|EPOLLOUT|EPOLLRDHUP|EPOLLERR;
            if (-1 == (conn->ep_code = epoll_ctl(conn->http_service->epfd, EPOLL_CTL_DEL, conn->fd, &ee))) {
                cehc_process_ep_add_err();
            }
        }
    }
}

static void
cehc_ep_set_conn(cehc_connection_t *conn, curl_socket_t fd, CURL *easy, int act) {
    // 经测试，libcurl不支持epoll的edge trigger。
    // 没有太深入研究，觉得curl的multi机制用level trigger还算合适，再大的并发也就是个client的并发。
    // 想研究的可自行研究下。
    int event_kind = (act & CURL_POLL_IN ? EPOLLIN : 0)|(act & CURL_POLL_OUT ? EPOLLOUT : 0)|
                     /*EPOLLET|*/EPOLLRDHUP|EPOLLERR;
    cehc_def_epoll_event;
    ee.events = (uint32_t)event_kind;
    ee.data.fd = fd;
    conn->fd = fd;
    conn->easy = easy;
    SpinLock l(&conn->http_service->ep_sl);
    if (conn->is_in_ep) { // 已经存在了，即做修改动作
        //printf("[DEBUG] %s: epoll_mod fd = %d\n", __FUNCTION__, fd);
        if ((conn->ep_code = epoll_ctl(conn->http_service->epfd, EPOLL_CTL_MOD, fd, &ee)) == -1) {
            conn->err_no = errno;
            fprintf(stderr, "epoll_ctl err = %s.", strerror(conn->err_no));
            // MOD失败，从ep中删除。
            if (-1 == epoll_ctl(conn->http_service->epfd, EPOLL_CTL_DEL, fd, &ee)) {
                int err = errno;
                fprintf(stderr, "epoll_ctl del err = %s.", strerror(err));
            } else {
                conn->is_in_ep = false;
            }

            cehc_complete_conn_by_ep_err(conn);
        }
    } else { // 不存在，新增动作
        //printf("[DEBUG] %s: epoll_add fd = %d\n", __FUNCTION__, fd);
        if ((conn->ep_code = epoll_ctl(conn->http_service->epfd, EPOLL_CTL_ADD, fd, &ee)) == -1) {
            cehc_process_ep_add_err();
        } else {
            conn->is_in_ep = true;
            //curl_multi_assign(conn->http_service->multi, fd, conn);
        }
    }
}

static int
cehc_curl_conn_cb(CURL *easy, curl_socket_t fd, int what, void *cbp, void *no_use_cbp) {
    cehc_connection_t *conn = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn);
    if (!conn) {
        fprintf(stdout, "WARNING: conn is null.");
        return 0;
    }

#ifdef DEBUG_LOG
    const char *what_str[] = {"none", "IN", "OUT", "INOUT", "REMOVE"};
    LOGDFUN6("fd = ", fd, ", what = ", what_str[what], ", url = ", conn->url);
#endif

    if (what == CURL_POLL_REMOVE) {
        cehc_ep_remove_conn(conn);
    } else {
        if (-1 == cehc_set_nonblocking(fd, conn)) {
            return -1;
        }
        cehc_ep_set_conn(conn, fd, easy, what);
    }

    return 0;
}

/**
 * 参考https://curl.haxx.se/libcurl/c/CURLMOPT_TIMERFUNCTION.html
 * @param multi
 * @param timeout_ms
 * @param userp
 * @return
 */
static int
cehc_curl_timer_cb(CURLM *multi,    /* multi handle */
                   long timeout_ms, /* see above */
                   void *userp)   /* private callback pointer */ {
//    LOGDFUN1(timeout_ms);
    if (userp) {
        cehc_http_service_t *hs = (cehc_http_service_t *)userp;
        if (-1 == timeout_ms) { // cancel timer
            hs->timer->UnsubscribeAllEvent();
        } else {
            timeout_ms = timeout_ms ? timeout_ms : 5;
            cehc_set_timer(hs, timeout_ms);
        }
    } else {
        fprintf(stdout, "WARNING: userp is empty!");
    }

    return 0;
}

static bool
cehc_init_curl_multi_service(cehc_http_service_t *hs) {
    CURLMcode cm_code = CURLM_OK;
    if ((cm_code = curl_multi_setopt(hs->multi, CURLMOPT_SOCKETFUNCTION, cehc_curl_conn_cb)) != CURLM_OK) {
        fprintf(stderr, "FATAL: %s", curl_multi_strerror(cm_code));
        return false;
    }
    if ((cm_code = curl_multi_setopt(hs->multi, CURLMOPT_TIMERDATA, hs)) != CURLM_OK) {
        fprintf(stderr, "FATAL: %s", curl_multi_strerror(cm_code));
        return false;
    }
    if ((cm_code = curl_multi_setopt(hs->multi, CURLMOPT_TIMERFUNCTION, cehc_curl_timer_cb)) != CURLM_OK) {
        fprintf(stderr, "FATAL: %s", curl_multi_strerror(cm_code));
        return false;
    }
    // 重要，少了会导致curl链接不足。
    if ((cm_code = curl_multi_setopt(hs->multi, CURLMOPT_MAXCONNECTS, 256)) != CURLM_OK) {
        fprintf(stderr, "FATAL: %s", curl_multi_strerror(cm_code));
        return false;
    }

    return true;
}

static void *
cehc_inner_run_http_serivce(void *ctx) {
    if (!ctx) {
        return NULL;
    }

    cehc_http_service_t *hs = (cehc_http_service_t*)ctx;
    int revents = 0;

    while (!hs->stop) {
        struct epoll_event ees[hs->ep_once_ev_cnt]; // ees -> epoll events
        int err = 0;
        int ees_cnt = epoll_wait(hs->epfd, ees, hs->ep_once_ev_cnt, hs->ep_timeout_ms);
        switch (ees_cnt) {
            case -1: {
                err = errno;
                if (EINTR != err) { // if not sys interrupt
                    // log err
                    fprintf(stderr, "epoll wait err = %s", strerror(err));
                }
                break;
            }
            case 0: {
                if (hs->stop) {
                    break;
                }

                if (-1 == hs->ep_timeout_ms) { // 无超时还返回了0个
                    err = errno;
                    if (EINTR != err) {
                        fprintf(stderr, "epoll wait err = %s", strerror(err));
                    }

                    continue;
                }
                // curl fd初始化
                std::unique_lock<std::mutex> l(hs->multi_handles_mtx);
                curl_multi_socket_action(hs->multi, CURL_SOCKET_TIMEOUT, 0, &(hs->running_count));
                cehc_check_multi_info(hs);
                break;
            }
            default: { // > 0，有事件接入。
                // 可以通过conn这个ctx隔离不同链接，因为每一个链接的conn都是独立的，非公享的，
                // 你可能需要根据业务自行扩展jrs_connection_t这个上下文类，可以学习nginx及nginx-rtmp进行功能细分，
                // 事件为事件，session为session，组合在conn之中。
                int i;
                for (i = 0; i < ees_cnt; ++i) {
                    revents = ees[i].events;
                    if ((revents & (EPOLLERR | EPOLLHUP))
                        && (revents & (EPOLLIN | EPOLLOUT)) == 0) {
                        /*
                         * if the errormsg events were returned without EPOLLIN or EPOLLOUT,
                         * then add these flags to handle the events at least in one
                         * active handler
                         */
                        fprintf(stderr, "epoll_wait() errormsg on fd: %d, events = ", ees[i].data.fd, revents);
                        revents |= EPOLLIN | EPOLLOUT;
                    }

                    CURLMcode cc = CURLM_OK;
                    std::unique_lock<std::mutex> l(hs->multi_handles_mtx);
                    if (revents & EPOLLIN) {
                        cc = curl_multi_socket_action(hs->multi, ees[i].data.fd,
                                                      CURL_CSELECT_IN, &(hs->running_count));
                    }

                    if (revents & EPOLLOUT) {
                        cc = curl_multi_socket_action(hs->multi, ees[i].data.fd,
                                                      CURL_CSELECT_OUT, &(hs->running_count));
                    }

                    if (CURLM_OK != cc) {
                        err = errno;
                        fprintf(stderr, "curl_multi_socket_action err = %s,"
                            " sys err = %s", curl_multi_strerror(cc), strerror(err));
                    }

                    cehc_check_multi_info(hs);
                }
                break;
            }
        }
    }

    std::unique_lock<std::mutex> l(hs->multi_handles_mtx);
    curl_multi_cleanup(hs->multi);
    return NULL;
}

/**
 * 初始化一个easy handle对应一个请求，之后交给multi handle托管。
 * 注意：本curl封装并不完全，以下curl easy设置为本封装保留，user切不可使用，否则会造成功能性错误。
 *    ->CURLOPT_WRITEDATA、CURLOPT_WRITEFUNCTION、CURLOPT_READDATA、CURLOPT_READFUNCTION
 *    ->CURLOPT_HEADERDATA、CURLOPT_HEADERFUNCTION
 *    ->CURLOPT_PRIVATE、CURLOPT_URL、CURLOPT_NOSIGNAL
 * @param url
 * @param hs 如果是c++，此参数实际应用时应当隐藏在http service之中不需要用户传入
 * @return 一个链接
 */
cehc_connection_t*
cehc_new_conn(cehc_newconn_params_ptr params) {
    if (!params) {
        fprintf(stdout, "WARNING: input params cannot be null!");
        return NULL;
    }

    cehc_connection_t *conn = (cehc_connection_t *) calloc(1, sizeof(cehc_connection_t));
    if (!conn) {
        int err = errno;
        fprintf(stderr, "calloc connection oom with err = %s", strerror(err));
        return NULL;
    }

    conn->errormsg[0] = '\0';
    conn->easy = curl_easy_init();
    if (!conn->easy) {
        conn->err_no = errno;
        auto errmsg = strerror(conn->err_no);
        fprintf(stderr, "curl_easy_init failed, exiting with errmsg = %s.", errmsg);
        sprintf(conn->errormsg, "%s", errmsg);
        return conn;
    }

    // 设置这个easy的行为
    conn->http_service = params->hs;
    conn->recv_cb = params->recv_cb;
    conn->send_cb = params->send_cb;
    conn->header_cb = params->header_cb;
    conn->complete_cb = params->complete_cb;
    conn->user_ctx = params->user_ctx;
    conn->url = strdup(params->url); // url need free in easy done.

    // ****Begin: 本封装保留的easy设置，user不可使用。****
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_URL, conn->url))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_WRITEDATA, conn))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_WRITEFUNCTION, cehc_receive_data))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_READDATA, conn))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_READFUNCTION, cehc_send_data))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_HEADERDATA, conn))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_HEADERFUNCTION, cehc_header_data))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_PRIVATE, conn))) {
        goto Label_init_err;
    }
    if (CURLE_OK != (conn->ce_code = curl_easy_setopt(conn->easy, CURLOPT_NOSIGNAL, 1L))) {
        goto Label_init_err;
    }
    // ****End: 本封装保留的easy设置，user不可使用。****
    return conn;

    Label_init_err:
    fprintf(stderr, "%s, %s", __func__, curl_easy_strerror(conn->ce_code));
    curl_easy_cleanup(conn->easy);
    return NULL;
}

/**
 * user使用完conn需要释放掉
 * @param conn, new_conn得到的conn的地址的地址
 */
void
cehc_delete_conn(cehc_connection_t **conn) {
    if (conn && *conn) {
        cehc_ep_remove_conn(*conn);
        curl_easy_cleanup((*conn)->easy);
        free((*conn)->url);
        free(*conn);
        *conn = NULL;
    }
}


static void
cehc_init_conn(cehc_connection_ptr conn) {
    conn->ce_code = CURLE_OK;
    conn->cm_code = CURLM_OK;
    conn->err_no = 0;
    conn->ep_code = 0;
    conn->fd = 0;
    conn->http_code = 0;
    conn->is_in_ep = false;
    bzero(conn->errormsg, sizeof(conn->errormsg));
}


/**
 * 加入到http serivce中跑，动作为non blocking。
 * @param conn
 * @param errmsg 长度上限为CURL_ERROR_SIZE
 * @return 成功为true, errmsg的strlen为0;失败为false并对输入参数errmsg赋值。
 */
bool
cehc_run_conn(cehc_connection_ptr conn, char *errmsg) {
    if (!conn) {
#define input_null_err "Any input param cannot be null!\0"
        if (errmsg)
            sprintf(errmsg, "%s", input_null_err);
        return false;
    }

    if (!conn->http_service || !conn->easy) {
#define field_null_err "Conn's http_service and easy field cannot be null!\0"
        if (errmsg)
            sprintf(errmsg, "%s", field_null_err);
        return false;
    }

    CURLMcode rc;
    // 交给multi托管
    cehc_init_conn(conn);
    std::unique_lock<std::mutex> l(conn->http_service->multi_handles_mtx);
    if ((rc = curl_multi_add_handle(conn->http_service->multi, conn->easy)) != CURLM_OK) {
        auto errm = curl_multi_strerror(rc);
        fprintf(stderr, "curl_multi_add_handle err with errmsg = %s.", errm);
        conn->cm_code = rc;
        sprintf(conn->errormsg, "%s", errm);
        if (errmsg)
            sprintf(errmsg, "%s", conn->errormsg);
        return false;
    }

    return true;
}


/**
 * 检查conn除了http code之外有无错误。
 */
bool
cehc_conn_ok_except_httpcode(cehc_connection_ptr conn) {
    return !(!conn || 0 != conn->err_no || 0 != conn->ep_code
             || CURLM_OK != conn->cm_code || CURLE_OK != conn->ce_code);

}


/**
 * 初始化curl服务，需要全局仅且只有一次调用。
 * 注意：建议在main函数最开始调用之。
 * @return
 */
bool
cehc_init_curl_global_service() {
    if (CURLE_OK != curl_global_init(CURL_GLOBAL_ALL)) {
        int err = errno;
        fprintf(stderr, "curl_global_init err = %s.", strerror(err));
        return false;
    }
    return true;
}

/**
 * 释放全局的curl服务资源。
 */
void
cehc_uninit_curl_global_service() {
    curl_global_cleanup();
}

/**
 * 创建一个新的连接服务(一个自管理连接池(curl multi + epoll))。
 * @param ep_ev_cnt
 * @param ep_once_ev_cnt
 * @param ep_timeout_ms
 * @return
 */
cehc_http_service_t *
cehc_new_http_service(int ep_ev_cnt, int ep_once_ev_cnt, int ep_timeout_ms) {
    // epoll
    int epfd = epoll_create(ep_ev_cnt);
    if (-1 == epfd) {
        int err = errno;
        fprintf(stderr, "epoll_create err = %s.", strerror(err));
        return NULL;
    }

    // curlm
    CURLM *cm = curl_multi_init();
    if (!cm) {
        int err = errno;
        fprintf(stderr, "curl_multi_init err = %s.", strerror(err));
        return NULL;
    }

    // http service
    cehc_http_service_t *hs = (cehc_http_service_t*)calloc(sizeof(cehc_http_service_t), 1);
    if (!hs) {
        fprintf(stderr, "%s oom when calloc cehc_http_service_t.", __func__);
        return NULL;
    }

    memset(hs, 0, sizeof(cehc_http_service_t));
    hs->ep_once_ev_cnt = ep_once_ev_cnt;
    hs->epfd = epfd;
    hs->ep_sl = UNLOCKED;
    hs->multi = cm;
    hs->ep_timeout_ms = ep_timeout_ms;
    hs->timer = new Timer();
    hs->timer_cb = cehc_timer_handler;

    return hs;
}

/**
 * 创建一个新的连接服务(一个自管理连接池(curl multi + epoll))。
 * @param ep_ev_cnt
 * @param ep_once_ev_cnt
 * @param ep_timeout_ms
 * @return
 */
cehc_http_service_t *
cehc_new_http_service_by_default_params() {
    return cehc_new_http_service(512, 512, -1);
}

bool
cehc_run_http_serivce(cehc_http_service_t *hs) {
    // 初始化curl multi service
    if (!cehc_init_curl_multi_service(hs)) {
        return false;
    }

    if (-1 == pthread_create(&hs->tid, NULL, cehc_inner_run_http_serivce, hs)) {
        int err = errno;
        fprintf(stderr, "pthread_create err = %s.", strerror(err));
        return false;
    }

    hs->timer->Start();
    return true;
}

/**
 * 释放一个http service，释放前，你最好先释放掉所有创建的connection。
 * @param phs hs的地址
 */
void
cehc_delete_http_serivce(cehc_http_service_t **phs) {
    if (phs && *phs) {
        cehc_http_service_t *hs = *phs;
        hs->stop = true;
        pthread_join(hs->tid, NULL);
        if (hs->timer) {
            hs->timer->Stop();
            delete hs->timer;
        }
        if (hs->multi) {
            curl_multi_cleanup(hs->multi);
        }
        if (hs->epfd) {
            close(hs->epfd);
        }

        free(hs);
        *phs = NULL;
    }
}
