#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>

/* Binds port 80 on every interface, so it survives switching between the
 * setup access point and a joined network. */
bool http_server_start(void);

#endif /* HTTP_SERVER_H */
