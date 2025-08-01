#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <microhttpd.h>
#include <json-c/json.h>
#include <time.h>
#include <sys/sysinfo.h>
#include "exporter.h"

static struct MHD_Daemon *daemon;
static struct json_object *metrics_cache = NULL;
static time_t last_update = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Enhanced metrics collection with caching */
char *collect_metrics(void)
{
    struct json_object *root, *metrics, *metric;
    char *result = NULL;
    time_t now = time(NULL);
    
    pthread_mutex_lock(&cache_mutex);
    
    /* Update cache every second */
    if (now - last_update >= 1) {
        if (metrics_cache) {
            json_object_put(metrics_cache);
        }
        
        root = json_object_new_object();
        metrics = json_object_new_array();
        
        /* Basic metrics */
        long tx_packets = 0, rx_packets = 0;
        long tx_bytes = 0, rx_bytes = 0;
        long avg_latency_ns = 0;
        
        FILE *f = fopen("/sys/kernel/virtio_nic_telemetry/tx_packets", "r");
        if (f) { fscanf(f, "%ld", &tx_packets); fclose(f); }
        
        f = fopen("/sys/kernel/virtio_nic_telemetry/rx_packets", "r");
        if (f) { fscanf(f, "%ld", &rx_packets); fclose(f); }
        
        f = fopen("/sys/kernel/virtio_nic_telemetry/total_bytes", "r");
        if (f) { fscanf(f, "%ld", &tx_bytes); fclose(f); }
        
        f = fopen("/sys/kernel/virtio_nic_telemetry/avg_latency_ns", "r");
        if (f) { fscanf(f, "%ld", &avg_latency_ns); fclose(f); }
        
        /* Add basic metrics */
        metric = json_object_new_object();
        json_object_object_add(metric, "name", json_object_new_string("virtio_nic_tx_packets"));
        json_object_object_add(metric, "value", json_object_new_int64(tx_packets));
        json_object_object_add(metric, "type", json_object_new_string("counter"));
        json_object_array_add(metrics, metric);
        
        metric = json_object_new_object();
        json_object_object_add(metric, "name", json_object_new_string("virtio_nic_rx_packets"));
        json_object_object_add(metric, "value", json_object_new_int64(rx_packets));
        json_object_object_add(metric, "type", json_object_new_string("counter"));
        json_object_array_add(metrics, metric);
        
        metric = json_object_new_object();
        json_object_object_add(metric, "name", json_object_new_string("virtio_nic_tx_bytes"));
        json_object_object_add(metric, "value", json_object_new_int64(tx_bytes));
        json_object_object_add(metric, "type", json_object_new_string("counter"));
        json_object_array_add(metrics, metric);
        
        metric = json_object_new_object();
        json_object_object_add(metric, "name", json_object_new_string("virtio_nic_avg_latency_ns"));
        json_object_object_add(metric, "value", json_object_new_int64(avg_latency_ns));
        json_object_object_add(metric, "type", json_object_new_string("gauge"));
        json_object_array_add(metrics, metric);
        
        /* Queue statistics */
        f = fopen("/sys/kernel/virtio_nic_telemetry/queue_stats", "r");
        if (f) {
            char line[512];
            int queue_id, numa_node, cpu_id;
            long rx_pkts, tx_pkts, rx_bytes, tx_bytes, pending;
            
            /* Skip header lines */
            fgets(line, sizeof(line), f);
            fgets(line, sizeof(line), f);
            
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%d\t%d\t%d\t%ld\t%ld\t%ld\t%ld\t%ld",
                           &queue_id, &numa_node, &cpu_id, &rx_pkts, &tx_pkts,
                           &rx_bytes, &tx_bytes, &pending) == 8) {
                    
                    metric = json_object_new_object();
                    json_object_object_add(metric, "name", json_object_new_string("virtio_nic_queue_stats"));
                    json_object_object_add(metric, "queue_id", json_object_new_int(queue_id));
                    json_object_object_add(metric, "numa_node", json_object_new_int(numa_node));
                    json_object_object_add(metric, "cpu_id", json_object_new_int(cpu_id));
                    json_object_object_add(metric, "rx_packets", json_object_new_int64(rx_pkts));
                    json_object_object_add(metric, "tx_packets", json_object_new_int64(tx_pkts));
                    json_object_object_add(metric, "rx_bytes", json_object_new_int64(rx_bytes));
                    json_object_object_add(metric, "tx_bytes", json_object_new_int64(tx_bytes));
                    json_object_object_add(metric, "pending_packets", json_object_new_int64(pending));
                    json_object_array_add(metrics, metric);
                }
            }
            fclose(f);
        }
        
        /* Flow statistics */
        f = fopen("/sys/kernel/virtio_nic_telemetry/flow_stats", "r");
        if (f) {
            char line[512];
            int flow_id;
            long packets, bytes, avg_latency, last_seen;
            
            /* Skip header lines */
            fgets(line, sizeof(line), f);
            fgets(line, sizeof(line), f);
            
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%d\t%ld\t%ld\t%ld\t%ld",
                           &flow_id, &packets, &bytes, &avg_latency, &last_seen) == 5) {
                    
                    metric = json_object_new_object();
                    json_object_object_add(metric, "name", json_object_new_string("virtio_nic_flow_stats"));
                    json_object_object_add(metric, "flow_id", json_object_new_int(flow_id));
                    json_object_object_add(metric, "packets", json_object_new_int64(packets));
                    json_object_object_add(metric, "bytes", json_object_new_int64(bytes));
                    json_object_object_add(metric, "avg_latency_ns", json_object_new_int64(avg_latency));
                    json_object_object_add(metric, "last_seen", json_object_new_int64(last_seen));
                    json_object_array_add(metrics, metric);
                }
            }
            fclose(f);
        }
        
        /* NUMA statistics */
        f = fopen("/sys/kernel/virtio_nic_telemetry/numa_stats", "r");
        if (f) {
            char line[512];
            int numa_node;
            long rx_pkts, tx_pkts, rx_bytes, tx_bytes, errors;
            
            /* Skip header lines */
            fgets(line, sizeof(line), f);
            fgets(line, sizeof(line), f);
            
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%d\t%ld\t%ld\t%ld\t%ld\t%ld",
                           &numa_node, &rx_pkts, &tx_pkts, &rx_bytes, &tx_bytes, &errors) == 6) {
                    
                    metric = json_object_new_object();
                    json_object_object_add(metric, "name", json_object_new_string("virtio_nic_numa_stats"));
                    json_object_object_add(metric, "numa_node", json_object_new_int(numa_node));
                    json_object_object_add(metric, "rx_packets", json_object_new_int64(rx_pkts));
                    json_object_object_add(metric, "tx_packets", json_object_new_int64(tx_pkts));
                    json_object_object_add(metric, "rx_bytes", json_object_new_int64(rx_bytes));
                    json_object_object_add(metric, "tx_bytes", json_object_new_int64(tx_bytes));
                    json_object_object_add(metric, "errors", json_object_new_int64(errors));
                    json_object_array_add(metrics, metric);
                }
            }
            fclose(f);
        }
        
        /* System metrics */
        struct sysinfo si;
        if (sysinfo(&si) == 0) {
            metric = json_object_new_object();
            json_object_object_add(metric, "name", json_object_new_string("virtio_nic_system_load"));
            json_object_object_add(metric, "load_1min", json_object_new_double(si.loads[0] / 65536.0));
            json_object_object_add(metric, "load_5min", json_object_new_double(si.loads[1] / 65536.0));
            json_object_object_add(metric, "load_15min", json_object_new_double(si.loads[2] / 65536.0));
            json_object_array_add(metrics, metric);
        }
        
        json_object_object_add(root, "metrics", metrics);
        json_object_object_add(root, "timestamp", json_object_new_int64(now));
        
        metrics_cache = root;
        last_update = now;
    }
    
    if (metrics_cache) {
        result = strdup(json_object_to_json_string(metrics_cache));
    }
    
    pthread_mutex_unlock(&cache_mutex);
    return result;
}

/* Prometheus format metrics */
char *collect_prometheus_metrics(void)
{
    char *result = NULL;
    char *json_metrics = collect_metrics();
    
    if (json_metrics) {
        struct json_object *root = json_tokener_parse(json_metrics);
        struct json_object *metrics_array;
        
        if (json_object_object_get_ex(root, "metrics", &metrics_array)) {
            int len = json_object_array_length(metrics_array);
            char *prometheus = malloc(1024 * len); // Estimate size
            int pos = 0;
            
            pos += sprintf(prometheus + pos, "# HELP virtio_nic_metrics VirtIO NIC performance metrics\n");
            pos += sprintf(prometheus + pos, "# TYPE virtio_nic_metrics counter\n");
            
            for (int i = 0; i < len; i++) {
                struct json_object *metric = json_object_array_get_idx(metrics_array, i);
                struct json_object *name, *value, *type;
                
                if (json_object_object_get_ex(metric, "name", &name) &&
                    json_object_object_get_ex(metric, "value", &value)) {
                    
                    const char *metric_name = json_object_get_string(name);
                    double metric_value = json_object_get_double(value);
                    
                    pos += sprintf(prometheus + pos, "%s %f\n", metric_name, metric_value);
                }
            }
            
            result = prometheus;
        }
        
        json_object_put(root);
        free(json_metrics);
    }
    
    return result;
}

static int metrics_cb(void *cls, struct MHD_Connection *c,
                      const char *url, const char *method,
                      const char *ver, const char *upload_data,
                      size_t *upload_data_size, void **con_cls)
{
    (void)cls; (void)ver; (void)upload_data; (void)upload_data_size; (void)con_cls;
    
    if (strcmp(method, "GET"))
        return MHD_NO;

    char *metrics = NULL;
    const char *content_type = "application/json";
    
    if (strcmp(url, "/metrics") == 0) {
        metrics = collect_prometheus_metrics();
        content_type = "text/plain";
    } else if (strcmp(url, "/api/v1/metrics") == 0) {
        metrics = collect_metrics();
    } else {
        return MHD_NO;
    }

    if (!metrics)
        return MHD_NO;

    struct MHD_Response *resp = MHD_create_response_from_buffer(strlen(metrics), metrics, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", content_type);
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    
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

void cleanup_exporter(void)
{
    if (daemon) {
        MHD_stop_daemon(daemon);
        daemon = NULL;
    }
    
    pthread_mutex_lock(&cache_mutex);
    if (metrics_cache) {
        json_object_put(metrics_cache);
        metrics_cache = NULL;
    }
    pthread_mutex_unlock(&cache_mutex);
}

int main(int argc, char **argv)
{
    int port = 9090;
    if (argc > 1)
        port = atoi(argv[1]);
    
    printf("Starting VirtIO NIC telemetry exporter on port %d\n", port);
    printf("Available endpoints:\n");
    printf("  GET /metrics - Prometheus format metrics\n");
    printf("  GET /api/v1/metrics - JSON format metrics\n");
    
    if (init_http_server(port) < 0) {
        fprintf(stderr, "Failed to start HTTP server\n");
        return 1;
    }
    
    printf("Telemetry exporter running on port %d\n", port);
    printf("Press Ctrl+C to stop\n");
    
    /* Wait for interrupt */
    while (1) {
        sleep(1);
    }
    
    cleanup_exporter();
    return 0;
}
