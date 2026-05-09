#include "struct.h"
#include "tracker.h"


// send get request


// helper function that does url encoding
char* url_encode(const uint8_t *bindata, size_t len) {
    char *strbfr = malloc((len*3) + 1); // 3 chars per byte + 1 for null temrinator
    char *current = strbfr;
    for (size_t i = 0; i < len; i++) {
        sprintf(current, "%%%02X", bindata[i]); // %% escape sequence prints %, pad with leading zeros, min width of 2, uppercase hex
        current += 3;
    }
    *current = '\0'; // null terminator
    return strbfr;
}


// puts together the above encoded parameters with tracker_url
char* build_tracker_url(const TorrentContext *ctx, 
                        const ClientProgress *progress, const char *event, 
                        int port) {
    char *hash = url_encode(ctx->info_hash, 20);
    char *peer = url_encode(ctx->peer_id, 20);
    size_t maxlen = strlen(ctx->tracker_url) + 512;
    char *urlbfr = malloc(maxlen); // *remember to free after http request is sent
    snprintf(urlbfr, maxlen, "%s?info_hash=%s&peer_id=%s&port=%d&uploaded=%lu&downloaded=%lu&left=%lu&event=%s&compact=1&no_peer_id=1", // AI added compact and no_peer_id parameters
                ctx->tracker_url, hash, peer, port, progress->uploaded, 
                progress->downloaded, progress->left, event);


    free(hash);
    free(peer);
    return urlbfr;
}


#ifdef LOOPBACK_TEST
int contact_tracker(const char *url, TrackerResponse *response) {
    (void)url;


    fprintf(stderr, "[TRACKER-STUB] LOOPBACK_TEST active – returning 127.0.0.1:6881\n");


    response->success   = true;
    response->interval  = 60;
    response->num_peers = 1;
    response->peers     = calloc(1, sizeof(PeerConnection));
    if (!response->peers) {
        response->success = false;
        snprintf(response->failure_reason, sizeof(response->failure_reason),
                 "calloc failed in loopback stub");
        return -1;
    }


    // ip in network byte order – inet_addr() already returns NBO
    response->peers[0].ip   = inet_addr("127.0.0.1");
    // port in network byte order to match what handle_peers_list stores
    response->peers[0].port = htons(6881);
    // peer_id left as all-zeros; peer_worker overwrites it after the handshake


    return 0;
}
#else
// host is everything btwn "http://" and ':', 
int contact_tracker(const char *url, TrackerResponse *response) { // make sure to free url whenever this function is used!!!
    // skip "http://"
    const char *host_start = url + 7;


    // find the colon for port and slash for path
    const char *slash = strchr(host_start, '/');


    char host[256];
    int port;


    if (*host_start == '[') { // AI written code lines 84-108; to add support IPv6 addresses
        const char *close = strchr(host_start, ']');
        if (!close) return -1;


        size_t host_len = close - (host_start + 1);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, host_start + 1, host_len);
        host[host_len] = '\0';


        if (close[1] != ':') return -1;
        port = atoi(close + 2);
    } else {
        const char *colon = strchr(host_start, ':');
        if (!colon) return -1;


        // "hostname:port/path"
        size_t host_len = colon - host_start;
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
        port = atoi(colon + 1);
    }


    // int getaddrinfo(const char *node, // hostname or IP address string
    //             const char *service, // service name, "http" or port "80" as string
    //             const struct addrinfo *hints, // optional structure to filter results
    //             struct addrinfo **res); // pointer to a pointer where the function will store the head of a dynamically allocated linked list of addrinfo structures
    int sock;


    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;


    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);


    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0) {
        response->success = false;
        snprintf(response->failure_reason, 512, "could not resolve host: %s", host);
        return -1;
    }


    if ((sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
        perror("socket() failed");
        freeaddrinfo(res);
        exit(1);
    }


    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock);
        freeaddrinfo(res);
        response->success = false;
        snprintf(response->failure_reason, 256, "connect() failed");
        return -1;
    }


    freeaddrinfo(res);


    // if no slash was found default to root path
    const char *path = slash ? slash : "/";
    char request[4096];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    send(sock, request, strlen(request), 0);


    char buffer[8192]; // read tracker's response into a buffer
    // bittorrent responses are bencoded dictionaries
    int total = 0;
    int bytes = 0;
    // AI suggested a loop because recv doesn't guarantee everything is returned at once
    // only returns how many bytes available in network buffer at that moment
    // handles response arriving in multiple chunks
    while ((bytes = recv(sock, buffer + total, sizeof(buffer) - total - 1, 0)) > 0)
        total += bytes;
    buffer[total] = '\0';


    // find start of bencoded data
    // AI suggested strstr and got rid of my while loop where i manually scanned it
    char *body = strstr(buffer, "\r\n\r\n");
    if (!body) {
        close(sock);
        response->success = false;
        return -1;
    }
    body += 4; // skip past \r\n\r\n


    // number of bytes belonging to body: total - (body - buffer)
    response->success = true;
    handle_response(body, total - (body - buffer), response);
    close(sock);
    return 0;
}
#endif


// dealing with announce-list
int contact_tracker_with_list(const TorrentContext *ctx,
                              const ClientProgress *progress, const char *event,
                              int port, TrackerResponse *response) {
    if (ctx->announce_list == NULL) { // see if it actually exists because it's an optional field
        // just use tracker_url field
        char *full_url = build_tracker_url(ctx, progress, event, port);
        int result = contact_tracker(full_url, response);
        free(full_url);
        return result;
    }


    int i = 0;
    while (ctx->announce_list[i] != NULL) {
        int j = 0;
        while (ctx->announce_list[i][j] != NULL) {
            // try each url
            char *tracker_url = ctx->announce_list[i][j];


            if (strncmp(tracker_url, "http://", 7) != 0) {
                j++;
                continue;
            }


            TorrentContext temp = *ctx; // make local copy to not change the caller's struct
            temp.tracker_url = (char *)tracker_url;
            char *full_url = build_tracker_url(&temp, progress, event, port);
            printf("[announce-list] using %s\n", full_url);
            fflush(stdout);
            int result = contact_tracker(full_url, response);
            free(full_url);
            if (result == 0) {
                return 0; // this tracker url works, stop trying the rest
            }
            j++;
        }
        i++;
    }


    // fallback to tracker_url
    char *full_url = build_tracker_url(ctx, progress, event, port);
    printf("[tracker_url fallback] using %s\n", full_url);
    fflush(stdout);
    int result = contact_tracker(full_url, response);
    free(full_url);
    return result;
}


void handle_response(const char *bendata, size_t len, TrackerResponse *response) {
    const char *ptr = bendata;
    const char *endptr = bendata + len;


    if (*ptr != 'd') { // must start w dictionary, checks the first byte is d
        response->success = false;
        return;
    }
    ptr++; // skip 'd'


    while (ptr < endptr && *ptr != 'e') { // each iteration reads a key-value pair
        char *end;
        long keylen = strtol(ptr, &end, 10);
        char *keystr = end + 1;
        char *valptr = keystr + keylen;


        // failure
        if (keylen == 14 && strncmp(keystr, "failure reason", 14) == 0) {
            response->success = false;
            long vallen = strtol(valptr, &end, 10);
            memcpy(response->failure_reason, end + 1, vallen);
            response->failure_reason[vallen] = '\0';
            return;
        } else if (keylen == 8 && strncmp(keystr, "interval", 8) == 0) {
            char *tmp;
            response->interval = strtol(valptr + 1, &tmp, 10);
            ptr = tmp + 1;
        } else if (keylen == 5 && strncmp(keystr, "peers", 5) == 0) {
            // right now only support for non compact format
            ptr = handle_peers_list(response, valptr); 
        } else {
            ptr = skip_bencode(valptr);
        }
    }
}


char* handle_peers_list(TrackerResponse *response, char *valptr) {
    char *end;
    if (*valptr == 'l') {
        char *ptr = valptr + 1;
        
        // count peers to know how much memory to alloc
        int count = 0;
        char *countptr = ptr;
        while (*countptr != 'e') {
            countptr = skip_bencode(countptr);
            count++;
        }

        printf("\n[peers] found %d peer(s) in dictionary format\n", count);
        fflush(stdout);
        
        response->num_peers = count;
        response->peers = malloc(sizeof(PeerConnection) * count);


        // parse peer dictionaries
        for (int i = 0; i < count; i++) {
            ptr++; // skip 'd'
            while (*ptr != 'e') {
                long keylen = strtol(ptr, &end, 10);
                char *keystr = end + 1;
                char *v_ptr = keystr + keylen;


                if (strncmp(keystr, "ip", 2) == 0) {
                    long ip_len = strtol(v_ptr, &end, 10);
                    char ip_buf[16];
                    memcpy(ip_buf, end + 1, ip_len);
                    ip_buf[ip_len] = '\0';
                    response->peers[i].ip = inet_addr(ip_buf);
                    ptr = end + 1 + ip_len;
                } else if (strncmp(keystr, "port", 4) == 0) {
                    response->peers[i].port = htons((uint16_t)strtol(v_ptr + 1, &ptr, 10));
                    ptr++; // skip 'e'
                } else if (strncmp(keystr, "peer id", 7) == 0) {
                    long id_len = strtol(v_ptr, &end, 10);
                    memcpy(response->peers[i].peer_id, end + 1, id_len);
                    ptr = end + id_len + 1;
                } else {
                    ptr = skip_bencode(v_ptr);
                }
            }
            ptr++; // skip dicitonary 'e'
        }
        return ptr + 1; // skip list 'e'
    } else if (*valptr >= '0' && *valptr <= '9') {
        // compact format does not have peer id, so get peer id when contacting the peer during handshake
        char *end;
        int str_len = strtol(valptr, &end, 10);

        printf("\n[peers] compact format detected, %d byte(s) = %d peer(s)\n", str_len, str_len / 6);
        fflush(stdout);

        char *ptr = end + 1;
        response->num_peers = str_len / 6; // string length divided by 6, 6 bytes per peer
        response->peers = malloc(sizeof(PeerConnection) * response->num_peers);


        for (int i = 0; i < response->num_peers; i++) {
            // ip is 4 bytes, port is 2 bytes
            memcpy(&response->peers[i].ip, ptr,4); // already in big endian so no need to convert
            ptr += 4;
            memcpy(&response->peers[i].port, ptr, 2);
            ptr += 2;
        }
        return ptr;
    }


    return skip_bencode(valptr);
}