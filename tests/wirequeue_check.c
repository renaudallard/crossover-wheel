/*
 * wirequeue_check - the rules the writer's queue has to keep.
 *
 * The queue exists because the daemon builds packets faster than the wheel
 * takes them, and the whole question is what to throw away. Getting that
 * wrong is not visible in a log: 0.1.23's plain queue looked healthy and was
 * quietly delivering forces a fifth of a second stale. So the rules are
 * written down here and checked, on any machine, with no wheel and no Mac.
 *
 * The packet bytes below are real ones, taken from Thrustmaster's own
 * capture tmp/oldffb/directX_constforce.pcapng.
 *
 * Copyright (c) 2026 Renaud Allard
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <string.h>

#include "t150/encode.h"
#include "t150/t150.h"

#include "wirequeue.h"

static int failures;

static void
check_int(const char *what, long got, long want)
{
	if (got == want)
		return;

	fprintf(stderr, "FAIL %s: want %ld, got %ld\n", what, want, got);
	failures++;
}

/* The next packet out, as a hex string, or "" when there is none. */
static const char *
next(struct t150_wirequeue *q)
{
	static char out[3 * T150_PKT_MAX + 1];
	struct t150_wire w;
	size_t i;

	out[0] = '\0';
	if (!t150_wq_pop(q, &w))
		return out;

	for (i = 0; i < w.len; i++)
		snprintf(out + i * 3, 4, "%02x ", w.buf[i]);
	if (w.len > 0)
		out[w.len * 3 - 1] = '\0';

	return out;
}

static void
check_next(const char *what, struct t150_wirequeue *q, const char *want)
{
	const char *got = next(q);

	if (strcmp(got, want) == 0)
		return;

	fprintf(stderr, "FAIL %s\n  want: %s\n  got:  %s\n", what, want, got);
	failures++;
}

#define PUSH(q, ...) do {						\
	const uint8_t p__[] = { __VA_ARGS__ };				\
	(void)t150_wq_push((q), p__, sizeof(p__));			\
} while (0)

/* ff_update for slot 0's constant, which is the packet a game modulates. */
#define UPDATE(q, level)	PUSH((q), 0x03, 0x0e, 0x00, (level))
#define CONTROL_PLAY(q)		PUSH((q), 0x41, 0x00, 0x41, 0x01)
#define CONTROL_STOP(q)		PUSH((q), 0x41, 0x00, 0x00, 0x01)

/*
 * The rule the whole file exists for. A game sending faster than the wheel
 * accepts must not build a backlog: the wheel holds one level per effect, so
 * a level still waiting when the next arrives was never going to be felt.
 */
static void
test_a_superseded_level_is_replaced_not_queued(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	UPDATE(&q, 0x10);
	UPDATE(&q, 0x20);
	UPDATE(&q, 0x30);

	check_int("three levels for one effect wait as one packet",
	    (long)t150_wq_depth(&q), 1);
	check_next("and the packet carries the newest level", &q,
	    "03 0e 00 30");
	check_next("with nothing behind it", &q, "");
	check_int("the two it replaced are counted as merged",
	    (long)q.merged, 2);
	check_int("and none of them counts as dropped", (long)q.dropped, 0);
}

/* Different parameters are different packets and all of them must go. */
static void
test_different_parameters_do_not_merge(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	UPDATE(&q, 0x10);			/* slot 0 */
	PUSH(&q, 0x03, 0x0f, 0x00, 0x20);	/* another parameter id */
	PUSH(&q, 0x01, 0x00, 0x00);		/* a commit */

	check_int("three parameters wait as three packets",
	    (long)t150_wq_depth(&q), 3);
	check_next("first in, first out", &q, "03 0e 00 10");
	check_next("then the second", &q, "03 0f 00 20");
	check_next("then the third", &q, "01 00 00");
}

/*
 * The merge that would be a bug. A stop and a play address the same slot and
 * differ only in the mode byte, and losing either one leaves the wheel doing
 * the opposite of what the game asked for. The key covers byte 2 for exactly
 * this.
 */
static void
test_a_stop_cannot_swallow_a_play(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	CONTROL_PLAY(&q);
	CONTROL_STOP(&q);

	check_int("a play and a stop are two packets",
	    (long)t150_wq_depth(&q), 2);
	check_next("the play goes first", &q, "41 00 41 01");
	check_next("and the stop follows it", &q, "41 00 00 01");
}

/*
 * The same rule, one packet further on, which is where it used to break. A
 * repeat merged into the oldest copy of itself, so a stop that arrived after
 * a play took the earlier stop's place in front of it and the wheel was left
 * playing an effect the game had stopped. Nothing swallowed anything: a play
 * and a stop stayed two packets throughout, and the order alone was wrong.
 */
static void
test_a_repeated_stop_does_not_overtake_a_play(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	CONTROL_STOP(&q);
	CONTROL_PLAY(&q);
	CONTROL_STOP(&q);

	check_int("a repeat behind a play is its own packet",
	    (long)t150_wq_depth(&q), 3);
	check_int("so nothing was merged", (long)q.merged, 0);
	check_next("the first stop goes first", &q, "41 00 00 01");
	check_next("then the play", &q, "41 00 41 01");
	check_next("and the game's last word is the wheel's", &q,
	    "41 00 00 01");
}

/*
 * The settings share the shape and the hazard. A safe state ends with the
 * autocenter force at zero and the enable at zero, and an enable that
 * merged into an earlier enable would arrive before the disable that
 * followed it, leaving the wheel gripped.
 */
static void
test_a_repeated_setting_does_not_overtake_its_opposite(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	PUSH(&q, 0x40, 0x04, 0x01, 0x00);	/* autocenter on */
	PUSH(&q, 0x40, 0x04, 0x00, 0x00);	/* and off again */
	PUSH(&q, 0x40, 0x04, 0x01, 0x00);	/* on once more */

	check_int("three states of one setting wait as three packets",
	    (long)t150_wq_depth(&q), 3);
	check_next("in the order they were asked for", &q, "40 04 01 00");
	check_next("second", &q, "40 04 00 00");
	check_next("and the newest last", &q, "40 04 01 00");
}

/*
 * Merging must not reorder. An update that arrives while its own commit is
 * still waiting has to stay in front of that commit, or the wheel is given a
 * level for an effect it has not been told about yet.
 */
static void
test_a_merge_keeps_its_place_in_the_queue(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	UPDATE(&q, 0x10);
	PUSH(&q, 0x01, 0x00, 0x00);		/* the commit that follows it */
	UPDATE(&q, 0x99);			/* a newer level, same packet */

	check_next("the newest level still comes before the commit", &q,
	    "03 0e 00 99");
	check_next("and the commit is still behind it", &q, "01 00 00");
}

/*
 * Two effects modulating at once, which is what a game actually does: the
 * depth is set by how many effects there are, not by how fast they change.
 */
static void
test_depth_is_bounded_by_the_effects_not_by_the_rate(void)
{
	struct t150_wirequeue q;
	int i;

	t150_wq_init(&q);
	for (i = 0; i < 500; i++) {
		UPDATE(&q, (uint8_t)i);
		CONTROL_PLAY(&q);
		PUSH(&q, 0x05, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x46, 0x54);
	}

	check_int("a thousand updates over three parameters wait as three",
	    (long)t150_wq_depth(&q), 3);
	check_int("and nothing was dropped", (long)q.dropped, 0);
}

/*
 * The backstop. Only distinct parameters can fill the queue now, so reaching
 * the limit means the wheel has stopped taking writes rather than merely
 * lagging behind. What is already in stays in: every one of those packets has
 * been reported written, and the caller has stopped keeping a copy.
 */
static void
test_a_full_queue_keeps_what_it_accepted(void)
{
	struct t150_wirequeue q;
	unsigned int i;

	t150_wq_init(&q);
	for (i = 0; i < T150_WQ_MAX + 2; i++) {
		uint8_t p[4] = { 0x03, (uint8_t)(i & 0xff),
		    (uint8_t)(i >> 8), 0x00 };

		(void)t150_wq_push(&q, p, sizeof(p));
	}

	check_int("the queue holds its limit and no more",
	    (long)t150_wq_depth(&q), (long)T150_WQ_MAX);
	check_int("the overflow is counted", (long)q.dropped, 2);
	check_next("and nothing already accepted was thrown away for it", &q,
	    "03 00 00 00");
}

/*
 * What the caller is told matters more than what is kept. A packet the queue
 * refuses must be refused out loud: the session records a packet as being on
 * the wheel the moment the write reports success, and stops resending it, so
 * a silent discard loses a force with every layer reporting success.
 */
static void
test_a_full_queue_refuses_rather_than_lying(void)
{
	struct t150_wirequeue q;
	uint8_t p[4] = { 0x03, 0x00, 0x00, 0x00 };
	unsigned int i;

	t150_wq_init(&q);
	for (i = 0; i < T150_WQ_MAX; i++) {
		p[1] = (uint8_t)(i & 0xff);
		p[2] = (uint8_t)(i >> 8);
		check_int("every packet up to the limit is taken",
		    t150_wq_push(&q, p, sizeof(p)), 0);
	}

	p[1] = 0xff;
	p[2] = 0xff;
	check_int("and the one that does not fit is refused",
	    t150_wq_push(&q, p, sizeof(p)), -1);
}

/* A wheel that has gone leaves nothing behind for one that comes back. */
static void
test_clearing_empties_the_queue(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	UPDATE(&q, 0x10);
	CONTROL_PLAY(&q);
	t150_wq_clear(&q);

	check_int("a cleared queue is empty", (long)t150_wq_depth(&q), 0);
	check_next("and hands out nothing", &q, "");

	/* And it still works afterwards, which a botched wrap would break. */
	UPDATE(&q, 0x20);
	check_next("a cleared queue still takes packets", &q, "03 0e 00 20");
}

/* Nothing the daemon cannot represent gets in. */
static void
test_refusals(void)
{
	struct t150_wirequeue q;
	uint8_t big[T150_PKT_MAX + 1];

	t150_wq_init(&q);
	memset(big, 0x03, sizeof(big));

	check_int("an oversized packet is refused",
	    t150_wq_push(&q, big, sizeof(big)), -1);
	check_int("an empty packet is refused", t150_wq_push(&q, big, 0), -1);
	check_int("and neither reaches the queue", (long)t150_wq_depth(&q), 0);
}

/*
 * A packet whose length differs is a different packet even when its first
 * bytes agree, which is what tells a condition's ff_first from its ff_update:
 * both open with 0x05.
 */
static void
test_length_is_part_of_the_key(void)
{
	struct t150_wirequeue q;

	t150_wq_init(&q);
	PUSH(&q, 0x05, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x46, 0x54);
	PUSH(&q, 0x05, 0x1c, 0x00, 0x00);

	check_int("same opening bytes, different length, two packets",
	    (long)t150_wq_depth(&q), 2);
}

/*
 * The property the whole design rests on: two packets that address different
 * things on the wheel must never look alike to the queue. Checked against the
 * real encoders rather than against hand written bytes, over every slot the
 * wheel has, because a key that collided across slots would silently drop one
 * slot's force in favour of another's and nothing in a log would say so.
 *
 * Within one slot the opposite is wanted and is not checked here: a slot
 * holds one effect at a time, so a slot changing from a constant to a sine
 * has its old packets superseded, which is a merge and is correct.
 */
static void
test_no_two_slots_look_alike(void)
{
	struct t150_wirequeue q;
	unsigned int slot;
	unsigned int pushed = 0;

	t150_wq_init(&q);

	for (slot = 0; slot < T150_SLOT_MAX; slot++) {
		struct t150_effect ef;
		uint8_t b[T150_PKT_MAX];
		size_t r;

		memset(&ef, 0, sizeof(ef));
		ef.kind = T150_EFFECT_CONSTANT;
		ef.slot = (uint8_t)slot;

		if ((r = t150_enc_ff_first(b, sizeof(b), &ef)) > 0) {
			(void)t150_wq_push(&q, b, r);
			pushed++;
		}
		if ((r = t150_enc_ff_update(b, sizeof(b), &ef)) > 0) {
			(void)t150_wq_push(&q, b, r);
			pushed++;
		}
		if ((r = t150_enc_ff_commit(b, sizeof(b), &ef)) > 0) {
			(void)t150_wq_push(&q, b, r);
			pushed++;
		}
		if ((r = t150_enc_control(b, sizeof(b), (uint8_t)slot, 1,
		    1)) > 0) {
			(void)t150_wq_push(&q, b, r);
			pushed++;
		}
		if ((r = t150_enc_control(b, sizeof(b), (uint8_t)slot, 0,
		    1)) > 0) {
			(void)t150_wq_push(&q, b, r);
			pushed++;
		}
	}

	check_int("every slot's packets stay distinct from every other's",
	    (long)t150_wq_depth(&q), (long)pushed);
	check_int("so nothing merged", (long)q.merged, 0);
	check_int("and nothing was dropped", (long)q.dropped, 0);
}

int
main(void)
{
	test_a_superseded_level_is_replaced_not_queued();
	test_different_parameters_do_not_merge();
	test_a_stop_cannot_swallow_a_play();
	test_a_repeated_stop_does_not_overtake_a_play();
	test_a_repeated_setting_does_not_overtake_its_opposite();
	test_a_merge_keeps_its_place_in_the_queue();
	test_depth_is_bounded_by_the_effects_not_by_the_rate();
	test_a_full_queue_keeps_what_it_accepted();
	test_a_full_queue_refuses_rather_than_lying();
	test_clearing_empties_the_queue();
	test_refusals();
	test_length_is_part_of_the_key();
	test_no_two_slots_look_alike();

	if (failures != 0) {
		fprintf(stderr, "wirequeue_check: %d failure(s)\n", failures);
		return 1;
	}

	printf("wirequeue_check: ok\n");
	return 0;
}
