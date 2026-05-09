#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "struct.h"
#include "tracker.h"
#include "bencode.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *test_name, int condition) {
    if (condition) {
        printf("[PASS] %s\n", test_name);
        tests_passed++;
    } else {
        printf("[FAIL] %s\n", test_name);
        tests_failed++;
    }
}

// TEST 1: single announce URL (no announce-list), compact format
// uses macOS-heaven.torrent which has no announce-list
void test_single_announce(const char *torrent_path) {
    printf("\n--- TEST 1: single announce URL + compact format ---\n");

    TorrentContext *ctx = parse_torrent((char *)torrent_path);
    if (!ctx) {
        printf("[FAIL] could not parse torrent\n");
        tests_failed++;
        return;
    }

    ClientProgress progress = { .uploaded = 0, .downloaded = 0, .left = ctx->total_length };
    TrackerResponse response;
    memset(&response, 0, sizeof(response));

    printf("tracker URL: %s\n", ctx->tracker_url);
    check("no announce-list present", ctx->announce_list == NULL);

    int result = contact_tracker_with_list(ctx, &progress, "started", 6881, &response);

    check("tracker contact succeeded", result == 0 && response.success);
    check("got at least one peer", response.num_peers > 0);
    check("interval is reasonable", response.interval > 0 && response.interval < 86400);

    if (response.num_peers > 0) {
        printf("sample peer: ");
        struct in_addr addr;
        addr.s_addr = response.peers[0].ip;
        printf("%s:%d\n", inet_ntoa(addr), ntohs(response.peers[0].port));
    }

    if (response.peers) {
        free(response.peers);
    }
    torrent_free(ctx);
}

// TEST 2: announce-list with working tracker (no bad URL injection)
// uses flatland.torrent which has an announce-list
void test_announce_list_success(const char *torrent_path) {
    printf("\n--- TEST 2: announce-list, first tracker works ---\n");

    TorrentContext *ctx = parse_torrent((char *)torrent_path);
    if (!ctx) {
        printf("[FAIL] could not parse torrent\n");
        tests_failed++;
        return;
    }

    ClientProgress progress = { .uploaded = 0, .downloaded = 0, .left = ctx->total_length };
    TrackerResponse response;
    memset(&response, 0, sizeof(response));

    check("announce-list is present", ctx->announce_list != NULL);

    printf("tier 0 tracker: %s\n", ctx->announce_list[0][0]);
    int result = contact_tracker_with_list(ctx, &progress, "started", 6881, &response);

    check("tracker contact succeeded", result == 0 && response.success);
    check("got at least one peer", response.num_peers > 0);

    if (response.peers) {
        free(response.peers);
    }
    torrent_free(ctx);
}

// TEST 3: announce-list with bad tracker in tier 0, falls back to tracker_url
// injects a fake URL into tier 0 to force fallback
void test_announce_list_fallback(const char *torrent_path) {
    printf("\n--- TEST 3: announce-list fallback (bad tier 0 tracker) ---\n");

    TorrentContext *ctx = parse_torrent((char *)torrent_path);
    if (!ctx) { printf("[FAIL] could not parse torrent\n"); tests_failed++; return; }

    // inject a bad tracker into tier 0
    if (ctx->announce_list != NULL && ctx->announce_list[0] != NULL) {
        free(ctx->announce_list[0][0]);
        ctx->announce_list[0][0] = strdup("http://faketracker.fake:6969/announce");
        printf("*** injected bad tracker into tier 0: %s\n", ctx->announce_list[0][0]);
    } else {
        printf("[SKIP] torrent has no announce-list, cannot test fallback\n");
        torrent_free(ctx);
        return;
    }

    ClientProgress progress = { .uploaded = 0, .downloaded = 0, .left = ctx->total_length };
    TrackerResponse response;
    memset(&response, 0, sizeof(response));

    int result = contact_tracker_with_list(ctx, &progress, "started", 6881, &response);

    check("fallback tracker contact succeeded", result == 0 && response.success);
    check("got at least one peer after fallback", response.num_peers > 0);

    if (response.peers) {
        free(response.peers);
    }
    torrent_free(ctx);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <single-announce.torrent> <announce-list.torrent>\n", argv[0]);
        fprintf(stderr, "  argv[1]: torrent with no announce-list (e.g. macOS-heaven.torrent)\n");
        fprintf(stderr, "  argv[2]: torrent with announce-list (e.g. flatland.torrent)\n");
        return 1;
    }

    printf("--- tracker.c test suite ---\n");

    test_single_announce(argv[1]);
    test_announce_list_success(argv[2]);
    test_announce_list_fallback(argv[2]);

    printf("\n--- results: %d passed, %d failed ---\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}