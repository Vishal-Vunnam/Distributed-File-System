#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/md5.h>

typedef struct {
    char ip[16];
    int port;
    int server_fd;
    struct hostent *server;
} server_info_t;

void handle_error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* ----------------------------------------------------------
   send_all() – send full buffer over TCP
---------------------------------------------------------- */
static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

/* ----------------------------------------------------------
   GENERIC sender() – send "COMMAND\nPAYLOAD\n"
---------------------------------------------------------- */
int sender(server_info_t *server, const char *command, const char *payload) {

    if (!payload) payload = "";

    char buffer[1024];
    int len = snprintf(buffer, sizeof(buffer), "%s %s\n", command, payload);

    printf("[SENDER] >> %s:%d : %s %s\n",
           server->ip, server->port, command, payload);

    if (send_all(server->server_fd, buffer, len) < 0) {
        perror("send failed");
        return -1;
    }

    return 0;
}

int get_response(server_info_t *server, char *buf, size_t buflen) {
    while (1) {
        ssize_t n = recv(server->server_fd, buf, buflen - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv failed");
            return -1;
        }
        buf[n] = '\0';
        return n;
    }
}
/* ----------------------------------------------------------
   LIST
---------------------------------------------------------- */
void list(server_info_t *servers, int server_count) {
    for (int i = 0; i < server_count; i++) {
        sender(&servers[i], "list", "");
    }
    for (int i = 0; i < server_count; i++) {
        char response[1024];
        int n = get_response(&servers[i], response, sizeof(response));
        if (n > 0) {
            printf("[LIST] Response from %s:%d : %s\n",
                   servers[i].ip, servers[i].port, response);
        }
    }
}

/* ----------------------------------------------------------
   GET
---------------------------------------------------------- */
void get(server_info_t *servers, int server_count,
         char **filenames, int file_count)
{
    for (int i = 0; i < server_count; i++) {
        for (int j = 0; j < file_count; j++) {
            sender(&servers[i], "get", filenames[j]);
        }
    }
}

/* ----------------------------------------------------------
   PUT HELPERS
---------------------------------------------------------- */

int put_sender(server_info_t *server, const char *data, size_t data_len,
               const char *filename)
{
    printf("[PUT] Sending %zu bytes of %s to %s:%d\n",
           data_len, filename, server->ip, server->port);

    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "put %s %zu\n", filename, data_len);

    if (send_all(server->server_fd, header, header_len) < 0) {
        perror("send header failed");
        return -1;
    }

    if (send_all(server->server_fd, data, data_len) < 0) {
        perror("send data failed");
        return -1;
    }

    return 0;
}

int hash_file_to_index(const char *filename, int server_count) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5((unsigned char*)filename, strlen(filename), digest);
    uint32_t h = *((uint32_t*)digest);
    return h % server_count;
}

/* ----------------------------------------------------------
   PUT (fake chunks for now)
---------------------------------------------------------- */
void put(server_info_t *servers, int server_count,
         char **filenames, int file_count)
{
    for (int f = 0; f < file_count; f++) {

        printf("[PUT] Uploading %s\n", filenames[f]);

        char *chunks[server_count];
        for (int c = 0; c < server_count; c++)
            chunks[c] = "FAKE_CHUNK_DATA";

        int h = hash_file_to_index(filenames[f], server_count);

        for (int j = 0; j < server_count; j++) {

            int srv = (h + j) % server_count;
            put_sender(&servers[srv], chunks[j], strlen(chunks[j]), filenames[f]);

            int second = (j + 1) % server_count;
            put_sender(&servers[srv], chunks[second], strlen(chunks[second]), filenames[f]);
        }
    }
}

/* ----------------------------------------------------------
   MAIN
---------------------------------------------------------- */
int main(int argc, char *argv[]) {

    char *files[256];
    int file_count = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> [files...]\n", argv[0]);
        exit(0);
    }

    char *command = argv[1];
    for (int i = 2; i < argc; i++)
        files[file_count++] = argv[i];

    /* ------------------------------------------------------
       READ CONFIG
    ------------------------------------------------------ */
    FILE *fp = fopen("dfc.conf", "r");
    if (!fp) handle_error("dfc.conf");

    server_info_t servers[4];
    int count = 0;

    char line[256];
    while (fgets(line, sizeof(line), fp) && count < 4) {

        size_t len = strlen(line);
        if (len && line[len - 1] == '\n')
            line[len - 1] = '\0';

        char *tok = strtok(line, " \t");
        char *ipport = NULL;

        if (!tok) continue;

        if (!strcmp(tok, "server")) {
            tok = strtok(NULL, " \t"); // dfsX
            tok = strtok(NULL, " \t");
            ipport = tok;
        } else {
            ipport = tok;
        }

        if (!ipport) continue;

        char ipbuf[16];
        int port;

        if (sscanf(ipport, "%15[^:]:%d", ipbuf, &port) == 2) {
            strncpy(servers[count].ip, ipbuf, sizeof(servers[count].ip));
            servers[count].port = port;
            count++;
        }
    }

    fclose(fp);

    /* ------------------------------------------------------
       CONNECT TO SERVERS
    ------------------------------------------------------ */
    for (int i = 0; i < count; i++) {

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) handle_error("socket");

        servers[i].server = gethostbyname(servers[i].ip);
        if (!servers[i].server) handle_error("gethostbyname");

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(servers[i].port);
        memcpy(&addr.sin_addr.s_addr,
               servers[i].server->h_addr,
               servers[i].server->h_length);

        if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            handle_error("connect");
        }

        servers[i].server_fd = sockfd;

        printf("Connected to %s:%d\n",
               servers[i].ip, servers[i].port);
    }

    /* ------------------------------------------------------
       HANDLE COMMAND
    ------------------------------------------------------ */
    if (!strcmp(command, "list"))      list(servers, count);
    else if (!strcmp(command, "get"))  get(servers, count, files, file_count);
    else if (!strcmp(command, "put"))  put(servers, count, files, file_count);
    else {
        fprintf(stderr, "Unknown command: %s\n", command);
        return EXIT_FAILURE;
    }

    return 0;
}
