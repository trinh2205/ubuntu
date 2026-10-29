
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define BUFFER_SIZE 4096

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <hostname> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *hostname = argv[1];
    const char *port = argv[2];

    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int sockfd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(hostname, port, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n",
                gai_strerror(status));
        return EXIT_FAILURE;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family,
                        rp->ai_socktype,
                        rp->ai_protocol);

        if (sockfd == -1)
            continue;

        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);

    if (sockfd == -1) {
        perror("connect");
        return EXIT_FAILURE;
    }

    char request[BUFFER_SIZE];

    int length = snprintf(
        request, sizeof(request),
        "GET / HTTP/1.0\r\n"
        "Host: %s\r\n"
        "\r\n",
        hostname
    );

    if (length < 0 || (size_t)length >= sizeof(request)) {
        fprintf(stderr, "Request is too long\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    size_t sent = 0;
    while (sent < (size_t)length) {
        ssize_t n = send(sockfd, request + sent,
                         (size_t)length - sent, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            perror("send");
            close(sockfd);
            return EXIT_FAILURE;
        }

        sent += (size_t)n;
    }

    char buffer[BUFFER_SIZE];
    ssize_t n;

    while ((n = recv(sockfd, buffer,
                     sizeof(buffer), 0)) > 0) {
        if (fwrite(buffer, 1, (size_t)n, stdout)
            != (size_t)n) {
            perror("fwrite");
            close(sockfd);
            return EXIT_FAILURE;
        }
    }

    if (n < 0)
        perror("recv");

    close(sockfd);
    return n < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
