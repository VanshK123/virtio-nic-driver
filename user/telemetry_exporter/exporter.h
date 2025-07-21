#ifndef EXPORTER_H
#define EXPORTER_H

int init_http_server(int port);
char *collect_metrics(void);
int main(int argc, char **argv);

#endif /* EXPORTER_H */
