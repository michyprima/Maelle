// newlib has no poll(); the network code only polls with Wi-Fi on, which the guest build doesn't run, see runtime.cpp
#pragma once
struct pollfd { int fd; short events, revents; };
typedef unsigned int nfds_t;
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#ifdef __cplusplus
extern "C"
#endif
int poll(struct pollfd *fds, nfds_t count, int timeout);
