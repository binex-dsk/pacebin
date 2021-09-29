#include "mongoose.h"
#include "index.h"

#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <crypt.h>
#include <time.h>

char *port = "8081";
char *data_dir = "/srv/pacebin";
char *seed = "secret";
char *proto = "http://";

static struct mg_http_serve_opts s_http_server_opts;

static void rec_mkdir(const char *dir) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp),"%s",dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    mkdir(tmp, S_IRWXU);
}

bool file_exists(char *filename) {
    struct stat buffer;
    return (stat (filename, &buffer) == 0);
}

char *get_paste_filename(char *link) {
    char *filename = malloc(strlen(data_dir) + strlen(link) + 8);
    sprintf(filename, "%s/paste/%s", data_dir, link);
    return filename;
}

char *get_del_filename(char *link) {
    char *filename = malloc(strlen(data_dir) + strlen(link) + 6);
    sprintf(filename, "%s/del/%s", data_dir, link);
    return filename;
}

bool paste_exists(char *link) {
    char *filename = get_paste_filename(link);
    bool ret = file_exists(filename);
    free(filename);
    return ret;
}

char *gen_random_link() {
    srand(time(NULL));
    char *short_link = malloc(17);
    do {
        for(size_t i = 0; i < 16; ++i) {
            sprintf(short_link + i, "%x", rand() % 16);
        }
    } while (paste_exists(short_link));
    return short_link;
}

char *gen_del_key(char *link) {
    char *salt = malloc(20);
    char *rand_str = malloc(17);
    srand(time(NULL));
    for (size_t i = 0; i < 16; ++i) {
        rand_str[i] = 37 + (rand() % 90); // random printable char
        if (rand_str[i] == 92 || rand_str[i] == 58 ||
            rand_str[i] == 59 || rand_str[i] == 42) --i; // chars not allowed for salts
    }
    rand_str[16] = 0;
    sprintf(salt, "$6$%s", rand_str);
    free(rand_str);

    char *use_link = malloc(strlen(link) + strlen(seed) + 1);
    sprintf(use_link, "%s%s", seed, link);

    char *del_key = crypt(use_link, salt);
    free(salt);
    free(use_link);

    return del_key;
}

void trim(char *str) {
    char *_str = str;
    int len = strlen(_str);

    while(*_str && *_str == '/') ++_str, --len;

    memmove(str, _str, len + 1);
}

#if DISABLE_CUSTOM_LINKS == 1
void handle_post(struct mg_connection *nc, char *content, char *host) {
    char *short_link = gen_random_link();
#else
void handle_post(struct mg_connection *nc, char *content, char *host, char *link) {
    char *short_link;
    if (strlen(link) == 0) {
        short_link = gen_random_link();
    } else if (strlen(link) >= 255) {
        return mg_http_reply(nc, 413, "", "paste link length can not exceed 255 characters");
    } else {
        short_link = strdup(link);
    }

    if (paste_exists(short_link)) {
        mg_http_reply(nc, 500, "", "a paste named %s already exists", short_link);
        return free(short_link);
    }

    if (strstr(short_link, "/")) {
        mg_http_reply(nc, 400, "", "short link can not contain slashes");
        return free(short_link);
    }
#endif

    char *paste_file = get_paste_filename(short_link);
    if (!mg_file_write(paste_file, content, strlen(content))) {
        fprintf(stderr, "failed to write to file %s", paste_file);
        mg_http_reply(nc, 500, "", "failed to write data");
        free(short_link);
        return free(paste_file);
    }
    free(paste_file);

    char *del_key = gen_del_key(short_link);
    char *del_file = get_del_filename(short_link);
    if (!mg_file_write(del_file, del_key, strlen(del_key))) {
        fprintf(stderr, "failed to write to file %s", del_file);
        mg_http_reply(nc, 500, "", "failed to write data");
        free(short_link);
        return free(del_file);
    }
    free(del_file);

    char *del_header = malloc(256);
    sprintf(del_header, "X-Delete-With: %s\r\n", del_key);

    mg_http_reply(nc, 201, del_header, "%s%s/%s", proto, host, short_link);
    free(del_header);
    free(short_link);
}

void handle_delete(struct mg_connection *nc, char *link, char *del_key) {
    if (paste_exists(link)) {
        char *del_file = get_del_filename(link);
        char *key = mg_file_read(del_file, NULL);
        if (strcmp(key, del_key) == 0) {
            char *paste_file = get_paste_filename(link);
            remove(paste_file);
            remove(del_file);
            mg_http_reply(nc, 204, "", "");

            free(paste_file);
        } else {
            mg_http_reply(nc, 403, "", "incorrect deletion key");
        }
        free(del_file);
        free(key);
    } else {
        mg_http_reply(nc, 404, "", "this paste does not exist");
    }
}

static void ev_handler(struct mg_connection *nc, int ev, void *p, void *f) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) p;
        char *uri = malloc(hm->uri.len + 1);

        snprintf(uri, hm->uri.len + 1, "%s", hm->uri.ptr);
        trim(uri);

        struct mg_str *pmhost = mg_http_get_header(hm, "Host");
        struct mg_str mhost;
        if (pmhost == NULL) {
            fprintf(stderr, "request sent with no Host header");
            mhost = mg_str("<UNKNOWN DOMAIN>");
        } else {
            mhost = *pmhost;
        }

        char *host = malloc(mhost.len + 1);
        snprintf(host, mhost.len + 1, "%s", mhost.ptr);

        char *body = strdup(hm->body.ptr);

        if (strncmp(hm->method.ptr, "POST", hm->method.len) == 0) {
#if DISABLE_CUSTOM_LINKS == 1
            handle_post(nc, body, host); // FIXME: return 400 on bad Content-Type
#else
            handle_post(nc, body, host, uri); // FIXME: return 400 on bad Content-Type
#endif
        } else if (strncmp(hm->method.ptr, "DELETE", hm->method.len) == 0) {
            handle_delete(nc, uri, body);
        } else if (strncmp(hm->method.ptr, "GET", hm->method.len) == 0) {
            if (strlen(uri) == 0) {
                mg_http_reply(nc, 200, "Content-Type: text/html\r\n", INDEX_HTML,
                                     host, host, host, host, host); // FIXME: need better solution
            } else {
                mg_http_serve_dir(nc, hm, &s_http_server_opts);
            }
        } else {
            mg_http_reply(nc, 405, "Allow: GET, POST, DELETE\r\n", "");
        }

        free(uri);
        free(host);
        free(body);
    }
}

int main(int argc, char *argv[]) {
    int index;
    int c;

    opterr = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    while ((c = getopt (argc, argv, "p:d:s:kh")) != -1) {
        switch (c) {
        case 'p':
            port = optarg;
            break;
        case 'd':
            data_dir = optarg;
            break;
        case 's':
            seed = optarg;
            break;
        case 'k':
            proto = "https://";
            break;
        case 'h':
            printf("pacebin: a minimal pastebin\n");
            printf("usage: %s [-p port] [-d data_dir] [-s seed] [-k]\n\n", argv[0]);
            printf("options:\n");
            printf("-p <port>\t\tport to use (default 8081)\n");
            printf("-d <data directory>\tdirectory to store data (default /srv/pacebin)\n");
            printf("-s <seed>\t\tsecret seed to use (DO NOT SHARE THIS; default 'secret')\n");
            printf("-k\t\t\treturns HTTPS URLs when uploading files, use with an HTTPS-enabled reverse proxy\n\n");
            printf("source: https://short.swurl.xyz/pacebin (submit bug reports, suggestions, etc. here)\n");
            return 0;
        case '?':
            if (optopt == 'p' || optopt == 'd' || optopt == 's') {
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            }
            else if (isprint (optopt)) {
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            }
            else {
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
            }
            return 1;
        default:
            abort();
        }
    }

    for (index = optind; index < argc; index++) {
        printf ("Non-option argument %s\n", argv[index]);
    }

    char *paste_dir = strcat(strdup(data_dir), "/paste");
    char *del_dir = strcat(strdup(data_dir), "/del");

    rec_mkdir(paste_dir);
    rec_mkdir(del_dir);

    free(paste_dir);
    free(del_dir);

    struct mg_mgr mgr;
    struct mg_connection *nc;

    char *root = malloc(strlen(data_dir) + 7);
    snprintf(root, strlen(data_dir) + 7, "%s/paste", data_dir);
    memset(&s_http_server_opts, 0, sizeof(s_http_server_opts));
    s_http_server_opts.root_dir = root;

    mg_mgr_init(&mgr);
    printf("Starting web server on port %s\n", port);
    char *str_port = malloc(20);
    sprintf(str_port, "http://0.0.0.0:%s", port);
    nc = mg_http_listen(&mgr, str_port, ev_handler, &mgr);
    if (nc == NULL) {
        printf("Failed to create listener\n");
        return 1;
    }

    for (;;) { mg_mgr_poll(&mgr, 1000); }
    mg_mgr_free(&mgr);
    return 0;
}

