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
/* Recommended max dic_cache and object sizes */

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

/* $begin sbuft */
typedef struct
{
    sem_t current_items;    /* Counts available current_items */
    short first_index;      /* buffer[(first_index+1)%n] is first item */
    short *buffer;          /* Buffer array */
    sem_t mutex_lock;       /* Protects accesses to buffer */
    short last_index;       /* buffer[last_index%n] is last item */
    short max_num_of_items; /* Maximum number of buffered_items */
    sem_t buffered_items;   /* Counts available buffered_items */
} s_buf;
/* $end sbuft */

typedef struct
{
    short last_index;       /* buffer[last_index%n] is last item */
    sem_t buffered_items;   /* Counts available buffered_items */
    char **buffer;          /* Buffer array */
    short max_num_of_items; /* Maximum number of buffered_items */
    sem_t current_items;    /* Counts available current_items */
    sem_t mutex_lock;       /* Protects accesses to buffer */
    short first_index;      /* buffer[(first_index+1)%n] is first item */
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
// void s_buf_in(s_buf *p, int n);
// void s_buf_dei(s_buf *p);
// void s_buf_i(s_buf *p, int item);
int s_buf_r(s_buf *p);
// void i_buf_l(l_buf *p, int n);
// void dein_buf_l(l_buf *p, int n);
// void inbuf_l(l_buf *p, char *item);
// char *r_buf_l(l_buf *p);

sem_t mutex_ind;      // initialized to 1; protects readerscount
s_buf s_buffer;       /* Shared buffer of connected descriptors */
short read_count = 0; // initialized to 0
dic_cache *h_cache;
l_buf l_buffer; /* Shared buffer of things to log */
short cache_size = 0;
sem_t m_cache; // initialized to 1; protects dic_cache

void sig_int_han(int sig_num)
{
    // Free all the memory that I've allocated
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
    // dein_buf_l(&l_buffer, SBUF_SIZE);
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

    ///////////////////////////////////////////
    // Read in request
    ///////////////////////////////////////////
    int socket_file_desc;
    struct sockaddr_in ip_addr;

    // Ignore SISPIPE signal
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
    // i_buf_l(&l_buffer, SBUF_SIZE);
    l_buffer.first_index = 0;
    l_buffer.last_index = 0; /* Empty buffer iff first_index == last_index */
    l_buffer.buffer = calloc(SBUF_SIZE, sizeof(char *));
    l_buffer.max_num_of_items = SBUF_SIZE;            /* Buffer holds max of n current_items */
    sem_init(&l_buffer.buffered_items, 0, SBUF_SIZE); /* Initially, buffer has n empty buffered_items */
    sem_init(&l_buffer.current_items, 0, 0);          /* Initially, buffer has zero data current_items */
    sem_init(&l_buffer.mutex_lock, 0, 1);             /* Binary semaphore for locking */
    int c_file_desc;
    // s_buf_in(&s_buffer, SBUF_SIZE);
    s_buffer.first_index = 0;
    s_buffer.last_index = 0; /* Empty buffer iff first_index == last_index */
    s_buffer.buffer = calloc(SBUF_SIZE, sizeof(int));
    s_buffer.max_num_of_items = SBUF_SIZE;            /* Buffer holds max of n current_items */
    sem_init(&s_buffer.buffered_items, 0, SBUF_SIZE); /* Initially, buffer has n empty buffered_items */
    sem_init(&s_buffer.current_items, 0, 0);          /* Initially, buffer has zero data current_items */
    sem_init(&s_buffer.mutex_lock, 0, 1);             /* Binary semaphore for locking */

    // Create logger thread
    pthread_create(&t_identification, NULL, threed, NULL);

    // Create worker threads
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
        s_buffer.buffer[(s_buffer.last_index) % (s_buffer.max_num_of_items)] = c_file_desc; /* Insert the item */
        sem_post(&s_buffer.mutex_lock);
        sem_post(&s_buffer.current_items);
        // s_buf_i(&s_buffer, c_file_desc); /* Insert cfd in buffer */
    }

    return 0;
}

void *threed(void *g)
{
    FILE *file_desc;
    pthread_detach(pthread_self());

    /* File pointer to hold reference of input file */

    /*  Open file in append mode. */

    // Have thread loop and write when it has logs available
    while (1)
    {
        // Collect log
        // char *logger = r_buf_l(&l_buffer);
        char *logger;

        sem_wait(&l_buffer.current_items);
        sem_wait(&l_buffer.mutex_lock);

        logger = l_buffer.buffer[(++l_buffer.first_index) % (l_buffer.max_num_of_items)]; /* Remove the item */
        sem_post(&l_buffer.mutex_lock);

        sem_post(&l_buffer.buffered_items);

        printf("logger thread: %s\n\n", logger);

        /* Append log to file */
        // file_desc = fopen("logFile.txt", "a");
        if ((file_desc = fopen("logFile.txt", "a")) == NULL)
        {
            /* Unable to open file hence exit */
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
    int c_file_desc = s_buf_r(&s_buffer); /* Remove connfd from buffer */ //line:conc:pre:removeconnfd

    int num_bytes_read, total_bytes_read_td = 0;
    char buffer[MAX_OBJECT_SIZE];
    memset(buffer, 0, MAX_OBJECT_SIZE);

    while (TRUE)
    {
        num_bytes_read = read(c_file_desc, buffer + total_bytes_read_td, MAX_OBJECT_SIZE);
        total_bytes_read_td += num_bytes_read;

        // Exit loop if reached end of request
        if (num_bytes_read == 0)
        {
            break;
        }
        // Re-loop if there is not enough data yet, and there is more
        if (total_bytes_read_td < 4)
        {
            continue;
        }

        // printf("\nbuffer: \n%s\n", buffer);
        if (!memcmp(buffer + total_bytes_read_td - 4, "\r\n\r\n", 4))
        {
            break;
        }
    }

    //////////////////////////////////////////////////////////////
    // Extract URL
    //////////////////////////////////////////////////////////////

    // This extracts 'GET' and copies it to the request we're building
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
    // This extracts the url
    strcpy(current_url, og_url);

    //This buffer is what is used to write to the client the response

    /////////////////////////////////////////////////////////////
    // Read from dic_cache
    /////////////////////////////////////////////////////////////

    sem_wait(&mutex_ind);

    if (read_count != TRUE)
    {
        sem_wait(&m_cache);
    }
    dic_cache *cache_node = h_cache;
    sem_post(&mutex_ind);
    ++read_count;

    // TODO: I think I'm getting my concurrency errors here
    // Check if the url is in the dic_cache.  If so, use the response
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

    ////////////////////////////////////////////////////////////////
    // Continue parsing
    // and build proxy request
    ////////////////////////////////////////////////////////////////

    char parse_string[PARSE_SIZE];
    strcpy(parse_string, current_url);
    char *r = parse_string;
    // If the website starts with https://, then strtok it again
    // to get the host
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

    // Add the directory to the request we're building
    // token = strtok(NULL, " ");
    short i = 0;
    short string_length = strlen(request_string);
    request_string[string_length + i] = ' ';
    i = i + 1;
    request_string[string_length + i] = '/';
    i = i + 1;
    strcpy(request_string + string_length + i, url_path);
    i = i - 1;
    // Add 'HTTP/1.0' to the request
    string_length = strlen(request_string);
    request_string[string_length] = ' ';
    strcpy(request_string + string_length + i, "HTTP/1.0\r\n");
    // Find the host line if it exists
    GET_token = strtok(NULL, "Host:");
    if (GET_token != NULL)
    {
        GET_token = strtok(NULL, " ");
        GET_token = strtok(NULL, "\r\n");
        strcpy(host_from_url, GET_token);
    }

    int request_len = strlen(request_string);
    // Add host name to the request
    strcpy(request_string + request_len, "Host: ");
    strcpy(request_string + request_len, host_from_url);
    strcpy(request_string + request_len, "\r\n");
    // Add the other things to the request
    strcpy(request_string + request_len, user_agent_hdr);
    strcpy(request_string + request_len, "Connection: close\r\n");
    strcpy(request_string + request_len, "Proxy-Connection: close\r\n");

    // Add on any additional request headers
    while ((GET_token = strtok(NULL, "\n")) != NULL)
    {
        // Don't add any of these
        if (strstr(GET_token, "Proxy-Connection:") != NULL || strstr(GET_token, "Connection:") != NULL)
            continue;

        request_len = strlen(request_string);

        strcpy(request_string + request_len, GET_token);
        strcpy(request_string + request_len, "\n");
    }
    // Terminate request
    strcpy(request_string + strlen(request_string), "\r\n");

    // printf("request:%s\n", request);

    /////////////////////////////////////////
    // Forward request to server
    /////////////////////////////////////////

    struct addrinfo *r_p;
    int ss;
    struct addrinfo address_hints;
    int file_desc;
    struct addrinfo *address_result;

    /* Obtain address(es) matching host/port */
    memset(&address_hints, 0, sizeof(struct addrinfo));
    address_hints.ai_protocol = 0;           /* Any protocol */
    address_hints.ai_socktype = SOCK_STREAM; /* TCP socket */
    address_hints.ai_flags = 0;
    address_hints.ai_family = AF_UNSPEC; /* Allow IPv4 or IPv6 */
    if (port_from_url == NULL || strstr(port_from_url, " ") != NULL)
    {
        strcpy(port_from_url, "80");
    }
    // printf("HOST: %s\n", host);
    // printf("DIRECTORY: %s\n", directory);
    // printf("PORT: %s\n", port);

    // If the host has been changed to have the port appended to it,
    // change it back to how it is without the port appended to it
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
    /* getaddrinfo() returns a list of address structures.
    Try each address until we successfully connect(2).
    If socket(2) (or connect(2)) fails, we (close the socket
    and) try the next address. */
    for (r_p = address_result; r_p != NULL; r_p = r_p->ai_next)
    {
        file_desc = socket(r_p->ai_family, r_p->ai_socktype, r_p->ai_protocol);
        if (file_desc == -1)
            continue;
        if (connect(file_desc, r_p->ai_addr, r_p->ai_addrlen) != -1)
            break; /* Success */
        close(file_desc);
    }
    if (r_p == NULL)
    { /* No address succeeded */
        fprintf(stderr, "Could not connect\n");
    }
    freeaddrinfo(address_result); /* No longer needed */

    // Write to server and read back response
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

    // printf("Return response:\n%s", outputBuffer);

    ////////////////////////////////////////////////////////
    // Add response to dic_cache (write)
    ////////////////////////////////////////////////////////

    sem_wait(&m_cache);

    // access and modify dic_cache
    printf("Adding to the dic_cache\n");
    dic_cache *ind = NULL;
    ind = malloc(sizeof(dic_cache));
    ind->node_value = calloc(MAX_OBJECT_SIZE, sizeof(char));
    ind->node_key = calloc(400, sizeof(char));
    memcpy(ind->node_value, out_buf, n_bytes_read);
    strcpy(ind->node_key, current_url);
    ind->size_of_node = n_bytes_read;
    // printf("ADING TO CACHE:\n");
    // for (int indx = 0; indx < n_bytes_read; indx++)
    // {
    //     printf("%c", ind->node_value[indx]);
    // }

    ind->next_node = h_cache;
    h_cache = ind;

    sem_post(&m_cache);

///////////////////////////////////////////
// Forward response back to client
///////////////////////////////////////////
f_resp:

    current_b_received = 0;
    b_sent = 0;
    do
    {
        current_b_received = write(c_file_desc, out_buf + b_sent, n_bytes_read - b_sent);
        if (current_b_received < 0)
        {
            // fprintf(stderr, "partial/failed write on line 263\n");
            return NULL;
        }
        b_sent += current_b_received;
    } while (b_sent < n_bytes_read);

    // Insert url and response to log file
    // inbuf_l(&l_buffer, current_url);
    sem_wait(&l_buffer.buffered_items);
    sem_wait(&l_buffer.mutex_lock);
    l_buffer.buffer[(++l_buffer.last_index) % (l_buffer.max_num_of_items)] = current_url; /* Insert the item */
    sem_post(&l_buffer.mutex_lock);
    sem_post(&l_buffer.current_items);

    close(c_file_desc);
    close(file_desc);

    free(out_buf);

    return 0;
}

/* Create an empty, bounded, shared FIFO buffer with n buffered_items */
/* $begin s_buf_init */
// void s_buf_in(s_buf *p, int num)
// {
//     p->buffer = calloc(num, sizeof(int));
//     p->max_num_of_items = num; /* Buffer holds max of n current_items */
//     p->first_index = 0;
//     p->last_index = 0;                    /* Empty buffer iff first_index == last_index */
//     sem_init(&p->buffered_items, 0, num); /* Initially, buffer has n empty buffered_items */
//     sem_init(&p->current_items, 0, 0);    /* Initially, buffer has zero data current_items */
//     sem_init(&p->mutex_lock, 0, 1);       /* Binary semaphore for locking */
// }
/* $end s_buf_init */

/* Clean up buffer sp */
/* $begin sbuf_deinit */
// void s_buf_dei(s_buf *p)
// {
//     free(p->buffer);
// }
/* $end sbuf_deinit */

/* Insert item onto the last_index of shared buffer p */
/* $begin sbuf_insert */
// void s_buf_i(s_buf *p, int item_to_insert)
// {
//     sem_wait(&p->buffered_items);
//     sem_wait(&p->mutex_lock);
//     p->buffer[(++p->last_index) % (p->max_num_of_items)] = item_to_insert; /* Insert the item */
//     sem_post(&p->mutex_lock);
//     sem_post(&p->current_items);
// }
/* $end sbuf_insert */

/* Remove and return the first item from buffer sp */
/* $begin sbuf_remove */
int s_buf_r(s_buf *p)
{
    int item;
    if (sem_wait(&p->current_items) < 0)
        printf("sem_wait error"); /* Wait for available item */
    if (sem_wait(&p->mutex_lock) < 0)
        printf("sem_wait error");                                 /* Lock the buffer */
    item = p->buffer[(++p->first_index) % (p->max_num_of_items)]; /* Remove the item */
    if (sem_post(&p->mutex_lock) < 0)
        printf("sem_post error"); /* Unlock the buffer */
    if (sem_post(&p->buffered_items) < 0)
        printf("sem_post error"); /* Announce available slot */
    return item;
}
/* $end sbuf_remove */

// void i_buf_l(l_buf *p, int num)
// {
//     p->buffer = calloc(num, sizeof(char *));
//     p->max_num_of_items = num;            /* Buffer holds max of n current_items */
//     p->first_index = p->last_index = 0;   /* Empty buffer iff first_index == last_index */
//     sem_init(&p->mutex_lock, 0, 1);       /* Binary semaphore for locking */
//     sem_init(&p->buffered_items, 0, num); /* Initially, buffer has n empty buffered_items */
//     sem_init(&p->current_items, 0, 0);    /* Initially, buffer has zero data current_items */
// }

// void dein_buf_l(l_buf *p, int num)
// {
//     for (int index = 0; index < num; index++)
//     {
//         free(p->buffer[index]);
//     }
//     free(p->buffer);
// }

// void inbuf_l(l_buf *p, char *item)
// {
//     if (sem_wait(&p->buffered_items) < 0)
//         printf("sem_wait error"); /* Wait for available slot */
//     if (sem_wait(&p->mutex_lock) < 0)
//         printf("sem_wait error");                                /* Lock the buffer */
//     p->buffer[(++p->last_index) % (p->max_num_of_items)] = item; /* Insert the item */
//     if (sem_post(&p->mutex_lock) < 0)
//         printf("sem_post error"); /* Unlock the buffer */
//     if (sem_post(&p->current_items) < 0)
//         printf("sem_post error"); /* Announce available item */
// }

// char *r_buf_l(l_buf *p)
// {
//     char *item_to_r;
//     if (sem_wait(&p->current_items) < 0)
//         printf("sem_wait error"); /* Wait for available item */
//     if (sem_wait(&p->mutex_lock) < 0)
//         printf("sem_wait error");                                      /* Lock the buffer */
//     item_to_r = p->buffer[(++p->first_index) % (p->max_num_of_items)]; /* Remove the item */
//     if (sem_post(&p->mutex_lock) < 0)
//         printf("sem_post error"); /* Unlock the buffer */
//     if (sem_post(&p->buffered_items) < 0)
//         printf("sem_post error"); /* Announce available slot */
//     return item_to_r;
// }