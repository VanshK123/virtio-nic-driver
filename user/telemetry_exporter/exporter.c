#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include "exporter.h"

static struct MHD_Daemon *daemon;

char *collect_metrics(void)
{
    long tx = 0, rx = 0;
    FILE *f = fopen("/sys/kernel/virtio_nic/tx", "r");
    if (f) { fscanf(f, "%ld", &tx); fclose(f); }
    f = fopen("/sys/kernel/virtio_nic/rx", "r");
    if (f) { fscanf(f, "%ld", &rx); fclose(f); }

    char *buf = malloc(128);
    if (!buf)
        return NULL;
    snprintf(buf, 128, "virtio_nic_tx %ld\nvirtio_nic_rx %ld\n", tx, rx);
    return buf;
}

static int metrics_cb(void *cls, struct MHD_Connection *c,
                      const char *url, const char *method,
                      const char *ver, const char *upload_data,
                      size_t *upload_data_size, void **con_cls)
{
    (void)cls; (void)ver; (void)upload_data; (void)upload_data_size; (void)con_cls;
    if (strcmp(method, "GET") || strcmp(url, "/metrics"))
        return MHD_NO;

    char *metrics = collect_metrics();
    if (!metrics)
        return MHD_NO;

    struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(metrics), metrics, MHD_RESPMEM_MUST_FREE);
    int ret = MHD_queue_response(c, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

int init_http_server(int port)
{
    daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, port,
                              NULL, NULL, metrics_cb, NULL, MHD_OPTION_END);
    return daemon ? 0 : -1;
}

int main(int argc, char **argv)
{
    int port = 9090;
    if (argc > 1)
        port = atoi(argv[1]);
    if (init_http_server(port) < 0) {
        fprintf(stderr, "Failed to start HTTP server\n");
        return 1;
    }
    printf("Telemetry exporter running on port %d\n", port);
    getchar();
    return 0;
}
