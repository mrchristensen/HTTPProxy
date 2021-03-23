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

#define MAX_OBJECT_SIZE 102400

static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct
{
    int *buffer;
    int n;
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} s_buf_t;

typedef struct
{
    char **buffer;
    int n;
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} l_buf_t;

typedef struct node
{
    char *key;
    char *value;
    int size;
    struct node *next;
} dic_cache;

void *threed(void *var_gp);
void *thread(void *var_gp);
void sbuf_init(s_buf_t *sp, int n);
void sbuf_deinit(s_buf_t *sp);
void sbuf_insert(s_buf_t *sp, int item);
int sbuf_remove(s_buf_t *sp);
void logbuf_init(l_buf_t *sp, int n);
void logbuf_deinit(l_buf_t *sp, int n);
void logbuf_insert(l_buf_t *sp, char *item);
char *logbuf_remove(l_buf_t *sp);

s_buf_t sbuf;
l_buf_t logbuf;
dic_cache *head;
sem_t cache_mutex;
sem_t readers_mutex;
int readers_count;
int sizeOfCache;

void sigint_handler(int signum)
{

    dic_cache *node = head;
    dic_cache *nodeToFree;
    while (node->key != NULL)
    {
        free(node->key);
        free(node->value);
        nodeToFree = node;
        node = node->next;
        free(nodeToFree);
    }
    free(node);
    sbuf_deinit(&sbuf);
    logbuf_deinit(&logbuf, SBUF_SIZE);

    exit(0);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sigint_handler);
    head = malloc(sizeof(dic_cache));
    sem_init(&cache_mutex, 0, 1);
    sem_init(&readers_mutex, 0, 1);
    readers_count = 0;
    sizeOfCache = 0;

    struct sockaddr_in ip4addr;
    int sfd;

    signal(SIGPIPE, SIG_IGN);

    ip4addr.sin_family = AF_INET;
    printf("port: %s\n", argv[1]);
    ip4addr.sin_port = htons(atoi(argv[1]));
    ip4addr.sin_addr.s_addr = INADDR_ANY;
    if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket error");
        exit(EXIT_FAILURE);
    }
    if (bind(sfd, (struct sockaddr *)&ip4addr, sizeof(struct sockaddr_in)) < 0)
    {
        close(sfd);
        perror("bind error");
        exit(EXIT_FAILURE);
    }
    if (listen(sfd, 100) < 0)
    {
        close(sfd);
        perror("listen error");
        exit(EXIT_FAILURE);
    }

    int cfd, i;
    pthread_t tid;
    logbuf_init(&logbuf, SBUF_SIZE);
    sbuf_init(&sbuf, SBUF_SIZE);

    pthread_create(&tid, NULL, threed, NULL);

    for (i = 0; i < NUM_THREADS; i++)
        pthread_create(&tid, NULL, thread, NULL);

    while (1)
    {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addr_len;

        cfd = accept(sfd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        sbuf_insert(&sbuf, cfd);
    }

    return 0;
}

void *threed(void *var_gp)
{
    pthread_detach(pthread_self());

    FILE *fPtr;

    while (1)
    {

        char *log = logbuf_remove(&logbuf);
        printf("logger thread: %s\n\n", log);

        fPtr = fopen("logFile.txt", "a");
        if (fPtr == NULL)
        {

            printf("\nUnable to open file.\n");
            printf("Please check whether file exists and you have write privilege.\n");
            exit(EXIT_FAILURE);
        }
        fputs(log, fPtr);
        fputs("\n", fPtr);
        fclose(fPtr);
        free(log);
    }
}

void *thread(void *var_gp)
{

    pthread_detach(pthread_self());
    int cfd = sbuf_remove(&sbuf);

    int nread = 0;
    int total_bytes_read = 0;
    char buffer[MAX_OBJECT_SIZE];
    memset(buffer, 0, MAX_OBJECT_SIZE);

    for (;;)
    {
        nread = read(cfd, buffer + total_bytes_read, MAX_OBJECT_SIZE);
        total_bytes_read += nread;

        if (nread == 0)
            break;

        if (total_bytes_read < 4)
            continue;

        if (!memcmp(buffer + total_bytes_read - 4, "\r\n\r\n", 4))
        {
            break;
        }
    }

    char request[4000];

    char host[400];
    char directory[400];
    char port[40];
    char *token = strtok(buffer, " ");
    strcpy(request, token);

    char *url = calloc(400, sizeof(char));
    char *preURL = strtok(NULL, " ");
    strcpy(url, preURL);

    char *outputBuffer = calloc(MAX_OBJECT_SIZE, sizeof(char));
    int bytes_read = 0;
    int cacheHit = 0;

    sem_wait(&readers_mutex);
    if (readers_count == 0)
    {
        sem_wait(&cache_mutex);
    }
    ++readers_count;
    sem_post(&readers_mutex);

    dic_cache *node = head;
    while (node->key != NULL)
    {
        printf("dic_cache key: %s\n", node->key);
        printf("url: %s\n", url);
        if (!strcmp(node->key, url))
        {
            printf("FOUND A MATCH IN CACHE\n");
            bytes_read = node->size;
            memcpy(outputBuffer, node->value, bytes_read);
            cacheHit = 1;
            break;
        }
        node = node->next;
    }

    sem_wait(&readers_mutex);
    --readers_count;
    if (readers_count == 0)
    {
        sem_post(&cache_mutex);
    }
    sem_post(&readers_mutex);

    if (cacheHit)
    {
        goto forwardResponse;
    }

    char strToParse[800];
    strcpy(strToParse, url);
    char *rest = strToParse;

    token = strtok_r(rest, "/", &rest);
    rest = rest + 1;
    if (strstr(token, "http:") != NULL)
    {
        token = strtok_r(rest, " ", &rest);
    }

    char firstAttempt[400];
    char plainHost[400];
    strcpy(firstAttempt, token);
    rest = firstAttempt;
    if (strstr(token, ":"))
    {
        token = strtok_r(rest, "/", &rest);
        strcpy(host, token);
        token = strtok_r(rest, "\0", &rest);
        strcpy(directory, token);

        char hostToParse[400];
        strcpy(hostToParse, host);
        rest = hostToParse;
        token = strtok_r(rest, ":", &rest);
        strcpy(plainHost, token);
        strcpy(port, rest);
    }
    else
    {
        token = strtok_r(rest, "/", &rest);
        strcpy(host, token);
        strcpy(directory, rest);
        strcpy(port, "80");
    }

    if (port[strlen(port) - 1] == '/')
    {
        port[strlen(port) - 1] = '\0';
    }

    int length = strlen(request);
    request[length] = ' ';
    request[length + 1] = '/';
    strcpy(request + length + 2, directory);

    length = strlen(request);
    request[length] = ' ';
    strcpy(request + length + 1, "HTTP/1.0\r\n");

    token = strtok(NULL, "Host:");
    if (token != NULL)
    {
        token = strtok(NULL, " ");
        token = strtok(NULL, "\r\n");
        strcpy(host, token);
    }

    strcpy(request + strlen(request), "Host: ");
    strcpy(request + strlen(request), host);
    strcpy(request + strlen(request), "\r\n");

    strcpy(request + strlen(request), user_agent_hdr);
    strcpy(request + strlen(request), "Connection: close\r\n");
    strcpy(request + strlen(request), "Proxy-Connection: close\r\n");

    while ((token = strtok(NULL, "\n")) != NULL)
    {

        if (strstr(token, "Proxy-Connection:") != NULL || strstr(token, "Connection:") != NULL)
            continue;

        strcpy(request + strlen(request), token);
        strcpy(request + strlen(request), "\n");
    }

    strcpy(request + strlen(request), "\r\n");

    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s, fd;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;
    if (port == NULL || strstr(port, " ") != NULL)
    {
        strcpy(port, "80");
    }

    if (plainHost != NULL && strstr(plainHost, " ") == NULL)
    {
        strcpy(host, plainHost);
    }
    s = getaddrinfo(host, port, &hints, &result);
    if (s != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        exit(EXIT_FAILURE);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1)
            break;
        close(fd);
    }
    if (rp == NULL)
    {
        fprintf(stderr, "Could not connect\n");
    }
    freeaddrinfo(result);

    bytes_read = strlen(request);
    int bytes_sent = 0;
    int current_bytes_received = 0;
    do
    {
        current_bytes_received = write(fd, request + bytes_sent, bytes_read - bytes_sent);
        if (current_bytes_received < 0)
        {
            fprintf(stderr, "partial/failed write on line 231\n");
            return NULL;
        }
        bytes_sent += current_bytes_received;

    } while (bytes_sent < bytes_read);

    bytes_read = 0;

    do
    {
        nread = read(fd, outputBuffer + bytes_read, MAX_OBJECT_SIZE);
        if (nread == -1)
        {
            perror("read");
            return NULL;
        }
        bytes_read += nread;
    } while (nread != 0);

    sem_wait(&cache_mutex);

    printf("Adding to the dic_cache\n");
    dic_cache *newHead = NULL;
    newHead = malloc(sizeof(dic_cache));
    newHead->key = calloc(400, sizeof(char));
    newHead->value = calloc(MAX_OBJECT_SIZE, sizeof(char));
    strcpy(newHead->key, url);
    memcpy(newHead->value, outputBuffer, bytes_read);
    newHead->size = bytes_read;
    printf("ADING TO CACHE:\n");
    for (int i = 0; i < bytes_read; i++)
    {
        printf("%c", newHead->value[i]);
    }

    newHead->next = head;
    head = newHead;

    sem_post(&cache_mutex);

forwardResponse:

    bytes_sent = 0;
    current_bytes_received = 0;
    do
    {
        current_bytes_received = write(cfd, outputBuffer + bytes_sent, bytes_read - bytes_sent);
        if (current_bytes_received < 0)
        {
            fprintf(stderr, "partial/failed write on line 263\n");
            return NULL;
        }
        bytes_sent += current_bytes_received;
    } while (bytes_sent < bytes_read);

    logbuf_insert(&logbuf, url);

    close(fd);
    close(cfd);
    free(outputBuffer);
    return 0;
}

void sbuf_init(s_buf_t *sp, int n)
{
    sp->buffer = calloc(n, sizeof(int));
    sp->n = n;
    sp->front = sp->rear = 0;
    sem_init(&sp->mutex, 0, 1);
    sem_init(&sp->slots, 0, n);
    sem_init(&sp->items, 0, 0);
}

void sbuf_deinit(s_buf_t *sp)
{
    free(sp->buffer);
}

void sbuf_insert(s_buf_t *sp, int item)
{
    if (sem_wait(&sp->slots) < 0)
        printf("sem_wait error");
    if (sem_wait(&sp->mutex) < 0)
        printf("sem_wait error");
    sp->buffer[(++sp->rear) % (sp->n)] = item;
    if (sem_post(&sp->mutex) < 0)
        printf("sem_post error");
    if (sem_post(&sp->items) < 0)
        printf("sem_post error");
}

int sbuf_remove(s_buf_t *sp)
{
    int item;
    if (sem_wait(&sp->items) < 0)
        printf("sem_wait error");
    if (sem_wait(&sp->mutex) < 0)
        printf("sem_wait error");
    item = sp->buffer[(++sp->front) % (sp->n)];
    if (sem_post(&sp->mutex) < 0)
        printf("sem_post error");
    if (sem_post(&sp->slots) < 0)
        printf("sem_post error");
    return item;
}

void logbuf_init(l_buf_t *sp, int n)
{
    sp->buffer = calloc(n, sizeof(char *));
    sp->n = n;
    sp->front = sp->rear = 0;
    sem_init(&sp->mutex, 0, 1);
    sem_init(&sp->slots, 0, n);
    sem_init(&sp->items, 0, 0);
}

void logbuf_deinit(l_buf_t *sp, int n)
{
    for (int i = 0; i < n; i++)
    {
        free(sp->buffer[i]);
    }
    free(sp->buffer);
}

void logbuf_insert(l_buf_t *sp, char *item)
{
    if (sem_wait(&sp->slots) < 0)
        printf("sem_wait error");
    if (sem_wait(&sp->mutex) < 0)
        printf("sem_wait error");
    sp->buffer[(++sp->rear) % (sp->n)] = item;
    if (sem_post(&sp->mutex) < 0)
        printf("sem_post error");
    if (sem_post(&sp->items) < 0)
        printf("sem_post error");
}

char *logbuf_remove(l_buf_t *sp)
{
    char *item;
    if (sem_wait(&sp->items) < 0)
        printf("sem_wait error");
    if (sem_wait(&sp->mutex) < 0)
        printf("sem_wait error");
    item = sp->buffer[(++sp->front) % (sp->n)];
    if (sem_post(&sp->mutex) < 0)
        printf("sem_post error");
    if (sem_post(&sp->slots) < 0)
        printf("sem_post error");
    return item;
}