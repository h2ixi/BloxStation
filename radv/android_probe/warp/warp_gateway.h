/* In-title SOCKS5 gateway through a Cloudflare WARP tunnel (warp_gateway.c). */
#ifndef WARP_GATEWAY_H
#define WARP_GATEWAY_H
/* Runs the gateway on 127.0.0.1:port in this thread (never returns unless
 * setup fails, -1). log receives one line at a time (NULL: stderr). */
int warp_gateway_run(const char *conf_path, unsigned short port, void (*log)(const char *line));
/* Sets up in this thread (listening when it returns), then runs the gateway
 * in a new one: 0, or -1 if setup or the thread failed. */
int warp_gateway_start(const char *conf_path, unsigned short port, void (*log)(const char *line));
#endif
