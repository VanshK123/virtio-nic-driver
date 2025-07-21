#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mnl/mnl.h>
#include "qos_agent.h"

static struct mnl_socket *nl;

int init_netlink(void)
{
    nl = mnl_socket_open(NETLINK_ROUTE);
    if (!nl)
        return -1;
    return 0;
}

int apply_rate_limit(int flow_id, int rate)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "tc class replace dev eth0 parent 1: classid 1:%d htb rate %dkbit",
             flow_id, rate);
    return system(cmd);
}

static int process_json(const char *json)
{
    int id, rate;
    if (sscanf(json, "{\"flow_id\":%d,\"rate\":%d}", &id, &rate) == 2)
        return apply_rate_limit(id, rate);
    return -1;
}

int main(int argc, char **argv)
{
    FILE *f = stdin;
    char buf[512];

    if (argc > 1)
        f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "Failed to open config\n");
        return 1;
    }

    if (init_netlink() < 0) {
        fprintf(stderr, "netlink init failed\n");
        return 1;
    }

    while (fgets(buf, sizeof(buf), f))
        process_json(buf);

    if (f != stdin)
        fclose(f);
    mnl_socket_close(nl);
    return 0;
}
