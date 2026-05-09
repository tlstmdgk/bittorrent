#ifndef STRUCT_H
#define STRUCT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stddef.h>
#include <stdio.h>
#include <strings.h>
#include <argp.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <inttypes.h>

#define MAX_PEER 50
#define MAX_BLOCK_SIZE 16384

struct client_arguments {
    int port; // port to listen to
};

typedef struct {
    uint8_t info_hash[20];
    uint8_t peer_id[20]; // my peer ID assigned by tracker
    
    char *tracker_url; // announce URL
    char ***announce_list; // list of tiers, where each tier is itself a list of tracker URLs
    
    char *file_name;
    uint64_t total_length;
    uint32_t piece_length;
    uint32_t num_pieces;
    
    uint8_t *piece_hashes; // large array with expected 20-byte SHA-1 hash for every piece
} TorrentContext;

typedef struct {
    uint8_t peer_id[20];
    int socket_fd; // active TCP socket
    uint32_t ip; // network byte order
    uint16_t port;
    
    bool am_choking; // true: I am choking them
    bool am_interested; // true: I want something they have
    bool peer_choking; // true: They are choking me
    bool peer_interested; // true: They want something I have
    
    // peer capabilities
    uint8_t *bitfield; // array of bits representing which pieces this peer possesses
    uint32_t bitfield_size; // length of bitfield

    // network buffers
    uint8_t *recv_buffer; // buffer for partially received messages
    size_t recv_len; // how many bytes are currently in the buffer
    
    time_t last_active; // timestamp for keep-alive checks
    time_t last_sent;

    // ai usage: gemini suggested using an _Atomic version of uint64_t to ensure no thread issues 
    // stores bytes sent to this peer since last interval
    _Atomic uint64_t bytes_uploaded_to;
    uint64_t upload_rate;
    bool optimistic_unchoke;
} PeerConnection;

typedef struct {
    int interval; // seconds to wait between requests
    char failure_reason[512]; // to store the 'failure' string if it exists 
    bool success; // AI suggested: not required by spec but added for convenience of error handling
    
    PeerConnection *peers;
    int num_peers; // how many peers the tracker returned
} TrackerResponse;

typedef struct {
    uint32_t index; // which piece this is (e.g., piece #5)
    uint32_t length; // length of this specific piece (the last piece might be shorter!)
    
    uint32_t num_blocks; // how many 16KB blocks make up this piece
    uint32_t blocks_received; // counter to know when we are finished
    bool *block_status; // array of booleans: true if we have that block
    
    uint8_t *data; // buffer holding the actual file data as it comes in
    bool is_complete; // true when all blocks are here and the SHA-1 hash matches
    bool in_progress;
} PieceState;


typedef struct {
    uint64_t downloaded; // total bytes successfully downloaded and verified
    uint64_t uploaded; // total bytes given to other peers
    uint64_t left; // total bytes remaining
    
    uint8_t *my_bitfield; // our own bitfield to send in the handshake
    PieceState *pieces; // array tracking the live state of every piece
    char *save_dir;
    pthread_mutex_t lock;
    struct timespec start_time;
    bool endgame;
} ClientProgress;

typedef struct {
    TorrentContext *ctx;
    char *save_dir;
    int port;
    ClientProgress *progress;
} DownloadArgs;

typedef struct {
    PeerConnection peer;
    TorrentContext *ctx;
    ClientProgress *progress;
    bool is_incoming;
} PeerThreadArgs;

typedef struct {
    TorrentContext *ctx;
    ClientProgress *progress;
    bool done_reported;
} ActiveTorrent;

#endif