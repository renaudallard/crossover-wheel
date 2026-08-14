/*
 * client.c - the proxy's end of the connection to the daemon.
 *
 * Finds the endpoint the daemon published, connects to loopback, and speaks
 * the protocol. Also runs the keepalive, which is not optional: the daemon
 * treats silence as a crashed game and releases the wheel, so a game holding
 * one steady force and calling nothing would otherwise lose it after half a
 * second.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proxy.h"

/* Comfortably inside T150_WATCHDOG_MS, with room for a scheduling hiccup. */
#define KEEPALIVE_MS	150

static CRITICAL_SECTION lock;
static SOCKET sock = INVALID_SOCKET;
static int keepalive_running;
static int started;
static int online;
/*
 * Bumped on every successful connect. An effect object compares it against
 * its own copy to notice that the daemon it uploaded to is not the daemon it
 * is talking to now, which happens whenever t150d is restarted under a
 * running game.
 */
static unsigned int generation;
/* Not before this tick, so a failing reconnect cannot become a spin. */
static ULONGLONG next_connect_ms;
#define RECONNECT_MS	1000

static int	connect_locked(void);

/*
 * T150_DEBUG speaks to stderr and OutputDebugString, which is enough from a
 * terminal and nothing at all from a game Steam relaunched, whose stderr
 * goes nowhere anyone can see. T150_LOG names a file to append the same
 * lines to, opened per line so any number of bottle processes can share it.
 * Either variable alone is enough to turn logging on.
 */
void
t150_log(const char *fmt, ...)
{
	static int checked, wanted, havelog;
	static char logpath[MAX_PATH];
	char buf[512];
	va_list ap;
	DWORD len;
	int n;

	if (!checked) {
		checked = 1;
		wanted = GetEnvironmentVariableA("T150_DEBUG", NULL, 0) > 0;
		len = GetEnvironmentVariableA("T150_LOG", logpath,
		    sizeof(logpath));
		havelog = len > 0 && len < sizeof(logpath);
	}
	if (!wanted && !havelog)
		return;

	n = snprintf(buf, sizeof(buf), "t150-dinput8: ");
	va_start(ap, fmt);
	(void)vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
	va_end(ap);

	if (wanted) {
		OutputDebugStringA(buf);
		fprintf(stderr, "%s", buf);
	}
	if (havelog) {
		FILE *fp = fopen(logpath, "a");

		if (fp != NULL) {
			(void)fputs(buf, fp);
			(void)fclose(fp);
		}
	}
}

/*
 * Where the daemon publishes its port and token.
 *
 * T150_ENDPOINT wins, because an installer knows exactly where it put
 * things. The guesses below go through Z:, Wine's mapping of the whole host
 * filesystem, to the daemon's place under a macOS home directory.
 */
/*
 * What call_locked() distinguishes. A refusal is the daemon answering an
 * error, which leaves the connection perfectly usable; a failure is the
 * transport breaking, which does not.
 */
#define CALL_FAILED	(-1)
#define CALL_REFUSED	1

/* Fill out with the endpoint under one macOS home; 0 only if it exists. */
static int
endpoint_under(const char *name, char *out, size_t outlen)
{
	if ((size_t)snprintf(out, outlen,
	    "Z:\\Users\\%s\\Library\\Application Support\\t150ffb\\endpoint",
	    name) >= outlen)
		return -1;
	if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES)
		return -1;

	return 0;
}

static int
endpoint_path(char *out, size_t outlen)
{
	WIN32_FIND_DATAA fd;
	HANDLE h;
	char user[256];
	DWORD n;
	int found = -1;

	/*
	 * GetEnvironmentVariableA returns the size it needed, not the size it
	 * wrote, when the buffer is too small, and it leaves the buffer
	 * untouched. Treating that as success handed an uninitialised path to
	 * fopen.
	 */
	n = GetEnvironmentVariableA("T150_ENDPOINT", out, (DWORD)outlen);
	if (n > 0 && n < outlen)
		return 0;

	/*
	 * The USERNAME guess only holds outside CrossOver: a bottle's Windows
	 * user is named "crossover" whoever owns the Mac, which test 17's log
	 * proved by watching the proxy look in Z:\Users\crossover. So the
	 * guess is checked against the filesystem, and when it misses, every
	 * home under Z:\Users is tried, because the one running the daemon
	 * has the endpoint file and the others have nothing.
	 *
	 * Bounded the same way as the read above, and for the same reason: a
	 * variable that did not fit returns the size it needed and leaves the
	 * buffer untouched, so testing only for non-zero would hand an
	 * uninitialised path on to snprintf.
	 */
	n = GetEnvironmentVariableA("USERNAME", user, (DWORD)sizeof(user));
	if (n > 0 && n < sizeof(user) && endpoint_under(user, out, outlen) == 0)
		return 0;

	if ((h = FindFirstFileA("Z:\\Users\\*", &fd)) == INVALID_HANDLE_VALUE)
		return -1;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		if (strcmp(fd.cFileName, ".") == 0 ||
		    strcmp(fd.cFileName, "..") == 0)
			continue;
		if (endpoint_under(fd.cFileName, out, outlen) == 0) {
			found = 0;
			break;
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);

	return found;
}

static int
read_endpoint(unsigned short *port, char *token, size_t tokenlen)
{
	char path[MAX_PATH], line[64];
	unsigned long p;
	FILE *fp;

	if (endpoint_path(path, sizeof(path)) != 0) {
		t150_log("no endpoint under Z:\\Users, is t150d running? "
		    "staying out of the way\n");
		return -1;
	}
	if ((fp = fopen(path, "r")) == NULL) {
		t150_log("no endpoint at %s, staying out of the way\n", path);
		return -1;
	}

	if (fgets(line, sizeof(line), fp) == NULL ||
	    (p = strtoul(line, NULL, 10)) == 0 || p > 0xffff ||
	    fgets(token, (int)tokenlen, fp) == NULL) {
		(void)fclose(fp);
		return -1;
	}
	(void)fclose(fp);

	token[strcspn(token, "\r\n")] = '\0';
	if (strlen(token) != T150_TOKEN_LEN)
		return -1;
	*port = (unsigned short)p;

	return 0;
}

/* Send a whole frame and read the single reply it earns. Lock held. */
static int
call_locked(uint8_t op, const void *payload, size_t len)
{
	uint8_t buf[T150_PROTO_HDR_LEN + T150_PROTO_MAX_PAYLOAD];
	struct t150_proto_hdr hdr;
	size_t n, off;
	int r;

	if (sock == INVALID_SOCKET || len > T150_PROTO_MAX_PAYLOAD)
		return CALL_FAILED;

	hdr.magic = T150_PROTO_MAGIC;
	hdr.version = T150_PROTO_VERSION;
	hdr.op = op;
	hdr.length = (uint16_t)len;

	if ((n = t150_proto_pack_hdr(buf, sizeof(buf), &hdr)) == 0)
		return CALL_FAILED;
	if (len > 0)
		memcpy(buf + n, payload, len);
	n += len;

	for (off = 0; off < n; off += (size_t)r) {
		r = send(sock, (const char *)buf + off, (int)(n - off), 0);
		if (r <= 0)
			return CALL_FAILED;
	}

	for (off = 0; off < T150_PROTO_HDR_LEN; off += (size_t)r) {
		r = recv(sock, (char *)buf + off,
		    (int)(T150_PROTO_HDR_LEN - off), 0);
		if (r <= 0)
			return CALL_FAILED;
	}
	if (t150_proto_unpack_hdr(buf, T150_PROTO_HDR_LEN, &hdr) != 0)
		return CALL_FAILED;

	for (off = 0; off < hdr.length; off += (size_t)r) {
		r = recv(sock, (char *)buf + off, (int)(hdr.length - off), 0);
		if (r <= 0)
			return CALL_FAILED;
	}

	if (hdr.op != T150_OP_OK) {
		t150_log("daemon refused op %u with error %u\n", op,
		    hdr.length >= 2 ? buf[0] : 0);
		return CALL_REFUSED;
	}

	return 0;
}

static void
drop_locked(void)
{
	if (sock != INVALID_SOCKET) {
		(void)closesocket(sock);
		sock = INVALID_SOCKET;
	}
	online = 0;
}

int
t150_client_call(uint8_t op, const void *payload, size_t len)
{
	int r;

	EnterCriticalSection(&lock);

	/*
	 * A dead socket used to stay dead. t150_client_start is reachable only
	 * from di_CreateDevice and di_EnumDevices, neither of which a game
	 * calls again for a device it already holds, so restarting the daemon
	 * under a running game cost that game its force feedback until it was
	 * restarted too. The tester hit this on every comparison he ran for us.
	 *
	 * Rate limited, because reconnecting takes the wheel from whoever else
	 * has it and a game that keeps asking must not turn that into a fight.
	 */
	if (sock == INVALID_SOCKET) {
		ULONGLONG now = GetTickCount64();

		if (now >= next_connect_ms) {
			next_connect_ms = now + RECONNECT_MS;
			(void)connect_locked();
		}
	}

	r = call_locked(op, payload, len);
	/*
	 * Only a transport failure costs the connection. The daemon answers
	 * an error rather than hanging up precisely so a game can carry on,
	 * and dropping the socket here threw that away: nothing reconnects,
	 * so one refused effect used to disable force feedback for the rest
	 * of the process.
	 */
	if (r == CALL_FAILED)
		drop_locked();
	LeaveCriticalSection(&lock);

	return r == 0 ? 0 : -1;
}

/*
 * The keepalive runs for the life of the process, and there is nowhere to stop
 * it from. The only teardown hook a DLL has is DllMain, which holds the loader
 * lock, so waiting on a thread there deadlocks; a game on its way out is
 * exactly the case the daemon's watchdog exists for, and it releases the wheel
 * half a second later whatever this thread does.
 */
static DWORD WINAPI
keepalive_main(LPVOID arg)
{
	(void)arg;

	for (;;) {
		Sleep(KEEPALIVE_MS);

		EnterCriticalSection(&lock);
		if (sock != INVALID_SOCKET &&
		    call_locked(T150_OP_KEEPALIVE, NULL, 0) != 0)
			drop_locked();
		LeaveCriticalSection(&lock);
	}

	/* Not reached. */
	return 0;
}

/*
 * Connect, say hello, and start the keepalive. Called the first time a game
 * asks about a device rather than from DllMain, where creating a thread is
 * not allowed.
 */
/* The connect itself, for a caller that already holds the lock. */
static int
connect_locked(void)
{
	char token[T150_TOKEN_LEN + 1];
	struct sockaddr_in sa;
	unsigned short port;
	WSADATA wsa;
	DWORD tv;
	int ok = 0;

	if (online)
		return 0;
	if (!started) {
		/*
		 * The lock is the caller's, as the comment above says and as
		 * every other exit from this function assumes. Releasing it
		 * here as well, which is what this did, left it released twice
		 * for one enter: a leftover from when this code was the body
		 * of t150_client_start and took the lock itself.
		 */
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return -1;
		started = 1;
	}

	if (read_endpoint(&port, token, sizeof(token)) != 0)
		goto out;
	if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
		goto out;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons(port);

	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		t150_log("cannot reach the daemon on port %u\n", port);
		goto out;
	}

	/*
	 * Bound the round trip. Every wrapped call is a blocking send and two
	 * blocking receives on the thread the game is waiting on, and winsock
	 * waits for ever by default, so a peer that stopped reading without
	 * closing would park the game's frame indefinitely. Freezing the game
	 * rather than degrading is the exact failure this proxy exists to
	 * avoid: it is the first of the five seams ARCHITECTURE.md lists
	 * against putting a bus driver in the bottle.
	 *
	 * Shorter than the daemon's watchdog, so a stall is noticed before the
	 * wheel is released for it, and long enough that an ordinary reply is
	 * never in danger. A timeout goes through drop_locked rather than
	 * being retried, because a late reply would arrive against the next
	 * call and desynchronise the stream; the reconnect in t150_client_call
	 * then re-establishes the session.
	 */
	tv = T150_WATCHDOG_MS / 2;
	(void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
	    sizeof(tv));
	(void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv,
	    sizeof(tv));
	if (call_locked(T150_OP_HELLO, token, T150_TOKEN_LEN) != 0) {
		t150_log("the daemon would not take our token\n");
		goto out;
	}

	/*
	 * One for the process, and its handle is closed straight away because
	 * nothing ever waits on it. Closing a thread handle does not end the
	 * thread.
	 */
	if (!keepalive_running) {
		HANDLE th = CreateThread(NULL, 0, keepalive_main, NULL, 0, NULL);

		if (th != NULL) {
			keepalive_running = 1;
			(void)CloseHandle(th);
		}
	}

	online = 1;
	ok = 1;
	generation++;
	t150_log("connected to the daemon on port %u\n", port);

out:
	if (!ok)
		drop_locked();

	return ok ? 0 : -1;
}

int
t150_client_start(void)
{
	int r;

	EnterCriticalSection(&lock);
	r = connect_locked();
	LeaveCriticalSection(&lock);

	return r;
}

unsigned int
t150_client_generation(void)
{
	unsigned int g;

	EnterCriticalSection(&lock);
	g = generation;
	LeaveCriticalSection(&lock);

	return g;
}

int
t150_client_online(void)
{
	int r;

	EnterCriticalSection(&lock);
	r = online;
	LeaveCriticalSection(&lock);

	return r;
}

/* Called from DllMain, so it does nothing that can block or load anything. */
void	t150_client_init_lock(void);
void	t150_client_free_lock(void);

void
t150_client_init_lock(void)
{
	InitializeCriticalSection(&lock);
}

void
t150_client_free_lock(void)
{
	DeleteCriticalSection(&lock);
}
