
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 8192
#define ROOT_FILE "www/index.html"
#define SERVER_NAME "LTHT-APUE-Server"

static int send_all(int fd, const void *data, size_t length)
{
    const char *p = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, p + sent, length - sent, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

static void format_http_date(time_t value, char *buffer, size_t size)
{
    struct tm tm_value;

    if (gmtime_r(&value, &tm_value) == NULL) {
        snprintf(buffer, size, "Thu, 01 Jan 1970 00:00:00 GMT");
        return;
    }

    strftime(buffer, size, "%a, %d %b %Y %H:%M:%S GMT",
             &tm_value);
}

static int send_error(int fd, int status, const char *reason,
                      const char *message, const char *extra_headers)
{
    char body[512];

    int body_length = snprintf(
        body, sizeof(body),
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>%d %s</title></head>"
        "<body><h1>%d %s</h1><p>%s</p></body></html>\n",
        status, reason, status, reason, message
    );

    if (body_length < 0 || (size_t)body_length >= sizeof(body))
        return -1;

    char date[64];
    char headers[1024];

    format_http_date(time(NULL), date, sizeof(date));

    int header_length = snprintf(
        headers, sizeof(headers),
        "HTTP/1.0 %d %s\r\n"
        "Date: %s\r\n"
        "Server: %s\r\n"
        "%s"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason, date, SERVER_NAME,
        extra_headers ? extra_headers : "",
        body_length
    );

    if (header_length < 0 ||
        (size_t)header_length >= sizeof(headers))
        return -1;

    if (send_all(fd, headers, (size_t)header_length) < 0)
        return -1;

    if (send_all(fd, body, (size_t)body_length) < 0)
        return -1;

    return 0;
}

static void handle_client(int client_fd)
{
    char request[BUFFER_SIZE];
    size_t used = 0;

    /*
     * Read until the end of the HTTP request headers.
     * This basic server supports requests without a message body.
     */
    while (used < sizeof(request) - 1) {
        ssize_t n = recv(client_fd, request + used,
                         sizeof(request) - 1 - used, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return;

        used += (size_t)n;
        request[used] = '\0';

        if (strstr(request, "\r\n\r\n") != NULL)
            break;
    }

    if (used == sizeof(request) - 1 &&
        strstr(request, "\r\n\r\n") == NULL) {
        send_error(client_fd, 400, "Bad Request",
                   "Request headers are too large.", NULL);
        return;
    }

    printf("\nReceived request:\n%.*s\n",
           (int)used, request);

    char method[16];
    char path[1024];
    char version[16];

    if (sscanf(request, "%15s %1023s %15s",
               method, path, version) != 3) {
        send_error(client_fd, 400, "Bad Request",
                   "Invalid HTTP request line.", NULL);
        return;
    }

    if (strcmp(version, "HTTP/1.0") != 0 &&
        strcmp(version, "HTTP/1.1") != 0) {
        send_error(client_fd, 400, "Bad Request",
                   "Unsupported HTTP version.", NULL);
        return;
    }

    if (strcmp(method, "GET") != 0 &&
        strcmp(method, "HEAD") != 0) {
        send_error(client_fd, 405, "Method Not Allowed",
                   "Only GET and HEAD are supported.",
                   "Allow: GET, HEAD\r\n");
        return;
    }

    /*
     * Only serve the homepage. Do not map arbitrary paths
     * to files on disk.
     */
    if (strcmp(path, "/") != 0 &&
        strcmp(path, "/index.html") != 0) {
        send_error(client_fd, 404, "Not Found",
                   "The requested resource was not found.", NULL);
        return;
    }

    FILE *file = fopen(ROOT_FILE, "rb");

    if (file == NULL) {
        perror("fopen " ROOT_FILE);
        send_error(client_fd, 500, "Internal Server Error",
                   "Unable to open the homepage.", NULL);
        return;
    }

    struct stat file_info;

    if (stat(ROOT_FILE, &file_info) < 0 ||
        !S_ISREG(file_info.st_mode)) {
        fclose(file);
        send_error(client_fd, 500, "Internal Server Error",
                   "The homepage is not a regular file.", NULL);
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        send_error(client_fd, 500, "Internal Server Error",
                   "Unable to read the homepage.", NULL);
        return;
    }

    long file_size = ftell(file);

    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        send_error(client_fd, 500, "Internal Server Error",
                   "Unable to determine the homepage size.", NULL);
        return;
    }

    size_t body_size = (size_t)file_size;
    char *body = malloc(body_size > 0 ? body_size : 1);

    if (body == NULL) {
        fclose(file);
        send_error(client_fd, 500, "Internal Server Error",
                   "Insufficient memory.", NULL);
        return;
    }

    size_t bytes_read = fread(body, 1, body_size, file);
    int read_error = ferror(file);
    fclose(file);

    if (read_error || bytes_read != body_size) {
        free(body);
        send_error(client_fd, 500, "Internal Server Error",
                   "Failed to read the homepage.", NULL);
        return;
    }

    char date[64];
    char modified[64];
    char headers[1024];

    format_http_date(time(NULL), date, sizeof(date));
    format_http_date(file_info.st_mtime, modified, sizeof(modified));

    /*
     * Handle If-Modified-Since.
     */
    char *line = request;

    while (*line != '\0') {
        char *end = strstr(line, "\r\n");

        if (end == NULL)
            break;

        *end = '\0';

        if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
            char *value = line + 18;

            while (*value == ' ' || *value == '\t')
                value++;

            struct tm parsed_time;
            memset(&parsed_time, 0, sizeof(parsed_time));

            char *parsed = strptime(
                value, "%a, %d %b %Y %H:%M:%S GMT",
                &parsed_time
            );

            if (parsed != NULL && *parsed == '\0') {
                time_t since = timegm(&parsed_time);

                if (since >= file_info.st_mtime) {
                    char not_modified_headers[512];

                    int hlen = snprintf(
                        not_modified_headers,
                        sizeof(not_modified_headers),
                        "HTTP/1.0 304 Not Modified\r\n"
                        "Date: %s\r\n"
                        "Server: %s\r\n"
                        "Last-Modified: %s\r\n"
                        "Connection: close\r\n\r\n",
                        date, SERVER_NAME, modified
                    );

                    if (hlen > 0 &&
                        (size_t)hlen < sizeof(not_modified_headers)) {
                        send_all(client_fd, not_modified_headers,
                                 (size_t)hlen);
                    }

                    free(body);
                    return;
                }
            }
        }

        line = end + 2;

        if (*line == '\r' && *(line + 1) == '\n')
            break;
    }

    int header_length = snprintf(
        headers, sizeof(headers),
        "HTTP/1.0 200 OK\r\n"
        "Date: %s\r\n"
        "Server: %s\r\n"
        "Last-Modified: %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        date, SERVER_NAME, modified, body_size
    );

    if (header_length < 0 ||
        (size_t)header_length >= sizeof(headers)) {
        free(body);
        return;
    }

    if (send_all(client_fd, headers, (size_t)header_length) < 0) {
        free(body);
        return;
    }

    /* HEAD sends the same headers as GET, but no body. */
    if (strcmp(method, "GET") == 0 && body_size > 0)
        send_all(client_fd, body, body_size);

    free(body);
}

int main(void)
{
    int server_fd;
    int opt = 1;
    struct sockaddr_in address;

    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("====================================\n");
    printf("       LTHT APUE HTTP SERVER\n");
    printf("====================================\n");
    printf("Listening on port %d\n", PORT);
    printf("Document root: %s\n", ROOT_FILE);
    printf("Press Ctrl+C to stop.\n");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;

            perror("accept");
            continue;
        }

        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}

