#include "bittorrent.h"
#include "peer_msg.h"
#include "struct.h"
#include "tracker.h"
#include "piece_manager.h"
#include "downloader.h"
#define PIPELINE_DEPTH 20
#define BAR_WIDTH 35

static double elapsed_sec(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec  - start->tv_sec) +
           (now.tv_nsec - start->tv_nsec) / 1e9;
}

static void fmt_bytes(char *buf, size_t bufsz, double bytes) {
    if (bytes >= 1073741824.0) snprintf(buf, bufsz, "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1048576.0) snprintf(buf, bufsz, "%.2f MB", bytes / 1048576.0);
    else if (bytes >= 1024.0) snprintf(buf, bufsz, "%.1f KB", bytes / 1024.0);
    else snprintf(buf, bufsz, "%.0f B",  bytes);
}

static void fmt_speed(char *buf, size_t bufsz, double bps) {
    if (bps >= 1048576.0) snprintf(buf, bufsz, "%.1f MB/s", bps / 1048576.0);
    else if (bps >= 1024.0) snprintf(buf, bufsz, "%.1f KB/s", bps / 1024.0);
    else snprintf(buf, bufsz, "%.0f B/s",  bps);
}

static void fmt_eta(char *buf, size_t bufsz, double secs) {
    if (secs <= 0 || secs > 86400 * 99) { snprintf(buf, bufsz, "--:--"); return; }
    int h = (int)secs / 3600;
    int m = ((int)secs % 3600) / 60;
    int s = (int)secs % 60;
    if (h > 0) snprintf(buf, bufsz, "%02d:%02d:%02d", h, m, s);
    else snprintf(buf, bufsz, "%02d:%02d", m, s);
}

// runs once per active torrent, prints to stderr
// progress bar thread
static void *progress_display(void *arg) {
    DownloadArgs *dargs = (DownloadArgs *)arg;
    ClientProgress *prog = dargs->progress;
    TorrentContext *ctx = dargs->ctx;
    uint64_t total = ctx->total_length;

    while (1) {
        pthread_mutex_lock(&prog->lock);
        uint64_t dl = prog->downloaded;
        uint64_t left = prog->left;
        pthread_mutex_unlock(&prog->lock);

        double elapsed = elapsed_sec(&prog->start_time);
        double speed = (elapsed > 0.1) ? (double)dl / elapsed : 0.0;
        double pct = (total > 0) ? 100.0 * dl / total : 0.0;
        double eta = (speed > 0 && left > 0) ? (double)left / speed : 0.0;

        // bar
        char bar[BAR_WIDTH + 2];
        int filled = (int)(BAR_WIDTH * pct / 100.0);
        for (int i = 0; i < BAR_WIDTH; i++)
            bar[i] = (i < filled) ? '=' : (i == filled ? '>' : ' ');
        bar[BAR_WIDTH] = '\0';

        char spd[20], eta_s[12], sz_dl[20], sz_tot[20];
        fmt_speed(spd, sizeof(spd), speed);
        fmt_eta  (eta_s, sizeof(eta_s), eta);
        fmt_bytes(sz_dl, sizeof(sz_dl), (double)dl);
        fmt_bytes(sz_tot, sizeof(sz_tot), (double)total);

        // \r keeps the bar on one line; spaces at the end clear leftover chars
        fprintf(stderr,
                "\r[%s] %5.1f%% | %s | ETA %s | %s / %s   ",
                bar, pct, spd, eta_s, sz_dl, sz_tot);
        fflush(stderr);

        if (dl >= total) {
            fprintf(stderr, "\n");
            fflush(stdout);
            break;
        }
        sleep(1);
    }
    return NULL;
}

// returns piece index we that need next
int pick_piece(ClientProgress *progress, TorrentContext *ctx, PeerConnection *peer) {
    if (!peer->bitfield) return -1;
    for (uint32_t i = 0; i < ctx->num_pieces; i++) {
        if (progress->pieces[i].is_complete) continue;
        if (progress->pieces[i].in_progress)  continue;
        if (!(peer->bitfield[i / 8] & (1 << (7 - (i % 8))))) continue;
        progress->pieces[i].in_progress = true;
        return (int)i;
    }
    // ai usage: claude caught this bug 
    send_not_interested(peer); // if they dont have anything we want send this 
    return -1;
}

void cleanup(int index, void *arg) {
    PeerThreadArgs *pargs = (PeerThreadArgs *) arg;
    PeerConnection *peer = &pargs->peer;
    ClientProgress *progress = pargs->progress;

    unregister_peer(peer);
    if (index != -1) {
        pthread_mutex_lock(&progress->lock);
        if (!progress->pieces[index].is_complete)
            progress->pieces[index].in_progress = false;
        pthread_mutex_unlock(&progress->lock);
    }
    close(peer->socket_fd);
    if (peer->bitfield) free(peer->bitfield);
    free(pargs);
    return;
}

// returns 0 to keep going, -1 to drop connection
static int handle_recv_result(PeerConnection *peer, int rc, int piece_idx, void *pargs) {
    if (rc == 0) return 0; // normal message, keep going
    if (rc == RECV_TIMEOUT) {
        if (time(NULL) - peer->last_sent >= KEEP_ALIVE_INTERVAL) {
            if (send_keep_alive(peer) < 0) {
                cleanup(piece_idx, pargs);
                return -1;
            }
            peer->last_sent = time(NULL);
        }
        return 0; // not fatal, keep going
    }
    cleanup(piece_idx, pargs);
    return -1; // real error, drop
}

static bool check_endgame(ClientProgress *progress, TorrentContext *ctx) {
    if (progress->left == 0) return false;
    for (uint32_t i = 0; i < ctx->num_pieces; i++) {
        if (!progress->pieces[i].is_complete && !progress->pieces[i].in_progress)
            return false; // found a piece nobody owns yet — not endgame
    }
    return true;
}

static void send_endgame_requests(PeerConnection *peer, ClientProgress *progress,
                                  TorrentContext *ctx) {
    pthread_mutex_lock(&progress->lock);
    for (uint32_t i = 0; i < ctx->num_pieces; i++) {
        PieceState *p = &progress->pieces[i];
        if (p->is_complete) continue;

        // only request from peers that actually have this piece
        if (!peer->bitfield || !(peer->bitfield[i/8] & (1 << (7 - (i % 8)))))
            continue;

        for (uint32_t b = 0; b < p->num_blocks; b++) {
            if (p->block_status[b]) continue;   // already have this block

            uint32_t offset = b * MAX_BLOCK_SIZE;
            uint32_t blk_len = p->length - offset;
            if (blk_len > MAX_BLOCK_SIZE) blk_len = MAX_BLOCK_SIZE;

            pthread_mutex_unlock(&progress->lock);
            send_request(peer, i, offset, blk_len);
            pthread_mutex_lock(&progress->lock);
        }
    }
    pthread_mutex_unlock(&progress->lock);
}

void *peer_worker(void *arg) {
    PeerThreadArgs *pargs = (PeerThreadArgs *) arg;
    PeerConnection *peer = &pargs->peer;
    TorrentContext *ctx = pargs->ctx;
    ClientProgress *progress = pargs->progress;

    int current_piece_idx = -1;

    // setup connection
    if (!pargs->is_incoming) {
        // we need to connect (outgoing)
        int sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            free(pargs);
            return NULL;
        }

        // set a timeout so we don't hang forever on a chud
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // connect to peer
        struct sockaddr_in addr = {0};
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = peer->ip;
        addr.sin_port = peer->port;

        if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
            close(sock);
            free(pargs);
            return NULL;
        }
        peer->socket_fd = sock;

        if (send_handshake(peer, ctx->info_hash, ctx->peer_id) < 0) {
            close(peer->socket_fd);
            free(pargs);
            return NULL;
        }
        if (receive_handshake(peer, ctx->info_hash) < 0) {
            close(peer->socket_fd);
            free(pargs);
            return NULL;
        }

        char *ip_str = inet_ntoa(addr.sin_addr);
        printf("Connected to peer IP: %s\n", ip_str);
        fflush(stdout);

    } else {
        // incoming handshake, we already have the socket but we are waiting for
        // their socket
        if (receive_handshake(peer, ctx->info_hash) < 0) {
            close(peer->socket_fd);
            free(pargs);
            return NULL;
        }
        if (send_handshake(peer, ctx->info_hash, ctx->peer_id) < 0) {
            close(peer->socket_fd);
            free(pargs);
            return NULL;
        }
    }

    peer->am_choking = true;
    peer->am_interested = false;
    peer->peer_choking = true;
    peer->peer_interested = false;

    // changed from / 8 to >> 3 for speed (this is most definitely already done
    // even if i dont manually bit shift)
    size_t bitfield_bytes = (ctx->num_pieces + 7) >> 3;
    peer->bitfield = calloc(bitfield_bytes, 1);
    if (!peer->bitfield) { close(peer->socket_fd); free(pargs); return NULL; } // if it failed just give up (close the sock)

    send_msg_bitfield(peer, progress->my_bitfield, bitfield_bytes);

    register_peer(peer);

    pthread_mutex_lock(&progress->lock);
    int needs_download = (progress->left > 0);
    pthread_mutex_unlock(&progress->lock);

    if (needs_download) {
        if (send_interested(peer) < 0) {
            cleanup(current_piece_idx, pargs);
            return NULL;
        }
    }

    // download loop for this specific peer
    while (1) {
        // check if download finished
        pthread_mutex_lock(&progress->lock);
        int is_done = (progress->left == 0);
        pthread_mutex_unlock(&progress->lock);

        if (is_done) {
            int rc = receive_peer_message(peer, progress, ctx);
            if (handle_recv_result(peer, rc, current_piece_idx, pargs) < 0)
                return NULL;
            continue;
        }

        if (peer->peer_choking) {
            int rc = receive_peer_message(peer, progress, ctx);
            if (handle_recv_result(peer, rc, current_piece_idx, pargs) < 0)
                return NULL;
            continue;
        }

        // init our current_piece_idx 
        if (current_piece_idx < 0) {
            pthread_mutex_lock(&progress->lock);
            current_piece_idx = pick_piece(progress, ctx, peer);
            pthread_mutex_unlock(&progress->lock);
        }

        // if theres nothing to download from this peer cleanup and move on
        if (current_piece_idx < 0) {
            pthread_mutex_lock(&progress->lock);
            if (!progress->endgame && check_endgame(progress, ctx)) {
                progress->endgame = true;
                printf("[endgame] entering endgame mode — blasting remaining blocks\n");
                fflush(stdout);
            }
            bool eg = progress->endgame;
            pthread_mutex_unlock(&progress->lock);

            if (eg && !peer->peer_choking) {
                send_endgame_requests(peer, progress, ctx);
            }

            if (receive_peer_message(peer, progress, ctx) < 0) {
                cleanup(current_piece_idx, pargs);
                return NULL;
            }
            continue;
        }

        PieceState *piece = &progress->pieces[current_piece_idx];
        // printf("Requesting piece %d from peer\n", piece_idx);

        uint32_t queued = 0;
        uint32_t next_block = 0;

        // fill the pipeline
        while (queued < PIPELINE_DEPTH && next_block < piece->num_blocks) {
            pthread_mutex_lock(&progress->lock);
            bool has = piece->block_status[next_block];
            pthread_mutex_unlock(&progress->lock);

            if (!has) {
                uint32_t offset = next_block * MAX_BLOCK_SIZE;
                uint32_t blk_len = piece->length - offset;
                if (blk_len > MAX_BLOCK_SIZE) blk_len = MAX_BLOCK_SIZE;

                if (send_request(peer, current_piece_idx, offset, blk_len) < 0) {
                    cleanup(current_piece_idx, pargs);
                    return NULL;
                }
                queued++;
            }
            next_block++;
        }

        if (queued == 0) {
            int rc = receive_peer_message(peer, progress, ctx);
            if (handle_recv_result(peer, rc, current_piece_idx, pargs) < 0)
                return NULL;
        }

        while (1) {
            pthread_mutex_lock(&progress->lock);
            int complete = piece->is_complete;
            pthread_mutex_unlock(&progress->lock);
            
            if (complete) break;

            int newly_completed = -1;
            int rc = receive_peer_message_ex(peer, progress, ctx, &newly_completed);
            if (handle_recv_result(peer, rc, current_piece_idx, pargs) < 0)
                return NULL;
            if (newly_completed >= 0)
                broadcast_have((uint32_t)newly_completed);
        }
        
        // unclaim and reset for the next loop
        current_piece_idx = -1;
    }

    double elapsed = elapsed_sec(&progress->start_time);
    char sz[24], spd[24];
    fmt_bytes(sz,  sizeof(sz),  (double)ctx->total_length);
    fmt_speed(spd, sizeof(spd), (double)ctx->total_length / elapsed);

    pthread_mutex_lock(&progress->lock);
    printf("\n[DONE] %s in %.2fs (%s)\n> ", sz, elapsed, spd);
    pthread_mutex_unlock(&progress->lock);

    fflush(stdout);

    close(peer->socket_fd);
    free(peer->bitfield);
    free(pargs);
    return NULL;
}

void *download(void *arg) {
    DownloadArgs *dargs = (DownloadArgs *)arg;
    TorrentContext *ctx = dargs->ctx;
    ClientProgress *progress = dargs->progress;

    clock_gettime(CLOCK_MONOTONIC, &progress->start_time);

    // if resume scan is already completed, nth to download
    // still need to register with tracker as seeder!1
    pthread_mutex_lock(&progress->lock);
    int already_complete = (progress->left == 0);
    pthread_mutex_unlock(&progress->lock);

    if (already_complete) {
        printf("[resume] Download already complete – seeding only.\n");
        fflush(stdout);
        return NULL;
    }

    // progress bar on a background thread 
    // just for aesthetics meow
    pthread_t prog_tid;
    if (pthread_create(&prog_tid, NULL, progress_display, dargs) == 0)
        pthread_detach(prog_tid);

    TrackerResponse tracker = {0};
    int rval = contact_tracker_with_list(ctx, progress, "started", dargs->port, &tracker);

    if (rval != 0 || !tracker.success) {
        fprintf(stderr, "Tracker contact failed: %s\n", tracker.failure_reason);
        return NULL;
    }
    printf("Tracker returned %d peer(s)\n", tracker.num_peers);
    fflush(stdout);

    // ---------make tcp connection with every peer ------------
    for (int i = 0; i < tracker.num_peers; i++) {
        PeerConnection *peer = &tracker.peers[i];

        // skip ourselves
        if (memcmp(peer->peer_id, ctx->peer_id, 20) == 0) continue;

        // package arguments for the thread
        PeerThreadArgs *pargs = malloc(sizeof(PeerThreadArgs));
        if (!pargs) continue;
        pargs->peer = *peer; 
        pargs->ctx = ctx;
        pargs->progress = progress;
        pargs->is_incoming = false;

        pthread_t tid;
        if (pthread_create(&tid, NULL, peer_worker, pargs) == 0) {
            pthread_detach(tid); // make it independent
        } else {
            free(pargs);
        }
    }

    return NULL;
}
