#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3";

#define MAXEVENTS 64

struct event_action;

int handle_new_client(struct event_action *ea_in);
int handle_receive_request(struct event_action *ea);

typedef struct entry
{
    char *key;
    char *value;
    int size;
    struct entry *next;
} cache;

struct requestInfo
{
    int valid;
    char *host;
    int port;
    char *path;
    char **headers;
};

struct responseInfo
{
    size_t size;
    char *data;
};

struct event_action
{
    int (*handler)(struct event_action *ea);
    void *data;
    int fd;
};

struct proxy_transaction
{
    struct requestInfo *request;
    struct responseInfo *response;

    char *buf;
    size_t bufpos;
    size_t buflen;
    size_t bufmaxlen;

    int clientfd;
    int serverfd;
};

int lfd;
int epfd;

int epoll_cnt = 0;
int done = 0;

void sigint_handler(int signal)
{
    done = 1;
}

void free_requestInfo(void *info_v)
{
    struct requestInfo *info = (struct requestInfo *)info_v;
    if (info->host)
    {
        Free(info->host);
    }
    if (info->path)
    {
        Free(info->path);
    }
    if (info->headers)
    {
        Free(info->headers);
    }
    Free(info);
}

void free_proxyTransaction(struct proxy_transaction *transaction)
{
    if (transaction->clientfd)
    {
        close(transaction->clientfd);
    }
    if (transaction->serverfd)
    {
        close(transaction->serverfd);
    }
    if (transaction->buf)
    {
        free(transaction->buf);
    }
    if (transaction->request)
    {
        free_requestInfo(transaction->request);
    }
    free(transaction);
}

int main(int argc, char **argv)
{
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    //socklen_t clientLen;
    struct epoll_event event;
    struct epoll_event *events;
    int i;
    struct event_action *ea;
    size_t n;

    printf("Start of proxy\n");
    printf("user_agent_hdr: %s\n", user_agent_hdr);

    lfd = Open_listenfd(argv[1]);

    // set fd to non-blocking (set flags while keeping existing flags)
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL, 0) | O_NONBLOCK);
    epfd = epoll_create1(0);
    
    ea = malloc(sizeof(struct event_action));
    ea->handler = handle_new_client;
    ea->fd = lfd;
    ea->data = NULL;

    event.data.ptr = ea;
    event.events = EPOLLIN | EPOLLET; // edge-triggered monitoring
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &event);
    printf("listening on with fd: %d\n", lfd);
    epoll_cnt++;

    /* Buffer where events are returned */
    events = calloc(MAXEVENTS, sizeof(event));

    while (epoll_cnt > 0)
    { //can't make timeout work. So waiting till no more events
        n = epoll_wait(epfd, events, MAXEVENTS, -1);
        if (done)
        {
            if (lfd != -1)
            {
                close(lfd);
                lfd = -1;
                epoll_cnt--;
            }
        }
        if (n == 0 || n == -1)
        { // size_t is unsigned, so -1 isn't actually < 0. Rip
            continue;
        }

        for (i = 0; i < n; i++)
        { //iterate through events and handle them
            struct event_action *ea2 = (struct event_action *)events[i].data.ptr;
            if (!ea2->handler(ea2))
            {
                if (ea2->data)
                {
                    free_proxyTransaction((struct proxy_transaction *)ea2->data);
                }
                if (ea2->fd != -1)
                {
                    close(ea2->fd);
                }
                free(ea2);
            }
        }
    }
    free(ea);
    free(events);
}

int handle_new_client(struct event_action *ea_in)
{
    socklen_t clientLen;
    int connfd;
    struct sockaddr_storage clientaddr;
    struct epoll_event event;
    struct proxy_transaction *argptr;
    struct event_action *ea;

    clientLen = sizeof(struct sockaddr_storage);

    // loop and get all available client connections
    while ((connfd = accept(ea_in->fd, (struct sockaddr *)&clientaddr, &clientLen)) > 0)
    {

        // set fd to non-blocking (set flags while keeping existing flags)
        fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL, 0) | O_NONBLOCK);

        ea = malloc(sizeof(struct event_action));
        ea->handler = handle_receive_request;
        ea->fd = connfd;
        argptr = malloc(sizeof(struct proxy_transaction));
        argptr->request = NULL;
        argptr->response = NULL;
        argptr->clientfd = connfd;
        argptr->serverfd = -1;
        argptr->bufmaxlen = 256;
        argptr->bufpos = 0;
        argptr->buf = malloc(argptr->bufmaxlen);

        // add event to epoll file descriptor
        ea->data = argptr;
        event.data.ptr = ea;
        event.events = EPOLLIN | EPOLLET; // use edge-triggered monitoring
        epoll_ctl(epfd, EPOLL_CTL_ADD, connfd, &event);
        epoll_cnt++;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
        // no more clients to accept()
        return 1;
    }
    return 0;

}

int read_all_available(int fd, char **buf_out, size_t *bufmaxlen, size_t *bufpos, int done_after_rnrn)
{
    char *buf = *buf_out;
    int reached_rnrn = 0;

    int read_count;
    while ((read_count = read(fd, buf + *bufpos, *bufmaxlen - *bufpos - 1)) > 0)
    {
        *bufpos += read_count;

        if (*bufpos == *bufmaxlen - 1)
        {
            buf = Realloc(buf, *bufmaxlen + 256);
            *bufmaxlen += 256;
        }

        if (done_after_rnrn && *bufpos >= 4 && strncmp("\r\n\r\n", buf + *bufpos - 4, 4) == 0)
        {
            // The end has been reached!
            reached_rnrn = 1;
            break;
        }
    }

    *buf_out = buf;

    if (read_count == 0 || reached_rnrn)
    {
        buf[*bufpos] = '\0';
        return *bufpos;
    }
    else if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
        return -1;
    }
    else
    {
        return -2;
    }
}

size_t parse_first_line(const char *input, struct requestInfo *info)
{
    memset(info, 0, sizeof(struct requestInfo));
    if (strncmp("GET", input, 3) != 0)
    {
        info->valid = 0;
        return -1;
    }

    const char *uri = input + 3; // skip GET

    while (isspace(*uri))
    {
        uri++;
    }

    if (strncmp("http://", uri, 7) != 0)
    {
        info->valid = 0;
        return -1;
    }

    const char *host = uri + 7; // skip http://
    const char *i = host;
    while (*i != ':' && *i != '/')
    {
        i++;
    }

    const char *endHost = i;
    if (*i != ':')
    {
        info->port = 80;
    }
    else
    {
        i++;
        const char *portBegin = i;
        while (isdigit(*i))
        {
            i++;
        }
        info->port = atoi(portBegin);
    }
    size_t hostLen = endHost - host;
    info->host = Malloc(hostLen + 1);
    memcpy(info->host, host, hostLen);
    info->host[hostLen] = '\0';

    const char *nextSpace = i;
    while (!isspace(*nextSpace))
        nextSpace++;

    size_t pathLen = nextSpace - i;
    info->path = Malloc(pathLen + 1);
    memcpy(info->path, i, pathLen);
    info->path[pathLen] = '\0';

    // Ensure protocol matches and eat crlf
    const char *protocol = nextSpace;
    while (isspace(*protocol))
        protocol++;

    if (strncmp("HTTP/1.", protocol, 7) != 0)
    {
        info->valid = 0;
        return -1;
    }
    if (protocol[7] != '0' && protocol[7] != '1')
    {
        info->valid = 0;
        return -1;
    }

    const char *headerBegin = protocol + 10; // Skip HTTP/1.1\r\n
    info->valid = 1;

    return (size_t)(headerBegin - input);
}

// This throws away the last part of the string if it doesn't end in \r\n
// I think any requests should always end in \r\n, but that is an assumption I'm making
size_t split_headers(char *unsplit, char ***split_out)
{
    int arr_size = 10;
    char **split = Malloc(sizeof(char *) * arr_size);
    int count = 0;
    char *begin = unsplit;
    while ((unsplit = strchr(unsplit, '\r')))
    {
        // Not sure if this check is necessary, because
        // I don't think you can have \r without \n, but
        // better safe than sorry.
        if (unsplit[1] == '\n')
        {
            // This will make it skip empty lines
            if (unsplit != begin)
            {

                unsplit[0] = '\0';
                if (count == arr_size)
                {
                    split = Realloc(split, sizeof(char *) * (arr_size + 10));
                    arr_size += 10;
                }
                split[count] = begin;
                count++;
            }

            unsplit += 2;
            begin = unsplit;
        }
    }

    *split_out = split;

    return count;
}

void on_response_sent(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;

    epoll_cnt--;

    close(transaction->clientfd);
    close(transaction->serverfd);

    free_proxyTransaction(transaction);
    ea->data = NULL;
}

int handle_send_response(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;

    int write_count;
    while ((write_count = write(transaction->clientfd, transaction->buf + transaction->bufpos, transaction->buflen - transaction->bufpos)) > 0)
    {
        transaction->bufpos += write_count;
    }

    if (write_count == 0)
    {
        on_response_sent(ea);
        return 0;
    }
    else if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
        return 1;
    }
    else
    {
        // An error occured, so clean up nicely.
        epoll_ctl(epfd, EPOLL_CTL_DEL, transaction->clientfd, NULL);
        epoll_cnt--;

        free_proxyTransaction(transaction);
        ea->data = NULL;

        return 0;
    }
}

void on_response_received(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;

    epoll_ctl(epfd, EPOLL_CTL_DEL, transaction->serverfd, NULL);
    epoll_cnt--;

    // add event to epoll file descriptor
    struct epoll_event event;
    event.data.ptr = ea;
    event.events = EPOLLOUT | EPOLLET; // use edge-triggered monitoring

    epoll_ctl(epfd, EPOLL_CTL_ADD, transaction->clientfd, &event);
    epoll_cnt++;

    ea->handler = handle_send_response;
    ea->fd = transaction->clientfd;

    transaction->bufpos = 0;
}

int handle_receive_response(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;

    int len = read_all_available(transaction->serverfd, &transaction->buf, &transaction->bufmaxlen, &transaction->bufpos, 0);

    if (len == -1)
    {
        // read_all_available encountered EWOULDBLOCK or EAGAIN
        return 1;
    }
    else if (len == -2)
    {
        // Some other error happened....
        // So close stuff nicely and keep on keepin' on!
        epoll_ctl(epfd, EPOLL_CTL_DEL, transaction->serverfd, NULL);
        epoll_cnt--;
        return 0;
    }
    else
    {
        transaction->buflen = len;

        on_response_received(ea);
        return 1;
    }
}

void on_request_sent(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;
    ea->handler = handle_receive_response;

    // add event to epoll file descriptor
    struct epoll_event event;
    event.data.ptr = ea;
    event.events = EPOLLIN | EPOLLET; // use edge-triggered monitoring
    ea->fd = transaction->serverfd;

    epoll_ctl(epfd, EPOLL_CTL_MOD, transaction->serverfd, &event);

    Free(transaction->buf);
    transaction->bufmaxlen = 256;
    transaction->bufpos = 0;
    transaction->buf = Malloc(transaction->bufmaxlen);
}

int handle_send_request(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;

    int write_count;
    while ((write_count = write(transaction->serverfd, transaction->buf + transaction->bufpos, transaction->buflen - transaction->bufpos)) > 0)
    {
        transaction->bufpos += write_count;
    }

    if (write_count == 0)
    {
        on_request_sent(ea);
        return 1;
    }
    return 0;

}

void prepare_request(char **headers, int header_count, struct requestInfo *req, struct proxy_transaction *transaction)
{
    size_t headers_maxlen = 256;
    char *headers_str = Malloc(headers_maxlen);
    headers_str[0] = '\0';
    size_t headers_len = 0;

    int needsHost = 1;
    for (int i = 0; i < header_count; i++)
    {
        if (strncmp("Host", headers[i], 4) == 0)
        {
            needsHost = 0;
        }
        else if (strncmp("User-Agent", headers[i], 10) == 0 ||
                 strncmp("Connection", headers[i], 10) == 0 ||
                 strncmp("Proxy-Connection", headers[i], 16) == 0)  //todo: abtract this
        {
            continue;
        }

        headers_maxlen = headers_len + strlen(headers[i]) + 3;
        headers_str = Realloc(headers_str, headers_maxlen);
        strcat(headers_str, headers[i]);
        strcat(headers_str, "\r\n");
        headers_len = strlen(headers_str);
    }

    if (needsHost)
    {
        headers_maxlen = headers_len + strlen(req->host) + 9;
        headers_str = Realloc(headers_str, headers_maxlen);
        strcat(headers_str, "Host: ");
        strcat(headers_str, req->host);
        strcat(headers_str, "\r\n");
    }

    char *out;
    int len = asprintf(&out, "GET %s HTTP/1.0\r\n%s\r\nUser-Agent: %s\r\nConnection: close\r\nProxy-Connection: close\r\n\r\n", req->path, headers_str, user_agent_hdr);
    Free(headers_str);

    Free(transaction->buf);
    transaction->buflen = len;
    transaction->bufmaxlen = len;
    transaction->bufpos = 0;
    transaction->buf = out;
}

int on_request_received(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;

    transaction->request = Malloc(sizeof(struct requestInfo));
    size_t firstLineLength = parse_first_line(transaction->buf, transaction->request);

    int headerCount = split_headers(transaction->buf + firstLineLength, &transaction->request->headers);
    prepare_request(transaction->request->headers, headerCount, transaction->request, transaction);

    char *port = Malloc(7);
    snprintf(port, 6, "%d", transaction->request->port);
    int fd = open_clientfd(transaction->request->host, port);
    Free(port);

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    transaction->serverfd = fd;
    ea->handler = handle_send_request;

    // add event to epoll file descriptor
    struct epoll_event event;
    event.data.ptr = ea;
    event.events = EPOLLOUT | EPOLLET; // use edge-triggered monitoring
    ea->fd = transaction->serverfd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, transaction->serverfd, &event);
    epoll_cnt++;

    epoll_ctl(epfd, EPOLL_CTL_DEL, transaction->clientfd, NULL);
    epoll_cnt--;

    return 1;
}

int handle_receive_request(struct event_action *ea)
{
    struct proxy_transaction *transaction = (struct proxy_transaction *)ea->data;
    int len = read_all_available(transaction->clientfd, &transaction->buf, &transaction->bufmaxlen, &transaction->bufpos, 1);

    if (len == -1)
    {
        return 1;
    }
    else if (len == -2)
    {
        epoll_ctl(epfd, EPOLL_CTL_DEL, transaction->clientfd, NULL);
        epoll_cnt--;
        return 0;
    }
    else
    {
        transaction->buflen = len;

        int ret = on_request_received(ea);
        if (ret == 0)
        {
            epoll_ctl(epfd, EPOLL_CTL_DEL, transaction->clientfd, NULL);
            epoll_cnt--;
        }
        return ret;
    }
}