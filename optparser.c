#include "struct.h"
#include "bittorrent.h"

// ./bittorrent -p <port>
error_t client_parser(int key, char *arg, struct argp_state *state) {
    struct client_arguments *args = state->input;
    error_t ret = 0;
    
    switch(key) {
    case 'p':
        args->port = atoi(arg);
        if (args->port <= 0 || args->port > 65535) {
            argp_error(state, "Invalid option for a port. Must be a number between 1 and 65535.");
        }
        break;
    default:
        ret = ARGP_ERR_UNKNOWN;
        break;
    }
    return ret;
}

struct client_arguments parseopt(int argc, char *argv[]) {
    struct argp_option options[] = {
        { "port", 'p', "port", 0, "The port the client listens to for peer connections", 0},
        {0}
    };

    struct argp argp_settings = { options, client_parser, 0, 0, 0, 0, 0 };

    struct client_arguments args;
    memset(&args, 0, sizeof(args));

    if (argp_parse(&argp_settings, argc, argv, 0, NULL, &args) != 0) {
        fprintf(stderr, "Error while parsing arguments\n");
        exit(1);
    }

    if (args.port == 0) {
        fprintf(stderr, "Error: Port is required.\nUsage: ./bittorrent -p <port>\n");
        exit(1);
    }

    return args;
}

void client_args_free(struct client_arguments *args) {
    if (args == NULL) {
        return;
	}
}