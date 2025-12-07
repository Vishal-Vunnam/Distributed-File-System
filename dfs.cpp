#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h>

using namespace std;

#define BUFSIZE 1024

// Global Variables
string directory_path;

void error(const char *msg) {
    perror(msg);
    exit(0);
}

/* ------------------------------------------------------
    SENDER
------------------------------------------------------ */
int sender(struct sockaddr_in *clientaddr, int sockfd, const char *buf, int buflen) {
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
    DIR *dir = opendir(directory_path.c_str());
    if (dir == NULL) {
        perror("opendir failed");
        return -1;
    }

    struct dirent *entry;
    string response = "";
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; // skip . and .. files
        response += entry->d_name;
        response += "\n";
    }

    closedir(dir);
    
    if (!response.empty()) {
        response.pop_back(); // Remove trailing comma
    }
    
    sender(clientaddr, sockfd, response.c_str(), response.length());
    return 0;
}

int handle_put(struct sockaddr_in *clientaddr, int sockfd, const string &filename,
               int chunk_index, const char *filecontents, int contentlen) {
    // Store with chunk index in filename: filename.chunk_index
    string filepath = directory_path + "/" + filename + "." + to_string(chunk_index);

    cout << "Opening file " << filepath << " for writing" << endl;

    // Open for write (overwrite if exists)
    FILE *fp = fopen(filepath.c_str(), "wb");
    if (fp == NULL) {
        perror("fopen failed");
        return -1;
    }

    size_t written = fwrite(filecontents, 1, contentlen, fp);
    if (written < (size_t)contentlen) {
        perror("fwrite failed");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return 0;
}

int handle_get(struct sockaddr_in *clientaddr, int sockfd, const string &filename) {
    // Find all chunks for this file (filename.0, filename.1, etc.)
    DIR *dir = opendir(directory_path.c_str());
    if (dir == NULL) {
        perror("opendir failed");
        const char *error_msg = "ERROR: Cannot open directory\n";
        sender(clientaddr, sockfd, error_msg, strlen(error_msg));
        return -1;
    }

    bool found_any = false;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        string entry_name = entry->d_name;

        // Check if this file starts with our filename and has a chunk suffix
        // Format: filename.N where N is the chunk index
        if (entry_name.length() > filename.length() + 1 &&
            entry_name.substr(0, filename.length()) == filename &&
            entry_name[filename.length()] == '.') {

            string suffix = entry_name.substr(filename.length() + 1);
            // Check if suffix is a number (chunk index)
            bool is_chunk = !suffix.empty();
            for (char c : suffix) {
                if (!isdigit(c)) {
                    is_chunk = false;
                    break;
                }
            }

            if (!is_chunk) continue;

            int chunk_index = stoi(suffix);
            string filepath = directory_path + "/" + entry_name;

            cout << "Opening file " << filepath << " for reading (chunk " << chunk_index << ")" << endl;

            FILE *fp = fopen(filepath.c_str(), "rb");
            if (fp == NULL) {
                perror("fopen failed");
                continue;
            }

            fseek(fp, 0, SEEK_END);
            long filesize = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            char *filebuf = (char*)malloc(filesize);
            if (!filebuf) {
                perror("malloc failed");
                fclose(fp);
                continue;
            }

            size_t read_bytes = fread(filebuf, 1, filesize, fp);
            fclose(fp);

            if (read_bytes < (size_t)filesize) {
                perror("fread failed");
                free(filebuf);
                continue;
            }

            // Send header: CHUNK <chunk_index> <size>\n
            char header[64];
            int header_len = snprintf(header, sizeof(header), "CHUNK %d %ld\n", chunk_index, filesize);

            cout << "Sending chunk " << chunk_index << " of " << filename
                 << " (" << filesize << " bytes)" << endl;

            if (sender(clientaddr, sockfd, header, header_len) < 0) {
                free(filebuf);
                closedir(dir);
                return -1;
            }

            // Send chunk data
            char *p = filebuf;
            long remaining = filesize;
            while (remaining > 0) {
                size_t to_send = (remaining > BUFSIZE) ? BUFSIZE : remaining;
                ssize_t bytes_sent = sender(clientaddr, sockfd, p, to_send);
                if (bytes_sent < 0) {
                    free(filebuf);
                    closedir(dir);
                    return -1;
                }
                p += to_send;
                remaining -= to_send;
            }

            free(filebuf);
            found_any = true;
        }
    }

    closedir(dir);

    if (!found_any) {
        const char *error_msg = "FILE_NOT_FOUND\n";
        sender(clientaddr, sockfd, error_msg, strlen(error_msg));
        cout << "No chunks found for " << filename << endl;
        return -1;
    }

    // Send end marker
    const char *end_msg = "END\n";
    sender(clientaddr, sockfd, end_msg, strlen(end_msg));

    return 0;
}

int router(char *buf, int buflen, struct sockaddr_in *clientaddr, int sockfd) {
    if (buflen < 1) {
        cerr << "Empty command received" << endl;
        return -1;
    }

    if (strncmp(buf, "list", 4) == 0) {
        return handle_list(clientaddr, sockfd);
    }
    else if (strncmp(buf, "put ", 4) == 0) {
        // Parse: put <filename> <chunk_index> <data_length>\n
        char filename[256];
        int chunk_index;
        size_t data_len;

        int parsed = sscanf(buf, "put %s %d %zu", filename, &chunk_index, &data_len);
        if (parsed != 3) {
            cerr << "Invalid PUT command format: " << buf << endl;
            return -1;
        }

        cout << "[PUT] Receiving " << data_len << " bytes for " << filename
             << " (chunk " << chunk_index << ")" << endl;

        // Find where the header ends (after the newline)
        char *data_start = strchr(buf, '\n');
        if (!data_start) {
            cerr << "Invalid PUT format: no newline in header" << endl;
            return -1;
        }
        data_start++; // Move past the newline

        // Calculate how much data was already received with the header
        size_t header_len = data_start - buf;
        size_t already_received = buflen - header_len;

        // Allocate buffer for file data
        char *filecontents = (char*)malloc(data_len);
        if (!filecontents) {
            perror("malloc failed");
            return -1;
        }

        // Copy data that came with the header
        size_t total_received = 0;
        if (already_received > 0) {
            size_t to_copy = (already_received > data_len) ? data_len : already_received;
            memcpy(filecontents, data_start, to_copy);
            total_received = to_copy;
        }

        // Read remaining data
        while (total_received < data_len) {
            ssize_t n = recv(sockfd, filecontents + total_received, data_len - total_received, 0);
            if (n <= 0) {
                if (n == 0) {
                    cerr << "Connection closed while receiving data" << endl;
                } else {
                    perror("recv failed");
                }
                free(filecontents);
                return -1;
            }
            total_received += n;
        }

        cout << "[PUT] Received " << total_received << " bytes total" << endl;

        int result = handle_put(clientaddr, sockfd, filename, chunk_index, filecontents, data_len);
        free(filecontents);
        return result;
    }
    else if (strncmp(buf, "get ", 4) == 0) {
        string filename = buf + 4;
        // Remove trailing newline if present
        size_t newline_pos = filename.find('\n');
        if (newline_pos != string::npos) {
            filename = filename.substr(0, newline_pos);
        }
        return handle_get(clientaddr, sockfd, filename);
    }
    else {
        cerr << "Unknown command: " << buf << endl;
        cerr << "Receiver message: " << buf << endl;
        return -1;
    }
}

/* ------------------------------------------------------
    MAIN
------------------------------------------------------ */
int main(int argc, char *argv[]) {
    int sockfd, portno, n;
    struct sockaddr_in serveraddr;
    struct sockaddr_in clientaddr;
    int optval;

    if (argc < 3) {
        cerr << "usage: " << argv[0] << " <directory> <port>" << endl;
        exit(0);
    }

    directory_path = argv[1];
    
    // Check if directory exists, create if not
    DIR *dir = opendir(directory_path.c_str());
    if (dir == NULL) {
        if (mkdir(directory_path.c_str(), 0777) < 0) {
            perror("mkdir failed");
            exit(1);
        }
    } else {
        closedir(dir);
    }

    portno = atoi(argv[2]);
    
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        error("ERROR opening socket");
    }
    
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
               (const void *)&optval, sizeof(int));
    
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);
    
    if (::bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        error("ERROR on binding");
    }

    if (listen(sockfd, 5) < 0) {
        error("ERROR on listen");
    }

    cout << "DFS Server listening on port " << portno 
         << ", serving directory: " << directory_path << endl;

    while (1) {
        int clientlen = sizeof(clientaddr);
        char buf[BUFSIZE];
        
        cout << "Waiting for new connection..." << endl;
        
        int clientfd = accept(sockfd, (struct sockaddr *)&clientaddr, 
                             (socklen_t *)&clientlen);
        if (clientfd < 0) {
            error("ERROR on accept");
        }
        
        bzero(buf, BUFSIZE);
        n = recv(clientfd, buf, BUFSIZE, 0);
        
        if (n < 0) {
            error("ERROR in recvfrom");
        }
        
        cout << "server " << portno << " received " << n << " bytes" << endl;
        cerr << "Receiver message: " << buf << endl;

        // Fork to handle request
        pid_t process_id = fork();
        if (process_id < 0) {
            perror("Fork failed");
            close(clientfd);
            continue;
        }
        
        if (process_id == 0) {
            // Child process
            close(sockfd); // Close the listening socket in child
            router(buf, n, &clientaddr, clientfd);
            close(clientfd); // Close client socket
            exit(0); // Terminate child process
        } else {
            // Parent process
            close(clientfd); // Close client socket in parent
            cout << "Forked process " << process_id << " to handle request" << endl;
        }
    }

    return 0;
}
