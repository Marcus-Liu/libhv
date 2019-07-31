#include "http_server.h"

#include "h.h"
#include "hmain.h"
#include "hloop.h"
#include "hbuf.h"

#include "HttpParser.h"
#include "FileCache.h"

#define RECV_BUFSIZE    4096
#define SEND_BUFSIZE    4096

static HttpService s_default_service;
static FileCache s_filecache;

/*
<!DOCTYPE html>
<html>
<head>
  <title>404 Not Found</title>
</head>
<body>
  <center><h1>404 Not Found</h1></center>
  <hr>
</body>
</html>
 */
static void make_http_status_page(http_status status_code, std::string& page) {
    char szCode[8];
    snprintf(szCode, sizeof(szCode), "%d ", status_code);
    const char* status_message = http_status_str(status_code);
    page += R"(<!DOCTYPE html>
<html>
<head>
  <title>)";
    page += szCode; page += status_message;
    page += R"(</title>
</head>
<body>
  <center><h1>)";
    page += szCode; page += status_message;
    page += R"(</h1></center>
  <hr>
</body>
</html>)";
}

static void master_init(void* userdata) {
#ifdef OS_UNIX
    char proctitle[256] = {0};
    snprintf(proctitle, sizeof(proctitle), "%s: master process", g_main_ctx.program_name);
    setproctitle(proctitle);
#endif
}
static void master_proc(void* userdata) {
    while(1) sleep(1);
}

static void worker_init(void* userdata) {
#ifdef OS_UNIX
    char proctitle[256] = {0};
    snprintf(proctitle, sizeof(proctitle), "%s: worker process", g_main_ctx.program_name);
    setproctitle(proctitle);
    signal(SIGNAL_RELOAD, signal_handler);
#endif
}

struct http_connect_userdata {
    http_server_t*          server;
    std::string             log;
    HttpParser              parser;
    HttpRequest             req;
    HttpResponse            res;

    http_connect_userdata() {
        parser.parser_request_init(&req);
    }
};

static void on_read(hio_t* io, void* buf, int readbytes) {
    //printf("on_read fd=%d readbytes=%d\n", io->fd, readbytes);
    http_connect_userdata* hcu = (http_connect_userdata*)io->userdata;
    HttpService* service = hcu->server->service;
    HttpRequest* req = &hcu->req;
    HttpResponse* res = &hcu->res;
    int ret, nparse;
    char* recvbuf = (char*)buf;
    int nrecv = readbytes;
    // recv -> http_parser -> http_request -> http_request_handler -> http_response -> send
    //printf("%s\n", recvbuf);
    nparse = hcu->parser.execute(recvbuf, nrecv);
    if (nparse != nrecv || hcu->parser.get_errno() != HPE_OK) {
        hcu->log += asprintf("http parser error: %s", http_errno_description(hcu->parser.get_errno()));
        hclose(io);
        return;
    }
    if (hcu->parser.get_state() == HP_MESSAGE_COMPLETE) {
        http_api_handler api = NULL;
        file_cache_t* fc = NULL;
        const char* content = NULL;
        int content_length = 0;
        bool send_in_one_packet = false;

        hcu->log += asprintf("[%s %s]", http_method_str(req->method), req->url.c_str());
        static std::string s_Server = std::string("httpd/") + std::string(get_compile_version());
        res->headers["Server"] = s_Server;
        // preprocessor
        if (service->preprocessor) {
            service->preprocessor(req, res);
        }
        ret = service->GetApi(req->url.c_str(), req->method, &api);
        if (api) {
            // api service
            api(req, res);
        }
        else {
            if (ret == HTTP_STATUS_METHOD_NOT_ALLOWED) {
                // Method Not Allowed
                res->status_code = HTTP_STATUS_METHOD_NOT_ALLOWED;
            }
            else if (req->method == HTTP_GET) {
                // web service
                std::string filepath = service->document_root;
                filepath += req->url.c_str();
                if (strcmp(req->url.c_str(), "/") == 0) {
                    filepath += service->home_page;
                }
                fc = s_filecache.Open(filepath.c_str());
                // Not Found
                if (fc == NULL) {
                    res->status_code = HTTP_STATUS_NOT_FOUND;
                }
                else {
                    // Not Modified
                    auto iter = req->headers.find("if-not-match");
                    if (iter != req->headers.end() &&
                        strcmp(iter->second.c_str(), fc->etag) == 0) {
                        res->status_code = HTTP_STATUS_NOT_MODIFIED;
                        fc = NULL;
                    }
                    else {
                        iter = req->headers.find("if-modified-since");
                        if (iter != req->headers.end() &&
                            strcmp(iter->second.c_str(), fc->last_modified) == 0) {
                            res->status_code = HTTP_STATUS_NOT_MODIFIED;
                            fc = NULL;
                        }
                    }
                }
            }
            else {
                // Not Implemented
                res->status_code = HTTP_STATUS_NOT_IMPLEMENTED;
            }

            // html page
            if (res->status_code >= 400 && res->body.size() == 0) {
                // error page
                if (service->error_page.size() != 0) {
                    std::string filepath = service->document_root;
                    filepath += '/';
                    filepath += service->error_page;
                    fc = s_filecache.Open(filepath.c_str());
                }

                // status page
                if (fc == NULL && res->body.size() == 0) {
                    res->content_type = TEXT_HTML;
                    make_http_status_page(res->status_code, res->body);
                }
            }
        }
        // postprocessor
        if (service->postprocessor) {
            service->postprocessor(req, res);
        }
        // send
        std::string header;
        time_t tt;
        time(&tt);
        char c_str[256] = {0};
        strftime(c_str, sizeof(c_str), "%a, %d %b %Y %H:%M:%S GMT", gmtime(&tt));
        res->headers["Date"] = c_str;
        if (fc && fc->filebuf.len) {
            content = (const char*)fc->filebuf.base;
            content_length = fc->filebuf.len;
            if (fc->content_type && *fc->content_type != '\0') {
                res->headers["Content-Type"] = fc->content_type;
            }
            res->headers["Content-Length"] = std::to_string(content_length);
            res->headers["Last-Modified"] = fc->last_modified;
            res->headers["Etag"] = fc->etag;
        }
        else if (res->body.size()) {
            content = res->body.c_str();
            content_length = res->body.size();
        }
        header = res->dump(true, false);
        if (header.size() + content_length <= SEND_BUFSIZE) {
            header.insert(header.size(), content, content_length);
            send_in_one_packet = true;
        }

        // send header
        hwrite(io->loop, io->fd, header.c_str(), header.size());
        // send body
        if (!send_in_one_packet && content_length != 0) {
            hwrite(io->loop, io->fd, content, content_length);
        }
        hcu->log += asprintf("=>[%d %s]", res->status_code, http_status_str(res->status_code));
        hclose(io);
    }
}

static void on_close(hio_t* io) {
    http_connect_userdata* hcu = (http_connect_userdata*)io->userdata;
    if (hcu) {
        hlogi("%s", hcu->log.c_str());
        delete hcu;
        io->userdata = NULL;
    }
}

static void on_accept(hio_t* io, int connfd) {
    //printf("on_accept listenfd=%d connfd=%d\n", io->fd, connfd);
    struct sockaddr_in* localaddr = (struct sockaddr_in*)io->localaddr;
    struct sockaddr_in* peeraddr = (struct sockaddr_in*)io->peeraddr;
    //char localip[64];
    char peerip[64];
    //inet_ntop(AF_INET, &localaddr->sin_addr, localip, sizeof(localip));
    inet_ntop(AF_INET, &peeraddr->sin_addr, peerip, sizeof(peerip));
    //printd("accept listenfd=%d connfd=%d [%s:%d] <= [%s:%d]\n", io->fd, connfd,
            //localip, ntohs(localaddr->sin_port),
            //peerip, ntohs(peeraddr->sin_port));
    // new http_connect_userdata
    // delete on_close
    http_connect_userdata* hcu = new http_connect_userdata;
    hcu->server = (http_server_t*)io->userdata;
    hcu->log += asprintf("[%s:%d]", peerip, ntohs(peeraddr->sin_port));

    nonblocking(connfd);
    HBuf* buf = (HBuf*)io->loop->userdata;
    hio_t* connio = hread(io->loop, connfd, buf->base, buf->len, on_read);
    connio->close_cb = on_close;
    connio->userdata = hcu;
}

void handle_cached_files(htimer_t* timer) {
    FileCache* pfc = (FileCache*)timer->userdata;
    if (pfc == NULL) {
        htimer_del(timer);
        return;
    }
    file_cache_t* fc = NULL;
    time_t tt;
    time(&tt);
    auto iter = pfc->cached_files.begin();
    while (iter != pfc->cached_files.end()) {
        fc = iter->second;
        if (tt - fc->stat_time > pfc->file_cached_time) {
            delete fc;
            iter = pfc->cached_files.erase(iter);
            continue;
        }
        ++iter;
    }
}

void fflush_log(hidle_t* idle) {
    hlog_fflush();
}

static void worker_proc(void* userdata) {
    http_server_t* server = (http_server_t*)userdata;
    int listenfd = server->listenfd;
    hloop_t loop;
    hloop_init(&loop);
    // one loop one readbuf.
    HBuf readbuf;
    readbuf.resize(RECV_BUFSIZE);
    loop.userdata = &readbuf;
    hio_t* listenio = haccept(&loop, listenfd, on_accept);
    listenio->userdata = server;
    // fflush logfile when idle
    hlog_set_fflush(0);
    hidle_add(&loop, fflush_log, INFINITE);
    // timer handle_cached_files
    htimer_t* timer = htimer_add(&loop, handle_cached_files, s_filecache.file_cached_time*1000);
    timer->userdata = &s_filecache;
    hloop_run(&loop);
}

int http_server_run(http_server_t* server, int wait) {
    // worker_processes
    if (server->worker_processes != 0 && g_worker_processes_num != 0 && g_worker_processes != NULL) {
        return ERR_OVER_LIMIT;
    }
    // service
    if (server->service == NULL) {
        server->service = &s_default_service;
    }
    // port
    server->listenfd = Listen(server->port);
    if (server->listenfd < 0) return server->listenfd;

#ifdef OS_WIN
    if (server->worker_processes > 1) {
        server->worker_processes = 1;
    }
#endif

    if (server->worker_processes == 0) {
        worker_proc(server);
    }
    else {
        // master-workers processes
        g_worker_processes_num = server->worker_processes;
        int bytes = g_worker_processes_num * sizeof(proc_ctx_t);
        g_worker_processes = (proc_ctx_t*)malloc(bytes);
        if (g_worker_processes == NULL) {
            perror("malloc");
            abort();
        }
        memset(g_worker_processes, 0, bytes);
        for (int i = 0; i < g_worker_processes_num; ++i) {
            proc_ctx_t* ctx = g_worker_processes + i;
            ctx->init = worker_init;
            ctx->init_userdata = NULL;
            ctx->proc = worker_proc;
            ctx->proc_userdata = server;
            spawn_proc(ctx);
        }
    }

    if (wait) {
        master_init(NULL);
        master_proc(NULL);
    }
    return 0;
}