#include "struct.h"
#include "bencode.h"

// generates peer id (Azureus-style)
// https://wiki.theory.org/BitTorrentSpecification#peer_id for details
void generate_peer_id(uint8_t *peer_id) {
    // format: -<Client ID><Version>-<Random Numbers>
    // using JP for my initials :) version 0.0.0.1
    memcpy(peer_id, "-JP0001-", 8);
    srand(time(NULL));
    for (int i = 8; i < 20; i++) {
        peer_id[i] = '0' + (rand() % 10);
    }
}

// helper function to skip a bencoded value and return the pointer 
// to the next element. returns NULL at error.
char* skip_bencode(char* ptr) {
    if (!ptr) {
        return NULL;
    }

    if (*ptr == 'i') {
        // int
        ptr = ptr + 1;
        while (*ptr && *ptr != 'e') {
            ptr = ptr + 1;
        }
        return ptr + 1;

    } else if (*ptr == 'l' || *ptr == 'd') {
        // list/dict
        ptr = ptr + 1;
        while (*ptr && *ptr != 'e') {
            ptr = skip_bencode(ptr);
        }
        return ptr + 1;
    } else if (*ptr >= '0' && *ptr <= '9') {
        // string
        char *end;
        long len = strtol(ptr, &end, 10);
        if (*end != ':') {
            return NULL;
        }
        return end + 1 + len;
    }

    return NULL;
}

// helper to handle the "announce" key
char* handle_announce(TorrentContext *ctx, char *val_ptr) {
    char *end;
    long len = strtol(val_ptr, &end, 10);
    ctx->tracker_url = malloc(len + 1);
    memcpy(ctx->tracker_url, end + 1, len);
    // null terminate
    ctx->tracker_url[len] = '\0';
    return end + 1 + len;
}

// helper to handle the "announce-list" key
char* handle_announce_list(TorrentContext *ctx, char *val_ptr) {
    if (*val_ptr != 'l') {
        // announce-list is list of lists
        return skip_bencode(val_ptr);
    }

    char *ptr = val_ptr + 1; // skip 'l'
    int tier_count = 0;

    // count the number of tiers (inner lists)
    char *count_ptr = ptr;
    while (*count_ptr != 'e') {
        tier_count = tier_count + 1;
        count_ptr = skip_bencode(count_ptr);
    }

    // allocate for tiers (1 for null)
    ctx->announce_list = calloc(tier_count + 1, sizeof(char **));

    // fill each tier
    for (int i = 0; i < tier_count; i++) {
        if (*ptr != 'l') { 
            ptr = skip_bencode(ptr);
            continue; 
        }

        char *tier_ptr = ptr + 1; // skip 'l'
        int tracker_count = 0;

        // count trackers in this tier
        char *count_tier = tier_ptr;
        while (*count_tier != 'e') {
            tracker_count = tracker_count + 1;
            count_tier = skip_bencode(count_tier);
        }

        // allocate array for URLs (1 for null)
        ctx->announce_list[i] = calloc(tracker_count + 1, sizeof(char *));

        for (int j = 0; j < tracker_count; j++) {
            char *end;
            long str_len = strtol(tier_ptr, &end, 10);
            char *str_data = end + 1;

            ctx->announce_list[i][j] = malloc(str_len + 1);
            memcpy(ctx->announce_list[i][j], str_data, str_len);
            // null terminate
            ctx->announce_list[i][j][str_len] = '\0';

            tier_ptr = str_data + str_len;
        }
        ptr = tier_ptr + 1; // skip 'e'
    }

    return ptr + 1; // skip 'e'
}

// helper to handle the "info" key
char* handle_info(TorrentContext *ctx, char *val_ptr) {
    char *start = val_ptr;
    char *dict_end = skip_bencode(val_ptr);
    int info_len = dict_end - start;

    // calculate the info hash
    struct sha1sum_ctx *hash_ctx = sha1sum_create(NULL, 0);
    if (hash_ctx) {
        sha1sum_finish(hash_ctx, (const uint8_t*) start, info_len, ctx->info_hash);
        sha1sum_destroy(hash_ctx);
    }

    // parse inside info dict
    char *ptr = start;
    if (*ptr == 'd') {
        ptr = ptr + 1;
        while (ptr < dict_end && *ptr != 'e') {
            char *end;
            long key_len = strtol(ptr, &end, 10);
            char *key_str = end + 1;
            char *i_val_ptr = key_str + key_len;

            // name: the filename
            if (key_len == 4 && strncmp(key_str, "name", 4) == 0) {
                long len = strtol(i_val_ptr, &end, 10);
                ctx->file_name = malloc(len + 1);
                memcpy(ctx->file_name, end + 1, len);
                ctx->file_name[len] = '\0';
                ptr = end + 1 + len;
            } 
            // length: length of the file in bytes
            else if (key_len == 6 && strncmp(key_str, "length", 6) == 0) {
                ctx->total_length = strtoull(i_val_ptr + 1, &end, 10);
                ptr = end + 1; // skip 'e'
            } 
            // piece length: number of bytes in each piece
            else if (key_len == 12 && strncmp(key_str, "piece length", 12) == 0) {
                ctx->piece_length = strtoul(i_val_ptr + 1, &end, 10);
                ptr = end + 1; // skip 'e'
            } 
            // pieces: concat of 20-byte SHA1 hash values
            else if (key_len == 6 && strncmp(key_str, "pieces", 6) == 0) {
                long len = strtol(i_val_ptr, &end, 10);
                ctx->num_pieces = len / 20; // each hash is 20 bytes
                ctx->piece_hashes = malloc(len);
                memcpy(ctx->piece_hashes, end + 1, len);
                ptr = end + 1 + len;
            } 
            else {
                // skip the rest
                ptr = skip_bencode(i_val_ptr);
            }
        }
    }
    return dict_end; // move main pointer
}

// parses .torrent file with name filename then returns malloc'ed
// TorrentContext. returns NULL at error.
TorrentContext* parse_torrent(char* filename) {

    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        return NULL;
    }

    // get file size and read into memory
    fseek(file, 0, SEEK_END);
    long unsigned int file_size = ftell(file);
    rewind(file);

    char *buffer = malloc(file_size);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, file_size, file) != file_size) {
        fclose(file);
        return NULL;
    }
    fclose(file);

    // calloc initialize pointers to NULL
    TorrentContext *ctx = calloc(1, sizeof(TorrentContext));

    if (!ctx) {
        free(buffer);
        return NULL;
    }

    generate_peer_id(ctx->peer_id);

    char *ptr = buffer;
    if (*ptr != 'd') { // .torrent files always start with dictionary
        free(buffer);
        torrent_free(ctx);
        return NULL; 
    }

    // skip 'd'
    ptr = ptr + 1;

    // main dict parsing loop
    while (ptr < buffer + file_size && *ptr != 'e') {
        char *end;
        long key_len = strtol(ptr, &end, 10);
        char *key_str = end + 1;
        char *val_ptr = key_str + key_len;

        // announce: the announce URL of the tracker
        if (key_len == 8 && strncmp(key_str, "announce", 8) == 0) {
            ptr = handle_announce(ctx, val_ptr);
        } 
        // (optional) extension for backwards-compatibility
        else if (key_len == 13 && strncmp(key_str, "announce-list", 13) == 0) {
            ptr = handle_announce_list(ctx, val_ptr);
        }
        // info: describes the file(s) of the torrent. single-file 
        // (torrent with no directory structure) or multi-file torrent
        else if (key_len == 4 && strncmp(key_str, "info", 4) == 0) {
            ptr = handle_info(ctx, val_ptr);
        } 
        else {
            // skip the rest
            ptr = skip_bencode(val_ptr);
        }
    }

    free(buffer);
    return ctx;
}

// frees fields of TorrentContext and itself. should be called at exit.
void torrent_free(TorrentContext* ctx) {
    if (ctx == NULL) return;

    if (ctx->tracker_url != NULL) {
        free(ctx->tracker_url);
    }

    if (ctx->announce_list != NULL) {
        for (int i = 0; ctx->announce_list[i] != NULL; i++) {
            for (int j = 0; ctx->announce_list[i][j] != NULL; j++) {
                free(ctx->announce_list[i][j]);
            }
            free(ctx->announce_list[i]);
        }
        free(ctx->announce_list);
    }
    
    if (ctx->file_name != NULL) {
        free(ctx->file_name);
    }

    if (ctx->piece_hashes != NULL) {
        free(ctx->piece_hashes);
    }

    free(ctx);
}