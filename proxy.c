#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdlib.h>
#include <pthread.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <string.h>
#include <stdio.h>

#define TRUE 1
#define URL_PATH_SIZE 256
#define NUM_THREADS 24
#define MAX_OBJECT_SIZE 102400
#define PORT_SIZE 32
#define FALSE 0
#define URL_SIZE 512
#define PARSE_SIZE 1024
#define SBUF_SIZE 16

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct
{
    sem_t current_items;
    short first_index;
    short *buffer;
    sem_t mutex_lock;
    short last_index;
    short max_num_of_items;
    sem_t buffered_items;
} s_buf;

typedef struct
{
    short last_index;
    sem_t buffered_items;
    char **buffer;
    short max_num_of_items;
    sem_t current_items;
    sem_t mutex_lock;
    short first_index;
} l_buf;

typedef struct cache_node
{
    char *node_key;
    char *node_value;
    short size_of_node;
    struct cache_node *next_node;
} dic_cache;

void *threed(void *g);
void *thread(void *g);
int s_buf_r(s_buf *p);

sem_t mutex_ind;
s_buf s_buffer;
short read_count = 0;
dic_cache *h_cache;
l_buf l_buffer;
short cache_size = 0;
sem_t m_cache;

void sig_int_han(int sig_num)
{
    dic_cache *tmp_node;
    dic_cache *n = h_cache;

    while (n->node_key != NULL)
    {
        tmp_node = n;

        free(n->node_value);
        free(n->node_key);
        free(tmp_node);

        n = n->next_node;
    }

    for (int index = 0; index < SBUF_SIZE; index++)
    {
        free(l_buffer.buffer[index]);
    }
    free(l_buffer.buffer);

    free(s_buffer.buffer);

    free(n);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sig_int_han);

    h_cache = malloc(sizeof(dic_cache));

    sem_init(&mutex_ind, 0, 1);
    sem_init(&m_cache, 0, 1);

    int socket_file_desc;
    struct sockaddr_in ip_addr;

    signal(SIGPIPE, SIG_IGN);

    ip_addr.sin_family = AF_INET;
    printf("port: %s\n", argv[1]);
    ip_addr.sin_addr.s_addr = INADDR_ANY;
    ip_addr.sin_port = htons(atoi(argv[1]));

    socket_file_desc = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_file_desc < 0)
    {
        perror("socket err");
        exit(0);
    }
    int bind_res = bind(socket_file_desc, (struct sockaddr *)&ip_addr, sizeof(struct sockaddr_in));
    if (bind_res < 0)
    {
        close(socket_file_desc);
        perror("bind err");
        exit(0);
    }
    int listen_res = listen(socket_file_desc, 100);
    if (listen_res < 0)
    {
        close(socket_file_desc);
        perror("listen err");
        exit(0);
    }

    pthread_t t_identification;
    int ind;

    l_buffer.first_index = 0;
    l_buffer.last_index = 0;
    l_buffer.buffer = calloc(SBUF_SIZE, sizeof(char *));
    l_buffer.max_num_of_items = SBUF_SIZE;
    sem_init(&l_buffer.buffered_items, 0, SBUF_SIZE);
    sem_init(&l_buffer.current_items, 0, 0);
    sem_init(&l_buffer.mutex_lock, 0, 1);
    int c_file_desc;

    s_buffer.first_index = 0;
    s_buffer.last_index = 0;
    s_buffer.buffer = calloc(SBUF_SIZE, sizeof(int));
    s_buffer.max_num_of_items = SBUF_SIZE;
    sem_init(&s_buffer.buffered_items, 0, SBUF_SIZE);
    sem_init(&s_buffer.current_items, 0, 0);
    sem_init(&s_buffer.mutex_lock, 0, 1);

    pthread_create(&t_identification, NULL, threed, NULL);

    for (ind = 0; ind < NUM_THREADS; ind++)
    {
        pthread_create(&t_identification, NULL, thread, NULL);
    }

    for (;;)
    {
        socklen_t peer_address_size;
        struct sockaddr_storage peer_address;

        c_file_desc = accept(socket_file_desc, (struct sockaddr *)&peer_address, &peer_address_size);

        sem_wait(&s_buffer.buffered_items);
        sem_wait(&s_buffer.mutex_lock);
        s_buffer.last_index += 1;
        s_buffer.buffer[(s_buffer.last_index) % (s_buffer.max_num_of_items)] = c_file_desc;
        sem_post(&s_buffer.mutex_lock);
        sem_post(&s_buffer.current_items);
    }

    return 0;
}

void *threed(void *g)
{
    FILE *file_desc;
    pthread_detach(pthread_self());

    while (1)
    {

        char *logger;

        sem_wait(&l_buffer.current_items);
        sem_wait(&l_buffer.mutex_lock);

        logger = l_buffer.buffer[(++l_buffer.first_index) % (l_buffer.max_num_of_items)];
        sem_post(&l_buffer.mutex_lock);

        sem_post(&l_buffer.buffered_items);

        printf("logger thread: %s\n\n", logger);

        if ((file_desc = fopen("logFile.txt", "a")) == NULL)
        {
            exit(0);
        }
        fputs(logger, file_desc);
        fputs("\n", file_desc);
        fclose(file_desc);
        free(logger);
    }
}

void *thread(void *g)
{
    pthread_detach(pthread_self());
    int c_file_desc = s_buf_r(&s_buffer);

    int num_bytes_read, total_bytes_read_td = 0;
    char buffer[MAX_OBJECT_SIZE];
    memset(buffer, 0, MAX_OBJECT_SIZE);

    while (TRUE)
    {
        num_bytes_read = read(c_file_desc, buffer + total_bytes_read_td, MAX_OBJECT_SIZE);
        total_bytes_read_td += num_bytes_read;

        if (num_bytes_read == 0)
        {
            break;
        }

        if (total_bytes_read_td < 4)
        {
            continue;
        }

        if (!memcmp(buffer + total_bytes_read_td - 4, "\r\n\r\n", 4))
        {
            break;
        }
    }

    char request_string[4000];
    char port_from_url[PORT_SIZE];
    char *GET_token = strtok(buffer, " ");
    char url_path[URL_PATH_SIZE];
    int n_bytes_read = 0;
    char *og_url = strtok(NULL, " ");
    int in_cache = FALSE;
    char host_from_url[URL_SIZE];
    char *current_url = calloc(URL_SIZE, sizeof(char));
    char *out_buf = calloc(MAX_OBJECT_SIZE, sizeof(char));
    strcpy(request_string, GET_token);
    strcpy(current_url, og_url);

    sem_wait(&mutex_ind);

    if (read_count != TRUE)
    {
        sem_wait(&m_cache);
    }
    dic_cache *cache_node = h_cache;
    sem_post(&mutex_ind);
    ++read_count;

    while (cache_node->node_key != NULL)
    {
        printf("dic_cache node_key: %s\n", cache_node->node_key);
        printf("url: %s\n", current_url);
        if (!strcmp(cache_node->node_key, current_url))
        {
            printf("FOUND A MATCH IN CACHE\n");
            in_cache = TRUE;
            n_bytes_read = cache_node->size_of_node;
            memcpy(out_buf, cache_node->node_value, n_bytes_read);
            break;
        }
        cache_node = cache_node->next_node;
    }

    sem_wait(&mutex_ind);
    --read_count;
    if (read_count == 0)
    {
        sem_post(&m_cache);
    }
    sem_post(&mutex_ind);

    if (in_cache != FALSE)
    {
        goto f_resp;
    }

    char parse_string[PARSE_SIZE];
    strcpy(parse_string, current_url);
    char *r = parse_string;
    GET_token = strtok_r(r, "/", &r);
    r = r + 1;
    if (strstr(GET_token, "http:") != NULL)
    {
        GET_token = strtok_r(r, " ", &r);
    }

    char host_plain[400];
    char f[400];
    strcpy(f, GET_token);
    r = f;
    if (strstr(GET_token, ":"))
    {
        char parse_host[400];
        GET_token = strtok_r(r, "/", &r);
        strcpy(host_from_url, GET_token);
        GET_token = strtok_r(r, "\0", &r);
        strcpy(url_path, GET_token);

        strcpy(parse_host, host_from_url);
        r = parse_host;
        GET_token = strtok_r(r, ":", &r);
        strcpy(port_from_url, r);
        strcpy(host_plain, GET_token);
    }
    else
    {
        GET_token = strtok_r(r, "/", &r);
        strcpy(url_path, r);
        strcpy(port_from_url, "80");
        strcpy(host_from_url, GET_token);
    }

    short tmp = strlen(port_from_url) - 1;
    if (port_from_url[tmp] == '/')
    {
        port_from_url[tmp] = '\0';
    }

    short i = 0;
    short string_length = strlen(request_string);
    request_string[string_length + i] = ' ';
    i = i + 1;
    request_string[string_length + i] = '/';
    i = i + 1;
    strcpy(request_string + string_length + i, url_path);
    i = i - 1;

    string_length = strlen(request_string);
    request_string[string_length] = ' ';
    strcpy(request_string + string_length + i, "HTTP/1.0\r\n");

    GET_token = strtok(NULL, "Host:");
    if (GET_token != NULL)
    {
        GET_token = strtok(NULL, " ");
        GET_token = strtok(NULL, "\r\n");
        strcpy(host_from_url, GET_token);
    }

    int request_len = strlen(request_string);

    strcpy(request_string + request_len, "Host: ");
    strcpy(request_string + request_len, host_from_url);
    strcpy(request_string + request_len, "\r\n");
    strcpy(request_string + request_len, user_agent_hdr);
    strcpy(request_string + request_len, "Connection: close\r\n");
    strcpy(request_string + request_len, "Proxy-Connection: close\r\n");

    while ((GET_token = strtok(NULL, "\n")) != NULL)
    {

        if (strstr(GET_token, "Proxy-Connection:") != NULL || strstr(GET_token, "Connection:") != NULL)
            continue;

        request_len = strlen(request_string);

        strcpy(request_string + request_len, GET_token);
        strcpy(request_string + request_len, "\n");
    }

    strcpy(request_string + strlen(request_string), "\r\n");

    struct addrinfo *r_p;
    int ss;
    struct addrinfo address_hints;
    int file_desc;
    struct addrinfo *address_result;

    memset(&address_hints, 0, sizeof(struct addrinfo));
    address_hints.ai_protocol = 0;
    address_hints.ai_socktype = SOCK_STREAM;
    address_hints.ai_flags = 0;
    address_hints.ai_family = AF_UNSPEC;
    if (port_from_url == NULL || strstr(port_from_url, " ") != NULL)
    {
        strcpy(port_from_url, "80");
    }

    if (host_plain != NULL && strstr(host_plain, " ") == NULL)
    {
        strcpy(host_from_url, host_plain);
    }
    ss = getaddrinfo(host_from_url, port_from_url, &address_hints, &address_result);
    if (ss != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ss));
        exit(1);
    }

    for (r_p = address_result; r_p != NULL; r_p = r_p->ai_next)
    {
        file_desc = socket(r_p->ai_family, r_p->ai_socktype, r_p->ai_protocol);
        if (file_desc == -1)
            continue;
        if (connect(file_desc, r_p->ai_addr, r_p->ai_addrlen) != -1)
            break;
        close(file_desc);
    }
    if (r_p == NULL)
    {
        fprintf(stderr, "Could not connect\n");
    }
    freeaddrinfo(address_result);

    n_bytes_read = strlen(request_string);
    int current_b_received = 0;
    int b_sent = 0;
    do
    {
        current_b_received = write(file_desc, request_string + b_sent, n_bytes_read - b_sent);
        if (current_b_received < 0)
        {
            return NULL;
        }
        b_sent += current_b_received;

    } while (b_sent < n_bytes_read);

    n_bytes_read = 0;

    do
    {
        num_bytes_read = read(file_desc, out_buf + n_bytes_read, MAX_OBJECT_SIZE);
        if (num_bytes_read == -1)
        {
            perror("READ");
            return NULL;
        }
        n_bytes_read = n_bytes_read + num_bytes_read;
    } while (num_bytes_read != 0);

    sem_wait(&m_cache);

    printf("Adding to the dic_cache\n");
    dic_cache *ind = NULL;
    ind = malloc(sizeof(dic_cache));
    ind->node_value = calloc(MAX_OBJECT_SIZE, sizeof(char));
    ind->node_key = calloc(400, sizeof(char));
    memcpy(ind->node_value, out_buf, n_bytes_read);
    strcpy(ind->node_key, current_url);
    ind->size_of_node = n_bytes_read;

    ind->next_node = h_cache;
    h_cache = ind;

    sem_post(&m_cache);

f_resp:

    current_b_received = 0;
    b_sent = 0;
    do
    {
        current_b_received = write(c_file_desc, out_buf + b_sent, n_bytes_read - b_sent);
        if (current_b_received < 0)
        {
            return NULL;
        }
        b_sent += current_b_received;
    } while (b_sent < n_bytes_read);

    sem_wait(&l_buffer.buffered_items);
    sem_wait(&l_buffer.mutex_lock);
    l_buffer.buffer[(++l_buffer.last_index) % (l_buffer.max_num_of_items)] = current_url;
    sem_post(&l_buffer.mutex_lock);
    sem_post(&l_buffer.current_items);

    close(c_file_desc);
    close(file_desc);

    free(out_buf);

    return 0;
}

int s_buf_r(s_buf *p)
{
    int item;
    if (sem_wait(&p->current_items) < 0)
        printf("sem_wait error");
    if (sem_wait(&p->mutex_lock) < 0)
        printf("sem_wait error");
    item = p->buffer[(++p->first_index) % (p->max_num_of_items)];
    if (sem_post(&p->mutex_lock) < 0)
        printf("sem_post error");
    if (sem_post(&p->buffered_items) < 0)
        printf("sem_post error");
    return item;
}
