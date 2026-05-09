#ifndef PIECE_MANAGER_H
#define PIECE_MANAGER_H

#include "struct.h"


int store_block(ClientProgress *progress, TorrentContext *ctx, uint32_t index, uint32_t begin, uint8_t *data, uint32_t len);

int verify_piece(TorrentContext *ctx, uint32_t index, uint8_t *data, uint32_t len);


void write_piece(TorrentContext *ctx, const char *save_dir, uint32_t index, uint8_t *data, uint32_t len);

int read_block_from_disk(TorrentContext *t, const char *save_dir, uint32_t index, uint32_t begin, uint32_t length,uint8_t *out_buf);

uint32_t scan_existing_pieces(TorrentContext *ctx, ClientProgress *progress);

#endif
