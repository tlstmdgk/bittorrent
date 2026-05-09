#include "struct.h"
#include "bencode.h"

char* url_encode(const uint8_t *bindata, size_t len);
char* build_tracker_url(const TorrentContext *ctx, 
                        const ClientProgress *progress, const char *event, 
                        int port);
int contact_tracker(const char *url, TrackerResponse *response);
int contact_tracker_with_list(const TorrentContext *ctx,
                              const ClientProgress *progress, const char *event,
                              int port, TrackerResponse *response);
void handle_response(const char *bendata, size_t len, TrackerResponse *response);
char* handle_peers_list(TrackerResponse *response, char *valptr);