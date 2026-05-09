#include "struct.h"
#include "piece_manager.h"
#include "hash.h"

uint32_t scan_existing_pieces(TorrentContext *ctx, ClientProgress *progress) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", progress->save_dir, ctx->file_name);

    FILE *probe = fopen(path, "rb");
    if (!probe) {
        printf("[resume] No existing file found at %s – starting fresh.\n", path);
        fflush(stdout);
        return 0;
    }
    fclose(probe);

    printf("[resume] Scanning existing file: %s\n", path);
    fflush(stdout);

    uint32_t verified = 0;
    uint8_t *buf = malloc(ctx->piece_length);
    if (!buf) {
        fprintf(stderr, "[resume] malloc failed – skipping scan.\n");
        return 0;
    }

    for (uint32_t i = 0; i < ctx->num_pieces; i++) {
        uint32_t piece_len = progress->pieces[i].length;

        FILE *f = fopen(path, "rb");
        if (!f) break;

        uint64_t offset = (uint64_t)i * ctx->piece_length;
        int seek_ok  = (fseeko(f, (off_t)offset, SEEK_SET) == 0);
        int read_ok  = seek_ok &&
                       (fread(buf, 1, piece_len, f) == piece_len);
        fclose(f);

        if (!read_ok) {
            
            continue;
        }

        if (!verify_piece(ctx, i, buf, piece_len)) {
            continue;
        }
        pthread_mutex_lock(&progress->lock);

        PieceState *ps = &progress->pieces[i];
        if (!ps->is_complete) {
            ps->is_complete      = true;
            ps->blocks_received  = ps->num_blocks;

            for (uint32_t b = 0; b < ps->num_blocks; b++)
                ps->block_status[b] = true;

            progress->downloaded += piece_len;
            progress->left       -= piece_len;

            progress->my_bitfield[i / 8] |= (uint8_t)(1 << (7 - (i % 8)));

            verified++;
        }

        pthread_mutex_unlock(&progress->lock);
    }

    free(buf);

    double pct = (ctx->total_length > 0)
                 ? 100.0 * (double)progress->downloaded / (double)ctx->total_length
                 : 0.0;
    printf("[resume] %u / %u pieces already verified (%.1f%% complete).\n",
           verified, ctx->num_pieces, pct);
    fflush(stdout);

    return verified;
}

int store_block(ClientProgress *progress, TorrentContext *ctx, uint32_t index, uint32_t begin, uint8_t *data, uint32_t len) {
    PieceState *piece = &progress->pieces[index];

    if (piece->is_complete) return -1; // ignore if already finished

    if (begin + len > piece->length) return -1; // if inccorrect math also ignore

    memcpy(piece->data + begin, data, len);

    uint32_t block_idx = begin / MAX_BLOCK_SIZE;
    if (!piece->block_status[block_idx]) {
        piece->block_status[block_idx] = true;
        piece->blocks_received++;
    }

    if (piece->blocks_received == piece->num_blocks) {
        if (verify_piece(ctx, index, piece->data, piece->length)) {
            piece->is_complete = true;
            piece->in_progress = false; // it has finished 
            progress->downloaded += piece->length;
            progress->left -= piece->length;

            // update our bitfield
            progress->my_bitfield[index / 8] |= (1 << (7 - (index % 8)));

            write_piece(ctx, progress->save_dir, index, piece->data, piece->length);
            //printf("Piece %u complete and verified\n", index);
            return (uint32_t)index;
        } else {
            printf("Piece %u failed hash check, resetting\n", index);
            memset(piece->block_status, 0, piece->num_blocks * sizeof(bool));
            piece->blocks_received = 0;
        }
    }
    return -1;
}

int verify_piece(TorrentContext *ctx, uint32_t index, uint8_t *data, uint32_t len) {
    uint8_t hash[20];
    struct sha1sum_ctx *sha = sha1sum_create(NULL, 0);
    sha1sum_finish(sha, data, len, hash);
    sha1sum_destroy(sha);
    return memcmp(hash, ctx->piece_hashes + (index * 20), 20) == 0;
}

void write_piece(TorrentContext *ctx, const char *save_dir, uint32_t index, uint8_t *data, uint32_t len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", save_dir, ctx->file_name);

    FILE *f = fopen(path, "r+b");
    if (!f) f = fopen(path, "w+b");
    if (!f) { perror("fopen"); return; }

    if (fseeko(f, (off_t)index * ctx->piece_length, SEEK_SET) != 0) {
        perror("fseeko");
        fclose(f);
        return;
    }
    if (fwrite(data, 1, len, f) != len) {
        perror("fwrite");
    }
    
    fclose(f);
}

int read_block_from_disk(TorrentContext *t, const char *save_dir, uint32_t index, uint32_t begin, uint32_t length, uint8_t *out_buf) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", save_dir, t->file_name);

    FILE *f = fopen(path, "rb"); // reading from binary file
    if (!f) return -1;

    uint64_t offset = (uint64_t)index * t->piece_length + begin;
    
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if (fread(out_buf, 1, length, f) != length) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}