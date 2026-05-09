#ifndef BITTORRENT_H
#define BITTORRENT_H
#include "struct.h"

struct client_arguments parseopt(int argc, char *argv[]);
TorrentContext* parse_torrent(char* filename);
void client_args_free(struct client_arguments *args);

void register_peer (PeerConnection *peer);
void unregister_peer(PeerConnection *peer);
void broadcast_have(uint32_t piece_index);

int setup_listening_socket(int port);
void handle_torrent_command(int port, char *filename, char *dir);
void *handle_incoming_peer(void *arg);
void broadcast_cancel(PeerConnection *skip_peer, uint32_t index,
                      uint32_t begin, uint32_t length);

#endif