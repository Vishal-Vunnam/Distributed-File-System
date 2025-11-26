#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

typedef struct {
    char ip[16];
    int port;
} server_info_t;

void handle_error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int sender(server_info_t *server, char *buf, int buflen) {
    // Implementation of sender function to send data to server
    return 0; // Placeholder
}

void list(server_info_t **servers, int server_count) {
    

}

void get(server_info_t **servers, int server_count, char **filenames, int file_count) {
    // Implementation of get function
}

void put(server_info_t **servers, int server_count, char **filenames, int file_count) {
   
}


int main (int argc, char * argv[]) {
    char *files[256];
    int file_count = 0;
    if (argc < 2) { 
        fprintf(stderr, "usage %s <command> <filename> ... <filename>\n", argv[0]);
        exit(0);
    }

    char* command = argv[1];

    for (int i = 2; i < argc; i++) {
        files[file_count++] = argv[i];
    }

    printf("Command: %s\n", command);
    printf("Files:\n");
    for (int i = 0; i < file_count; i++) {
        printf("  %s\n", files[i]);
    }
    // Get Server details from config file
    FILE * fp = fopen("dfc.conf", "r");
    if (fp == NULL) {
        perror("Error opening dfc.conf");
        return EXIT_FAILURE;   
    }
    server_info_t servers[4];
    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        printf("Server: %s", line);
        if (count < 4) {
            /* Trim trailing newline */
            size_t _len = strlen(line);
            if (_len > 0 && line[_len-1] == '\n') {
                line[_len-1] = '\0';
            }

            /* Accept lines like "server dfs1 127.0.0.1:10001" or just "127.0.0.1:10001" */
            {
                char *saveptr = NULL;
                char *tokens[4];
                int t = 0;
                char *s = line;
                char *tok = strtok_r(s, " \t\r\n", &saveptr);
                while (tok && t < 4) {
                    tokens[t++] = tok;
                    tok = strtok_r(NULL, " \t\r\n", &saveptr);
                }

                char *ipport = NULL;
                if (t == 0) {
                    fprintf(stderr, "Empty config line\n");
                    break;
                }
                if (strcmp(tokens[0], "server") == 0) {
                    if (t >= 3) ipport = tokens[2];
                    else { fprintf(stderr, "Invalid server entry: %s\n", line); break; }
                } else {
                    ipport = tokens[t-1];
                }

                char ipbuf[16];
                int port;
                if (sscanf(ipport, "%15[^:]:%d", ipbuf, &port) == 2) {
                    strncpy(servers[count].ip, ipbuf, sizeof(servers[count].ip));
                    servers[count].ip[sizeof(servers[count].ip) - 1] = '\0';
                    servers[count].port = port;
                    count++;
                } else {
                    fprintf(stderr, "Invalid server entry: %s\n", line);
                    break;
                }
            }
        } else {
            fprintf(stderr, "Too many servers in config (max 4)\n");
            break;
        }
    }
    fclose(fp);
    // 


    if (strcmp(command, "list") == 0) {
        list(servers, count);
    } else if (strcmp(command, "get") == 0) {
        get(servers, count, files, file_count);
    } else if (strcmp(command, "put") == 0) {
        put(servers, count, files, file_count);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
        return EXIT_FAILURE;
    }







}