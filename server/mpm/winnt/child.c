/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef WIN32

#include "httpd.h"
#include "http_main.h"
#include "http_log.h"
#include "http_config.h"  /* for read_config */
#include "http_core.h"    /* for get_remote_host */
#include "http_connection.h"
#include "apr_portable.h"
#include "apr_thread_proc.h"
#include "apr_getopt.h"
#include "apr_strings.h"
#include "apr_lib.h"
#include "apr_shm.h"
#include "apr_thread_mutex.h"
#include "ap_mpm.h"
#include "ap_config.h"
#include "ap_listen.h"
#include "mpm_default.h"
#include "mpm_winnt.h"
#include "mpm_common.h"
#include <malloc.h>
#include "apr_atomic.h"

#ifdef __MINGW32__
#include <mswsock.h>
#endif 

/* shared with mpm_winnt.c */
extern DWORD my_pid;

/* used by parent to signal the child to start and exit */
/* shared with mpm_winnt.c, but should be private to child.c */
apr_proc_mutex_t *start_mutex;
HANDLE exit_event;

/* Queue for managing the passing of winnt_conn_ctx_t between
 * the accept and worker threads.
 */
typedef struct winnt_conn_ctx_t_s {
    struct winnt_conn_ctx_t_s *next;
    OVERLAPPED Overlapped;
    apr_socket_t *sock;
    SOCKET accept_socket;
    char buff[2*PADDED_ADDR_SIZE];
    struct sockaddr *sa_server;
    int sa_server_len;
    struct sockaddr *sa_client;
    int sa_client_len;
    apr_pool_t *ptrans;
    apr_bucket_alloc_t *ba;
    short socket_family;
} winnt_conn_ctx_t;

typedef enum {
    IOCP_CONNECTION_ACCEPTED = 1,
    IOCP_WAIT_FOR_RECEIVE = 2,
    IOCP_WAIT_FOR_TRANSMITFILE = 3,
    IOCP_SHUTDOWN = 4
} io_state_e;

static apr_pool_t *pchild;
static int shutdown_in_progress = 0;
static int workers_may_exit = 0;
static unsigned int g_blocked_threads = 0;
static HANDLE max_requests_per_child_event;

static apr_thread_mutex_t  *child_lock;
static apr_thread_mutex_t  *qlock;
static winnt_conn_ctx_t *qhead = NULL;
static winnt_conn_ctx_t *qtail = NULL;
static int num_completion_contexts = 0;
static int max_num_completion_contexts = 0;
static HANDLE ThreadDispatchIOCP = NULL;
static HANDLE qwait_event = NULL;

static void mpm_recycle_completion_context(winnt_conn_ctx_t *context)
{
    /* Recycle the completion context.
     * - clear the ptrans pool
     * - put the context on the queue to be consumed by the accept thread
     * Note:
     * context->accept_socket may be in a disconnected but reusable
     * state so -don't- close it.
     */
    if (context) {
        apr_pool_clear(context->ptrans);
        context->ba = apr_bucket_alloc_create(context->ptrans);
        context->next = NULL;
        ResetEvent(context->Overlapped.hEvent);
        apr_thread_mutex_lock(qlock);
        if (qtail) {
            qtail->next = context;
        } else {
            qhead = context;
            SetEvent(qwait_event);
        }
        qtail = context;
        apr_thread_mutex_unlock(qlock);
    }
}

static winnt_conn_ctx_t *mpm_get_completion_context(void)
{
    apr_status_t rv;
    winnt_conn_ctx_t *context = NULL;

    while (1) {
        /* Grab a context off the queue */
        apr_thread_mutex_lock(qlock);
        if (qhead) {
            context = qhead;
            qhead = qhead->next;
            if (!qhead)
                qtail = NULL;
        } else {
            ResetEvent(qwait_event);
        }
        apr_thread_mutex_unlock(qlock);

        if (!context) {
            /* We failed to grab a context off the queue, consider allocating
             * a new one out of the child pool. There may be up to
             * (ap_threads_per_child + num_listeners) contexts in the system
             * at once.
             */
            if (num_completion_contexts >= max_num_completion_contexts) {
                /* All workers are busy, need to wait for one */
                static int reported = 0;
                if (!reported) {
                    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, ap_server_conf,
                                 "Server ran out of threads to serve "
                                 "requests. Consider raising the "
                                 "ThreadsPerChild setting");
                    reported = 1;
                }

                /* Wait for a worker to free a context. Once per second, give
                 * the caller a chance to check for shutdown. If the wait
                 * succeeds, get the context off the queue. It must be
                 * available, since there's only one consumer.
                 */
                rv = WaitForSingleObject(qwait_event, 1000);
                if (rv == WAIT_OBJECT_0)
                    continue;
                else /* Hopefully, WAIT_TIMEOUT */
                    return NULL;
            } else {
                /* Allocate another context.
                 * Note: Multiple failures in the next two steps will cause
                 * the pchild pool to 'leak' storage. I don't think this
                 * is worth fixing...
                 */
                apr_allocator_t *allocator;

                apr_thread_mutex_lock(child_lock);
                context = (winnt_conn_ctx_t *)apr_pcalloc(pchild,
                                                     sizeof(winnt_conn_ctx_t));


                context->Overlapped.hEvent = CreateEvent(NULL, TRUE,
                                                         FALSE, NULL);
                if (context->Overlapped.hEvent == NULL) {
                    /* Hopefully this is a temporary condition ... */
                    ap_log_error(APLOG_MARK, APLOG_WARNING, apr_get_os_error(),
                                 ap_server_conf,
                                 "mpm_get_completion_context: "
                                 "CreateEvent failed.");

                    apr_thread_mutex_unlock(child_lock);
                    return NULL;
                }

                /* Create the transaction pool */
                apr_allocator_create(&allocator);
                apr_allocator_max_free_set(allocator, ap_max_mem_free);
                rv = apr_pool_create_ex(&context->ptrans, pchild, NULL, 
                                        allocator);
                if (rv != APR_SUCCESS) {
                    ap_log_error(APLOG_MARK, APLOG_WARNING, rv, ap_server_conf,
                                 "mpm_get_completion_context: Failed "
                                 "to create the transaction pool.");
                    CloseHandle(context->Overlapped.hEvent);

                    apr_thread_mutex_unlock(child_lock);
                    return NULL;
                }
                apr_allocator_owner_set(allocator, context->ptrans);
                apr_pool_tag(context->ptrans, "transaction");

                context->accept_socket = INVALID_SOCKET;
                context->ba = apr_bucket_alloc_create(context->ptrans);
                apr_atomic_inc32(&num_completion_contexts);

                apr_thread_mutex_unlock(child_lock);
                break;
            }
        } else {
            /* Got a context from the queue */
            break;
        }
    }

    return context;
}

static apr_status_t mpm_post_completion_context(winnt_conn_ctx_t *context,
                                                io_state_e state)
{
    LPOVERLAPPED pOverlapped;
    if (context)
        pOverlapped = &context->Overlapped;
    else
        pOverlapped = NULL;

    PostQueuedCompletionStatus(ThreadDispatchIOCP, 0, state, pOverlapped);
    return APR_SUCCESS;
}


/*
 * find_ready_listener()
 * Only used by Win9* and should go away when the win9*_accept() function is
 * reimplemented using apr_poll().
 */
static ap_listen_rec *head_listener;

static APR_INLINE ap_listen_rec *find_ready_listener(fd_set * main_fds)
{
    ap_listen_rec *lr;
    SOCKET nsd;

    lr = head_listener;
    do {
        apr_os_sock_get(&nsd, lr->sd);
        if (FD_ISSET(nsd, main_fds)) {
            head_listener = lr->next;
            if (!head_listener) {
                head_listener = ap_listeners;
            }
            return lr;
        }
        lr = lr->next;
        if (!lr) {
            lr = ap_listeners;
        }
    } while (lr != head_listener);
    return NULL;
}


/* Windows NT/2000 specific code...
 * Accept processing for on Windows NT uses a producer/consumer queue
 * model. An accept thread accepts connections off the network then issues
 * PostQueuedCompletionStatus() to awake a thread blocked on the ThreadDispatch
 * IOCompletionPort.
 *
 * winnt_accept()
 *    One or more accept threads run in this function, each of which accepts
 *    connections off the network and calls PostQueuedCompletionStatus() to
 *    queue an io completion packet to the ThreadDispatch IOCompletionPort.
 * winnt_get_connection()
 *    Worker threads block on the ThreadDispatch IOCompletionPort awaiting
 *    connections to service.
 */
#define MAX_ACCEPTEX_ERR_COUNT 100
static unsigned int __stdcall winnt_accept(void *lr_)
{
    ap_listen_rec *lr = (ap_listen_rec *)lr_;
    apr_os_sock_info_t sockinfo;
    winnt_conn_ctx_t *context = NULL;
    DWORD BytesRead;
    SOCKET nlsd;
    int rv, err_count = 0;
#if APR_HAVE_IPV6
    SOCKADDR_STORAGE ss_listen;
    int namelen = sizeof(ss_listen);
#endif

    apr_os_sock_get(&nlsd, lr->sd);

#if APR_HAVE_IPV6
    if (getsockname(nlsd, (struct sockaddr *)&ss_listen, &namelen) == SOCKET_ERROR) {
        ap_log_error(APLOG_MARK, APLOG_ERR, apr_get_netos_error(), 
                     ap_server_conf,
                     "winnt_accept: getsockname error on listening socket, "
                     "is IPv6 available?");
        return 1;
   }
#endif

    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                 "Child %d: Starting thread to listen on port %d.", 
                 my_pid, lr->bind_addr->port);
    while (!shutdown_in_progress) {
        if (!context) {
            context = mpm_get_completion_context();
            if (!context) {
                /* Temporary resource constraint? */
                Sleep(0);
                continue;
            }
        }

        /* Create and initialize the accept socket */
#if APR_HAVE_IPV6
        if (context->accept_socket == INVALID_SOCKET) {
            context->accept_socket = socket(ss_listen.ss_family, SOCK_STREAM, 
                                            IPPROTO_TCP);
            context->socket_family = ss_listen.ss_family;
        }
        else if (context->socket_family != ss_listen.ss_family) {
            closesocket(context->accept_socket);
            context->accept_socket = socket(ss_listen.ss_family, SOCK_STREAM, 
                                            IPPROTO_TCP);
            context->socket_family = ss_listen.ss_family;
        }

        if (context->accept_socket == INVALID_SOCKET) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, apr_get_netos_error(), 
                         ap_server_conf,
                         "winnt_accept: Failed to allocate an accept socket. "
                         "Temporary resource constraint? Try again.");
            Sleep(100);
            continue;
        }
#else
        if (context->accept_socket == INVALID_SOCKET) {
            context->accept_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (context->accept_socket == INVALID_SOCKET) {
                /* Another temporary condition? */
                ap_log_error(APLOG_MARK, APLOG_WARNING, apr_get_netos_error(), 
                             ap_server_conf,
                             "winnt_accept: Failed to allocate an accept "
                             "socket. Temporary resource constraint? "
                             "Retrying.");
                Sleep(100);
                continue;
            }
        }
#endif
        /* AcceptEx on the completion context. The completion context will be
         * signaled when a connection is accepted.
         */
        if (!AcceptEx(nlsd, context->accept_socket,
                      context->buff,
                      0,
                      PADDED_ADDR_SIZE,
                      PADDED_ADDR_SIZE,
                      &BytesRead,
                      &context->Overlapped)) {
            rv = apr_get_netos_error();
            if ((rv == APR_FROM_OS_ERROR(WSAEINVAL)) ||
                (rv == APR_FROM_OS_ERROR(WSAENOTSOCK))) {
                /* We can get here when:
                 * 1) the client disconnects early
                 * 2) TransmitFile does not properly recycle the accept socket (typically
                 *    because the client disconnected)
                 * 3) there is VPN or Firewall software installed with buggy AcceptEx implementation
                 * 4) the webserver is using a dynamic address that has changed
                 */
                ++err_count;
                closesocket(context->accept_socket);
                context->accept_socket = INVALID_SOCKET;
                if (err_count > MAX_ACCEPTEX_ERR_COUNT) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, rv, ap_server_conf,
                                 "Child %d: Encountered too many AcceptEx "
                                 "faults accepting client connections. "
                                 "Possible causes: dynamic address renewal, "
                                 "or incompatible VPN or firewall software. "
                                 "Try the directive 'AcceptFilter none'.",
                                 my_pid);
                    err_count = 0;
                }
                continue;
            }
            else if ((rv != APR_FROM_OS_ERROR(ERROR_IO_PENDING)) &&
                     (rv != APR_FROM_OS_ERROR(WSA_IO_PENDING))) {
                ++err_count;
                if (err_count > MAX_ACCEPTEX_ERR_COUNT) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, rv, ap_server_conf,
                                 "Child %d: Encountered too many AcceptEx "
                                 "faults accepting client connections. "
                                 "Possible causes: Unknown. "
                                 "Try the directive 'AcceptFilter none'.",
                                 my_pid);
                    err_count = 0;
                }
                closesocket(context->accept_socket);
                context->accept_socket = INVALID_SOCKET;
                continue;
            }
            err_count = 0;

            /* Wait for pending i/o.
             * Wake up once per second to check for shutdown .
             * XXX: We should be waiting on exit_event instead of polling
             */
            while (1) {
                rv = WaitForSingleObject(context->Overlapped.hEvent, 1000);
                if (rv == WAIT_OBJECT_0) {
                    if (context->accept_socket == INVALID_SOCKET) {
                        /* socket already closed */
                        break;
                    }
                    if (!GetOverlappedResult((HANDLE)context->accept_socket,
                                             &context->Overlapped,
                                             &BytesRead, FALSE)) {
                        ap_log_error(APLOG_MARK, APLOG_WARNING,
                                     apr_get_os_error(), ap_server_conf,
                             "winnt_accept: Asynchronous AcceptEx failed.");
                        closesocket(context->accept_socket);
                        context->accept_socket = INVALID_SOCKET;
                    }
                    break;
                }
                /* WAIT_TIMEOUT */
                if (shutdown_in_progress) {
                    closesocket(context->accept_socket);
                    context->accept_socket = INVALID_SOCKET;
                    break;
                }
            }
            if (context->accept_socket == INVALID_SOCKET) {
                continue;
            }
        }
        err_count = 0;
        /* Inherit the listen socket settings. Required for
         * shutdown() to work
         */
        if (setsockopt(context->accept_socket, SOL_SOCKET,
                       SO_UPDATE_ACCEPT_CONTEXT, (char *)&nlsd,
                       sizeof(nlsd))) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, apr_get_netos_error(),
                         ap_server_conf,
                         "setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed.");
            /* Not a failure condition. Keep running. */
        }

        /* Get the local & remote address */
        GetAcceptExSockaddrs(context->buff,
                             0,
                             PADDED_ADDR_SIZE,
                             PADDED_ADDR_SIZE,
                             &context->sa_server,
                             &context->sa_server_len,
                             &context->sa_client,
                             &context->sa_client_len);

        sockinfo.os_sock = &context->accept_socket;
        sockinfo.local   = context->sa_server;
        sockinfo.remote  = context->sa_client;
        sockinfo.family  = context->sa_server->sa_family;
        sockinfo.type    = SOCK_STREAM;
        apr_os_sock_make(&context->sock, &sockinfo, context->ptrans);

        /* When a connection is received, send an io completion notification to
         * the ThreadDispatchIOCP. This function could be replaced by
         * mpm_post_completion_context(), but why do an extra function call...
         */
        PostQueuedCompletionStatus(ThreadDispatchIOCP, 0,
                                   IOCP_CONNECTION_ACCEPTED,
                                   &context->Overlapped);
        context = NULL;
    }
    if (!shutdown_in_progress) {
        /* Yow, hit an irrecoverable error! Tell the child to die. */
        SetEvent(exit_event);
    }
    ap_log_error(APLOG_MARK, APLOG_INFO, APR_SUCCESS, ap_server_conf,
                 "Child %d: Accept thread exiting.", my_pid);
    return 0;
}


static winnt_conn_ctx_t *winnt_get_connection(winnt_conn_ctx_t *context)
{
    int rc;
    DWORD BytesRead;
    LPOVERLAPPED pol;
#ifdef _WIN64
    ULONG_PTR CompKey;
#else
    DWORD CompKey;
#endif

    mpm_recycle_completion_context(context);

    apr_atomic_inc32(&g_blocked_threads);
    while (1) {
        if (workers_may_exit) {
            apr_atomic_dec32(&g_blocked_threads);
            return NULL;
        }
        rc = GetQueuedCompletionStatus(ThreadDispatchIOCP, &BytesRead,
                                       &CompKey, &pol, INFINITE);
        if (!rc) {
            rc = apr_get_os_error();
            ap_log_error(APLOG_MARK, APLOG_DEBUG, rc, ap_server_conf,
                         "Child %d: GetQueuedComplationStatus returned %d",
                         my_pid, rc);
            continue;
        }

        switch (CompKey) {
        case IOCP_CONNECTION_ACCEPTED:
            context = CONTAINING_RECORD(pol, winnt_conn_ctx_t, Overlapped);
            break;
        case IOCP_SHUTDOWN:
            apr_atomic_dec32(&g_blocked_threads);
            return NULL;
        default:
            apr_atomic_dec32(&g_blocked_threads);
            return NULL;
        }
        break;
    }
    apr_atomic_dec32(&g_blocked_threads);

    return context;
}


/*
 * worker_main()
 * Main entry point for the worker threads. Worker threads block in
 * win*_get_connection() awaiting a connection to service.
 */
static unsigned int __stdcall worker_main(void *thread_num_val)
{
    static int requests_this_child = 0;
    winnt_conn_ctx_t *context = NULL;
    int thread_num = (int)thread_num_val;
    ap_sb_handle_t *sbh;

    while (1) {
        conn_rec *c;
        apr_int32_t disconnected;

        ap_update_child_status_from_indexes(0, thread_num, SERVER_READY, NULL);

        /* Grab a connection off the network */
        context = winnt_get_connection(context);

        if (!context) {
            /* Time for the thread to exit */
            break;
        }

        /* Have we hit MaxRequestPerChild connections? */
        if (ap_max_requests_per_child) {
            requests_this_child++;
            if (requests_this_child > ap_max_requests_per_child) {
                SetEvent(max_requests_per_child_event);
            }
        }

        ap_create_sb_handle(&sbh, context->ptrans, 0, thread_num);
        c = ap_run_create_connection(context->ptrans, ap_server_conf,
                                     context->sock, thread_num, sbh,
                                     context->ba);

        if (c) {
            ap_process_connection(c, context->sock);
            apr_socket_opt_get(context->sock, APR_SO_DISCONNECTED,
                               &disconnected);
            if (!disconnected) {
                context->accept_socket = INVALID_SOCKET;
                ap_lingering_close(c);
            }
            else {
                /* If the socket is disconnected but we are not using acceptex,
                 * we cannot reuse the socket. Disconnected sockets are removed
                 * from the apr_socket_t struct by apr_sendfile() to prevent the
                 * socket descriptor from being inadvertently closed by a call
                 * to apr_socket_close(), so close it directly.
                 */
                /* XXX Study me for NT;
                 * closesocket(context->accept_socket);
                 * context->accept_socket = INVALID_SOCKET;
                 */
            }
        }
        else {
            /* ap_run_create_connection closes the socket on failure */
            context->accept_socket = INVALID_SOCKET;
        }
    }

    ap_update_child_status_from_indexes(0, thread_num, SERVER_DEAD,
                                        (request_rec *) NULL);

    return 0;
}


static void cleanup_thread(HANDLE *handles, int *thread_cnt, 
                           int thread_to_clean)
{
    int i;

    CloseHandle(handles[thread_to_clean]);
    for (i = thread_to_clean; i < ((*thread_cnt) - 1); i++)
        handles[i] = handles[i + 1];
    (*thread_cnt)--;
}


/*
 * child_main()
 * Entry point for the main control thread for the child process.
 * This thread creates the accept thread, worker threads and
 * monitors the child process for maintenance and shutdown
 * events.
 */
static void create_listener_thread()
{
    int tid;
    int num_listeners = 0;
    /* Start an accept thread per listener
     * XXX: Why would we have a NULL sd in our listeners?
     */
    ap_listen_rec *lr;

    /* Number of completion_contexts allowed in the system is
     * (ap_threads_per_child + num_listeners). We need the additional
     * completion contexts to prevent server hangs when ThreadsPerChild
     * is configured to something less than or equal to the number
     * of listeners. This is not a usual case, but people have
     * encountered it.
     */
    for (lr = ap_listeners; lr ; lr = lr->next) {
        num_listeners++;
    }
    max_num_completion_contexts = ap_threads_per_child + num_listeners;

    /* Now start a thread per listener */
    for (lr = ap_listeners; lr; lr = lr->next) {
        if (lr->sd != NULL) {
            /* A smaller stack is sufficient.
             * To convert to CreateThread, the returned handle cannot be 
             * ignored, it must be closed/joined.
             */
            _beginthreadex(NULL, 65536, winnt_accept,
                           (void *) lr, stack_res_flag, &tid);
        }
    }
}


void child_main(apr_pool_t *pconf)
{
    apr_status_t status;
    apr_hash_t *ht;
    ap_listen_rec *lr;
    HANDLE child_events[2];
    HANDLE *child_handles;
    int listener_started = 0;
    int threads_created = 0;
    int watch_thread;
    int time_remains;
    int cld;
    int tid;
    int rv;
    int i;

    apr_pool_create(&pchild, pconf);
    apr_pool_tag(pchild, "pchild");

    ap_run_child_init(pchild, ap_server_conf);
    ht = apr_hash_make(pchild);

    /* Initialize the child_events */
    max_requests_per_child_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!max_requests_per_child_event) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, apr_get_os_error(), ap_server_conf,
                     "Child %d: Failed to create a max_requests event.", 
                     my_pid);
        exit(APEXIT_CHILDINIT);
    }
    child_events[0] = exit_event;
    child_events[1] = max_requests_per_child_event;

    /*
     * Wait until we have permission to start accepting connections.
     * start_mutex is used to ensure that only one child ever
     * goes into the listen/accept loop at once.
     */
    status = apr_proc_mutex_lock(start_mutex);
    if (status != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_ERR, status, ap_server_conf,
                     "Child %d: Failed to acquire the start_mutex. "
                     "Process will exit.", my_pid);
        exit(APEXIT_CHILDINIT);
    }
    ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, ap_server_conf,
                 "Child %d: Acquired the start mutex.", my_pid);

    /*
     * Create the worker thread dispatch IOCompletionPort
     */
    /* Create the worker thread dispatch IOCP */
    ThreadDispatchIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
                                                NULL, 0, 0); 
    apr_thread_mutex_create(&qlock, APR_THREAD_MUTEX_DEFAULT, pchild);
    qwait_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!qwait_event) {
        ap_log_error(APLOG_MARK, APLOG_CRIT, apr_get_os_error(), 
                     ap_server_conf,
                     "Child %d: Failed to create a qwait event.", my_pid);
        exit(APEXIT_CHILDINIT);
    }

    /*
     * Create the pool of worker threads
     */
    ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, ap_server_conf,
                 "Child %d: Starting %d worker threads.",
                 my_pid, ap_threads_per_child);
    child_handles = (HANDLE) apr_pcalloc(pchild, ap_threads_per_child
                                                  * sizeof(HANDLE));
    apr_thread_mutex_create(&child_lock, APR_THREAD_MUTEX_DEFAULT, pchild);

    while (1) {
        for (i = 0; i < ap_threads_per_child; i++) {
            int *score_idx;
            int status = ap_scoreboard_image->servers[0][i].status;
            if (status != SERVER_GRACEFUL && status != SERVER_DEAD) {
                continue;
            }
            ap_update_child_status_from_indexes(0, i, SERVER_STARTING, NULL);
        
            child_handles[i] = CreateThread(NULL, ap_thread_stacksize,
                                            worker_main, (void *) i,
                                            stack_res_flag, &tid);
            if (child_handles[i] == 0) {
                ap_log_error(APLOG_MARK, APLOG_CRIT, apr_get_os_error(),
                             ap_server_conf,
                             "Child %d: CreateThread failed. Unable to "
                             "create all worker threads. Created %d of the %d "
                             "threads requested with the ThreadsPerChild "
                             "configuration directive.",
                             my_pid, threads_created, ap_threads_per_child);
                ap_signal_parent(SIGNAL_PARENT_SHUTDOWN);
                goto shutdown;
            }
            threads_created++;
            /* Save the score board index in ht keyed to the thread handle. 
             * We need this when cleaning up threads down below...
             */
            apr_thread_mutex_lock(child_lock);
            score_idx = apr_pcalloc(pchild, sizeof(int));
            *score_idx = i;
            apr_hash_set(ht, &child_handles[i], sizeof(HANDLE), score_idx);
            apr_thread_mutex_unlock(child_lock);
        }
        /* Start the listener only when workers are available */
        if (!listener_started && threads_created) {
            create_listener_thread();
            listener_started = 1;
            winnt_mpm_state = AP_MPMQ_RUNNING;
        }
        if (threads_created == ap_threads_per_child) {
            break;
        }
        /* Check to see if the child has been told to exit */
        if (WaitForSingleObject(exit_event, 0) != WAIT_TIMEOUT) {
            break;
        }
        /* wait for previous generation to clean up an entry in the scoreboard
         */
        apr_sleep(1 * APR_USEC_PER_SEC);
    }

    /* Wait for one of three events:
     * exit_event:
     *    The exit_event is signaled by the parent process to notify
     *    the child that it is time to exit.
     *
     * max_requests_per_child_event:
     *    This event is signaled by the worker threads to indicate that
     *    the process has handled MaxRequestsPerChild connections.
     *
     * TIMEOUT:
     *    To do periodic maintenance on the server (check for thread exits,
     *    number of completion contexts, etc.)
     *
     * XXX: thread exits *aren't* being checked.
     *
     * XXX: other_child - we need the process handles to the other children
     *      in order to map them to apr_proc_other_child_read (which is not
     *      named well, it's more like a_p_o_c_died.)
     *
     * XXX: however - if we get a_p_o_c handle inheritance working, and
     *      the parent process creates other children and passes the pipes
     *      to our worker processes, then we have no business doing such
     *      things in the child_main loop, but should happen in master_main.
     */
    while (1) {
#if !APR_HAS_OTHER_CHILD
        rv = WaitForMultipleObjects(2, (HANDLE *)child_events, FALSE, INFINITE);
        cld = rv - WAIT_OBJECT_0;
#else
        rv = WaitForMultipleObjects(2, (HANDLE *)child_events, FALSE, 1000);
        cld = rv - WAIT_OBJECT_0;
        if (rv == WAIT_TIMEOUT) {
            apr_proc_other_child_refresh_all(APR_OC_REASON_RUNNING);
        }
        else
#endif
            if (rv == WAIT_FAILED) {
            /* Something serious is wrong */
            ap_log_error(APLOG_MARK, APLOG_CRIT, apr_get_os_error(),
                         ap_server_conf,
                         "Child %d: WAIT_FAILED -- shutting down server", 
                         my_pid);
            break;
        }
        else if (cld == 0) {
            /* Exit event was signaled */
            ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, ap_server_conf,
                         "Child %d: Exit event signaled. Child process is "
                         "ending.", my_pid);
            break;
        }
        else {
            /* MaxRequestsPerChild event set by the worker threads.
             * Signal the parent to restart
             */
            ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, ap_server_conf,
                         "Child %d: Process exiting because it reached "
                         "MaxRequestsPerChild. Signaling the parent to "
                         "restart a new child process.", my_pid);
            ap_signal_parent(SIGNAL_PARENT_RESTART);
            break;
        }
    }

    /*
     * Time to shutdown the child process
     */

 shutdown:

    winnt_mpm_state = AP_MPMQ_STOPPING;

    /* Close the listening sockets. Note, we must close the listeners
     * before closing any accept sockets pending in AcceptEx to prevent
     * memory leaks in the kernel.
     */
    for (lr = ap_listeners; lr ; lr = lr->next) {
        apr_socket_close(lr->sd);
    }

    /* Shutdown listener threads and pending AcceptEx socksts
     * but allow the worker threads to continue consuming from
     * the queue of accepted connections.
     */
    shutdown_in_progress = 1;

    Sleep(1000);

    /* Tell the worker threads to exit */
    workers_may_exit = 1;

    /* Release the start_mutex to let the new process (in the restart
     * scenario) a chance to begin accepting and servicing requests
     */
    rv = apr_proc_mutex_unlock(start_mutex);
    if (rv == APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, rv, ap_server_conf,
                     "Child %d: Released the start mutex", my_pid);
    }
    else {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, ap_server_conf,
                     "Child %d: Failure releasing the start mutex", my_pid);
    }

    /* Shutdown the worker threads
     * Post worker threads blocked on the ThreadDispatch IOCompletion port
     */
    while (g_blocked_threads > 0) {
        ap_log_error(APLOG_MARK, APLOG_INFO, APR_SUCCESS, ap_server_conf,
                     "Child %d: %d threads blocked on the completion port",
                     my_pid, g_blocked_threads);
        for (i=g_blocked_threads; i > 0; i--) {
            PostQueuedCompletionStatus(ThreadDispatchIOCP, 0, 
                                       IOCP_SHUTDOWN, NULL);
        }
        Sleep(1000);
    }
    /* Empty the accept queue of completion contexts */
    apr_thread_mutex_lock(qlock);
    while (qhead) {
        CloseHandle(qhead->Overlapped.hEvent);
        closesocket(qhead->accept_socket);
        qhead = qhead->next;
    }
    apr_thread_mutex_unlock(qlock);

    /* Give busy threads a chance to service their connections,
     * (no more than the global server timeout period which 
     * we track in msec remaining).
     */
    watch_thread = 0;
    time_remains = (int)(ap_server_conf->timeout / APR_TIME_C(1000));

    while (threads_created)
    {
        int nFailsafe = MAXIMUM_WAIT_OBJECTS;
        DWORD dwRet;

        /* Every time we roll over to wait on the first group
         * of MAXIMUM_WAIT_OBJECTS threads, take a breather,
         * and infrequently update the error log.
         */
        if (watch_thread >= threads_created) {
            if ((time_remains -= 100) < 0)
                break;

            /* Every 30 seconds give an update */
            if ((time_remains % 30000) == 0) {
                ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, 
                             ap_server_conf,
                             "Child %d: Waiting %d more seconds "
                             "for %d worker threads to finish.", 
                             my_pid, time_remains / 1000, threads_created);
            }
            /* We'll poll from the top, 10 times per second */
            Sleep(100);
            watch_thread = 0;
        }

        /* Fairness, on each iteration we will pick up with the thread
         * after the one we just removed, even if it's a single thread.
         * We don't block here.
         */
        dwRet = WaitForMultipleObjects(min(threads_created - watch_thread,
                                           MAXIMUM_WAIT_OBJECTS),
                                       child_handles + watch_thread, 0, 0);

        if (dwRet == WAIT_FAILED) {
            break;
        }
        if (dwRet == WAIT_TIMEOUT) {
            /* none ready */
            watch_thread += MAXIMUM_WAIT_OBJECTS;
            continue;
        }
        else if (dwRet >= WAIT_ABANDONED_0) {
            /* We just got the ownership of the object, which
             * should happen at most MAXIMUM_WAIT_OBJECTS times.
             * It does NOT mean that the object is signaled.
             */
            if ((nFailsafe--) < 1)
                break;
        }
        else {
            watch_thread += (dwRet - WAIT_OBJECT_0);
            if (watch_thread >= threads_created)
                break;
            cleanup_thread(child_handles, &threads_created, watch_thread);
        }
    }

    /* Kill remaining threads off the hard way */
    if (threads_created) {
        ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, ap_server_conf,
                     "Child %d: Terminating %d threads that failed to exit.",
                     my_pid, threads_created);
    }
    for (i = 0; i < threads_created; i++) {
        int *score_idx;
        TerminateThread(child_handles[i], 1);
        CloseHandle(child_handles[i]);
        /* Reset the scoreboard entry for the thread we just whacked */
        score_idx = apr_hash_get(ht, &child_handles[i], sizeof(HANDLE));
        if (score_idx) {
            ap_update_child_status_from_indexes(0, *score_idx, SERVER_DEAD, NULL);
        }
    }
    ap_log_error(APLOG_MARK, APLOG_NOTICE, APR_SUCCESS, ap_server_conf,
                 "Child %d: All worker threads have exited.", my_pid);

    apr_thread_mutex_destroy(child_lock);

    apr_pool_destroy(pchild);
    CloseHandle(exit_event);
}

#endif /* def WIN32 */
