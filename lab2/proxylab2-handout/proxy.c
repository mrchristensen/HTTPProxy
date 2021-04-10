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

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_EVENTS 128
#define HEADER_SIZE 10
#define PROXY_SIZE 16
#define PORT_SIZE 6
#define BUFFER_LENGTH 256
#define TRUE 1
#define FALSE 0

#define INDEX 2

#define NULL_CHAR '\0'

#define TCP_PORT 80

#define EWOULDBLOCK_OR_EAGAIN -1
#define OTHER_ERROR -2

int listen_fd;
int epoll_fd;

int epoll_cnt = 0;
int sig_int_sent = FALSE;

static const char *user_agent_hdr = "Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3";

struct event_state;

int client_event(struct event_state *event_state_in);
int do_recv_req(struct event_state *event_state);

struct event_state
{
    int fd;

    void *event_data;

    int (*handler)(struct event_state *event_state);
};

struct proxy_state
{
    int server_fd;
    int client_fd;

    size_t buffer_max_length;
    size_t buffer_length;
    size_t buffer_pos;

    char *buffer;

    struct request_info *req_info;
};

struct request_info
{
    char **header_array;

    char *path;
    char *host_name;
    int port;

    int valid_request;
};

void free_info(struct request_info *req_info)
{
    if (req_info->header_array) Free(req_info->header_array);
    if (req_info->path) Free(req_info->path);
    if (req_info->host_name) Free(req_info->host_name);
    Free(req_info);

    return;
}

void free_state(struct proxy_state *state)
{
    if (state->req_info != 0) free_info(state->req_info);
    if (state->server_fd != 0) close(state->server_fd);
    if (state->buffer != 0) free(state->buffer);
    if (state->client_fd != 0) close(state->client_fd);
    free(state);

    return;
}

void sig_int_term_handler(int sig)
{
    sig_int_sent = TRUE;

    return;
}

int main(int argc, char **argv)
{
    printf("Start of main\n");

    size_t max_num_events;
    struct event_state *event_state;
    struct epoll_event *epoll_events;
    struct epoll_event epoll_event;

    signal(SIGPIPE, SIG_IGN);
    
    signal(SIGTERM, sig_int_term_handler);
    signal(SIGINT, sig_int_term_handler);

    listen_fd = Open_listenfd(argv[1]);
    
    printf("user_agent_hdr: %s\n", user_agent_hdr);
    printf("listen_fd: %d\n", listen_fd);

    epoll_fd = epoll_create1(0);

    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);

    event_state = malloc(sizeof(struct event_state));

    event_state->event_data = NULL;
    event_state->fd = listen_fd;
    event_state->handler = client_event;
    epoll_event.data.ptr = event_state;
    epoll_event.events = EPOLLIN | EPOLLET;

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &epoll_event);
    epoll_cnt++;

    epoll_events = calloc(MAX_EVENTS, sizeof(epoll_event));

    while (epoll_cnt > 0)
    {
        max_num_events = epoll_wait(epoll_fd, epoll_events, MAX_EVENTS, EWOULDBLOCK_OR_EAGAIN);

        if (sig_int_sent)
        {
            if (listen_fd != EWOULDBLOCK_OR_EAGAIN)
            {
                epoll_cnt--;
                listen_fd = EWOULDBLOCK_OR_EAGAIN;
                close(listen_fd);
            }
        }

        if (max_num_events == 0 || max_num_events == EWOULDBLOCK_OR_EAGAIN) continue;

        for (int index = 0; index < max_num_events; index++)
        {
            struct event_state *temp_event_state = (struct event_state *)epoll_events[index].data.ptr;
            
            if (!temp_event_state->handler(temp_event_state))
            {
                if (temp_event_state->fd != EWOULDBLOCK_OR_EAGAIN) close(temp_event_state->fd);
                if (temp_event_state->event_data != 0) free_state((struct proxy_state *)temp_event_state->event_data);
                
                free(temp_event_state);
            }
        }
    }
    free(epoll_events);
    free(event_state);
}

int read_possible_message(int fd, char **buffer_out, size_t *buffer_max_length, size_t *buffer_pos, int reached_end)
{
    char *buffer = *buffer_out;
    int end = FALSE;

    int read_count;
    while ((read_count = read(fd, buffer + *buffer_pos, *buffer_max_length - *buffer_pos - 1)) > FALSE)
    {
        *buffer_pos += read_count;

        if (*buffer_pos == *buffer_max_length - 1)
        {
            buffer = Realloc(buffer, *buffer_max_length + 256);
            *buffer_max_length += 256;
        }

        if (reached_end != 0 && *buffer_pos >= 4 && strncmp("\r\n\r\n", buffer + *buffer_pos - 4, 4) == FALSE)
        {
            end = TRUE;
            break;
        }
    }

    *buffer_out = buffer;

    if (read_count == 0 || end != 0)
    {
        buffer[*buffer_pos] = NULL_CHAR;
        return *buffer_pos;
    }

    return (errno == EWOULDBLOCK || errno == EAGAIN) ? EWOULDBLOCK_OR_EAGAIN : OTHER_ERROR;
}

int client_event(struct event_state *new_event_state)
{
    struct event_state *event_state;
    struct proxy_state *proxy_state;

    struct epoll_event event;
    struct sockaddr_storage client_addr;

    socklen_t client_length = sizeof(struct sockaddr_storage);
    int fd;

    while ((fd = accept(new_event_state->fd, (struct sockaddr *)&client_addr, &client_length)) > 0)
    {

        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        
        proxy_state = malloc(sizeof(struct proxy_state));
        proxy_state->buffer_max_length = BUFFER_LENGTH;
        proxy_state->buffer_pos = 0;
        proxy_state->server_fd = EWOULDBLOCK_OR_EAGAIN;
        proxy_state->buffer = malloc(proxy_state->buffer_max_length);
        proxy_state->client_fd = fd;

        event_state = malloc(sizeof(struct event_state));
        event_state->fd = fd;
        event_state->handler = do_recv_req;

        event_state->event_data = proxy_state;
        event.data.ptr = event_state;
        event.events = EPOLLIN | EPOLLET;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
        epoll_cnt++;
    }

    return (errno == EWOULDBLOCK || errno == EAGAIN) ? 1 : 0;
}

size_t parse_line(const char *input, struct request_info *info)
{
    const char *uri;
    const char *index;
    const char *host_start_index;
    const char *host_end_index;
    size_t host_length;
    const char *next_whitespace_index;
    const char *header_start_index;

    memset(info, 0, sizeof(struct request_info));
    if (strncmp("GET", input, 3) != FALSE)
    {
        info->valid_request = FALSE;
        return EWOULDBLOCK_OR_EAGAIN;
    }

    uri = input + 3;

    while (isspace(*uri)) uri++;

    if (strncmp("http://", uri, 7) != FALSE)
    {
        info->valid_request = FALSE;
        return EWOULDBLOCK_OR_EAGAIN;
    }

    host_start_index = uri + 7;
    index = host_start_index;

    while (*index != ':' && *index != '/') index++;

    host_end_index = index;
    
    if (*index != ':') info->port = TCP_PORT;
    else
    {
        index++;
        const char *port_start_index = index;
        while (isdigit(*index)) index++;
        info->port = atoi(port_start_index);
    }

    host_length = host_end_index - host_start_index;
    info->host_name = Malloc(host_length + 1);
    memcpy(info->host_name, host_start_index, host_length);
    info->host_name[host_length] = NULL_CHAR;

    next_whitespace_index = index;

    while (!isspace(*next_whitespace_index)) next_whitespace_index++;

    size_t path_len = next_whitespace_index - index;

    info->path = Malloc(path_len + 1);
    memcpy(info->path, index, path_len);

    info->path[path_len] = NULL_CHAR;

    const char *protocol = next_whitespace_index;
    while (isspace(*protocol)) protocol++;

    if ((strncmp("HTTP/1.", protocol, 7) != FALSE) || (protocol[7] != '0' && protocol[7] != '1'))
    {
        info->valid_request = FALSE;
        return EWOULDBLOCK_OR_EAGAIN;
    }

    header_start_index = protocol + HEADER_SIZE;
    info->valid_request = TRUE;

    return (size_t)(header_start_index - input);
}

int send_response_event(struct event_state *event_state)
{
    struct proxy_state *state = (struct proxy_state *)event_state->event_data;

    int write_count;
    while ((write_count = write(state->client_fd, state->buffer + state->buffer_pos, state->buffer_length - state->buffer_pos)) > FALSE) state->buffer_pos += write_count;
    
    if (write_count == 0)
    {
        struct proxy_state *state = (struct proxy_state *)event_state->event_data;

        event_state->event_data = NULL;

        close(state->server_fd);
        close(state->client_fd);

        free_state(state);

        epoll_cnt--;

        return 0;
    }
    else if (errno == EWOULDBLOCK || errno == EAGAIN) return 1;
    else
    {
        event_state->event_data = NULL;

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, state->client_fd, NULL);
        epoll_cnt--;

        free_state(state);

        return 0;
    }
}

int do_recv_resp(struct event_state *event_state)
{
    struct proxy_state *state = (struct proxy_state *)event_state->event_data;

    int length = read_possible_message(state->server_fd, &state->buffer, &state->buffer_max_length, &state->buffer_pos, 0);

    if (length == EWOULDBLOCK_OR_EAGAIN) return 1;
    else if (length == OTHER_ERROR)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, state->server_fd, NULL);
        epoll_cnt--;
        return 0;
    }
    else
    {
        state->buffer_length = length;

        struct proxy_state *state = (struct proxy_state *)event_state->event_data;

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, state->server_fd, NULL);
        epoll_cnt--;

        struct epoll_event event;
        event.data.ptr = event_state;
        event.events = EPOLLOUT | EPOLLET;

        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, state->client_fd, &event);
        epoll_cnt++;

        event_state->handler = send_response_event;
        event_state->fd = state->client_fd;

        state->buffer_pos = 0;
        return 1;
    }
}

int do_send_req(struct event_state *event_state)
{
    int write_count;
    struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

    while ((write_count = write(transaction->server_fd, transaction->buffer + transaction->buffer_pos, transaction->buffer_length - transaction->buffer_pos)) > FALSE) transaction->buffer_pos += write_count;

    if (write_count == 0)
    {
        struct epoll_event event;

        event_state->handler = do_recv_resp;

        struct proxy_state *state = (struct proxy_state *)event_state->event_data;

        event_state->fd = state->server_fd;

        event.events = EPOLLIN | EPOLLET;
        event.data.ptr = event_state;

        Free(state->buffer);

        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, state->server_fd, &event);

        state->buffer_max_length = BUFFER_LENGTH;
        state->buffer = Malloc(state->buffer_max_length);
        state->buffer_pos = 0;
        return 1;
    }
    return 0;
}

int do_recv_req(struct event_state *event_state)
{
    struct proxy_state *state = (struct proxy_state *)event_state->event_data;
    int length = read_possible_message(state->client_fd, &state->buffer, &state->buffer_max_length, &state->buffer_pos, 1);

    if (length == EWOULDBLOCK_OR_EAGAIN) return 1;
    else if (length == OTHER_ERROR)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, state->client_fd, NULL);
        epoll_cnt--;
        return 0;
    }
    else
    {
        state->buffer_length = length;

        do_recv_req(event_state);

        size_t headers_max_length = BUFFER_LENGTH;
        size_t headers_len = 0;
        struct proxy_state *transaction = (struct proxy_state *)event_state->event_data;

        transaction->req_info = Malloc(sizeof(struct request_info));
        size_t firstLineLength = parse_line(transaction->buffer, transaction->req_info);

        char *headers_string = Malloc(headers_max_length);
        headers_string[0] = NULL_CHAR;

        int header_count;

        char ***split_header = &transaction->req_info->header_array;

        int count = 0;
        int header_array_size = HEADER_SIZE;
        char **split = Malloc(sizeof(char *) * header_array_size);

        char *original_header = transaction->buffer + firstLineLength;
        char *begin = original_header;

        while ((original_header = strchr(original_header, '\r')))
        {
            if (original_header[1] == '\n')
            {
                if (original_header != begin)
                {
                    original_header[0] = NULL_CHAR;
                    if (count == header_array_size)
                    {
                        split = Realloc(split, sizeof(char *) * (header_array_size + HEADER_SIZE));
                        header_array_size += HEADER_SIZE;
                    }
                    split[count] = begin;
                    count++;
                }

                original_header += INDEX;

                begin = original_header;
            }
        }

        *split_header = split;

        header_count = count;

        int needs_host = TRUE;
        for (int index = 0; index < header_count; index++)
        {
            if (strncmp("Host", transaction->req_info->header_array[index], 4) == FALSE) needs_host = FALSE;
            else if (strncmp("User-Agent", transaction->req_info->header_array[index], HEADER_SIZE) == FALSE ||
                     strncmp("Connection", transaction->req_info->header_array[index], HEADER_SIZE) == FALSE ||
                     strncmp("Proxy-Connection", transaction->req_info->header_array[index], PROXY_SIZE) == 0)  continue;


            headers_max_length = headers_len + strlen(transaction->req_info->header_array[index]) + 3;
            headers_string = Realloc(headers_string, headers_max_length);

            strcat(headers_string, transaction->req_info->header_array[index]);
            strcat(headers_string, "\r\n");

            headers_len = strlen(headers_string);
        }

        if (needs_host == TRUE)
        {
            headers_max_length = headers_len + strlen(transaction->req_info->host_name) + 9;
            headers_string = Realloc(headers_string, headers_max_length);

            strcat(headers_string, "Host: ");
            strcat(headers_string, transaction->req_info->host_name);
            strcat(headers_string, "\r\n");
        }

        struct epoll_event event;

        char *output;
        int length = asprintf(&output, "GET %s HTTP/1.0\r\n%s\r\nUser-Agent: %s\r\nConnection: close\r\nProxy-Connection: close\r\n\r\n", transaction->req_info->path, headers_string, user_agent_hdr);
        Free(headers_string);

        Free(transaction->buffer);
        transaction->buffer_length = transaction->buffer_max_length = length;
        transaction->buffer_pos = 0;
        transaction->buffer = output;

        char *port = Malloc(7);
        snprintf(port, PORT_SIZE, "%d", transaction->req_info->port);
        int fd = open_clientfd(transaction->req_info->host_name, port);
        Free(port);

        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

        transaction->server_fd = fd;
        event_state->handler = do_send_req;

        event_state->fd = transaction->server_fd;
        event.events = EPOLLOUT | EPOLLET;
        event.data.ptr = event_state;

        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, transaction->server_fd, &event);
        epoll_cnt++;

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, transaction->client_fd, NULL);
        epoll_cnt--;

        return 1;

    }
}