#ifndef OSITO_TSSH_POLL_H
#define OSITO_TSSH_POLL_H
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
struct pollfd { int fd; short events, revents; };
int tssh_poll(struct pollfd *fds, unsigned long n, int timeout);
#define poll tssh_poll
#endif
