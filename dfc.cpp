#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <openssl/md5.h>

using namespace std;

struct ServerInfo {
    string ip;
    int port;
    int server_fd;
    struct hostent *server;
};

struct ChunkedFile {
    char *data;
    size_t size;
    ChunkedFile* next;
    
    ChunkedFile() : data(nullptr), size(0), next(nullptr) {}
    ~ChunkedFile() { if (data) free(data); }
};

void handle_error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* ----------------------------------------------------------
   send_all() – send full buffer over TCP
---------------------------------------------------------- */
static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char*)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

/* ----------------------------------------------------------
   connect_to_server() – open a fresh TCP connection
---------------------------------------------------------- */
int connect_to_server(ServerInfo *server) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    struct hostent *host = gethostbyname(server->ip.c_str());
    if (!host) {
        perror("gethostbyname");
        close(sockfd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server->port);
    memcpy(&addr.sin_addr.s_addr, host->h_addr, host->h_length);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* ----------------------------------------------------------
   GENERIC sender() – open connection and send "COMMAND PAYLOAD\n"
   Returns the socket fd on success (caller must close), -1 on failure
---------------------------------------------------------- */
int sender(ServerInfo *server, const char *command, const char *payload) {
    if (!payload) payload = "";

    int sockfd = connect_to_server(server);
    if (sockfd < 0) {
        return -1;
    }

    char buffer[1024];
    int len = snprintf(buffer, sizeof(buffer), "%s %s\n", command, payload);

    if (send_all(sockfd, buffer, len) < 0) {
        perror("send failed");
        close(sockfd);
        return -1;
    }

    // Store the new socket in the server struct for response reading
    server->server_fd = sockfd;
    return sockfd;
}

char* get_response(ServerInfo *server, char *buf, size_t buflen, int timeout_sec = 5) {
    time_t start_time = time(NULL);
    while (1) {
        if (time(NULL) - start_time > timeout_sec) {
            cout << "[GET RESPONSE] Timeout after " << timeout_sec << " seconds" << endl;
            close(server->server_fd);
            return nullptr;
        }
        ssize_t n = recv(server->server_fd, buf, buflen - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv failed");
            close(server->server_fd);
            return nullptr;
        }
        buf[n] = '\0';
        close(server->server_fd);
        return buf;
    }
}

int hash_file_to_index(const char *filename, int server_count) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5((unsigned char*)filename, strlen(filename), digest);
    uint32_t h = *((uint32_t*)digest);
    return h % server_count;
}

/* ----------------------------------------------------------
   LIST
---------------------------------------------------------- */
void list(vector<ServerInfo> &servers) {

    map <string, int> file_map;
    for (auto &server : servers) {
        int sockfd = sender(&server, "list", "");
        if (sockfd < 0) continue;
        char response[1024];
        char* result = get_response(&server, response, sizeof(response));

        // parse response and update file_map
        char *line = strtok(result, "\n");
        while (line != nullptr) {
            // remove chunk .[int] suffix
            string entry_name(line);
            size_t pos = entry_name.rfind('.');
            if (pos != string::npos) {
                string suffix = entry_name.substr(pos + 1);
                if (all_of(suffix.begin(), suffix.end(), ::isdigit)) {
                    entry_name = entry_name.substr(0, pos);
                }
            }
            file_map[entry_name]++;
            line = strtok(nullptr, "\n");
        }
        for (auto &entry : file_map) {
            if (entry.second == servers.size()) {
                cout << entry.first << endl;
            } else { 
                cout << entry.first << " (incomplete) "  << endl;
            }
        }
        // if (result != nullptr) {
        //     cout << "[LIST] Response from " << server.ip << ":" << server.port
        //          << " : " << response << endl;
        // }
        // get_response closes the socket
    }
}

/* ----------------------------------------------------------
   GET - Helper to receive exactly n bytes
---------------------------------------------------------- */
static int recv_all(int fd, char *buf, size_t len, int timeout_sec = 10) {
    time_t start = time(NULL);
    size_t total = 0;
    while (total < len) {
        if (time(NULL) - start > timeout_sec) {
            cerr << "[RECV] Timeout" << endl;
            return -1;
        }
        ssize_t n = recv(fd, buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv failed");
            return -1;
        }
        if (n == 0) {
            // Connection closed
            return total;
        }
        total += n;
    }
    return total;
}

/* ----------------------------------------------------------
   GET - Read a line (up to newline) from socket
---------------------------------------------------------- */
static int recv_line(int fd, char *buf, size_t maxlen, int timeout_sec = 10) {
    time_t start = time(NULL);
    size_t pos = 0;
    while (pos < maxlen - 1) {
        if (time(NULL) - start > timeout_sec) {
            return -1;
        }
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            break; // Connection closed
        }
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    return pos;
}

/* ----------------------------------------------------------
   GET - Fetch chunks from a single server
---------------------------------------------------------- */
int fetch_chunks_from_server(ServerInfo *server, const char *filename,
                               map<int, ChunkedFile*> &chunks) {
    int sockfd = sender(server, "get", filename);
    if (sockfd < 0) {
        return -1;
    }

    char line[256];
    while (1) {
        int line_len = recv_line(sockfd, line, sizeof(line));
        if (line_len <= 0) {
            break;
        }

        // Check for end marker
        if (strncmp(line, "END", 3) == 0) {
            cout << "[GET] End of response from " << server->ip << ":" << server->port << endl;
            break;
        }

        // Check for file not found
        if (strncmp(line, "FILE_NOT_FOUND", 14) == 0) {
            cout << "[GET] No chunks on " << server->ip << ":" << server->port << endl;
            break;
        }

        // Parse chunk header: CHUNK <index> <size>\n
        int chunk_index;
        long chunk_size;
        if (sscanf(line, "CHUNK %d %ld", &chunk_index, &chunk_size) != 2) {
            cerr << "[GET] Invalid chunk header: " << line << endl;
            break;
        }

        // Only store if we don't already have this chunk
        if (chunks.find(chunk_index) != chunks.end()) {
            // Skip this chunk's data - we already have it
            char *discard = (char*)malloc(chunk_size);
            if (discard) {
                recv_all(sockfd, discard, chunk_size);
                free(discard);
            }
            cout << "[GET] Skipping duplicate chunk " << chunk_index << endl;
            continue;
        }

        // Allocate and receive chunk data
        char *data = (char*)malloc(chunk_size);
        if (!data) {
            perror("malloc failed");
            break;
        }

        int received = recv_all(sockfd, data, chunk_size);
        if (received != chunk_size) {
            cerr << "[GET] Failed to receive chunk " << chunk_index
                 << " (got " << received << "/" << chunk_size << " bytes)" << endl;
            free(data);
            break;
        }

        ChunkedFile *chunk = new ChunkedFile();
        chunk->data = data;
        chunk->size = chunk_size;
        chunks[chunk_index] = chunk;

    }

    close(sockfd);
    return 0;
}

void get(vector<ServerInfo> &servers, vector<string> &filenames) {
    int server_count = servers.size();

    printf("[GET] Starting file retrieval for %zu files\n", filenames.size());
    for (const auto &filename : filenames) {
        cout << "[GET] Downloading " << filename << endl;

        map<int, ChunkedFile*> chunks;

        // Query each server sequentially to collect chunks
        for (int j = 0; j < server_count; j++) {
            if (fetch_chunks_from_server(&servers[j], filename.c_str(), chunks) < 0) {
                cerr << "[GET] Error fetching chunks from "
                     << servers[j].ip << ":" << servers[j].port << " attempting second server" << endl;
                // fetch_chunks_from_server(&servers[(j + 1) % server_count], filename.c_str(), chunks);
            }


            // Check if we have all chunks
            if ((int)chunks.size() == server_count) {
                cout << "[GET] Got all " << server_count << " chunks" << endl;
                break;
            }
        }

        // Check if we have all chunks
        bool have_all = true;
        for (int i = 0; i < server_count; i++) {
            if (chunks.find(i) == chunks.end()) {
                cerr << "[GET] Missing chunk " << i << " for " << filename << endl;
                // attempt second server
                have_all = false;

            }
        }

        if (!have_all) {
            cout << filename << " incomplete" << endl;
            for (auto &pair : chunks) {
                delete pair.second;
            }
            continue;
        }

        // Reassemble file
        ofstream outfile(filename, ios::binary);
        if (!outfile) {
            perror("fopen failed");
            for (auto &pair : chunks) {
                delete pair.second;
            }
            continue;
        }

        for (int i = 0; i < server_count; i++) {
            outfile.write(chunks[i]->data, chunks[i]->size);
            delete chunks[i];
        }
        outfile.close();
        cout << "[GET] Successfully reassembled file " << filename << endl;
    }
}

/* ----------------------------------------------------------
   PUT HELPERS
---------------------------------------------------------- */
int put_sender(ServerInfo *server, const char *data, size_t data_len,
               const char *filename, int chunk_index) {
    // remove slashes from filename
    if (strchr(filename, '/')) {
        filename = strrchr(filename, '/') + 1;
    }


    // Open a fresh connection
    int sockfd = connect_to_server(server);
    if (sockfd < 0) {
        return -1;
    }

    // Send header with total data length
    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "put %s %d %zu\n", filename, chunk_index, data_len);

    // Send header first
    if (send_all(sockfd, header, header_len) < 0) {
        perror("send header failed");
        close(sockfd);
        return -1;
    }

    // Then send all the data
    if (send_all(sockfd, data, data_len) < 0) {
        perror("send data failed");
        close(sockfd);
        return -1;
    }

    cout << "[PUT] Sent " << data_len << " bytes with header: " << header;

    close(sockfd);
    return 0;
}

/* ----------------------------------------------------------
   PUT
---------------------------------------------------------- */
void put(vector<ServerInfo> &servers, vector<string> &filenames) {
    int server_count = servers.size();

    for (const auto &filename : filenames) {
        // Extract base filename for hashing (strip path)
        string base_filename = filename;
        size_t slash_pos = filename.rfind('/');
        if (slash_pos != string::npos) {
            base_filename = filename.substr(slash_pos + 1);
        }

        int h = hash_file_to_index(base_filename.c_str(), server_count);

        ifstream infile(filename, ios::binary | ios::ate);
        if (!infile) {
            perror("fopen failed");
            continue;
        }

        streamsize filesize = infile.tellg();
        infile.seekg(0, ios::beg);

        // Use map to store chunks
        map<int, vector<char>> chunks;
        size_t base_chunk_size = filesize / server_count;
        int remaining = filesize % server_count;
        
        for (int i = 0; i < server_count; i++) {
            size_t this_chunk_size = base_chunk_size + (i < remaining ? 1 : 0);
            chunks[i].resize(this_chunk_size);
            infile.read(chunks[i].data(), this_chunk_size);
        }
        infile.close();

        for (int j = 0; j < server_count; j++) {
            cout << "[PUT] Sending chunk " << j << " of " << filename 
                 << " (size " << chunks[j].size() << " bytes)" << endl;
                 
            int srv = (h + j) % server_count;
            if (put_sender(&servers[srv], chunks[j].data(), chunks[j].size(), 
                          filename.c_str(), j) < 0) {
                cerr << "[PUT] Failed to send chunk " << j << " of " << filename 
                     << " to server " << servers[srv].ip << ":" << servers[srv].port << endl;
            }

            int second = (srv + 1) % server_count;
            if (put_sender(&servers[second], chunks[j].data(), chunks[j].size(), 
                          filename.c_str(), j) < 0) {
                cerr << "Failed to send chunk " << j << " of " << filename 
                     << " to server " << servers[second].ip << ":" << servers[second].port << endl;
            }
        }
    }
}

/* ----------------------------------------------------------
   MAIN
---------------------------------------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        cerr << "usage: " << argv[0] << " <command> [files...]" << endl;
        printf("usage: %s <command> [files...]\n", argv[0]);
        exit(0);
    }

    string command = argv[1];
    vector<string> files;
    for (int i = 2; i < argc; i++) {
        files.push_back(argv[i]);
    }

    /* ------------------------------------------------------
       READ CONFIG
    ------------------------------------------------------ */
    ifstream config("dfc.conf");
    if (!config) {
        handle_error("dfc.conf");
    }

    vector<ServerInfo> servers;
    string line;

    while (getline(config, line) && servers.size() < 4) {
        // Parse line: "server dfsX ip:port" or "ip:port"
        size_t pos = line.find("server");
        if (pos != string::npos) {
            // Extract IP:port after "server dfsX"
            pos = line.find_last_of(" \t");
            if (pos != string::npos) {
                line = line.substr(pos + 1);
            }
        }

        // Parse ip:port
        pos = line.find(':');
        if (pos != string::npos) {
            ServerInfo server;
            server.ip = line.substr(0, pos);
            server.port = stoi(line.substr(pos + 1));
            servers.push_back(server);
        }
    }
    config.close();

    /* ------------------------------------------------------
       HANDLE COMMAND (connections are opened per-request)
    ------------------------------------------------------ */
    cout << "Executing command: " << command << endl;
    
    if (command == "list") {
        list(servers);
    } else if (command == "get") {
        get(servers, files);
    } else if (command == "put") {
        put(servers, files);
    } else {
        cerr << "Unknown command: " << command << endl;
        return EXIT_FAILURE;
    }

    return 0;
}