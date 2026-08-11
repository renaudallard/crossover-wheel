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
	/*
	 * Bumped every time the backend re-acquires the wheel. Acquiring
	 * scrubs every slot and the session has no other way to learn that,
	 * so this is what tells it to forget what it believes the wheel
	 * holds. Without it a wheel unplugged and replugged mid-race stays
	 * empty for the rest of the session, silently, because the session
	 * would go on suppressing an upload it thinks the wheel already has.
	 */
	unsigned int	 epoch;
	/*
	 * Called from the daemon's loop whether or not anything is being
	 * written, so a backend that has lost its device can look for it
	 * again on its own clock. Without it the only thing that ever
	 * triggered a rescan was a write, and after a safe state there are no
	 * writes: the session has cleared every slot, so nothing is dirty and
	 * the tick emits nothing. A wheel replugged into that silence would
	 * never be found however long it sat there. Optional; the logging
	 * backend has nothing to look for.
	 */
	void		(*tick)(void *priv, uint64_t now_ms);
};

/* The logging backend, which drives nothing and records everything. */
int	t150_backend_fake(struct t150_backend *be, FILE *fp);

#ifdef __APPLE__
/*
 * The real one. Writes to the wheel with IOHIDDeviceSetReport and a
 * non-seizing open, so CrossOver keeps reading it throughout. gap_ms
 * optionally pauses between packets, see hid_darwin.c for why that is off by
 * default. Succeeds even when no wheel is attached yet, and picks one up
 * when it appears.
 */
int	t150_backend_hid(struct t150_backend *be, long vid, long pid,
	    unsigned int gap_ms, int verbose);
#endif

/*
 * A ramp is not in the wheel's protocol, so it is sent as a constant that
 * the daemon re-computes as it slides. This is how often it does that, and
 * it is a recompute rather than a write: what the recompute changes is the
 * slot's desired state, and the emitter below decides whether that is worth
 * a packet.
 */
#define T150_RAMP_TICK_MS	20u

/*
 * The shortest gap between two emission passes, measured from the end of the
 * last one. This is a floor on the interval and not a timer: a tick with
 * nothing to send arms nothing and sleeps until the watchdog, so an idle
 * daemon still wakes twice a second rather than 250 times.
 *
 * A game updating one effect faster than this has its updates coalesced to
 * the newest, which is what it would have wanted: the wheel holds one value
 * per slot, so an intermediate value was superseded before it could have
 * been felt. Assetto Corsa's physics runs at 333 Hz, one update every three
 * milliseconds, so about every other update is superseded and the wheel sees
 * roughly 250 Hz of a signal whose own bandwidth is lower again.
 *
 * Four milliseconds rather than the two a 500 Hz emitter would use, because
 * nothing here has ever measured what packet rate this wheel sustains and
 * every hardware run on record was paced slower. Raise it when a measurement
 * says it can be raised, and expect the argument for raising it to be a
 * driver who can feel the difference rather than a number that looks better.
 */
#define T150_EMIT_MS		4u

/*
 * The most slots one pass will write. Sixteen dirty slots at three packets
 * each is a burst long enough to matter, and the pass resumes where it left
 * off, so this bounds a pass without starving anything.
 */
#define T150_EMIT_SLOTS		4u

/* Every packet the daemon builds fits in this. */
#define T150_PKT_MAX		16u

/* One packet exactly as it went out, for deciding whether it need go again. */
struct t150_wire {
	uint8_t	len;			/* 0 when nothing has been sent yet */
	uint8_t	buf[T150_PKT_MAX];
};

struct t150_slot {
	uint8_t			used;
	uint8_t			playing;
	uint8_t			source_kind;	/* what the game asked for */
	uint8_t			iterations;
	uint8_t			dirty;		/* desired state is not on the wheel */
	uint64_t		started_ms;
	struct t150_ramp	ramp;		/* kept for the slicing */
	struct t150_effect	ef;		/* downgraded, as desired */
	struct t150_wire	sent[3];	/* first, update and commit as sent */
};

struct t150_session {
	struct t150_backend	*be;
	struct t150_slot	 slots[T150_SLOT_MAX];
	uint64_t		 last_frame_ms;
	uint64_t		 next_emit_ms;	/* no emission pass before this */
	uint64_t		 next_ramp_ms;	/* no ramp recompute before this */
	unsigned int		 epoch;		/* the backend epoch we believe */
	int			 hello;
	int			 armed;		/* the wheel may be holding a force */
	int			 input_open;	/* we opened the wheel's input and owe it a close */
	int			 verbose;
	unsigned int		 range_deg;	/* 0 leaves the wheel's own */
	int			 always_triple;	/* -t: re-send the set on any change */
	uint8_t			 next_slot;	/* where the next pass resumes */
	uint8_t			 io_err;	/* a write failed, owed to the next upload */
	uint8_t			 replay_starts;	/* the wheel came back, re-start what was playing */
	uint8_t			 emit_failed;	/* last pass failed, do not spin on it */
	unsigned		 peer_port;	/* which connection this is, for the log */
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
 * Reconcile against the backend, fire the watchdog if the client has gone
 * quiet, slide any running ramp into its slot's desired state, and put on the
 * wheel whatever changed since the last pass. This is the only place effect
 * parameters reach the wheel: a frame sets what is wanted and this decides
 * when it goes.
 *
 * Returns how many milliseconds may pass before it needs calling again, which
 * is the watchdog remainder whenever there is nothing to send.
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
 *
 * Both of these do nothing for a session that never reached the wheel, which
 * is any connection that did not get past hello. Undoing what such a client
 * did means sending nothing, and any local process can open the port.
 */
void	t150_session_end(struct t150_session *s, const char *why);

/*
 * The same release, for the daemon on its way out rather than for a client.
 * Unconditional: the backend opens the wheel's input on its own account too,
 * so leaving is the one moment to say the safe state outright.
 */
void	t150_session_shutdown(struct t150_session *s, const char *why);

#endif /* T150D_H */
