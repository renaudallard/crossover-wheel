/*
 * t150d.h - internals of the force feedback daemon.
 *
 * The session is deliberately free of sockets and of clocks: it is handed a
 * frame and the current time, and it hands back a reply. That is what lets
 * tests/daemon_check.c drive every rule in it, including the watchdog,
 * without a socket, without a wheel and without waiting for real time to
 * pass.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef T150D_H
#define T150D_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "t150/effect.h"
#include "t150/proto.h"

/*
 * Where the packets go. The daemon owns one of these; on macOS it will write
 * to the wheel, and everywhere else it writes to a log so the whole stack
 * can be exercised without hardware.
 */
struct t150_backend {
	const char	*name;
	int		(*write)(void *priv, const uint8_t *buf, size_t len);
	void		(*close)(void *priv);
	void		 *priv;
};

/* The logging backend, which drives nothing and records everything. */
int	t150_backend_fake(struct t150_backend *be, FILE *fp);

/*
 * A ramp is not in the wheel's protocol, so it is sent as a constant that
 * the daemon re-sends as it slides. This is how often it does that.
 */
#define T150_RAMP_TICK_MS	20u

struct t150_slot {
	uint8_t			used;
	uint8_t			playing;
	uint8_t			source_kind;	/* what the game asked for */
	uint8_t			iterations;
	uint64_t		started_ms;
	int32_t			last_level;	/* last sliced ramp level */
	struct t150_ramp	ramp;		/* kept for the slicing */
	struct t150_effect	ef;		/* downgraded, as sent */
};

struct t150_session {
	struct t150_backend	*be;
	struct t150_slot	 slots[T150_SLOT_MAX];
	uint64_t		 last_frame_ms;
	int			 hello;
	int			 armed;		/* the wheel may be holding a force */
	int			 verbose;
	char			 token[T150_TOKEN_LEN + 1];
};

struct t150_reply {
	uint8_t	op;
	uint8_t	payload[8];
	size_t	len;
};

void	t150_session_init(struct t150_session *s, struct t150_backend *be,
	    const char *token);

/*
 * Handle one frame. Always produces a reply, because an error a game can
 * read is better than a hangup it cannot. Returns -1 only when the session
 * should be torn down afterwards, which today means BYE.
 */
int	t150_session_frame(struct t150_session *s, uint8_t op,
	    const uint8_t *payload, size_t len, uint64_t now_ms,
	    struct t150_reply *rep);

/*
 * Slide any running ramp and fire the watchdog if the client has gone quiet.
 * Returns how many milliseconds may pass before it needs calling again.
 */
unsigned int t150_session_tick(struct t150_session *s, uint64_t now_ms);

/*
 * Stop every effect and release the autocenter, leaving the wheel limp. Used
 * on the watchdog, on disconnect, when a second client displaces the first,
 * and on the way out.
 */
void	t150_session_panic(struct t150_session *s, const char *why);

/*
 * The client has gone for good rather than gone quiet: panic, then close the
 * wheel's input so it returns to its own autocenter. The wheel renders no
 * effect while no input is open, so whatever opens one has to close it.
 */
void	t150_session_end(struct t150_session *s, const char *why);

#endif /* T150D_H */
