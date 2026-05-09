// ai usage: used gemini to refine structures; implemented a better naming convention and suggested type enum for msg ids
#ifndef PEER_MSG_H
#define PEER_MSG_H

#include "struct.h"
#define KEEP_ALIVE_INTERVAL 120 // seconds between keep-alives
#define RECV_TIMEOUT -2 // recv timed out (not a fatal error)

#define BT_PROTOCOL_STRING "BitTorrent protocol"

typedef enum {
    MSG_CHOKE = 0,
    MSG_UNCHOKE = 1,
    MSG_INTERESTED = 2,
    MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4,
    MSG_BITFIELD = 5,
    MSG_REQUEST = 6,
    MSG_PIECE = 7,
    MSG_CANCEL = 8
} bt_message_id_t;

// packed attribute ensures the compiler doesn't insert empty padding bytes
// recall the ec for udp project

struct __attribute__((packed)) bt_handshake_t {
    uint8_t pstrlen; // 19
    char pstr[19]; // “BitTorrent protocol” string
    uint8_t reserved[8]; // all zeros
    uint8_t info_hash[20]; // SHA1 hash of the info dictionary
    uint8_t peer_id[20]; // unique id for the client
};

struct __attribute__((__packed__)) bt_msg_header_t {
    uint32_t length; // msg length
    uint8_t message_id; // uses bt_message_id_t enum
};

struct __attribute__((__packed__)) bt_payload_have_t {
    uint32_t piece_index; // index of a piece just downloaded
};

struct __attribute__((__packed__)) bt_payload_request_cancel_t{
    uint32_t index;
    uint32_t begin; // byte offset within the piece
    uint32_t length; // requested length (usually 16KB)
};

struct __attribute__((__packed__)) bt_payload_piece_t {
    uint32_t index;
    uint32_t begin; // byte offset within the piece
    uint8_t block[]; // actual subset of the piece (variable length)
};

// in our google doc we had a bit field peer message but due to it's dynamic nature, we will NOT make a c struct for this message type

// function prototypes
int send_handshake(PeerConnection *peer, const uint8_t *info_hash, const uint8_t *my_peer_id);

int receive_handshake(PeerConnection *peer, uint8_t *expected_info_hash);

int send_peer_message(PeerConnection *peer, bt_message_id_t msg_id, const void *payload, uint32_t payload_len);

// sends peer messages + does local changes
int send_choke(PeerConnection *peer);
int send_unchoke(PeerConnection *peer);
int send_interested(PeerConnection *peer);
int send_not_interested(PeerConnection *peer);
int send_msg_have(PeerConnection *peer, uint32_t piece_index);
int send_msg_bitfield(PeerConnection *peer, uint8_t *bitfield, uint32_t bitfield_size);
int send_request(PeerConnection *peer, uint32_t index, uint32_t begin, uint32_t length);
int send_piece(PeerConnection *peer, uint32_t index, uint32_t begin, const uint8_t *block, uint32_t length);
int send_keep_alive(PeerConnection *peer);

int receive_peer_message(PeerConnection *peer, ClientProgress *progress, TorrentContext *ctx);

int receive_handshake_incoming(int sock_fd, struct bt_handshake_t *out);

int receive_peer_message_ex(PeerConnection *peer, ClientProgress *progress, TorrentContext *torrent, int *completed_piece);

#endif