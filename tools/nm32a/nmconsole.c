/*
 * nmconsole -- reverse-telnet server for the NM-32A's 32 async ports.
 *
 * What IOS gives you as "line 1/n on TCP 2000+line", this gives you as
 * /dev/ttyNMn on TCP 2000+n.  Port 16 -> TCP 2016.
 *
 * One process, one select() loop, no per-port forks: the card's ports are
 * console lines, so the traffic is tiny and a single loop keeps the whole
 * thing inspectable.  One TCP client per port at a time, last one wins --
 * matching what a console server is actually used for.
 *
 * Speaks enough of the telnet protocol to be usable from an ordinary telnet
 * client: a raw relay forwards the client's IAC negotiation straight to the
 * console as garbage, which is what "telnet host 2016" would otherwise put on
 * the far end's login prompt.  On connect it offers the usual console-server
 * options (WILL ECHO, WILL SGA, DO SGA), answers later negotiation with a
 * refusal, strips IAC sequences from the client stream, and doubles any 0xFF
 * going the other way.  --raw turns all of that off for netcat-style use.
 *
 *   nmconsole [--base 2000] [--ports 32] [--speed 9600] [--raw]
 *             [--port-speed N:BAUD]...   one port, repeatable
 *             [--speeds FILE]            a table of them
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define MAXP 32

struct port {
	int	listen_fd;
	int	client_fd;
	int	tty_fd;
	int	tcp_port;
};

static struct port ports[MAXP];
static int nports = MAXP, base_port = 2000, speed = 9600, raw_mode = 0;

/*
 * Per-port speed.  A console server on a mixed rack needs this: a modern switch
 * console often runs at 115200 while older gear defaults to 9600, and one
 * global rate makes one of them unreadable.  -1 means "use the global default".
 *
 * open_tty() used to force the global speed onto every port each time it opened
 * one, so an stty set by hand was silently undone on the next client connect.
 */
static int port_speed[MAXP];

static void set_port_speed(int n, int baud)
{
	if (n >= 0 && n < MAXP)
		port_speed[n] = baud;
}

/*
 * Read a speed table:  <port> <baud>   # comment
 * Port numbers are tty/TCP indices (0-31), i.e. TCP 2000+n.  Note the physical
 * breakout is 1-based: breakout port N is index N-1.
 */
static int load_speed_conf(const char *path)
{
	char line[128];
	int n, baud, count = 0;
	FILE *f = fopen(path, "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *h = strchr(line, '#');
		if (h)
			*h = 0;
		if (sscanf(line, "%d %d", &n, &baud) == 2) {
			set_port_speed(n, baud);
			count++;
		}
	}
	fclose(f);
	return count;
}

/* telnet protocol, RFC 854 */
#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define BRK  243	/* telnet Send Break */
#define IP   244	/* Interrupt Process -- some clients send this instead */
#define SE   240
#define OPT_ECHO 1
#define OPT_SGA  3

/*
 * Strip telnet control sequences from the client's stream, in place.
 * Any option the client asks us to enable is refused (WONT/DONT): a console
 * server has nothing to negotiate beyond the initial offer.
 */
static int telnet_filter(int fd, int tty_fd, unsigned char *b, int n)
{
	int i, o = 0;

	for (i = 0; i < n; i++) {
		if (b[i] != IAC) {
			b[o++] = b[i];
			continue;
		}
		if (i + 1 >= n)
			break;			/* trailing IAC: drop it */
		switch (b[i + 1]) {
		case IAC:			/* escaped 0xFF -> literal */
			b[o++] = IAC;
			i++;
			break;
		case DO:
		case DONT:
		case WILL:
		case WONT: {
			unsigned char reply[3];

			if (i + 2 >= n) { i = n; break; }
			reply[0] = IAC;
			reply[1] = (b[i + 1] == DO || b[i + 1] == DONT) ? WONT : DONT;
			reply[2] = b[i + 2];
			(void)!write(fd, reply, 3);
			i += 2;
			break;
		}
		case BRK:
		case IP:
			/*
			 * Send Break.  Without this the client's break request
			 * is swallowed by the default case below and nothing
			 * reaches the line -- which is useless on a console
			 * server, since a break is how you get a Cisco
			 * supervisor's attention (rommon) or a stuck getty's.
			 *
			 * tcsendbreak() goes through the tty layer to the
			 * driver's ->break_ctl.  Deliberate breaks only: this is
			 * the one path that should ever raise one.
			 */
			if (tty_fd >= 0)
				tcsendbreak(tty_fd, 0);
			i++;
			break;
		case SB:			/* skip to SE */
			while (i + 1 < n && !(b[i] == IAC && b[i + 1] == SE))
				i++;
			i++;
			break;
		default:
			i++;			/* two-byte command */
			break;
		}
	}
	return o;
}

/* double any 0xFF heading out, so console data cannot look like IAC */
static void telnet_write(int fd, const unsigned char *b, int n)
{
	unsigned char out[1024];
	int i, o = 0;

	for (i = 0; i < n; i++) {
		out[o++] = b[i];
		if (b[i] == IAC && o < (int)sizeof(out))
			out[o++] = IAC;
		if (o >= (int)sizeof(out) - 1) {
			(void)!write(fd, out, o);
			o = 0;
		}
	}
	if (o)
		(void)!write(fd, out, o);
}

static speed_t to_speed(int b)
{
	switch (b) {
	case 1200:   return B1200;
	case 2400:   return B2400;
	case 4800:   return B4800;
	case 9600:   return B9600;
	case 19200:  return B19200;
	case 38400:  return B38400;
	case 57600:  return B57600;
	case 115200: return B115200;
	}
	return B9600;
}

static int open_tty(int n)
{
	char path[32];
	struct termios t;
	int fd;

	snprintf(path, sizeof(path), "/dev/ttyNM%d", n);
	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (!tcgetattr(fd, &t)) {
		int b = (port_speed[n] > 0) ? port_speed[n] : speed;

		cfmakeraw(&t);
		cfsetispeed(&t, to_speed(b));
		cfsetospeed(&t, to_speed(b));
		t.c_cflag |= CLOCAL | CREAD;
		t.c_cflag &= ~CRTSCTS;
		tcsetattr(fd, TCSANOW, &t);
	}
	return fd;
}

static int listen_on(int tcp_port)
{
	struct sockaddr_in a;
	int fd, one = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(tcp_port);
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 1)) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	int i, up = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--base") && i + 1 < argc)
			base_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ports") && i + 1 < argc)
			nports = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
			speed = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--port-speed") && i + 1 < argc) {
			int n, b;                       /* --port-speed 0:115200 */
			if (sscanf(argv[++i], "%d:%d", &n, &b) == 2)
				set_port_speed(n, b);
		} else if (!strcmp(argv[i], "--speeds") && i + 1 < argc)
			load_speed_conf(argv[++i]);
		else if (!strcmp(argv[i], "--raw"))
			raw_mode = 1;
	}
	if (nports > MAXP)
		nports = MAXP;
	signal(SIGPIPE, SIG_IGN);

	for (i = 0; i < nports; i++) {
		ports[i].client_fd = -1;
		ports[i].tty_fd = -1;
		ports[i].tcp_port = base_port + i;
		ports[i].listen_fd = listen_on(ports[i].tcp_port);
		if (ports[i].listen_fd < 0) {
			fprintf(stderr, "port %d: cannot listen on %d: %s\n",
				i, ports[i].tcp_port, strerror(errno));
			continue;
		}
		up++;
	}
	printf("nmconsole: %d ports at %d baud, TCP %d..%d\n",
	       up, speed, base_port, base_port + nports - 1);
	printf("  port 16 (first of the third octal cable) -> telnet <host> %d\n",
	       base_port + 16);
	fflush(stdout);

	for (;;) {
		fd_set r;
		int max = -1;

		FD_ZERO(&r);
		for (i = 0; i < nports; i++) {
			if (ports[i].listen_fd >= 0 && ports[i].client_fd < 0) {
				FD_SET(ports[i].listen_fd, &r);
				if (ports[i].listen_fd > max) max = ports[i].listen_fd;
			}
			if (ports[i].client_fd >= 0) {
				FD_SET(ports[i].client_fd, &r);
				if (ports[i].client_fd > max) max = ports[i].client_fd;
			}
			if (ports[i].tty_fd >= 0) {
				FD_SET(ports[i].tty_fd, &r);
				if (ports[i].tty_fd > max) max = ports[i].tty_fd;
			}
		}
		if (max < 0) { sleep(1); continue; }
		if (select(max + 1, &r, NULL, NULL, NULL) < 0) {
			if (errno == EINTR) continue;
			break;
		}

		for (i = 0; i < nports; i++) {
			struct port *pt = &ports[i];
			char buf[512];
			int n, one = 1;

			/* new client */
			if (pt->listen_fd >= 0 && pt->client_fd < 0 &&
			    FD_ISSET(pt->listen_fd, &r)) {
				pt->client_fd = accept(pt->listen_fd, NULL, NULL);
				if (pt->client_fd >= 0) {
					setsockopt(pt->client_fd, IPPROTO_TCP,
						   TCP_NODELAY, &one, sizeof(one));
					pt->tty_fd = open_tty(i);
					if (pt->tty_fd < 0) {
						dprintf(pt->client_fd,
							"cannot open /dev/ttyNM%d\r\n", i);
						close(pt->client_fd);
						pt->client_fd = -1;
					} else {
						if (!raw_mode) {
							/* character-at-a-time, we echo */
							static const unsigned char hello[] = {
								IAC, WILL, OPT_ECHO,
								IAC, WILL, OPT_SGA,
								IAC, DO,   OPT_SGA,
							};
							(void)!write(pt->client_fd, hello,
								     sizeof(hello));
						}
						printf("port %d: client connected\n", i);
						fflush(stdout);
					}
				}
			}
			/* network -> serial */
			if (pt->client_fd >= 0 && FD_ISSET(pt->client_fd, &r)) {
				n = read(pt->client_fd, buf, sizeof(buf));
				if (n <= 0) {
					close(pt->client_fd); pt->client_fd = -1;
					if (pt->tty_fd >= 0) { close(pt->tty_fd); pt->tty_fd = -1; }
					printf("port %d: client gone\n", i); fflush(stdout);
				} else if (pt->tty_fd >= 0) {
					if (!raw_mode)
						n = telnet_filter(pt->client_fd,
								  pt->tty_fd,
								  (unsigned char *)buf, n);
					if (n > 0)
						(void)!write(pt->tty_fd, buf, n);
				}
			}
			/* serial -> network */
			if (pt->tty_fd >= 0 && FD_ISSET(pt->tty_fd, &r)) {
				n = read(pt->tty_fd, buf, sizeof(buf));
				if (n > 0 && pt->client_fd >= 0) {
					if (raw_mode)
						(void)!write(pt->client_fd, buf, n);
					else
						telnet_write(pt->client_fd,
							     (unsigned char *)buf, n);
				}
			}
		}
	}
	return 0;
}
