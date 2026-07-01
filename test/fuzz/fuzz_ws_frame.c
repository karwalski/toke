/*
 * fuzz_ws_frame.c — libFuzzer entry point for WebSocket frame decoding.
 *
 * Feeds arbitrary raw bytes straight into ws_decode_frame() — the exact
 * function that consumes attacker-controlled bytes off a client socket.
 * This targets the length/mask handling that produced 120.6 HTT-04
 * (ws_recv integer overflow → heap overflow via crafted payload_len).
 *
 * On success the decoded frame is released with ws_frame_free().
 *
 * Build:  make fuzz-ws-frame   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>

#include "../../src/stdlib/ws.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    uint64_t consumed = 0;
    WsFrameResult r = ws_decode_frame(data, (uint64_t)size, &consumed);
    if (!r.is_err && r.frame)
        ws_frame_free(r.frame);

    return 0;
}
