#include "struct.h"
#include "bittorrent.h"
#include "tracker.h"
#include "peer_msg.h"
#include "downloader.h"
#include "piece_manager.h"

#define MAX_TORRENTS 16
static ActiveTorrent g_torrents[MAX_TORRENTS];
static int g_num_torrents = 0;

#define MAX_CONNECTED_PEERS 128

#define UNCHOKE_SLOTS 4
#define CHOKE_INTERVAL 10   // for fibrillation
#define OPTIMISTIC_INTERVAL 30   // rotate optimistic unchoke every 30s via WIKI

static PeerConnection *g_optimistic_peer = NULL;

static pthread_mutex_t g_peers_lock = PTHREAD_MUTEX_INITIALIZER;
static PeerConnection *g_peers[MAX_CONNECTED_PEERS];
static int g_num_peers = 0;

void register_peer(PeerConnection *peer) {
    pthread_mutex_lock(&g_peers_lock);
    if (g_num_peers < MAX_CONNECTED_PEERS)
        g_peers[g_num_peers++] = peer;
    pthread_mutex_unlock(&g_peers_lock);
}

void unregister_peer(PeerConnection *peer) {
    pthread_mutex_lock(&g_peers_lock);
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i] == peer) {
            g_peers[i] = g_peers[--g_num_peers];
            break;
        }
    }
    pthread_mutex_unlock(&g_peers_lock);
}

void broadcast_have(uint32_t piece_index) {

    int fds[MAX_CONNECTED_PEERS];
    int count = 0;

    pthread_mutex_lock(&g_peers_lock);
    for (int i = 0; i < g_num_peers; i++)
        fds[count++] = g_peers[i]->socket_fd;
    pthread_mutex_unlock(&g_peers_lock);

    uint8_t frame[9];
    uint32_t net_len   = htonl(5);
    uint32_t net_index = htonl(piece_index);
    memcpy(frame + 0, &net_len,   4);
    frame[4] = 4; // MSG_HAVE
    memcpy(frame + 5, &net_index, 4);

    for (int i = 0; i < count; i++) {

        send(fds[i], frame, sizeof(frame), MSG_NOSIGNAL);
    }
}

// helper to set up the listening socket
int setup_listening_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("Socket creation failed");
        exit(1);
    }

    // ai usage: allow immediate rebind after a crash/restart 
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }

    if (listen(sock, 10) < 0) {
        perror("Listen failed");
        exit(1);
    }

    return sock;
}

void handle_torrent_command(int port, char* filename, char* dir) {
    printf("Attempting to load torrent: %s (saving to: %s)\n", filename, dir);
    fflush(stdout);
    TorrentContext *ctx = parse_torrent(filename);

    if (ctx == NULL) {
        fprintf(stderr, "Error parsing torrent file: %s\n> ", filename);
    } else {
        // init client progress
        ClientProgress *progress = malloc(sizeof(ClientProgress));
        progress->downloaded = 0;
        progress->save_dir = strdup(dir);
        progress->uploaded = 0;
        progress->left = ctx->total_length;
        pthread_mutex_init(&progress->lock, NULL);
        clock_gettime(CLOCK_MONOTONIC, &progress->start_time);

        // one bit per piece, rounded up to nearest byte
        size_t bitfield_size = (ctx->num_pieces + 7) / 8;
        progress->my_bitfield = calloc(bitfield_size, 1);

        // one PieceState per piece
        progress->pieces = calloc(ctx->num_pieces, sizeof(PieceState));
        for (uint32_t i = 0; i < ctx->num_pieces; i++) {
            // calculate piece length FIRST
            uint32_t piece_len;
            if (i == ctx->num_pieces - 1) {
                piece_len = ctx->total_length - (uint64_t)i * ctx->piece_length;
            } else {
                piece_len = ctx->piece_length;
            }

            uint32_t num_blocks = (piece_len + MAX_BLOCK_SIZE - 1) / MAX_BLOCK_SIZE;

            progress->pieces[i].index = i;
            progress->pieces[i].length = piece_len;
            progress->pieces[i].num_blocks = num_blocks;
            progress->pieces[i].blocks_received = 0;
            progress->pieces[i].block_status = calloc(num_blocks, sizeof(bool));
            progress->pieces[i].data = malloc(piece_len);
            progress->pieces[i].is_complete = false;
            progress->pieces[i].in_progress = false;
        }
    
        uint32_t already_have = scan_existing_pieces(ctx, progress);
        if (already_have == ctx->num_pieces) {
            printf("[resume] File is already complete – nothing to download.\n");
            fflush(stdout);
        }

        printf("Successfully loaded torrent file: %s\n", filename);
        fflush(stdout);
        DownloadArgs *dargs = malloc(sizeof(DownloadArgs));
        dargs->ctx = ctx;
        dargs->progress = progress;
        dargs->save_dir = strdup(dir);
        dargs->port = port;

        //spin up the thread
        pthread_t tid;
        if (pthread_create(&tid, NULL, download, dargs) != 0) {
            fprintf(stderr, "Failed to create download thread\n");
            fflush(stdout);
            free(dargs->save_dir);
            free(dargs);
            torrent_free(ctx);
        } else {
            pthread_detach(tid);
        }
        if (g_num_torrents < MAX_TORRENTS) {
            g_torrents[g_num_torrents].ctx = ctx;
            g_torrents[g_num_torrents].progress = progress;
            g_num_torrents++;
        }
    }
}

void *handle_incoming_peer(void *arg) {
    int sock_fd = *(int *)arg;
    free(arg);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // read their handshake raw first
    struct bt_handshake_t hs;
    if (receive_handshake_incoming(sock_fd, &hs) == -1) {
        close(sock_fd);
        return NULL;
    }

    // find which torrent they want by matching info_hash
    ActiveTorrent *match = NULL;
    for (int i = 0; i < g_num_torrents; i++) {
        if (memcmp(g_torrents[i].ctx->info_hash, hs.info_hash, 20) == 0) {
            match = &g_torrents[i];
            break;
        }
    }

    if (!match) {
        close(sock_fd);
        return NULL;
    }

    PeerConnection *peer = calloc(1, sizeof(PeerConnection));
    if (!peer) { close(sock_fd); return NULL; }

    peer->socket_fd = sock_fd;
    peer->am_choking = false;
    peer->am_interested = false;
    memcpy(peer->peer_id, hs.peer_id, 20);

    if (send_handshake(peer, match->ctx->info_hash, match->ctx->peer_id) < 0) {
        close(sock_fd);
        free(peer);
        return NULL;
    }

    size_t bf_size = (match->ctx->num_pieces + 7) / 8;
    send_msg_bitfield(peer, match->progress->my_bitfield, bf_size);

    send_choke(peer); // choking thread will figure it up
    register_peer(peer);
    while (1) {
        int rc = receive_peer_message(peer, match->progress, match->ctx);
        if (rc == 0) continue;
        if (rc == RECV_TIMEOUT) {
            if (time(NULL) - peer->last_sent >= KEEP_ALIVE_INTERVAL) {
                if (send_keep_alive(peer) < 0) break;
                peer->last_sent = time(NULL);
            }
            continue;
        }
        break;
    }

    unregister_peer(peer);
    close(sock_fd);
    if (peer->bitfield) free(peer->bitfield);
    free(peer);
    return NULL;
}

static void run_choking_algorithm(bool rotate_optimistic) {
    // need to snapshot peer list within threads
    pthread_mutex_lock(&g_peers_lock);
    PeerConnection *peers[MAX_CONNECTED_PEERS];
    int n = g_num_peers;
    memcpy(peers, g_peers, n * sizeof(PeerConnection *));
    pthread_mutex_unlock(&g_peers_lock);

    // getting upload rate
    for (int i = 0; i < n; i++) {
        uint64_t sent = __atomic_exchange_n(&peers[i]->bytes_uploaded_to, 0, __ATOMIC_RELAXED);
        peers[i]->upload_rate = sent / CHOKE_INTERVAL;
    }

    // collect only peers who are interested in us
    PeerConnection *interested[MAX_CONNECTED_PEERS];
    int ni = 0;
    for (int i = 0; i < n; i++) {
        if (peers[i]->peer_interested)
            interested[ni++] = peers[i];
    }

    // sort interested peers by upload_rate descending (selection sort, n is small)
    // ai usage: bro im not gonna code a sorting algo for netowrks !!
    for (int i = 0; i < ni - 1; i++) {
        for (int j = i + 1; j < ni; j++) {
            if (interested[j]->upload_rate > interested[i]->upload_rate) {
                PeerConnection *tmp = interested[i];
                interested[i] = interested[j];
                interested[j] = tmp;
            }
        }
    }

    // unchoke top 4 (see bittorrent wiki)
    for (int i = 0; i < ni; i++) {
        if (i < UNCHOKE_SLOTS) {
            if (interested[i]->am_choking)
                send_unchoke(interested[i]);
        }
    }

    // handle optimistic unchoke
    if (rotate_optimistic || g_optimistic_peer == NULL) {
        // pick a random peer
        PeerConnection *candidates[MAX_CONNECTED_PEERS];
        int nc = 0;
        for (int i = UNCHOKE_SLOTS; i < ni; i++)
            candidates[nc++] = interested[i];

        // choke the old optimistic peer if it lost its regular slot
        if (g_optimistic_peer) {
            g_optimistic_peer->optimistic_unchoke = false;
        }

        if (nc > 0) {
            g_optimistic_peer = candidates[rand() % nc];
            g_optimistic_peer->optimistic_unchoke = true;
            if (g_optimistic_peer->am_choking)
                send_unchoke(g_optimistic_peer);
        } else {
            g_optimistic_peer = NULL;
        }
    } else {
        // verify the existing optimistic peer is still connected
        bool still_here = false;
        for (int i = 0; i < n; i++) {
            if (peers[i] == g_optimistic_peer) { still_here = true; break; }
        }
        if (!still_here) g_optimistic_peer = NULL;
    }

    // choke everyone not in a regular slot and not the optimistic peer
    for (int i = UNCHOKE_SLOTS; i < ni; i++) {
        if (interested[i] != g_optimistic_peer) {
            if (!interested[i]->am_choking)
                send_choke(interested[i]);
            interested[i]->optimistic_unchoke = false;
        }
    }
}

// ai usage = i was orginally gonna call my send_msg_cancel that was already coded but gemini said that would result in many individual peer malloc free calls because it usings send_peer_msg so now we will just build the frame in this function again
void broadcast_cancel(PeerConnection *skip_peer, uint32_t index,
                      uint32_t begin, uint32_t length) {
    // snapshot fds under lock, excluding the peer who just delivered
    int fds[MAX_CONNECTED_PEERS];
    int count = 0;

    pthread_mutex_lock(&g_peers_lock);
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i] != skip_peer)
            fds[count++] = g_peers[i]->socket_fd;
    }
    pthread_mutex_unlock(&g_peers_lock);

    // build the cancel frame directly to avoid per-peer malloc
    uint8_t frame[17];
    uint32_t net_len = htonl(13); // 1 (id) + 12 (payload)
    uint32_t net_index = htonl(index);
    uint32_t net_begin = htonl(begin);
    uint32_t net_length = htonl(length);
    memcpy(frame + 0, &net_len, 4);
    frame[4] = MSG_CANCEL;
    memcpy(frame + 5, &net_index, 4);
    memcpy(frame + 9, &net_begin, 4);
    memcpy(frame + 13, &net_length, 4);

    for (int i = 0; i < count; i++)
        send(fds[i], frame, sizeof(frame), MSG_NOSIGNAL);
}

void *monitor_thread() {
    while (true) {
        sleep(1);
        for (int i = 0; i < g_num_torrents; i++) {
            if (g_torrents[i].done_reported) continue;  // skip already reported
            ClientProgress *p = g_torrents[i].progress;
            TorrentContext *ctx = g_torrents[i].ctx;
            if (p->downloaded >= ctx->total_length) {
                struct timespec end_time;
                clock_gettime(CLOCK_MONOTONIC, &end_time);
                double dt = (end_time.tv_sec - p->start_time.tv_sec) + (end_time.tv_nsec - p->start_time.tv_nsec) / 1e9;
                printf("\n[DONE] %s downloaded!\n", ctx->file_name);
                printf("[DONE] took %02d:%02d minutes.\n> ", (int) (dt / 60), ((int) dt) % 60);
                fflush(stdout);
                g_torrents[i].done_reported = true;  // mark it
            }
        }
    }
    return NULL;
}

void *choking_thread(void *arg) {
    (void)arg;
    int cycles = 0;
    while (1) {
        sleep(CHOKE_INTERVAL);
        cycles++;
        // every 3rd cycle = 30 seconds → rotate the optimistic unchoke
        bool rotate = (cycles % (OPTIMISTIC_INTERVAL / CHOKE_INTERVAL) == 0);
        run_choking_algorithm(rotate);
    }
    return NULL;
}

#ifndef TESTING
int main(int argc, char *argv[]) {
    struct client_arguments args = parseopt(argc, argv);
   
    printf("Client started. Listening on port: %d\n", args.port);
    printf("Supported commands:\n");
    printf("> torrent <filename>.torrent <dir>\n");
    printf("> quit\n");
    printf("\n> ");
    fflush(stdout);

    int listen_sock = setup_listening_socket(args.port);

    // monitor thread
    pthread_t mon;
    pthread_create(&mon, NULL, monitor_thread, NULL);
    pthread_detach(mon);

    // choking thread
    pthread_t choke_tid;
    pthread_create(&choke_tid, NULL, choking_thread, NULL);
    pthread_detach(choke_tid);

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    
    fds[1].fd = listen_sock;
    fds[1].events = POLLIN;

    char line[512];
    while (true) {

        int ready = poll(fds, 2, -1); 
        if (ready < 0) {
            perror("Poll failed");
            break;
        }

        // user input to command line
        if (fds[0].revents & POLLIN) {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                break;
            }

            char cmd[32] = {0};
            char filename[256] = {0};
            char dir[256] = {0};

            int parsed = sscanf(line, "%31s %255s %255s", cmd, filename, dir);

            if (parsed >= 1 && strcmp(cmd, "torrent") == 0) {
                if (parsed == 3) {
                    printf("Attempting to load: %s, dir: %s\n", filename, dir);
                    fflush(stdout);
                    handle_torrent_command(args.port, filename, dir); 
                } else {
                    printf("Unknown command.\n> ");
                    fflush(stdout);
                }
            } 
            else if (parsed == 1 && (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0)) {
                printf("Shutting down BitTorrent client...\n");
                fflush(stdout);
                break;
            }
            else {
                printf("Unknown command: %s\n> ", cmd);
                fflush(stdout);
            }

            // wait on thread to finish then we want to take in new stdin? 
            // this may slow things down
            // printf("> ");
            fflush(stdout);
        }

        // handle incoming new peers
        if (fds[1].revents & POLLIN) {
            struct sockaddr_in peer_addr;
            socklen_t addr_len = sizeof(peer_addr);
            int new_peer_sock = accept(listen_sock, (struct sockaddr *)&peer_addr, &addr_len);
            
            if (new_peer_sock >= 0) {
                printf("\nnew peer connected\n> ");
                fflush(stdout);
                
                // handle incoming peer thread
                int *fd = malloc(sizeof(int));
                *fd = new_peer_sock;
                pthread_t t;
                pthread_create(&t, NULL, handle_incoming_peer, fd);
                pthread_detach(t);
            }
        }
    }

    client_args_free(&args);
    close(listen_sock);
    return 0;
}
#endif