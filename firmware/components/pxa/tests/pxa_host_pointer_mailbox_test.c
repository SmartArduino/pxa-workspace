#include "pxa_host_pointer_mailbox.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static pxa_host_pointer_event_t event(uint8_t phase, int32_t x) {
    pxa_host_pointer_event_t value = {0};
    value.phase = phase;
    value.x = x;
    return value;
}

static pxa_host_pointer_event_t pointer_event(uint8_t id, uint8_t phase,
                                              int32_t x) {
    pxa_host_pointer_event_t value = event(phase, x);
    value.id = id;
    value.instance_id = 7;
    value.surface = 1;
    value.node = 2;
    return value;
}

static void test_contiguous_moves_are_coalesced(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t output;
    pxa_host_pointer_event_t first = event(PXA_HOST_POINTER_MOVE_PHASE, 1);
    pxa_host_pointer_event_t second = event(PXA_HOST_POINTER_MOVE_PHASE, 2);
    pxa_host_pointer_mailbox_init(&mailbox);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &first));
    assert(pxa_host_pointer_mailbox_push(&mailbox, &second));
    assert(mailbox.count == 1);
    assert(pxa_host_pointer_mailbox_take(&mailbox, 16000, 16000, &output));
    assert(output.x == 2);
}

static void test_move_before_edge_bypasses_throttle(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t output;
    pxa_host_pointer_event_t move = event(PXA_HOST_POINTER_MOVE_PHASE, 3);
    pxa_host_pointer_event_t up = event(2, 4);
    pxa_host_pointer_mailbox_init(&mailbox);
    mailbox.move_clocks[0].id = move.id;
    mailbox.move_clocks[0].active = 1;
    mailbox.move_clocks[0].has_last_move = 1;
    mailbox.move_clocks[0].last_move_us = 1000;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &move));
    assert(pxa_host_pointer_mailbox_push(&mailbox, &up));
    assert(pxa_host_pointer_mailbox_take(&mailbox, 1001, 16000, &output));
    assert(output.phase == PXA_HOST_POINTER_MOVE_PHASE);
    assert(pxa_host_pointer_mailbox_take(&mailbox, 1001, 16000, &output));
    assert(output.phase == 2);
}

static void test_different_pointer_moves_are_not_coalesced(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t output;
    pxa_host_pointer_event_t first =
        pointer_event(0, PXA_HOST_POINTER_MOVE_PHASE, 10);
    pxa_host_pointer_event_t second =
        pointer_event(1, PXA_HOST_POINTER_MOVE_PHASE, 20);
    pxa_host_pointer_mailbox_init(&mailbox);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &first));
    assert(pxa_host_pointer_mailbox_push(&mailbox, &second));
    assert(mailbox.count == 2);
    assert(pxa_host_pointer_mailbox_take(&mailbox, 16000, 16000, &output));
    assert(output.id == 0 && output.x == 10);
    assert(pxa_host_pointer_mailbox_take(&mailbox, 16000, 16000, &output));
    assert(output.id == 1 && output.x == 20);
}

static void test_each_pointer_has_an_independent_move_cadence(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t output;
    pxa_host_pointer_event_t first =
        pointer_event(0, PXA_HOST_POINTER_MOVE_PHASE, 10);
    pxa_host_pointer_event_t second =
        pointer_event(1, PXA_HOST_POINTER_MOVE_PHASE, 20);
    pxa_host_pointer_event_t next_first =
        pointer_event(0, PXA_HOST_POINTER_MOVE_PHASE, 11);
    pxa_host_pointer_event_t next_second =
        pointer_event(1, PXA_HOST_POINTER_MOVE_PHASE, 21);

    pxa_host_pointer_mailbox_init(&mailbox);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &first));
    assert(pxa_host_pointer_mailbox_push(&mailbox, &second));
    assert(pxa_host_pointer_mailbox_take(&mailbox, 1000, 16000, &output));
    assert(output.id == 0);
    assert(pxa_host_pointer_mailbox_take(&mailbox, 1000, 16000, &output));
    assert(output.id == 1);

    assert(pxa_host_pointer_mailbox_push(&mailbox, &next_first));
    assert(pxa_host_pointer_mailbox_push(&mailbox, &next_second));
    assert(!pxa_host_pointer_mailbox_take(&mailbox, 16999, 16000, &output));
    assert(pxa_host_pointer_mailbox_take(&mailbox, 17000, 16000, &output));
    assert(output.id == 0 && output.x == 11);
    assert(pxa_host_pointer_mailbox_take(&mailbox, 17000, 16000, &output));
    assert(output.id == 1 && output.x == 21);
}

static void test_edge_evicts_move_from_full_mailbox(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t value;
    unsigned index;
    pxa_host_pointer_mailbox_init(&mailbox);
    for (index = 0; index < PXA_HOST_POINTER_MAILBOX_CAPACITY - 1u; ++index) {
        value = event(2, (int32_t)index);
        assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    }
    value = event(PXA_HOST_POINTER_MOVE_PHASE, 99);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value = event(3, 100);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    assert(mailbox.count == PXA_HOST_POINTER_MAILBOX_CAPACITY);
    assert(!pxa_host_pointer_mailbox_has_move(&mailbox));
    assert(mailbox.events[mailbox.count - 1u].x == 100);
}

static void test_full_edge_mailbox_rejects_new_events(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t value;
    unsigned index;
    pxa_host_pointer_mailbox_init(&mailbox);
    for (index = 0; index < PXA_HOST_POINTER_MAILBOX_CAPACITY; ++index) {
        value = event(2, (int32_t)index);
        assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    }
    value = event(PXA_HOST_POINTER_MOVE_PHASE, 100);
    assert(!pxa_host_pointer_mailbox_push(&mailbox, &value));
    value = event(3, 101);
    assert(!pxa_host_pointer_mailbox_push(&mailbox, &value));
}

static void test_move_throttle_and_reset(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t output;
    pxa_host_pointer_event_t move = event(PXA_HOST_POINTER_MOVE_PHASE, 5);
    pxa_host_pointer_mailbox_init(&mailbox);
    mailbox.move_clocks[0].id = move.id;
    mailbox.move_clocks[0].active = 1;
    mailbox.move_clocks[0].has_last_move = 1;
    mailbox.move_clocks[0].last_move_us = 1000;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &move));
    assert(!pxa_host_pointer_mailbox_move_is_due(&mailbox, 16999, 16000));
    assert(!pxa_host_pointer_mailbox_take(&mailbox, 16999, 16000, &output));
    assert(pxa_host_pointer_mailbox_move_is_due(&mailbox, 17000, 16000));
    assert(pxa_host_pointer_mailbox_take(&mailbox, 17000, 16000, &output));
    pxa_host_pointer_mailbox_init(&mailbox);
    assert(mailbox.count == 0);
    assert(!mailbox.move_clocks[0].active);
}

static void test_interleaved_streams_keep_only_latest_samples(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t output;
    unsigned sample, stream;
    pxa_host_pointer_mailbox_init(&mailbox);
    for (sample = 0; sample < 100; ++sample) {
        for (stream = 0; stream < 4; ++stream) {
            /* Two fingers, each subscribed at a child and its ancestor. */
            pxa_host_pointer_event_t value = pointer_event(
                (uint8_t)(stream / 2), PXA_HOST_POINTER_MOVE_PHASE,
                (int32_t)sample);
            value.node += stream % 2;
            value.timestamp_us = sample * 5000;
            assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
        }
        assert(mailbox.count == 4);
    }
    for (stream = 0; stream < 4; ++stream) {
        assert(pxa_host_pointer_mailbox_take(&mailbox, stream * 16000,
                                             16000, &output));
        assert(output.id == stream / 2 && output.node == 2 + stream % 2);
        assert(output.x == 99 && output.timestamp_us == 495000);
    }
    assert(mailbox.count == 0);
}

static void test_coalescing_never_crosses_an_edge(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t output;
    pxa_host_pointer_event_t value = pointer_event(
        0, PXA_HOST_POINTER_MOVE_PHASE, 10);
    unsigned index;
    pxa_host_pointer_mailbox_init(&mailbox);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value.phase = 2;
    value.x = 20;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value.phase = PXA_HOST_POINTER_DOWN_PHASE;
    value.x = 30;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value.phase = PXA_HOST_POINTER_MOVE_PHASE;
    value.x = 40;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value.x = 50;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    assert(mailbox.count == 4);
    for (index = 0; index < 4; ++index) {
        assert(pxa_host_pointer_mailbox_take(&mailbox, index * 16000,
                                             16000, &output));
        assert(output.x == (index == 3 ? 50 : (int32_t)(index + 1) * 10));
    }

    /* A full queue must reject a new move rather than rewrite a move from
     * before a reliable edge (including an edge of another finger). */
    pxa_host_pointer_mailbox_init(&mailbox);
    value.phase = PXA_HOST_POINTER_MOVE_PHASE;
    value.x = 10;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    for (index = 1; index < PXA_HOST_POINTER_MAILBOX_CAPACITY; ++index) {
        value.phase = 2;
        assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    }
    value.phase = PXA_HOST_POINTER_MOVE_PHASE;
    value.x = 99;
    assert(!pxa_host_pointer_mailbox_push(&mailbox, &value));
    assert(mailbox.events[0].x == 10);
}

static void test_stream_identity_includes_owner_surface_and_node(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t value = pointer_event(
        0, PXA_HOST_POINTER_MOVE_PHASE, 1);
    pxa_host_pointer_mailbox_init(&mailbox);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value.instance_id++;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value.surface++;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    value.node++;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    assert(mailbox.count == 4);
    value.x = 99;
    assert(pxa_host_pointer_mailbox_push(&mailbox, &value));
    assert(mailbox.count == 4 && mailbox.events[3].x == 99);
}

static void test_internal_layout_stays_bounded(void) {
    assert(sizeof(pxa_esp_host_command_t) <= 144u);
    assert(sizeof(pxa_host_pointer_event_t) <= 40u);
    assert(sizeof(pxa_host_pointer_mailbox_t) <= 464u);
}

static void test_backpressure_and_inflight_changes(void) {
    pxa_host_pointer_mailbox_t mailbox;
    pxa_host_pointer_event_t snapshot, next;
    uint32_t epoch;
    pxa_host_pointer_mailbox_init(&mailbox);
    for (unsigned i = 0; i < PXA_HOST_POINTER_MAILBOX_CAPACITY; ++i) {
        next = pointer_event(0, i & 1 ? 2 : 0, (int)i);
        next.timestamp_us = i + 1;
        assert(pxa_host_pointer_mailbox_push(&mailbox, &next));
    }
    /* Rejected downstream writes leave every edge, including its time,
     * available for retry. Acceptance removes exactly the accepted sample. */
    for (unsigned i = 0; i < PXA_HOST_POINTER_MAILBOX_CAPACITY; ++i) {
        for (unsigned retry = 0; retry < 20; ++retry) {
            assert(pxa_host_pointer_mailbox_peek(&mailbox, 100, 16, &snapshot, &epoch));
            assert(snapshot.x == (int)i && snapshot.timestamp_us == i + 1);
            assert(mailbox.count == PXA_HOST_POINTER_MAILBOX_CAPACITY - i);
        }
        assert(pxa_host_pointer_mailbox_commit(&mailbox, &snapshot, epoch, 100));
    }
    next = pointer_event(0, PXA_HOST_POINTER_MOVE_PHASE, 1);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &next));
    assert(pxa_host_pointer_mailbox_peek(&mailbox, 200, 16, &snapshot, &epoch));
    next.x = 2; /* Producer replaces a move while Host submits its snapshot. */
    assert(pxa_host_pointer_mailbox_push(&mailbox, &next));
    assert(!pxa_host_pointer_mailbox_commit(&mailbox, &snapshot, epoch, 200));
    assert(mailbox.count == 1 && mailbox.events[0].x == 2);
    assert(pxa_host_pointer_mailbox_peek(&mailbox, 200, 16, &snapshot, &epoch));
    next = pointer_event(0, 2, 3); /* Appending an edge does not invalidate it. */
    assert(pxa_host_pointer_mailbox_push(&mailbox, &next));
    assert(pxa_host_pointer_mailbox_commit(&mailbox, &snapshot, epoch, 200));
    assert(pxa_host_pointer_mailbox_peek(&mailbox, 200, 16, &snapshot, &epoch));
    pxa_host_pointer_mailbox_reset(&mailbox);
    assert(pxa_host_pointer_mailbox_push(&mailbox, &snapshot));
    assert(!pxa_host_pointer_mailbox_commit(&mailbox, &snapshot, epoch, 200));
    assert(mailbox.count == 1); /* Even an identical post-reset edge survives. */
}

int main(void) {
    test_contiguous_moves_are_coalesced();
    test_move_before_edge_bypasses_throttle();
    test_different_pointer_moves_are_not_coalesced();
    test_each_pointer_has_an_independent_move_cadence();
    test_edge_evicts_move_from_full_mailbox();
    test_full_edge_mailbox_rejects_new_events();
    test_move_throttle_and_reset();
    test_interleaved_streams_keep_only_latest_samples();
    test_coalescing_never_crosses_an_edge();
    test_stream_identity_includes_owner_surface_and_node();
    test_internal_layout_stays_bounded();
    test_backpressure_and_inflight_changes();
    return 0;
}
