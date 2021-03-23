#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#define NUM_THREADS 20
#define SBUF_SIZE 15
#define TRUE 1
#define FALSE 0
#define PORT_SIZE 32
#define URL_SIZE 512
#define URL_PATH_SIZE 256
#define PARSE_SIZE 1024
/* Recommended max dic_cache and object sizes */
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

/* $begin sbuft */
typedef struct
{
    int *buffer;          /* Buffer array */
    int max_num_of_items; /* Maximum number of buffered_items */
    int first_index;      /* buffer[(first_index+1)%n] is first item */
    int last_index;       /* buffer[last_index%n] is last item */
    sem_t mutex_lock;     /* Protects accesses to buffer */
    sem_t buffered_items; /* Counts available buffered_items */
    sem_t current_items;  /* Counts available current_items */
} s_buf;
/* $end sbuft */

typedef struct
{
    char **buffer;        /* Buffer array */
    int max_num_of_items; /* Maximum number of buffered_items */
    int first_index;      /* buffer[(first_index+1)%n] is first item */
    int last_index;       /* buffer[last_index%n] is last item */
    sem_t mutex_lock;     /* Protects accesses to buffer */
    sem_t buffered_items; /* Counts available buffered_items */
    sem_t current_items;  /* Counts available current_items */
} l_buf;

typedef struct cache_node
{
    char *node_key;
    char *node_value;
    int size_of_node;
    struct cache_node *next_node;
} dic_cache;

void *threed(void *var_gp);
void *thread(void *var_gp);
void s_buf_in(s_buf *s_p, int n);
void s_buf_dei(s_buf *s_p);
void s_buf_i(s_buf *s_p, int item);
int s_buf_r(s_buf *s_p);
void i_buf_l(l_buf *s_p, int n);
void dein_buf_l(l_buf *s_p, int n);
void inbuf_l(l_buf *s_p, char *item);
char *r_buf_l(l_buf *s_p);

s_buf s_buffer; /* Shared buffer of connected descriptors */
l_buf l_buffer; /* Shared buffer of things to log */
dic_cache *h_cache;
sem_t m_cache;   // initialized to 1; protects dic_cache
sem_t mutex_ind; // initialized to 1; protects readerscount
int read_count;  // initialized to 0
int cache_size;

void sig_int_han(int sig_num)
{
    // Free all the memory that I've allocated
    dic_cache *n = h_cache;
    dic_cache *tmp_node;
    while (n->node_key != NULL)
    {
        free(n->node_key);
        free(n->node_value);
        tmp_node = n;
        n = n->next_node;
        free(tmp_node);
    }
    free(n);
    s_buf_dei(&s_buffer);
    dein_buf_l(&l_buffer, SBUF_SIZE);

    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sig_int_han);
    h_cache = malloc(sizeof(dic_cache));
    sem_init(&m_cache, 0, 1);
    sem_init(&mutex_ind, 0, 1);
    read_count = 0;
    cache_size = 0;

    ///////////////////////////////////////////
    // Read in request
    ///////////////////////////////////////////
    struct sockaddr_in ip_addr;
    int socket_file_desc;

    // Ignore SISPIPE signal
    signal(SIGPIPE, SIG_IGN);

    ip_addr.sin_family = AF_INET;
    printf("port: %s\n", argv[1]);
    ip_addr.sin_port = htons(atoi(argv[1]));
    ip_addr.sin_addr.s_addr = INADDR_ANY;
    if ((socket_file_desc = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket err");
        exit(0);
    }
    if (bind(socket_file_desc, (struct sockaddr *)&ip_addr, sizeof(struct sockaddr_in)) < 0)
    {
        close(socket_file_desc);
        perror("bind err");
        exit(0);
    }
    if (listen(socket_file_desc, 100) < 0)
    {
        close(socket_file_desc);
        perror("listen err");
        exit(0);
    }

    int c_file_desc, i;
    pthread_t t_identification;
    i_buf_l(&l_buffer, SBUF_SIZE);
    s_buf_in(&s_buffer, SBUF_SIZE);

    // Create logger thread
    pthread_create(&t_identification, NULL, threed, NULL);

    // Create worker threads
    for (i = 0; i < NUM_THREADS; i++)
        pthread_create(&t_identification, NULL, thread, NULL);

    for (;;)
    {
        struct sockaddr_storage peer_address;
        socklen_t peer_address_size;

        c_file_desc = accept(socket_file_desc, (struct sockaddr *)&peer_address, &peer_address_size);
        s_buf_i(&s_buffer, c_file_desc); /* Insert cfd in buffer */
    }

    return 0;
}

void *threed(void *var_gp)
{
    pthread_detach(pthread_self());

    /* File pointer to hold reference of input file */
    FILE *file_desc;

    /*  Open file in append mode. */

    // Have thread loop and write when it has logs available
    while (1)
    {
        // Collect log
        char *log = r_buf_l(&l_buffer);
        printf("logger thread: %s\n\n", log);

        /* Append log to file */
        file_desc = fopen("logFile.txt", "a");
        if (file_desc == NULL)
        {
            /* Unable to open file hence exit */
            printf("\nUnable to open file.\n");
            printf("Please check whether file exists and you have write privilege.\n");
            exit(0);
        }
        fputs(log, file_desc);
        fputs("\n", file_desc);
        fclose(file_desc);
        free(log);
    }
}

void *thread(void *var_gp)
{
    pthread_detach(pthread_self());
    int c_file_desc = s_buf_r(&s_buffer); /* Remove connfd from buffer */ //line:conc:pre:removeconnfd

    int num_bytes_read = 0;
    int total_bytes_read_td = 0;
    char buffer[MAX_OBJECT_SIZE];
    memset(buffer, 0, MAX_OBJECT_SIZE);

    while (TRUE)
    {
        num_bytes_read = read(c_file_desc, buffer + total_bytes_read_td, MAX_OBJECT_SIZE);
        total_bytes_read_td += num_bytes_read;

        // Exit loop if reached end of request
        if (num_bytes_read == 0)
            break;
        // Re-loop if there is not enough data yet, and there is more
        if (total_bytes_read_td < 4)
            continue;

        // printf("\nbuffer: \n%s\n", buffer);
        if (!memcmp(buffer + total_bytes_read_td - 4, "\r\n\r\n", 4))
        {
            break;
        }
    }

    char request_string[4000];

    //////////////////////////////////////////////////////////////
    // Extract URL
    //////////////////////////////////////////////////////////////

    // This extracts 'GET' and copies it to the request we're building
    char host_from_url[URL_SIZE];
    char url_path[URL_PATH_SIZE];
    char port_from_url[PORT_SIZE];
    char *GET_token = strtok(buffer, " ");
    strcpy(request_string, GET_token);
    // This extracts the url
    char *current_url = calloc(URL_SIZE, sizeof(char));
    char *og_url = strtok(NULL, " ");
    strcpy(current_url, og_url);

    //This buffer is what is used to write to the client the response
    char *out_buf = calloc(MAX_OBJECT_SIZE, sizeof(char));
    int n_bytes_read = 0;
    int in_cache = FALSE;

    /////////////////////////////////////////////////////////////
    // Read from dic_cache
    /////////////////////////////////////////////////////////////

    sem_wait(&mutex_ind);
    if (read_count == FALSE)
    {
        sem_wait(&m_cache);
    }
    ++read_count;
    sem_post(&mutex_ind);

    // TODO: I think I'm getting my concurrency errors here
    // Check if the url is in the dic_cache.  If so, use the response
    dic_cache *cache_node = h_cache;
    while (cache_node->node_key != NULL)
    {
        printf("dic_cache node_key: %s\n", cache_node->node_key);
        printf("url: %s\n", current_url);
        if (!strcmp(cache_node->node_key, current_url))
        {
            printf("FOUND A MATCH IN CACHE\n");
            n_bytes_read = cache_node->size_of_node;
            memcpy(out_buf, cache_node->node_value, n_bytes_read);
            in_cache = TRUE;
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

    if (in_cache == TRUE)
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

    char f[400];
    char host_plain[400];
    strcpy(f, GET_token);
    r = f;
    if (strstr(GET_token, ":"))
    {
        GET_token = strtok_r(r, "/", &r);
        strcpy(host_from_url, GET_token);
        GET_token = strtok_r(r, "\0", &r);
        strcpy(url_path, GET_token);

        char parse_host[400];
        strcpy(parse_host, host_from_url);
        r = parse_host;
        GET_token = strtok_r(r, ":", &r);
        strcpy(host_plain, GET_token);
        strcpy(port_from_url, r);
    }
    else
    {
        GET_token = strtok_r(r, "/", &r);
        strcpy(host_from_url, GET_token);
        strcpy(url_path, r);
        strcpy(port_from_url, "80");
    }

    if (port_from_url[strlen(port_from_url) - 1] == '/')
    {
        port_from_url[strlen(port_from_url) - 1] = '\0';
    }

    // Add the directory to the request we're building
    // token = strtok(NULL, " ");
    int string_length = strlen(request_string);
    request_string[string_length] = ' ';
    request_string[string_length + 1] = '/';
    strcpy(request_string + string_length + 2, url_path);
    // Add 'HTTP/1.0' to the request
    string_length = strlen(request_string);
    request_string[string_length] = ' ';
    strcpy(request_string + string_length + 1, "HTTP/1.0\r\n");
    // Find the host line if it exists
    GET_token = strtok(NULL, "Host:");
    if (GET_token != NULL)
    {
        GET_token = strtok(NULL, " ");
        GET_token = strtok(NULL, "\r\n");
        strcpy(host_from_url, GET_token);
    }
    // Add host name to the request
    strcpy(request_string + strlen(request_string), "Host: ");
    strcpy(request_string + strlen(request_string), host_from_url);
    strcpy(request_string + strlen(request_string), "\r\n");
    // Add the other things to the request
    strcpy(request_string + strlen(request_string), user_agent_hdr);
    strcpy(request_string + strlen(request_string), "Connection: close\r\n");
    strcpy(request_string + strlen(request_string), "Proxy-Connection: close\r\n");

    // Add on any additional request headers
    while ((GET_token = strtok(NULL, "\n")) != NULL)
    {
        // Don't add any of these
        if (strstr(GET_token, "Proxy-Connection:") != NULL || strstr(GET_token, "Connection:") != NULL)
            continue;

        strcpy(request_string + strlen(request_string), GET_token);
        strcpy(request_string + strlen(request_string), "\n");
    }
    // Terminate request
    strcpy(request_string + strlen(request_string), "\r\n");

    // printf("request:%s\n", request);

    /////////////////////////////////////////
    // Forward request to server
    /////////////////////////////////////////

    struct addrinfo address_hints;
    struct addrinfo *address_result, *r_p;
    int ss, file_desc;

    /* Obtain address(es) matching host/port */
    memset(&address_hints, 0, sizeof(struct addrinfo));
    address_hints.ai_family = AF_UNSPEC;     /* Allow IPv4 or IPv6 */
    address_hints.ai_socktype = SOCK_STREAM; /* TCP socket */
    address_hints.ai_flags = 0;
    address_hints.ai_protocol = 0; /* Any protocol */
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
    int b_sent = 0;
    int current_b_received = 0;
    do
    {
        current_b_received = write(file_desc, request_string + b_sent, n_bytes_read - b_sent);
        if (current_b_received < 0)
        {
            fprintf(stderr, "partial/failed write on line 231\n");
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
            perror("read");
            return NULL;
        }
        n_bytes_read += num_bytes_read;
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
    ind->node_key = calloc(400, sizeof(char));
    ind->node_value = calloc(MAX_OBJECT_SIZE, sizeof(char));
    strcpy(ind->node_key, current_url);
    memcpy(ind->node_value, out_buf, n_bytes_read);
    ind->size_of_node = n_bytes_read;
    printf("ADING TO CACHE:\n");
    for (int indx = 0; indx < n_bytes_read; indx++)
    {
        printf("%c", ind->node_value[indx]);
    }

    ind->next_node = h_cache;
    h_cache = ind;

    sem_post(&m_cache);

///////////////////////////////////////////
// Forward response back to client
///////////////////////////////////////////
f_resp:

    b_sent = 0;
    current_b_received = 0;
    do
    {
        current_b_received = write(c_file_desc, out_buf + b_sent, n_bytes_read - b_sent);
        if (current_b_received < 0)
        {
            fprintf(stderr, "partial/failed write on line 263\n");
            return NULL;
        }
        b_sent += current_b_received;
    } while (b_sent < n_bytes_read);

    // Insert url and response to log file
    inbuf_l(&l_buffer, current_url);

    close(file_desc);
    close(c_file_desc);
    free(out_buf);
    return 0;
}

/* Create an empty, bounded, shared FIFO buffer with n buffered_items */
/* $begin s_buf_init */
void s_buf_in(s_buf *s_p, int num)
{
    s_p->buffer = calloc(num, sizeof(int));
    s_p->max_num_of_items = num;            /* Buffer holds max of n current_items */
    s_p->first_index = s_p->last_index = 0; /* Empty buffer iff first_index == last_index */
    sem_init(&s_p->mutex_lock, 0, 1);       /* Binary semaphore for locking */
    sem_init(&s_p->buffered_items, 0, num); /* Initially, buffer has n empty buffered_items */
    sem_init(&s_p->current_items, 0, 0);    /* Initially, buffer has zero data current_items */
}
/* $end s_buf_init */

/* Clean up buffer sp */
/* $begin sbuf_deinit */
void s_buf_dei(s_buf *s_p)
{
    free(s_p->buffer);
}
/* $end sbuf_deinit */

/* Insert item onto the last_index of shared buffer s_p */
/* $begin sbuf_insert */
void s_buf_i(s_buf *s_p, int item_to_insert)
{
    if (sem_wait(&s_p->buffered_items) < 0)
        printf("sem_wait error"); /* Wait for available slot */
    if (sem_wait(&s_p->mutex_lock) < 0)
        printf("sem_wait error");                                                /* Lock the buffer */
    s_p->buffer[(++s_p->last_index) % (s_p->max_num_of_items)] = item_to_insert; /* Insert the item */
    if (sem_post(&s_p->mutex_lock) < 0)
        printf("sem_post error"); /* Unlock the buffer */
    if (sem_post(&s_p->current_items) < 0)
        printf("sem_post error"); /* Announce available item */
}
/* $end sbuf_insert */

/* Remove and return the first item from buffer sp */
/* $begin sbuf_remove */
int s_buf_r(s_buf *s_p)
{
    int item;
    if (sem_wait(&s_p->current_items) < 0)
        printf("sem_wait error"); /* Wait for available item */
    if (sem_wait(&s_p->mutex_lock) < 0)
        printf("sem_wait error");                                       /* Lock the buffer */
    item = s_p->buffer[(++s_p->first_index) % (s_p->max_num_of_items)]; /* Remove the item */
    if (sem_post(&s_p->mutex_lock) < 0)
        printf("sem_post error"); /* Unlock the buffer */
    if (sem_post(&s_p->buffered_items) < 0)
        printf("sem_post error"); /* Announce available slot */
    return item;
}
/* $end sbuf_remove */

void i_buf_l(l_buf *s_p, int num)
{
    s_p->buffer = calloc(num, sizeof(char *));
    s_p->max_num_of_items = num;            /* Buffer holds max of n current_items */
    s_p->first_index = s_p->last_index = 0; /* Empty buffer iff first_index == last_index */
    sem_init(&s_p->mutex_lock, 0, 1);       /* Binary semaphore for locking */
    sem_init(&s_p->buffered_items, 0, num); /* Initially, buffer has n empty buffered_items */
    sem_init(&s_p->current_items, 0, 0);    /* Initially, buffer has zero data current_items */
}

void dein_buf_l(l_buf *s_p, int num)
{
    for (int index = 0; index < num; index++)
    {
        free(s_p->buffer[index]);
    }
    free(s_p->buffer);
}

void inbuf_l(l_buf *s_p, char *item)
{
    if (sem_wait(&s_p->buffered_items) < 0)
        printf("sem_wait error"); /* Wait for available slot */
    if (sem_wait(&s_p->mutex_lock) < 0)
        printf("sem_wait error");                                      /* Lock the buffer */
    s_p->buffer[(++s_p->last_index) % (s_p->max_num_of_items)] = item; /* Insert the item */
    if (sem_post(&s_p->mutex_lock) < 0)
        printf("sem_post error"); /* Unlock the buffer */
    if (sem_post(&s_p->current_items) < 0)
        printf("sem_post error"); /* Announce available item */
}

char *r_buf_l(l_buf *s_p)
{
    char *item_to_r;
    if (sem_wait(&s_p->current_items) < 0)
        printf("sem_wait error"); /* Wait for available item */
    if (sem_wait(&s_p->mutex_lock) < 0)
        printf("sem_wait error");                                            /* Lock the buffer */
    item_to_r = s_p->buffer[(++s_p->first_index) % (s_p->max_num_of_items)]; /* Remove the item */
    if (sem_post(&s_p->mutex_lock) < 0)
        printf("sem_post error"); /* Unlock the buffer */
    if (sem_post(&s_p->buffered_items) < 0)
        printf("sem_post error"); /* Announce available slot */
    return item_to_r;
}