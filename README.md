# Final Class Project: BitTorrent

## Description
A peer-to-peer BitTorrent client implementation in C. Only supports downloading as of May 9, 2026. 

## Usage
Use it to download files from peers using .torrent files. Currently supporting HTTP with compact format and for both single `announce` URLs and `announce-list`.

### Setup
1. Clone the repository
```bash
git clone https://github.com/tlstmdgk/bittorrent.git
cd bittorrent
```
2. Build the Docker image
```bash
docker build -t bittorrent .
```
3. Run the container
```bash
docker run -it bittorrent
```

### Use
To start an instance of the client, run
```bash
./bittorrent -p <port>
```

Following is an example of a `bittorrent` command.
```bash
./bittorrent -p 6881
```

Run the following to start downloading filename.torrent file to the download-path.
```bash
torrent <filename.torrent> <download-path>
```
This will load the provided torrent file and communicate with the tracker and peers to start downloading the file identified. In addition, you can start up another torrent file using the same format while downloading one, with the cost of overwhelming the CLI.

Following is an example of a `torrent` command.
```bash
torrent ./torrents/flatland.torrent ./downloads/
```

To exit,
```bash
quit
```
or
```bash
exit
```
