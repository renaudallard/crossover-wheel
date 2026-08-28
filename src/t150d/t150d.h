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

#include <stdatomic.h>
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
	 *
	 * Atomic because with -w the writer thread bumps it while the poll
	 * thread reads it.
	 */
	atomic_uint	 epoch;
	/*
	 * How many packets the backend took and then could not put on the
	 * wheel. Monotonic, and read the way epoch is: the session keeps its
	 * own copy and reconciles when the two differ.
	 *
	 * A backend that writes on the caller's thread never moves this,
	 * because it answers about the packet it was given and there is
	 * nothing to carry. One with a writer answers before the wheel has
	 * the packet, so a refusal it meets afterwards belongs to a packet
	 * already reported as taken, and this is the only honest way to say
	 * so: a count nobody can consume, rather than a flag whose first
	 * reader takes it from every other.
	 */
	atomic_uint	 lost;
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
	/*
	 * Whether the backend has a backlog: whether a packet handed over now
	 * would go behind others still waiting rather than out next.
	 *
	 * Not whether the wheel is idle, which is a different question and not
	 * the useful one. The writer takes a packet off its queue before it
	 * writes it, so a transfer in flight leaves the queue empty and this
	 * answers yes, and that is right: the pass that answer allows builds
	 * the next packet while the wheel takes the current one, and it lands
	 * at the front of an empty queue rather than behind anything.
	 *
	 * Only a backend with a writer thread can answer at all, because it is
	 * the only one that takes a packet and returns before the wheel has
	 * it, and so the only one with a queue of its own to be empty. One
	 * that writes on the calling thread leaves this NULL, so that a NULL
	 * hook is the whole test: an early pass there would put a game's frame
	 * behind a USB transfer, which is the cost the writer exists to
	 * remove.
	 */
	int		(*idle)(void *priv);
	/*
	 * Wait until everything handed over has been tried on the wheel.
	 * Returns 0 when the queue emptied, -1 when it did not.
	 *
	 * Only that. Whether any of it was refused is the lost count above,
	 * which the caller brackets its own writes with: a hook that answered
	 * both questions had to consume something to answer the second, and
	 * anything it consumed was taken from somebody else.
	 *
	 * This exists because a backend with a writer thread answers a write
	 * the moment the bytes are copied, so a 0 from it means queued and not
	 * delivered. Every rule in session.c is written against the other
	 * meaning: slot_stop records a refused stop from the answer it gets,
	 * and the safe state forgets a slot only when its stop returned 0. A
	 * stop that was merely queued and then refused therefore let the safe
	 * state erase a slot the wheel was still rendering, with armed cleared
	 * so the watchdog would never look at it again.
	 *
	 * Only the safe state calls this. It is off the hot path, it is the one
	 * place where knowing beats being quick, and it is the path whose
	 * failure leaves a force on somebody's hands. A backend that writes on
	 * the caller's thread already tells the truth and leaves this NULL.
	 */
	int		(*drain)(void *priv);
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
	    unsigned int gap_ms, int verbose, int threaded,
	    uint32_t autocenter);
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
 * milliseconds, so while this floor is the interval about every other update
 * is superseded and the wheel sees roughly 250 Hz of a signal whose own
 * bandwidth is lower again. With a writer that is keeping up it is not the
 * interval and nothing is superseded, which is the paragraph at the end.
 *
 * Four milliseconds rather than the two a 500 Hz emitter would use, because
 * nothing here has ever measured what packet rate this wheel sustains and
 * every hardware run on record was paced slower. Raise it when a measurement
 * says it can be raised, and expect the argument for raising it to be a
 * driver who can feel the difference rather than a number that looks better.
 *
 * A backend with a writer thread paces itself, so a pass can run ahead of this
 * whenever that thread has no backlog: see the idle callback above and
 * emit_now in session.c. That is off unless -E asks for it, and it was the
 * default for exactly three releases.
 *
 * It went back to being off because it is the only change to how this daemon
 * drives the wheel in about thirty releases, and force feedback started
 * stopping mid session in the release that introduced it, with the wheel found
 * back at its boot identity. That is what a device does when it resets itself.
 * Nothing proves the two are connected and the measurement to settle it has
 * not been made, but the early pass has never been shown to help anybody
 * either, so it is not worth carrying that suspicion by default. RESEARCH.md
 * A51.
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

/*
 * What the -v log has already said about starting a slot. Two bits rather than
 * one flag, because a start has two answers and each is worth exactly one
 * line: a wheel that has gone says it could not start, the wheel that comes
 * back says it did, and a wheel alternating between the two at frame rate says
 * each of them once and then nothing at all.
 */
#define T150_SAID_STARTED	0x01u
#define T150_SAID_REFUSED	0x02u

/* One packet exactly as it went out, for deciding whether it need go again. */
struct t150_wire {
	uint8_t	len;			/* 0 when nothing has been sent yet */
	uint8_t	buf[T150_PKT_MAX];
};

struct t150_slot {
	uint8_t			used;
	/*
	 * What the game wants, and only that. It is cleared by a stop whether
	 * or not the stop reached the wheel, because a refused stop is still a
	 * stop the game asked for: leaving it set had the re-acquire replay
	 * start the very force the game had asked to be rid of.
	 */
	uint8_t			playing;
	/*
	 * And what the wheel may still be doing about it. A stop the wheel
	 * refused leaves this behind so the slot survives to be stopped again,
	 * rather than being forgotten by a memset while the wheel pulls.
	 */
	uint8_t			stop_owed;
	uint8_t			source_kind;	/* what the game asked for */
	uint8_t			iterations;
	uint8_t			dirty;		/* desired state is not on the wheel */
	/*
	 * Which of the two answers above the log has already given for this
	 * slot. It cannot be read off playing, which is what it used to be:
	 * playing is what the game wants and do_start records it before the
	 * write on purpose, so a start the wheel refused left it standing and
	 * the start that finally landed said nothing at all.
	 *
	 * A stop reopens it, and so does a slot being loaded from empty,
	 * because either makes the next start a new event. A re-upload of a
	 * slot that is already loaded does not, because a game animating a
	 * force re-uploads it and starts it again on every frame and losing it
	 * there would put three hundred lines a second into a report. The
	 * wheel going away reopens it too, since what was said was said about
	 * a wheel that is no longer there.
	 */
	uint8_t			start_said;
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
	uint64_t		 last_param_log_ms;	/* -v effect parameters, rate limited */
	unsigned int		 epoch;		/* the backend epoch we believe */
	unsigned int		 lost;		/* and its dropped packet count */
	int			 hello;
	/*
	 * A connection that is only proving its token, because someone else
	 * holds the wheel. Such a session states its device settings on the
	 * tick after it is promoted rather than inside the hello itself: the
	 * newcomer proves the token before the incumbent has been made safe,
	 * so writing them there told the wheel to render at the newcomer's
	 * strength while the outgoing client's forces were still playing. The
	 * input open stays in the hello, deliberately, because the promoted
	 * session needs the wheel listening from the moment it takes over.
	 */
	uint8_t			 pending;
	uint8_t			 settings_owed;
	int			 armed;		/* the wheel may be holding a force */
	int			 input_open;	/* the wheel's input is open on our account */
	int			 verbose;
	unsigned int		 range_deg;	/* 0 leaves the wheel's own */
	/*
	 * The device gain the client last asked for, in DirectInput's 0 to
	 * 10000. Full until it says otherwise, which is DirectInput's own
	 * default and the honest starting point. Remembered because the wheel
	 * forgets it and this is the only thing that can put it back: the
	 * proxy sends DIPROP_FFGAIN once and never again.
	 */
	uint32_t		 gain;
	/*
	 * The centring spring, in the same 0 to 10000.
	 *
	 * These are two different questions and so they are two fields.
	 * autocenter is what -a asked for: what a person wants the wheel to
	 * feel like when no game is driving it, which is where every safe
	 * state leaves it. Zero is limp, is the default, and is what a game
	 * sending its own forces wants.
	 *
	 * client_autocenter is what the game asked for while it drives, and is
	 * what a re-acquired wheel is given back, because the wheel forgets it
	 * and the client has no reason to say it twice.
	 */
	uint32_t		 autocenter;
	uint32_t		 client_autocenter;
	int			 client_set_autocenter;
	int			 always_triple;	/* -t: re-send the set on any change */
	int			 early_pass;	/* -E: may emit ahead of the floor */
	uint8_t			 next_slot;	/* where the next pass resumes */
	uint8_t			 io_err;	/* a write failed, owed to the next upload */
	uint8_t			 replay_starts;	/* the wheel came back, re-start what was playing */
	uint8_t			 emit_failed;	/* last pass failed, do not spin on it */
	/*
	 * A start frame that named a slot this daemon does not have, said once
	 * for the session. Our own proxy cannot send one, so a second is a
	 * broken client repeating itself rather than news.
	 */
	uint8_t			 bad_slot_said;
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
 * Carry the stops a displaced session could not get to the wheel over to the
 * session replacing it, so the one thing that knows the wheel may still be
 * pulling does not go with the connection that learned it.
 */
void	t150_session_inherit_stops(struct t150_session *to,
	    const struct t150_session *from);

/*
 * The client has gone for good rather than gone quiet: make the wheel safe
 * and let the session go. The wheel's input stays open, because the daemon
 * still holds the wheel and the firmware rests both pedals at maximum while
 * no input is open, which would leave the next game calibrating them pressed.
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
