#ifndef _GNU_SOURCE
#define _GNU_SOURCE TRUE
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
#define MAX_EVENTS 128

#define TRUE 1
#define FALSE 0

#define NULL_CHAR '\0'

#define EWOULDBLOCK_OR_EAGAIN -1
#define OTHER_ERROR -2

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3";

struct event_state;

int handle_new_client(struct event_state *event_state_in);
int handle_receive_request(struct event_state *event_state);

struct request_info
{
    int valid_request;
    char *host_name;
    int port;
    char *path;
    char **header_array;
};

struct event_state
{
    int (*handler)(struct event_state *event_state);
    void *event_data;
    int fd;
};

struct proxy_state
{
    struct request_info *req_info;

    char *buffer;
    size_t buffer_pos;
    size_t buffer_length;
    size_t buffer_max_length;

    int client_fd;
    int server_fd;
};

int listen_fd;
int epoll_fd;

int epoll_cnt = 0;
int sig_int_sent = FALSE;

void sig_int_term_handler(int sig)
{
    sig_int_sent = TRUE;
}

void free_request_info(void *req_info)
{
    struct request_info *info = (struct request_info *) req_info;
    if (info->host_name)
    {
        Free(info->host_name);
    }
    if (info->path)
    {
        Free(info->path);
    }
    if (info->header_array)
    {
        Free(info->header_array);
    }
    Free(info);
}

void free_proxy_state(struct proxy_state *state)
{
    if (state->client_fd != 0)
    {
        close(state->client_fd);
    }
    if (state->server_fd != 0)
    {
        close(state->server_fd);
    }
    if (state->buffer != 0)
    {
        free(state->buffer);
    }
    if (state->req_info != 0)
    {
        free_request_info(state->req_info);
    }
    free(state);
}

int main(int argc, char **argv)
{
    signal(SIGINT, sig_int_term_handler);
    signal(SIGTERM, sig_int_term_handler);
    signal(SIGPIPE, SIG_IGN);

    struct epoll_event event;
    struct epoll_event *events;
    struct event_state *event_state;
    size_t max_num_events;

    printf("Start of proxy\n");
    printf("user_agent_hdr: %s\n", user_agent_hdr);

    listen_fd = Open_listenfd(argv[1]);

    // set fd to non-blocking (set flags while keeping existing flags)
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);
    epoll_fd = epoll_create1(0);
    
    event_state = malloc(sizeof(struct event_state));
    event_state->handler = handle_new_client;
    event_state->fd = listen_fd;
    event_state->event_data = NULL;

    event.data.ptr = event_state;
    event.events = EPOLLIN | EPOLLET; // edge-triggered monitoring
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);
    printf("listening on with fd: %d\n", listen_fd);
    epoll_cnt++;

    /* Buffer where events are returned */
    events = calloc(MAX_EVENTS, sizeof(event));

    while (epoll_cnt > 0)
    { //can't make timeout work. So waiting till no more events
        max_num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, EWOULDBLOCK_OR_EAGAIN);
        if (sig_int_sent)
        {
            if (listen_fd != EWOULDBLOCK_OR_EAGAIN)
            {
                close(listen_fd);
                listen_fd = EWOULDBLOCK_OR_EAGAIN;
                epoll_cnt--;
            }
        }
        if (max_num_events == 0 || max_num_events == EWOULDBLOCK_OR_EAGAIN)
        { // size_t is unsigned, so -1 isn't actually < 0. Rip
            continue;
        }

        for (int index = 0; index < max_num_events; index++)
        { //iterate through events and handle them
            struct event_state *temp_event_state = (struct event_state *)events[index].data.ptr;
            if (!temp_event_state->handler(temp_event_state))
            {
                if (temp_event_state->event_data != 0)
                {
                    free_proxy_state((struct proxy_state *)temp_event_state->event_data);
                }
                if (temp_event_state->fd != EWOULDBLOCK_OR_EAGAIN)
                {
                    close(temp_event_state->fd);
                }
                free(temp_event_state);
            }
        }
    }
    free(event_state);
    free(events);
}

int handle_new_client(struct event_state *event_state_in)
{
    socklen_t client_length;
    int conn_fd;
    struct sockaddr_storage client_addr;
    struct epoll_event event;
    struct proxy_state *proxy_state;
    struct event_state *event_state;

    client_length = sizeof(struct sockaddr_storage);

    // loop and get all available client connections
    while ((conn_fd = accept(event_state_in->fd, (struct sockaddr *)&client_addr, &client_length)) > 0)
    {

        // set fd to non-blocking (set flags while keeping existing flags)
        fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL, 0) | O_NONBLOCK);

        event_state = malloc(sizeof(struct event_state));
        event_state->handler = handle_receive_request;
        event_state->fd = conn_fd;
        proxy_state = malloc(sizeof(struct proxy_state));
        proxy_state->req_info = NULL;
        proxy_state->client_fd = conn_fd;
        proxy_state->server_fd = EWOULDBLOCK_OR_EAGAIN;
        proxy_state->buffer_max_length = 256;
        proxy_state->buffer_pos = 0;
        proxy_state->buffer = malloc(proxy_state->buffer_max_length);

        // add event to epoll file descriptor
        event_state->event_data = proxy_state;
        event.data.ptr = event_state;
        event.events = EPOLLIN | EPOLLET; // use edge-triggered monitoring
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &event);
        epoll_cnt++;
    }

    if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
        // no more clients to accept()
        return 1;
    }
    return 0;

}

int read_all_available(int fd, char **buf_out, size_t *buf_max_length, size_t *buf_pos, int reached_end)
{
    char *buf = *buf_out;
    int end = FALSE;

    int read_count;
    while ((read_count = read(fd, buf + *buf_pos, *buf_max_length - *buf_pos - 1)) > FALSE)
    {
        *buf_pos += read_count;

        if (*buf_pos == *buf_max_length - 1)
        {
            buf = Realloc(buf, *buf_max_length + 256);
            *buf_max_length += 256;
        }

        if (reached_end != 0 && *buf_pos >= 4 && strncmp("\r\n\r\n", buf + *buf_pos - 4, 4) == FALSE)
        {
            // The end has been reached!
            end = TRUE;
            break;
        }
    }

    *buf_out = buf;

    if (read_count == 0 || end != 0)
    {
        buf[*buf_pos] = NULL_CHAR;
        return *buf_pos;
    }
    else if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
        return EWOULDBLOCK_OR_EAGAIN;
    }
    else
    {
        return OTHER_ERROR;
    }
}

size_t parse_first_line(const char *input, struct request_info *info)
{
    memset(info, 0, sizeof(struct request_info));
    if (strncmp("GET", input, 3) != FALSE)
    {
        info->valid_request = FALSE;
        return EWOULDBLOCK_OR_EAGAIN;
    }

    const char *uri = input + 3; // skip GET

    while (isspace(*uri))
    {
        uri++;
    }

    if (strncmp("http://", uri, 7) != FALSE)
    {
        info->valid_request = FALSE;
        return EWOULDBLOCK_OR_EAGAIN;
    }

    const char *host_start_index = uri + 7; // skip http://
    const char *index = host_start_index;
    while (*index != ':' && *index != '/')
    {
        index++;
    }

    const char *host_end_index = index;
    if (*index != ':')
    {
        info->port = 80;
    }
    else
    {
        index++;
        const char *port_start_index = index;
        while (isdigit(*index))
        {
            index++;
        }
        info->port = atoi(port_start_index);
    }
    size_t host_length = host_end_index - host_start_index;
    info->host_name = Malloc(host_length + 1);
    memcpy(info->host_name, host_start_index, host_length);
    info->host_name[host_length] = NULL_CHAR;

    const char *next_whitespace_index = index;
    while (!isspace(*next_whitespace_index))
        next_whitespace_index++;

    size_t path_len = next_whitespace_index - index;
    info->path = Malloc(path_len + 1);
    memcpy(info->path, index, path_len);
    info->path[path_len] = NULL_CHAR;

    // Ensure protocol matches and eat crlf
    const char *protocol = next_whitespace_index;
    while (isspace(*protocol))
        protocol++;

    if (strncmp("HTTP/1.", protocol, 7) != FALSE)
    {
        info->valid_request = FALSE;
        return EWOULDBLOCK_OR_EAGAIN;
    }
    if (protocol[7] != '0' && protocol[7] != '1')
    {
        info->valid_request = FALSE;
        return EWOULDBLOCK_OR_EAGAIN;
    }

    const char *header_start_index = protocol + 10; // Skip HTTP/1.1\r\n
    info->valid_request = TRUE;

    return (size_t)(header_start_index - input);
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

                unsplit[0] = NULL_CHAR;
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

void on_response_sent(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

    epoll_cnt--;

    close(transaction->client_fd);
    close(transaction->server_fd);

    free_proxy_state(transaction);
    event_state->event_data = NULL;
}

int handle_send_response(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

    int write_count;
    while ((write_count = write(transaction->client_fd, transaction->buffer + transaction->buffer_pos, transaction->buffer_length - transaction->buffer_pos)) > FALSE)
    {
        transaction->buffer_pos += write_count;
    }

    if (write_count == 0)
    {
        on_response_sent(event_state);
        return 0;
    }
    else if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
        return 1;
    }
    else
    {
        // An error occured, so clean up nicely.
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, transaction->client_fd, NULL);
        epoll_cnt--;

        free_proxy_state(transaction);
        event_state->event_data = NULL;

        return 0;
    }
}

void on_response_received(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, transaction->server_fd, NULL);
    epoll_cnt--;

    // add event to epoll file descriptor
    struct epoll_event event;
    event.data.ptr = event_state;
    event.events = EPOLLOUT | EPOLLET; // use edge-triggered monitoring

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, transaction->client_fd, &event);
    epoll_cnt++;

    event_state->handler = handle_send_response;
    event_state->fd = transaction->client_fd;

    transaction->buffer_pos = 0;
}

int handle_receive_response(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

    int length = read_all_available(transaction->server_fd, &transaction->buffer, &transaction->buffer_max_length, &transaction->buffer_pos, 0);

    if (length == EWOULDBLOCK_OR_EAGAIN)
    {
        // read_all_available encountered EWOULDBLOCK or EAGAIN
        return 1;
    }
    else if (length == OTHER_ERROR)
    {
        // Some other error happened....
        // So close stuff nicely and keep on keepin' on!
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, transaction->server_fd, NULL);
        epoll_cnt--;
        return 0;
    }
    else
    {
        transaction->buffer_length = length;

        on_response_received(event_state);
        return 1;
    }
}

void on_request_sent(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;
    event_state->handler = handle_receive_response;

    // add event to epoll file descriptor
    struct epoll_event event;
    event.data.ptr = event_state;
    event.events = EPOLLIN | EPOLLET; // use edge-triggered monitoring
    event_state->fd = transaction->server_fd;

    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, transaction->server_fd, &event);

    Free(transaction->buffer);
    transaction->buffer_max_length = 256;
    transaction->buffer_pos = 0;
    transaction->buffer = Malloc(transaction->buffer_max_length);
}

int handle_send_request(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

    int write_count;
    while ((write_count = write(transaction->server_fd, transaction->buffer + transaction->buffer_pos, transaction->buffer_length - transaction->buffer_pos)) > FALSE)
    {
        transaction->buffer_pos += write_count;
    }

    if (write_count == 0)
    {
        on_request_sent(event_state);
        return 1;
    }
    return 0;

}

void prepare_request(char **header_array, int header_count, struct request_info *req_info, struct proxy_state *transaction)
{
    size_t headers_max_length = 256;
    char *headers_string = Malloc(headers_max_length);
    headers_string[0] = NULL_CHAR;
    size_t headers_len = 0;

    int needs_host = TRUE; 
    for (int index = 0; index < header_count; index++)
    {
        if (strncmp("Host", header_array[index], 4) == FALSE)
        {
            needs_host = FALSE;
        }
        else if (strncmp("User-Agent", header_array[index], 10) == FALSE ||
                 strncmp("Connection", header_array[index], 10) == FALSE ||
                 strncmp("Proxy-Connection", header_array[index], 16) == 0)  //todo: abtract this
        {
            continue;
        }

        headers_max_length = headers_len + strlen(header_array[index]) + 3;
        headers_string = Realloc(headers_string, headers_max_length);
        strcat(headers_string, header_array[index]);
        strcat(headers_string, "\r\n");
        headers_len = strlen(headers_string);
    }

    if (needs_host != 0)
    {
        headers_max_length = headers_len + strlen(req_info->host_name) + 9;
        headers_string = Realloc(headers_string, headers_max_length);
        strcat(headers_string, "Host: ");
        strcat(headers_string, req_info->host_name);
        strcat(headers_string, "\r\n");
    }

    char *output;
    int length = asprintf(&output, "GET %s HTTP/1.0\r\n%s\r\nUser-Agent: %s\r\nConnection: close\r\nProxy-Connection: close\r\n\r\n", req_info->path, headers_string, user_agent_hdr);
    Free(headers_string);

    Free(transaction->buffer);
    transaction->buffer_length = length;
    transaction->buffer_max_length = length;
    transaction->buffer_pos = 0;
    transaction->buffer = output;
}

int on_request_received(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

    transaction->req_info = Malloc(sizeof(struct request_info));
    size_t firstLineLength = parse_first_line(transaction->buffer, transaction->req_info);

    int headerCount = split_headers(transaction->buffer + firstLineLength, &transaction->req_info->header_array);
    prepare_request(transaction->req_info->header_array, headerCount, transaction->req_info, transaction);

    char *port = Malloc(7);
    snprintf(port, 6, "%d", transaction->req_info->port);
    int fd = open_clientfd(transaction->req_info->host_name, port);
    Free(port);

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    transaction->server_fd = fd;
    event_state->handler = handle_send_request;

    // add event to epoll file descriptor
    struct epoll_event event;
    event.data.ptr = event_state;
    event.events = EPOLLOUT | EPOLLET; // use edge-triggered monitoring
    event_state->fd = transaction->server_fd;

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, transaction->server_fd, &event);
    epoll_cnt++;

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, transaction->client_fd, NULL);
    epoll_cnt--;

    return 1;
}

int handle_receive_request(struct event_state *event_state)
{
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;
    int length = read_all_available(transaction->client_fd, &transaction->buffer, &transaction->buffer_max_length, &transaction->buffer_pos, 1);

    if (length == EWOULDBLOCK_OR_EAGAIN)
    {
        return 1;
    }
    else if (length == OTHER_ERROR)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, transaction->client_fd, NULL);
        epoll_cnt--;
        return 0;
    }
    else
    {
        transaction->buffer_length = length;

        int ret = on_request_received(event_state);
        if (ret == 0)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, transaction->client_fd, NULL);
            epoll_cnt--;
        }
        return ret;
    }
}