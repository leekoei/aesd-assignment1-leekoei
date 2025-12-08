#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define _POSIX_C_SOURCE 200809L
#define SOCKET_PORT 9000
#define BUFFER_SIZE 1024
#define FILE_PATH "/var/tmp/aesdsocketdata"
    
static volatile bool is_terminated = false;

static void handle_signal(int signal)
{
    is_terminated = true;
}

static int install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    // sigemptyset(&sa.sa_mask);
    // sa.sa_flags = SA_RESTART;    // or 0 if you prefer EINTR on blocking calls

    if (sigaction(SIGINT,  &sa, NULL) == -1) return -1;
    if (sigaction(SIGTERM, &sa, NULL) == -1) return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    /**
     * Assignment 5 part 1 implementation
     */
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    FILE *file = NULL;
    int ret = 0;

    openlog(NULL, 0, LOG_USER);

    /* Open a stream socket */
    int socket_server = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_server == -1) {
        printf("Failed to create socket\n");
        ret = -1;
        goto exit_syslog;
    }
    printf("Socket created successfully\n");

    /* Enable address reuse */
    int enabled = 1;
    if (setsockopt(socket_server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        ret = -1;
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
        ret = -1;
        goto exit_socket_server;
    }
    printf("Socket bound successfully\n");

    /* Support -d option to run as daemon */
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        /* Fork off the parent process */
        pid_t pid = fork();
        if (pid < 0) {
            printf("Failed to fork process\n");
            ret = -1;
            goto exit_socket_server;
        }
        /* If we got a good PID, then we can exit the parent process. */
        if (pid > 0) {
            exit(0);
        }
        /* Change the file mode mask */
        umask(0);
        /* Create a new SID for the child process */
        if (setsid() < 0) {
            printf("Failed to create new SID\n");
            ret = -1;
            goto exit_socket_server;
        }
        /* Change the current working directory */
        if (chdir("/") < 0) {
            printf("Failed to change directory to /\n");
            ret = -1;
            goto exit_socket_server;
        }
        /* Close out the standard file descriptors */
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }

    /* Setup signal handler */
    if (install_handlers() == -1) {
        printf("Failed to install signal handlers\n");
        ret = -4;
        goto exit_socket_server;
    }

    /* Listen for incoming connections */
    ret = listen(socket_server, 5);
    if (ret == -1) {
        printf("Failed to listen on socket\n");
        ret = -1;
        goto exit_socket_server;
    }
    printf("Socket listening successfully\n");

    int socket_client;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    while(!is_terminated)
    {
        /* Accept a connection */
        socket_client = accept(socket_server, (struct sockaddr *)&client_addr, &client_addr_len);
        if (socket_client == -1) {
            printf("Failed to accept connection\n");
            ret = -2;
            goto exit_socket_server;
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

        /* Open a file to store the received data */
        file = fopen(FILE_PATH, "a+");
        if (file == NULL) {
            printf("Failed to open file\n");
            ret = -3;
            goto exit_client_socket;
        }
        
        /* Receive data from the client and write to the file */
        while ((bytes_received = recv(socket_client, buffer, sizeof(buffer), 0)) > 0) {
            fwrite(buffer, 1, bytes_received, file);
            /* Check for newline character to end reception */
            if (buffer[bytes_received - 1] == '\n') {
                break;
            }
        }
        printf("Received %zd bytes from client\n", bytes_received);

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
                        ret = -5;
                        goto exit_file;
                    }
                    send_bytes += sent;
                }
            }
            if (nread < sizeof(buffer)) {
                if (feof(file)) {
                    break; // End of file
                } else if (ferror(file)) {
                    printf("Failed to read from file\n");
                    ret = -4;
                    goto exit_file;
                }
            }
        }

        syslog(LOG_INFO, "Closed connection from %d.%d.%d.%d\n",
               (client_addr.sin_addr.s_addr & 0xFF),
               (client_addr.sin_addr.s_addr >> 8) & 0xFF,
               (client_addr.sin_addr.s_addr >> 16) & 0xFF,
               (client_addr.sin_addr.s_addr >> 24) & 0xFF);

        fclose(file);
        close(socket_client);
    }

    syslog(LOG_INFO, "Caught signal, exiting\n");

exit_file:
    fclose(file); 
exit_client_socket:
    close(socket_client);
exit_socket_server:
    close(socket_server);
exit_syslog:
    closelog();
    remove(FILE_PATH);
    return ret;
}
