CFLAGS = -Wall -Werror -g -Og -Iincludes -Wextra -std=gnu99 
LDFLAGS = -lssl -lcrypto -pthread
CC = gcc $(CFLAGS)

all: bittorrent

bittorrent: bittorrent.o peer_msg.o hash.o tracker.o bencode.o optparser.o downloader.o piece_manager.o
	$(CC) -o bittorrent bittorrent.o peer_msg.o hash.o tracker.o bencode.o optparser.o downloader.o piece_manager.o $(LDFLAGS)

bittorrent.o: bittorrent.c bittorrent.h struct.h
	$(CC) -c bittorrent.c

peer_msg.o: peer_msg.c peer_msg.h struct.h
	$(CC) -c peer_msg.c

hash.o: hash.c hash.h
	$(CC) -c hash.c

tracker.o: tracker.c tracker.h bittorrent.h struct.h
	$(CC) -c tracker.c

bencode.o: bencode.c bittorrent.h struct.h
	$(CC) -c bencode.c

optparser.o: optparser.c bittorrent.h struct.h
	$(CC) -c optparser.c

downloader.o: downloader.c bittorrent.h struct.h peer_msg.h tracker.h
	$(CC) -c downloader.c

piece_manager.o: piece_manager.c bittorrent.h struct.h peer_msg.h 
	$(CC) -c piece_manager.c

TORRENT ?= myfile.torrent
PORT_SEEDER ?= 6881
PORT_LEECHER ?= 6882
DIR_SEEDER ?= /tmp/seed
DIR_LEECHER ?= /tmp/leech

tracker-loopback.o: tracker.c tracker.h bittorrent.h struct.h
	$(CC) -DLOOPBACK_TEST -c tracker.c -o tracker-loopback.o

bittorrent-leecher: bittorrent.o peer_msg.o hash.o tracker-loopback.o bencode.o optparser.o downloader.o piece_manager.o
	$(CC) -o bittorrent-leecher bittorrent.o peer_msg.o hash.o tracker-loopback.o bencode.o optparser.o downloader.o piece_manager.o $(LDFLAGS)

bittorrent-seeder: bittorrent
	cp bittorrent bittorrent-seeder

test_tracker: test_tracker.o tracker.o bencode.o hash.o optparser.o downloader.o peer_msg.o piece_manager.o bittorrent.c
	$(CC) -DTESTING -o test_tracker test_tracker.o tracker.o bencode.o hash.o optparser.o downloader.o peer_msg.o piece_manager.o bittorrent.c $(LDFLAGS)

test_tracker.o: test_tracker.c tracker.h bittorrent.h struct.h
	$(CC) -c test_tracker.c

loopback: bittorrent-seeder bittorrent-leecher

# AI usage: create pipeline for easy testing

run-seeder: bittorrent-seeder
	@echo "=== SEEDER  port=$(PORT_SEEDER)  torrent=$(TORRENT)  dir=$(DIR_SEEDER) ==="
	echo "torrent $(TORRENT) $(DIR_SEEDER)" | ./bittorrent-seeder -p $(PORT_SEEDER)

run-leecher: bittorrent-leecher
	@echo "=== LEECHER port=$(PORT_LEECHER) torrent=$(TORRENT)  dir=$(DIR_LEECHER) ==="
	echo "torrent $(TORRENT) $(DIR_LEECHER)" | ./bittorrent-leecher -p $(PORT_LEECHER)
clean:
	rm -f bittorrent bittorrent-seeder bittorrent-leecher *.o