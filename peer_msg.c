#include "struct.h"
#include "peer_msg.h"
#include "piece_manager.h"
#include "bittorrent.h"

// ai usage: suggested a while loop for recieved and sent handshake bits just in case we didn't receive and send everything in one go
// moved while loop to an aux function(s)
// all functions return 0 on success and -1 on failure

int send_all(int socket_fd, uint8_t *buf, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t s = send(socket_fd, buf + total_sent, len - total_sent, 0);
        if (s < 0) {
            return -1;
        }
        total_sent += s;
    }
    return 0;
}

int recv_all(int socket_fd, uint8_t *buf, size_t len) {
    size_t total_received = 0;
    while (total_received < len) {
        ssize_t r = recv(socket_fd, buf + total_received, len - total_received, 0);
        if (r <= 0) {
            // ai usage: make recv_all signal timeouts more distinctly
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return RECV_TIMEOUT;
            return -1;   // real error or closed connection
        }
        total_received += r;
    }
    return 0;
}

// send handshake 
// assumes tcp connect is established
int send_handshake(PeerConnection *peer, const uint8_t *info_hash, const uint8_t *my_peer_id) {
    // creates handshake 
    struct bt_handshake_t handshake; 
    memset(&handshake, 0, sizeof(handshake)); // sets reserved bits to 0
    handshake.pstrlen = 19; 
    memcpy(handshake.pstr, BT_PROTOCOL_STRING, 19);
    memcpy(handshake.info_hash, info_hash, 20);
    memcpy(handshake.peer_id, my_peer_id, 20);

    return send_all(peer->socket_fd, (uint8_t*)&handshake, sizeof(handshake));
}

// recieve handshake
// if function returns -1, drop the connection
int receive_handshake(PeerConnection *peer, uint8_t *expected_info_hash) {
    struct bt_handshake_t received; 

    if (recv_all(peer->socket_fd, (uint8_t *)&received, sizeof(received)) == -1) { // didn't recieve all 
        close(peer->socket_fd); 
        peer->socket_fd = -1;
        return -1;
    } 

    // checking for incorrect fields
    if ((received.pstrlen != 19) || 
        (memcmp(received.pstr, BT_PROTOCOL_STRING, 19) != 0) ||
            (memcmp(received.info_hash, expected_info_hash, 20) != 0)) {
                close(peer->socket_fd); // close socket
                peer->socket_fd = -1;
                return -1;
    }

    // if it passes all the checks, store peer_id, and keep socket open
    memcpy(peer->peer_id, received.peer_id, 20); 
    return 0; 
}

// used specifically for when a new peer connects
// caller decicdes what happens
int receive_handshake_incoming(int sock_fd, struct bt_handshake_t *out) {
    if (recv_all(sock_fd, (uint8_t *)out, sizeof(*out)) < 0) return -1;
    if (out->pstrlen != 19) return -1;
    if (memcmp(out->pstr, BT_PROTOCOL_STRING, 19) != 0) return -1;
    return 0;
}

// ai usage: suggested a general send function with send functions specific peer messages for better readabilty + code abstraction

// general wrapper for sending peer messages
int send_peer_message(PeerConnection *peer, bt_message_id_t msg_id, const void *payload, uint32_t payload_len) {
    struct bt_msg_header_t header; 
    header.length = htonl(1 + payload_len);
    header.message_id = msg_id;

    // buffer to send header and payload
    size_t total_size = sizeof(header) + payload_len;
    uint8_t *buf = malloc(total_size);
    if (!buf) return -1; // if malloc returns null 

    memcpy(buf, &header, sizeof(header));
    if (payload != NULL && payload_len > 0) {
        memcpy(buf + sizeof(header), payload, payload_len);
    }

    int ret = send_all(peer->socket_fd, buf, total_size);

    peer->last_sent = time(NULL);
    free(buf);

    return ret;
}

// the send function does the sending but we need to change our bit
// call these functions rather than send_peer_message directly
int send_choke(PeerConnection *peer) {
    if (send_peer_message(peer, MSG_CHOKE, NULL, 0) == 0) {
        peer->am_choking = true;
        return 0;
    }
    return -1;
}

int send_unchoke(PeerConnection *peer) {
    if (send_peer_message(peer, MSG_UNCHOKE, NULL, 0) == 0) {
        peer->am_choking = false;
        return 0;
    }
    return -1;
}
 
int send_interested(PeerConnection *peer) {
    if (send_peer_message(peer, MSG_INTERESTED, NULL, 0) == 0) {
        peer->am_interested = true;
        return 0;
    }
    return -1;
}

int send_not_interested(PeerConnection *peer) {
    if (send_peer_message(peer, MSG_NOT_INTERESTED, NULL, 0) == 0) {
        peer->am_interested = false;
        return 0;
    }
    return -1;
}

// use index returned from store_block function and call this function to alert peers that u have the piece
int send_msg_have(PeerConnection *peer, uint32_t piece_index) {
    struct bt_payload_have_t have;
    have.piece_index = htonl(piece_index);
    return send_peer_message(peer, MSG_HAVE, &have, sizeof(have));
}

// call once right after handshake
int send_msg_bitfield(PeerConnection *peer, uint8_t *bitfield, uint32_t bitfield_size) {
    return send_peer_message(peer, MSG_BITFIELD, bitfield, bitfield_size);
}

// need to indices to Big-Endian 
int send_request(PeerConnection *peer, uint32_t index, uint32_t begin, uint32_t length) {
    struct bt_payload_request_cancel_t req;
    req.index = htonl(index);
    req.begin = htonl(begin);
    req.length = htonl(length);
    
    return send_peer_message(peer, MSG_REQUEST, &req, sizeof(req));
}

int send_piece(PeerConnection *peer, uint32_t index, uint32_t begin, const uint8_t *block, uint32_t length) {
    size_t payload_len = 8 + length; // 2 uint32_ts + block length

    uint8_t *payload = malloc(payload_len);
    if (!payload) return -1;

    uint32_t net_index = htonl(index);
    uint32_t net_begin = htonl(begin);

    // pack the payload
    memcpy(payload, &net_index, 4);
    memcpy(payload + 4, &net_begin, 4);
    memcpy(payload + 8, block, length);

    int ret = send_peer_message(peer, MSG_PIECE, payload, payload_len);
    free(payload);
    return ret;
}

// need to keep track of recieved keep alive pings
int send_keep_alive(PeerConnection *peer) {
    uint32_t length = 0;
    return send_all(peer->socket_fd, (uint8_t*)&length, 4);
}

// Thin wrapper kept for all existing call sites.
int receive_peer_message(PeerConnection *peer, ClientProgress *progress,
                         TorrentContext *torrent) {
    return receive_peer_message_ex(peer, progress, torrent, NULL);
}

int receive_peer_message_ex(PeerConnection *peer, ClientProgress *progress,
                             TorrentContext *torrent, int *completed_piece) {
    uint32_t len_prefix;
    
    int rc = recv_all(peer->socket_fd, (uint8_t *)&len_prefix, 4);
    if (rc == RECV_TIMEOUT) return RECV_TIMEOUT;
    if (rc == -1) return -1;

    len_prefix = ntohl(len_prefix);

    // check for keep-alive ping
    if (len_prefix == 0) {
        peer->last_active = time(NULL);
        return 0;
    }

    // else get the message id
    uint8_t id;
    if (recv_all(peer->socket_fd, &id, 1) == -1) {
        return -1; 
    }

    // calc payload length and get it
    uint32_t payload_len = len_prefix - 1;
    uint8_t *payload = NULL;
    if (payload_len > 0) {
        payload = malloc(payload_len);
        if (!payload) return -1;
        if (recv_all(peer->socket_fd, payload, payload_len) == -1) {
            free(payload);
            return -1;
        }
    }
    // parse the msg ids
    switch (id) {
        case MSG_CHOKE: {
            peer->peer_choking = true;
            break;
        }
        case MSG_UNCHOKE: {
            peer->peer_choking = false;
            break;
        }
        case MSG_INTERESTED: {
            peer->peer_interested = true;
            break;
        }
        case MSG_NOT_INTERESTED: {
            peer->peer_interested = false;
            break;
        }
        // ai usage: i didn't know how to correctly update the bitfield so i was discussing with gemini how bit torrent does it
        case MSG_HAVE: {
            uint32_t index;
            memcpy(&index, payload, 4);
            index = ntohl(index);
            if (index < torrent->num_pieces) {
                if (!peer->bitfield) {
                    size_t sz = (torrent->num_pieces + 7) / 8;
                    peer->bitfield = calloc(sz, 1);
                    peer->bitfield_size = sz;
                }
                peer->bitfield[index/8] |= (1 << (7 - (index % 8)));

                // if we don't have that piece, express interest
                if (!progress->pieces[index].is_complete) {
                    send_interested(peer);
                }
            }
            break;
        }
        case MSG_BITFIELD: {
            if (peer->bitfield) free(peer->bitfield);
            peer->bitfield_size = payload_len;
            peer->bitfield = malloc(payload_len);
            if (!peer->bitfield) { free(payload); return -1; }
            memcpy(peer->bitfield, payload, payload_len);

            for (uint32_t i = 0; i < torrent->num_pieces; i++) {
                if (!progress->pieces[i].is_complete &&
                    (peer->bitfield[i/8] & (1 << (7 - (i % 8))))) {
                    send_interested(peer);
                    break;
                }
            }
            break;
        }
        case MSG_REQUEST: {
            if (peer->am_choking) break; // if we are choking them just ignore them

            struct bt_payload_request_cancel_t *req = (struct bt_payload_request_cancel_t *)payload;
            uint32_t idx = ntohl(req->index);
            uint32_t begin = ntohl(req->begin);
            uint32_t length = ntohl(req->length);

            // ai usage: i didn't account for this
            if (length > MAX_BLOCK_SIZE) break;

            // if valid index and we have it
            if (idx < torrent->num_pieces && progress->pieces[idx].is_complete) {
                uint8_t *block = malloc(length);
                if (!block) {
                    free(payload);
                    return -1; 
                }
                if (read_block_from_disk(torrent, progress->save_dir, idx, begin, length, block) == 0) {
                    if (send_piece(peer, idx, begin, block, length) == -1) {
                        free(block);
                        free(payload);
                        return -1;
                    }
                    pthread_mutex_lock(&progress->lock);
                    progress->uploaded += length;
                    pthread_mutex_unlock(&progress->lock);
                    // ai usage: __ATOMIC_RELAXED -> fast, doesn't need to sync now but needs to later for final accurate result
                    // adds length to pointer
                    __atomic_fetch_add(&peer->bytes_uploaded_to, length, __ATOMIC_RELAXED);

                }
                free(block);
            }
            break;
        }
        case MSG_PIECE: {
            if (payload_len < 8) break;
            uint32_t index, begin;
            memcpy(&index, payload, 4);
            memcpy(&begin, payload + 4, 4);
            index = ntohl(index);
            begin = ntohl(begin);

            uint8_t *block_data = payload + 8;
            uint32_t block_len = payload_len - 8;

            if (index < torrent->num_pieces) {
                int result = store_block(progress, torrent, index, begin, block_data, block_len);
                if (result >= 0) {
                    if (completed_piece != NULL)
                        *completed_piece = result;
                }

                // endgame mode
                pthread_mutex_lock(&progress->lock);
                bool eg = progress->endgame;
                pthread_mutex_unlock(&progress->lock);

                if (eg) {
                    uint32_t blk_len = MAX_BLOCK_SIZE;
                    if (begin + blk_len > progress->pieces[index].length)
                        blk_len = progress->pieces[index].length - begin;
                    broadcast_cancel(peer, index, begin, blk_len);
                }
            }
            break;
        }
        case MSG_CANCEL: {
            // TODO
            break;
        }
        default: {
            fprintf(stderr, "Unknown Message ID: %u\n", id);
            break;
        }
    }
    peer->last_active = time(NULL);
    if (payload) free(payload);
    return 0;
}