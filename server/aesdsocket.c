#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define _POSIX_C_SOURCE 200809L
#define SOCKET_PORT 9000
#define BUFFER_SIZE 1024

#ifdef USE_AESD_CHAR_DEVICE
#define FILE_PATH "/dev/aesdchar"
#else
#define FILE_PATH "/var/tmp/aesdsocketdata"
#endif

static const char *pidfile = "/var/run/aesdsocket.pid";

static void write_pidfile(void) {
    int fd = open(pidfile, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd >= 0) { dprintf(fd, "%d\n", getpid()); close(fd); }
}

// Structure to hold thread data
struct thread_data{
    pthread_t thread_id;
    bool thread_complete_success;
    char *file_path;
    int sock_client;
    pthread_mutex_t *mutex;
};

// Structure for linked list node to track threads
struct thread_node {
    struct thread_data *data;
    SLIST_ENTRY(thread_node) links;
};
SLIST_HEAD(node_head, thread_node);

static volatile bool is_terminated = false;

static void handle_signal(int signal)
{
    is_terminated = true;
}

static pthread_mutex_t mutex;

void * thread_func(void* thread_param) {
    struct thread_data *data = (struct thread_data *)thread_param;
    FILE *file = NULL;
    int socket_client = data->sock_client;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    /* Open a file to store the received data */
    file = fopen(FILE_PATH, "a+");
    if (file == NULL) {
        printf("Failed to open file\n");
        return NULL;
    }

    // Obtain the mutex
    if ( pthread_mutex_lock(data->mutex) ) {
        if (file) fclose(file);
        return NULL;
    }

    /* Receive data from the client and write to the file */
    while ((bytes_received = recv(socket_client, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes_received, file);
        /* Check for newline character to end reception */
        if (buffer[bytes_received - 1] == '\n') {
            break;
        }
    }

    fflush(file);
    if (pthread_mutex_unlock(data->mutex)) { fclose(file); return NULL; }

    /* Send the full content of the file back to the client */
    size_t send_bytes;

    /* Reset file pointer to the beginning */
    fseek(file, 0, SEEK_SET);
    while(!is_terminated) {
        size_t nread = fread(buffer, 1, sizeof(buffer), file);
        if (nread > 0) {
            send_bytes = 0;
            while (send_bytes < nread) {
                ssize_t sent = send(socket_client, buffer + send_bytes, nread - send_bytes, MSG_NOSIGNAL);
                if (sent < 0) {
                    if (errno == EINTR) {
                        continue; // Retry sending if interrupted
                    }
                    printf("Failed to send data to client\n");
                    return NULL;
                }
                send_bytes += sent;
            }
        }
        if (nread < sizeof(buffer)) {
            if (feof(file)) {
                break; // End of file
            } else if (ferror(file)) {
                printf("Failed to read from file\n");
                return NULL;
            }
        }
    }

    if (file) fclose(file);
    if (socket_client >= 0) close(socket_client);

    data->thread_complete_success = true;
    return NULL;
}

static int install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;

    if (sigaction(SIGINT,  &sa, NULL) == -1) return -16;
    if (sigaction(SIGTERM, &sa, NULL) == -1) return -17;
    return 0;
}

// /* The timer thread handler */
// static void timer_thread(union sigval sv) {
//     FILE *file = NULL;
//     char buffer[BUFFER_SIZE];

//     /* Open the file to add timestamp */
//     file = fopen(FILE_PATH, "a+");
//     if (file == NULL) {
//         printf("Failed to open file\n");
//         return;
//     }

//     // Obtain the mutex
//     if ( pthread_mutex_lock(&mutex) ) {
//         if (file) fclose(file);
//         return;
//     }

//     /* Format a timestamp string */
//     sprintf(buffer, "timestamp:%s", ctime(&(time_t){time(NULL)}));
//     /* Write the current time to the file */
//     fwrite(buffer, 1, strlen(buffer), file);
//     if (file) fclose(file);

//     if ( pthread_mutex_unlock(&mutex) ) {
//         return;
//     }

//     printf("%s", buffer);
// }

int main(int argc, char *argv[])
{
    /* Assignment 6 part 1 implementation */
    FILE *file = NULL;
    int ret = 0;

    openlog(NULL, 0, LOG_USER);

    /* Support -d option to run as daemon */
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); ret = -6; goto exit_socket_server; }
        if (pid > 0) _exit(0);

        if (setsid() < 0) { perror("setsid"); ret = -7; goto exit_socket_server; }

        pid = fork();
        if (pid < 0) { perror("fork2"); ret = -6; goto exit_socket_server; }
        if (pid > 0) _exit(0);

        umask(0);
        if (chdir("/") < 0) { perror("chdir"); ret = -8; goto exit_socket_server; }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        write_pidfile();
    }

    // /* Start the 10 seconds timer */
    // timer_t timerid;
    // struct sigevent sev;
    // struct itimerspec its, disarm = {0};
    // pthread_attr_t timer_attr;

    // pthread_attr_init(&timer_attr);
    // pthread_attr_setdetachstate(&timer_attr, PTHREAD_CREATE_DETACHED);

    // memset(&sev, 0, sizeof(sev));
    // sev.sigev_notify = SIGEV_THREAD;
    // sev.sigev_notify_function = timer_thread;
    // sev.sigev_notify_attributes = &timer_attr;
    // sev.sigev_value.sival_ptr = NULL;

    // if (timer_create(CLOCK_MONOTONIC, &sev, &timerid) == -1) {
    //     printf("Failed to create timer\n");
    //     ret = -1;
    //     goto exit_syslog;
    // }
    // pthread_attr_destroy(&timer_attr);

    // its.it_value.tv_sec = 10;
    // its.it_value.tv_nsec = 0;
    // its.it_interval.tv_sec = 10;
    // its.it_interval.tv_nsec = 0;

    // if (timer_settime(timerid, 0, &its, NULL) == -1) {
    //     printf("Failed to start timer\n");
    //     ret = -2;
    //     goto exit_syslog;
    // }

    /* Open a stream socket */
    int socket_server = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_server == -1) {
        printf("Failed to create socket\n");
        ret = -3;
        goto exit_syslog;
    }
    printf("Socket created successfully\n");

    /* Enable address reuse */
    int enabled = 1;
    if (setsockopt(socket_server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        ret = -4;
        goto exit_socket_server;
    }

    /* Bind the socket to port SOCKET_PORT */
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SOCKET_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(socket_server, (struct sockaddr *)&server_addr, sizeof(struct sockaddr));
    printf("Binding socket to port %d with ret=%d\n", SOCKET_PORT, ret);
    if (ret == -1) {
        printf("Failed to bind socket\n");
        ret = -5;
        goto exit_socket_server;
    }
    printf("Socket bound successfully\n");

    /* Setup signal handler */
    if (install_handlers() == -1) {
        printf("Failed to install signal handlers\n");
        ret = -9;
        goto exit_socket_server;
    }

    /* Listen for incoming connections */
    ret = listen(socket_server, 5);
    if (ret == -1) {
        printf("Failed to listen on socket\n");
        ret = -10;
        goto exit_socket_server;
    }
    printf("Socket listening successfully\n");

    // Setup the client socket
    int socket_client = -1;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Setup the thread mutex
    ret = pthread_mutex_init(&mutex, NULL);
    if (ret != 0) {
        printf("Failed to initialize mutex\n");
        ret = -11;
        goto exit_socket_server;
    }

    // Setup the linked list for threads
    struct node_head head = SLIST_HEAD_INITIALIZER(head);
    SLIST_INIT(&head);

    while(!is_terminated)
    {
        /* Accept a connection */
        socket_client = accept(socket_server, (struct sockaddr *)&client_addr, &client_addr_len);
        if (socket_client == -1) {
            if (is_terminated && (errno == EINTR || errno == EBADF)) {
                ret = 0;
                goto cleanup_threads;
            }
            printf("Failed to accept connection\n");
            ret = -12;
            goto cleanup_threads;
        }

        /* Create a new thread for each client connection */
        struct thread_data *data = malloc(sizeof(struct thread_data));
        if (data == NULL) {
            printf("Failed to allocate memory for thread data\n");
            ret = -13;
            goto cleanup_threads;
        }
        data->sock_client = socket_client;
        data->file_path = FILE_PATH;
        data->mutex = &mutex;
        data->thread_complete_success = false;
        ret = pthread_create(&data->thread_id, NULL, thread_func, data);
        if (ret != 0) {
            free(data);
            printf("Failed to create thread\n");
            ret = -14;
            goto cleanup_threads;
        }

        printf("Accepted connection from %d.%d.%d.%d\n",
               (client_addr.sin_addr.s_addr & 0xFF),
               (client_addr.sin_addr.s_addr >> 8) & 0xFF,
               (client_addr.sin_addr.s_addr >> 16) & 0xFF,
               (client_addr.sin_addr.s_addr >> 24) & 0xFF);
        syslog(LOG_INFO, "Accepted connection from %d.%d.%d.%d\n",
               (client_addr.sin_addr.s_addr & 0xFF),
               (client_addr.sin_addr.s_addr >> 8) & 0xFF,
               (client_addr.sin_addr.s_addr >> 16) & 0xFF,
               (client_addr.sin_addr.s_addr >> 24) & 0xFF);

        /* Add the thread to the linked list */
        struct thread_node *node = malloc(sizeof(struct thread_node));
        if (node == NULL) {
            printf("Failed to allocate memory for thread node\n");
            ret = -15;
            goto cleanup_threads;
        }
        node->data = data;
        SLIST_INSERT_HEAD(&head, node, links);

        /* Iterate the link list and delete the node with completed thread */
        struct thread_node *current = SLIST_FIRST(&head);
        struct thread_node *next;
        while (current != NULL) {
            next = SLIST_NEXT(current, links);
            if (current->data->thread_complete_success) {
                pthread_join(current->data->thread_id, NULL);
                free(current->data);
                SLIST_REMOVE(&head, current, thread_node, links);
                free(current);
            }
            current = next;
        }
    }

cleanup_threads:
{
    /* Join and free any remaining threads and notes */
    struct thread_node *current = SLIST_FIRST(&head);
    struct thread_node *next;
    while (current != NULL) {
        next = SLIST_NEXT(current, links);
        pthread_join(current->data->thread_id, NULL);
        free(current->data);
        SLIST_REMOVE(&head, current, thread_node, links);
        free(current);
        current = next;
    }
    pthread_mutex_destroy(&mutex);
}

    if (file) fclose(file);
    if (socket_client >= 0) close(socket_client);
exit_socket_server:
    if (socket_server >= 0) close(socket_server);
    // /* Delete the timer */
    // timer_settime(timerid, 0, &disarm, NULL);
    // timer_delete(timerid);
exit_syslog:
    closelog();
    remove(FILE_PATH);
    return ret;
}
