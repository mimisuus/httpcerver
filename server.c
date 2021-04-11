#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>


#define SERVER_PORT 80 // Requires root, set to >1023 to avoid
#define MAX_BACKLOG 5 // Maximum of 5 pending connections
#define BUFFER_SIZE 8192
#define MAX_PARAM_LENGTH 40
char buffer[BUFFER_SIZE];

typedef struct {
    const char *extension;
    const char *file_type;
} file_map;

file_map file_types [] = {
    {".htm", "text/html"},
    {".html", "text/html"},
    {".xml", "text/xml"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/ico"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".pdf", "application/pdf"},
    {".mp4", "video/mp4"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {0, 0},
};

void print_err_n_exit(void) {
    fprintf(stderr, "Error: %s\n", strerror(errno));
    exit(-1);
}

void close_connection(int connection) {
    if (shutdown(connection, SHUT_RDWR) == -1) {
        close(connection);
        print_err_n_exit();
    }
    close(connection);
}

void return_error(int connection, char error_code[]) {
    sprintf(buffer,"HTTP/1.1 %s\n"
                   "Server: mieserver\n"
                   "Connection: close\n\n", error_code);
    write(connection, buffer, strlen(buffer));
    close_connection(connection);
}

const char* get_filetype(char *path) {
    const char *dot = strrchr(path, '.');
    if(dot && dot != path) {
        for (file_map *m = file_types; m->extension; m++) {
            if (!strcmp(m->extension, dot)) {
                return m->file_type;
            }
        }
    }
    return NULL;
}

int get_word_from_buffer(char *param_location, int start_index) {
    char c;
    int curr_word_len = 0;
    for (int i = start_index; (c = buffer[i]) != '\0'; i++) {
        if (isspace(c)) {
            // Can have multiple white-space characters in a row
            if (curr_word_len == 0) continue;

            strncpy(param_location, buffer + i - curr_word_len, curr_word_len);
            return i;
        }
        else curr_word_len++;
    }
    return -1;
}

int main(int argc, char *argv[]) {
    // File descriptor for a stream-oriented socket using the IPv4 protocol 
    int socketfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketfd == -1) {
        print_err_n_exit();
    }

    /*
     * Make reusing the address possible, in case socket is still waiting.
     * So no need to wait for several minutes before running server again.
     * Only really useful for faster testing, should probably be disabled otherwise.
     */
    const int enable = 1;
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1) {
        close(socketfd);
        print_err_n_exit();
    }

    // Address information for server
    struct sockaddr_in s_address;

    // sin_zero is never used, but part of the specification to set it to 0's
    memset(&s_address.sin_zero, 0, 8);
    s_address.sin_family = AF_INET;
    // Port number in network byte order
    s_address.sin_port = htons(SERVER_PORT); 
    // s_address.sin_addr.s_addr = inet_addr("127.0.0.1"); // Use instead if local only
    s_address.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the socket_address to the socket
    if (bind(socketfd, (struct sockaddr *)&s_address, sizeof(s_address)) == -1){
        close(socketfd);
        print_err_n_exit();
    }

    if (listen(socketfd, MAX_BACKLOG) == -1) {
        close(socketfd);
        print_err_n_exit();
    } else {
        printf("Listening on port %d ...\n", SERVER_PORT);
    }

    // Each loop is one request
    for (;;) {
        // Address information for client
        struct sockaddr_in c_address;
        socklen_t c_address_len;

        memset(buffer, 0, sizeof(buffer));

        int connectfd = accept(socketfd, (struct sockaddr *)&c_address, &c_address_len);
        if (connectfd == -1) {
            close(socketfd);
            print_err_n_exit();
        }
        printf("Accepted a request\n");

        const long request_len = read(connectfd, buffer, BUFFER_SIZE);
        if (request_len < 0) {
            close(connectfd);
            close(socketfd);
            print_err_n_exit();
        }
        if (request_len == 0) {
            close(connectfd);
            printf("Connection closed");
            continue;
        }

        const int buffer_end = request_len < BUFFER_SIZE ? request_len : 0;
        buffer[buffer_end] = '\0';

        int next_index = 0;
        char *method = malloc(MAX_PARAM_LENGTH);
        next_index = get_word_from_buffer(method, next_index);

        char *path = malloc(MAX_PARAM_LENGTH);
        next_index = get_word_from_buffer(path, next_index);

        if (path[0] == '/') memmove(path, path+1, strlen(path)); // Remove first '/'

        if (path[0] == '\0') path = "index.html"; // default homepage

        // Only implementing GET for now
        if (!strncmp(method, "GET", 4)) {
            if (strstr(path, "..")) {
                printf("Client tried accessing a parent directory, closing connection..\n");
                return_error(connectfd, "400 Bad Request");
                continue;
            }
            // Check in case file doesn't exist
            if (access(path, F_OK) != 0) {
                return_error(connectfd, "404 Not Found");
                continue;
            } 

            FILE* page_file = fopen(path, "r");
            if (page_file == NULL) {
                // A file without read permissions
                return_error(connectfd, "403 Forbidden");
                continue;
            }

            // Get file size
            fseek(page_file, 0L, SEEK_END);
            int file_len = ftell(page_file);
            rewind(page_file);

            const char *file_type = get_filetype(path);

            if (file_type == NULL) {
                return_error(connectfd, "400 Bad Request");
                continue;
            }

            sprintf(buffer,"HTTP/1.1 200 OK\n"
                           "Server: mieserver\n"
                           "Content-Length: %d\n"
                           "Connection: close\n"
                           "Content-Type: %s\n\n", file_len, file_type);
            write(connectfd, buffer, strlen(buffer));

            long count_block;
            while((count_block = fread(&buffer, sizeof(char), file_len, page_file)) > 0) {
                write(connectfd, buffer, count_block);
            }
        }

        // Stop transmissions/receptions
        close_connection(connectfd);
    }
    close(socketfd);
    return 0;
}