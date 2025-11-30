#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h> 
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>

#define BUFSIZE 1024

// Global Variables
char directory_path[256];


void error(const char *msg) {
    perror(msg);
    exit(0);
}



/* ------------------------------------------------------
    SENDER & ROUTER
------------------------------------------------------ */

int sender(struct sockaddr_in *clientaddr, int sockfd, char *buf, int buflen) {
    ssize_t bytes_sent = send(sockfd, buf, buflen, 0); 
    if (bytes_sent < 0) {
        perror("ERROR in sendto");
        return -1;
    }
    return bytes_sent;
}


/* ------------------------------------------------------
    COMMAND HANDLERS
------------------------------------------------------ */

int handle_list(struct sockaddr_in *clientaddr, int sockfd) {

    DIR *dir = opendir(directory_path);
    if (dir == NULL) {
        perror("opendir failed");
        return -1;
    }

    struct dirent *entry;
    char response[BUFSIZE] = "";
    while ((entry = readdir(dir)) != NULL) {
        strcat(response, entry->d_name);
        strcat(response, ",");
    }

    closedir(dir); 
    free(entry);
    int response_len = strlen(response);
    if (response_len > 0) {
        response[response_len - 1] = '\0'; // Remove trailing comma
    }
    sender(clientaddr, sockfd, response, strlen(response));

    return 0;
}

int handle_put(struct sockaddr_in *clientaddr, int sockfd, char *filename, char *filecontents, int contentlen) {
    // check if file exists
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", directory_path, filename);
    FILE *fp = fopen(filepath, "ab");
    if (fp == NULL) {
        perror("fopen failed");
        return -1;
    }

    size_t written = fwrite(filecontents, 1, contentlen, fp);
    if (written < contentlen) {
        perror("fwrite failed");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return 0;
}

int handle_get(struct sockaddr_in *clientaddr, int sockfd, char *filename) {
    // Implementation for handling get command
    return 0;
}

int router(char *buf, int buflen, struct sockaddr_in *clientaddr, int sockfd) {
    if (buflen < 1) {
        fprintf(stderr, "Empty command received\n");
        return -1;
    }

    if(strncmp(buf, "list", 4) == 0) { 
        return handle_list(clientaddr, sockfd);
    }
    else if(strncmp(buf, "put ", 4) == 0) { 
        // Extract filename and file contents from buf
        char *filename_start = buf + 4;
        char *newline_pos = strchr(filename_start, '\n');
        if (newline_pos == NULL) {
            fprintf(stderr, "Invalid PUT command format\n");
            return -1;
        }
        *newline_pos = '\0'; // Null-terminate filename
        char *filecontents = newline_pos + 1;
        int contentlen = buflen - (filecontents - buf);

        return handle_put(clientaddr, sockfd, filename_start, filecontents, contentlen);
    }
    else if(strncmp(buf, "get ", 4) == 0) { 
        char *filename = buf + 4;
        // Remove trailing newline if present
        char *newline_pos = strchr(filename, '\n');
        if (newline_pos != NULL) {
            *newline_pos = '\0';
        }
        return handle_get(clientaddr, sockfd, filename);
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", buf);
        return -1;
    }
}


/* ------------------------------------------------------
    MAIN
------------------------------------------------------ */   
int main(int argc, char *argv[]) {
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct sockaddr_in clientaddr;
    int optval;
    struct hostent *server; 
    char *hostname; 

    if (argc < 3) { 
         fprintf(stderr, "usage %s <directory> <port>\n", argv[0]);
         exit(0);
    }

    strcpy(directory_path, argv[1]);
    DIR *dir = opendir(directory_path);
    if (dir == NULL) {
        perror("opendir failed");
        exit(1);
    }
    closedir(dir);

    portno = atoi(argv[2]);
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
        error("ERROR opening socket");
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
               (const void *)&optval , sizeof(int));
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
             sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    if (listen(sockfd, 5) < 0)
        error("ERROR on listen");

    printf("DFS Server listening on port %d, serving directory: %s\n", portno, directory_path);
    



    while(1) { 
        int clientlen = sizeof(clientaddr);
        char buf[BUFSIZE];
        int clientfd = accept(sockfd, (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen);
        if (clientfd < 0) 
            error("ERROR on accept");
        // this is where you thread
        bzero(buf, BUFSIZE);
        n = recv(clientfd, buf, BUFSIZE, 0);
        
        if (n < 0)
            error("ERROR in recvfrom");
        printf("server received %d bytes\n", n);

        router(buf, n, &clientaddr, clientfd);
    }

} 

