/*
 * test_msghandler: feeds peer-to-peer network messages (one JSON message per
 * line) into handle_network_msg(), with the ubus / uci / iwinfo side effects
 * stubbed out. It exists mainly to check that malformed or hostile messages
 * from another "DAWN" instance are rejected rather than crashing the daemon,
 * so it is most useful built with -fsanitize=address,undefined.
 *
 *   test_msghandler [file]    (reads stdin if no file given; '#' = comment)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datastorage.h"
#include "dawn_iwinfo.h"
#include "dawn_uci.h"
#include "msghandler.h"
#include "ubus.h"

int main(int argc, char **argv) {
    FILE *f = stdin;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int count = 0;

    // Exercise the config parser too (pushes are otherwise ignored by policy)
    network_config.accept_config_push = ACCEPT_CONFIG_PUSH_ANY;

    if (argc > 1 && !(f = fopen(argv[1], "r"))) {
        perror(argv[1]);
        return 1;
    }

    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = '\0';
        if (n == 0 || line[0] == '#')
            continue;

        printf("MSG %d: %.100s%s\n", ++count, line, n > 100 ? "..." : "");
        handle_network_msg(line, true);
    }

    free(line);
    if (f != stdin)
        fclose(f);

    printf("test_msghandler: processed %d messages\n", count);
    return 0;
}
