/**
 * Methods for lightweight/specific HTTP for API communication.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <arpa/inet.h>

#include "libp2p/net/p2pnet.h"
#include "libp2p/utils/logger.h"
#include "ipfs/core/api.h"

volatile int conns_count;

pthread_t listen_thread = 0;

struct s_list api_list;

/**
 * Create a new bitswap exchange
 * @param ptr is the connection index in api_list, integer not pointer, cast required.
 * @returns nothing
 */
void *api_connection_thread (void *ptr)
{
	int timeout, s, r;
	const INT_TYPE i = (INT_TYPE) ptr;
	char buf[MAX_READ+1];
	char client[INET_ADDRSTRLEN];

	buf[MAX_READ] = '\0';

	s = api_list.conns[i]->socket;
	timeout = api_list.timeout;

	if (socket_read_select4(s, timeout) <= 0) {
		libp2p_logger_error("api", "Client connection timeout.\n");
		goto quit;
	}
	r = read(s, buf, sizeof buf);
	if (r <= 0) {
		libp2p_logger_error("api", "Read from client fail.\n");
		goto quit;
	}
	buf[r] = '\0';
	if (cstrstart(buf, "GET ")) {
		// just an error message, because it's not used.
		write_cstr (s, "HTTP/1.1 404 Not Found\r\n"
			       "Content-Type: text/plain; charset=utf-8\r\n"
			       "X-Content-Type-Options: nosniff\r\n"
			       "Content-Length: 19\r\n\r\n"
			       "404 page not found\n");
	//} else if (cstrstart(buf, "POST ")) {
		// TODO: Handle chunked/gzip/form-data/json POST requests.
	} else {
		write_cstr (s, "HTTP/1.1 400 Bad Request\r\n"
			       "Content-Type: text/plain\r\n"
			       "Connection: close\r\n\r\n"
			       "400 Bad Request");
	}

quit:
	if (inet_ntop(AF_INET, &(api_list.conns[i]->ipv4), client, INET_ADDRSTRLEN) == NULL)
		strcpy(client, "UNKNOW");
	libp2p_logger_error("api", "Closing client connection %s:%d (%d).\n", client, api_list.conns[i]->port, i+1);
	close(s);
	free (api_list.conns[i]);
	api_list.conns[i] = NULL;
	conns_count--;

	return NULL;
}

/**
 * Close all connections stopping respectives pthreads and free allocated memory.
 */
void api_connections_cleanup (void)
{
	int i;

	if (conns_count > 0 && api_list.conns) {
		for (i = 0 ; i < api_list.max_conns ; i++) {
			if (api_list.conns[i]->pthread) {
				pthread_cancel (api_list.conns[i]->pthread);
				close (api_list.conns[i]->socket);
				free (api_list.conns[i]);
				api_list.conns[i] = NULL;
			}
		}
		conns_count = 0;
	}
	if (api_list.conns) {
		free (api_list.conns);
		api_list.conns = NULL;
	}
}

/**
 * Pthread to keep in background dealing with client connections.
 * @param ptr is not used.
 * @returns nothing
 */
void *api_listen_thread (void *ptr)
{
	int s;
	INT_TYPE i;
	uint32_t ipv4;
	uint16_t port;
	char client[INET_ADDRSTRLEN];

	conns_count = 0;

	for (;;) {
		s = socket_accept4(api_list.socket, &ipv4, &port);
		if (s <= 0) {
			break;
		}
		if (conns_count >= api_list.max_conns) { // limit reached.
			libp2p_logger_error("api", "Limit of connections reached (%d).\n", api_list.max_conns);
			close (s);
			continue;
		}

		conns_count++;
		for (i = 0 ; i < api_list.max_conns && api_list.conns[i] ; i++);
		api_list.conns[i] = malloc (sizeof (struct s_conns));
		if (!api_list.conns[i]) {
			libp2p_logger_error("api", "Fail to allocate memory to accept connection.\n");
			close (s);
			continue;
		}
		if (inet_ntop(AF_INET, &ipv4, client, INET_ADDRSTRLEN) == NULL)
			strcpy(client, "UNKNOW");
		libp2p_logger_error("api", "Accept connection %s:%d (%d/%d), pthread %d.\n", client, port, conns_count, api_list.max_conns, i+1);
		api_list.conns[i]->socket = s;
		api_list.conns[i]->ipv4   = ipv4;
		api_list.conns[i]->port   = port;
		if (pthread_create(&(api_list.conns[i]->pthread), NULL, api_connection_thread, (void*)i)) {
			libp2p_logger_error("api", "Create pthread fail.\n");
			free (api_list.conns[i]);
			api_list.conns[i] = NULL;
			conns_count--;
			close(s);
		}
	}
	api_connections_cleanup ();
	return NULL;
}

/**
 * Start API interface daemon.
 * @param port.
 * @param max_conns.
 * @param timeout time out of client connection.
 * @returns 0 when failure or 1 if success.
 */
int api_start (uint16_t port, int max_conns, int timeout)
{
	int s;
	size_t alloc_size = sizeof(void*) * max_conns;

	api_list.ipv4 = hostname_to_ip("127.0.0.1"); // api is listening only on loopback.
	api_list.port = port;

	if (listen_thread != 0) {
		libp2p_logger_error("api", "API already running.\n");
		return 0;
	}

	if ((s = socket_listen(socket_tcp4(), &(api_list.ipv4), &(api_list.port))) <= 0) {
		libp2p_logger_error("api", "Failed to init API. port: %d\n", port);
		return 0;
	}

	api_list.socket = s;
	api_list.max_conns = max_conns;
	api_list.timeout = timeout;

	api_list.conns = malloc (alloc_size);
	if (!api_list.conns) {
		close (s);
		libp2p_logger_error("api", "Error allocating memory.\n");
		return 0;
	}
	memset(api_list.conns, 0, alloc_size);

	if (pthread_create(&listen_thread, NULL, api_listen_thread, NULL)) {
		close (s);
		free (api_list.conns);
		api_list.conns = NULL;
		listen_thread = 0;
		libp2p_logger_error("api", "Error creating thread for API.\n");
		return 0;
	}

	return 1;
}

/**
 * Stop API.
 * @returns 0 when failure or 1 if success.
 */
int api_stop (void)
{
	if (!listen_thread) return 0;
	pthread_cancel(listen_thread);

	api_connections_cleanup ();

	listen_thread = 0;

	return 1;
}
