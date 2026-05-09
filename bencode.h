#include "struct.h"
#include "bittorrent.h"
#include "hash.h"

void generate_peer_id(uint8_t *peer_id);
char* skip_bencode(char* ptr);
char* handle_announce(TorrentContext *ctx, char *val_ptr);
char* handle_announce_list(TorrentContext *ctx, char *val_ptr);
char* handle_info(TorrentContext *ctx, char *val_ptr);
TorrentContext* parse_torrent(char* filename);
void torrent_free(TorrentContext* ctx);