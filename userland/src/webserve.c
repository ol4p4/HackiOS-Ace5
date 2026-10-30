/*
 * webserve: the smallest HTTP file server, for pages the device has to load
 * over loopback.
 *
 *   usage: webserve [ROOT] [PORT]            (default /redwood/share/web, 8080)
 *
 * SpringBoard refuses to open a file:// URL handed to it by another process
 * (battery_test open prints "it REFUSES"), and with no Wi-Fi there is no other
 * origin a page could come from. Loopback needs no driver, so the page is
 * served on 127.0.0.1 and opened as http://127.0.0.1:PORT/ -- which also
 * exercises the socket path without a radio.
 *
 * HTTP/1.0 answers only, one request per connection, one connection at a time:
 * the pages are self-contained, so there are no subresources to serve in
 * parallel. Started from rc.jobs with a trailing & (see init.c run_rc).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_ROOT "/redwood/share/web"
#define DEFAULT_PORT 8080

static const char *
content_type(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (dot == NULL) {
		return "application/octet-stream";
	}
	if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
		return "text/html; charset=utf-8";
	}
	if (strcasecmp(dot, ".js") == 0) {
		return "text/javascript; charset=utf-8";
	}
	if (strcasecmp(dot, ".css") == 0) {
		return "text/css; charset=utf-8";
	}
	if (strcasecmp(dot, ".json") == 0) {
		return "application/json";
	}
	if (strcasecmp(dot, ".png") == 0) {
		return "image/png";
	}
	if (strcasecmp(dot, ".svg") == 0) {
		return "image/svg+xml";
	}
	return "application/octet-stream";
}

/* One request line: "GET /path HTTP/1.1". Anything else is a 400. */
static void
request_path(const char *request, char *path, size_t size)
{
	const char *start = strchr(request, ' ');
	const char *end;

	path[0] = '\0';
	if (start == NULL || strncmp(request, "GET ", 4) != 0) {
		return;
	}
	start++;
	end = strchr(start, ' ');
	if (end == NULL || (size_t)(end - start) >= size) {
		return;
	}
	memcpy(path, start, (size_t)(end - start));
	path[end - start] = '\0';
}

static void
send_all(int fd, const char *data, size_t size)
{
	while (size > 0) {
		ssize_t wrote = write(fd, data, size);

		if (wrote <= 0) {
			return;                 /* the client went away; not our problem */
		}
		data += wrote;
		size -= (size_t)wrote;
	}
}

static void
serve(int fd, const char *root, const char *target)
{
	char path[512];
	char body[512];
	char header[512];
	struct stat st;
	FILE *file;
	int status = 200, length = 0;

	/* "/" is the page this server exists for; nothing else has an index. */
	const char *name = (strcmp(target, "/") == 0) ? "refresh.html" : target + 1;

	if (target[0] != '/' || strstr(name, "..") != NULL || strchr(name, '\0') == NULL) {
		status = 400;
	} else if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path) ||
	    stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		status = 404;
	}

	if (status == 200 && st.st_size > 0x7fffffff) {
		status = 413;
	}

	if (status == 200) {
		file = fopen(path, "rb");
		if (file == NULL) {
			status = 404;
		}
	}
	if (status == 200) {
		length = snprintf(header, sizeof(header),
		    "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
		    "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
		    content_type(path), (long long)st.st_size);
		send_all(fd, header, (size_t)length);
		for (;;) {
			size_t got = fread(header, 1, sizeof(header), file);

			if (got == 0) {
				break;
			}
			send_all(fd, header, got);
		}
		fclose(file);
		printf("webserve: GET %s -> 200 %s (%lld bytes)\n", target, name, (long long)st.st_size);
	} else {
		int wrote = snprintf(body, sizeof(body),
		    "<!doctype html><title>webserve</title><p>%d: %s\n", status, target);

		length = snprintf(header, sizeof(header),
		    "HTTP/1.0 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
		    "Content-Length: %d\r\nConnection: close\r\n\r\n",
		    status, status == 400 ? "Bad Request" : status == 404 ? "Not Found" : "Too Large",
		    wrote);
		send_all(fd, header, (size_t)length);
		send_all(fd, body, (size_t)wrote);
		printf("webserve: GET %s -> %d\n", target, status);
	}
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	const char *root = argc > 1 ? argv[1] : DEFAULT_ROOT;
	int port = argc > 2 ? atoi(argv[2]) : DEFAULT_PORT;
	struct sockaddr_in addr;
	int listener;

	if (port <= 0 || port > 65535) {
		fprintf(stderr, "webserve: bad port %s\n", argv[2]);
		return 2;
	}
	/* Safari closes a connection it no longer waits for; that must not kill us. */
	signal(SIGPIPE, SIG_IGN);

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		perror("webserve: socket");
		return 1;
	}
	int one = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		/* No 127.0.0.1 on lo0 yet: any address still answers on loopback. */
		perror("webserve: bind 127.0.0.1, trying any");
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
			perror("webserve: bind");
			return 1;
		}
	}
	if (listen(listener, 8) != 0) {
		perror("webserve: listen");
		return 1;
	}
	printf("webserve: serving %s on http://127.0.0.1:%d/\n", root, port);
	fflush(stdout);

	for (;;) {
		int client = accept(listener, NULL, NULL);
		char request[2048];
		char target[512];
		ssize_t got;

		if (client < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("webserve: accept");
			return 1;
		}
		got = read(client, request, sizeof(request) - 1);
		request[got > 0 ? got : 0] = '\0';
		request_path(request, target, sizeof(target));
		if (target[0] == '\0') {
			close(client);
			continue;
		}
		serve(client, root, target);
		close(client);
	}
}
