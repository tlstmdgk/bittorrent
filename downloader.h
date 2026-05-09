void *download(void *arg);
void *peer_worker(void *arg);
void add_explicit_peer(TorrentContext *ctx, ClientProgress *progress, const char *ip, int port);