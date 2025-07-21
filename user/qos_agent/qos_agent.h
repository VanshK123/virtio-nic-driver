#ifndef QOS_AGENT_H
#define QOS_AGENT_H

int init_netlink(void);
int apply_rate_limit(int flow_id, int rate);
int main(int argc, char **argv);

#endif /* QOS_AGENT_H */
