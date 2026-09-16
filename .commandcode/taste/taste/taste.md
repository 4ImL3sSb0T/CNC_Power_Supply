# Taste
- Communicates in Chinese and expects replies in Chinese. Confidence: 0.7
- Asks "为什么" about build failures — wants the root cause explained (the causal chain, not just a patch) rather than an immediate fix. Confidence: 0.5
- Reports build failures by pasting the raw compiler/ninja output verbatim (full command lines and errors) rather than summarizing. Confidence: 0.6
- Prefers the project's short integer typedefs (i32/u32/u8 from common_def.h) over C-style int32/uint32 naming, and wants them applied consistently — including migrating existing files (e.g. fifo_buffer.c) to use common_def types. Confidence: 0.6
- Favors the minimal change that gets the build green over adding new abstractions; e.g. replaces an unwritten logging call with plain printf instead of designing/implementing a log module. Confidence: 0.5
- Prefers bounded, size-aware string formatting (snprintf with an explicit buffer size) over unbounded sprintf. Confidence: 0.5
- Return-type convention: functions returning a status/error code should use the project's `exit_code_t` from common_def.h (EXIT_OK for success, specific codes like EXIT_FAIL/EXIT_INVALID_PARAM/EXIT_NO_MEMORY for failure); functions that return a length/count value (e.g. bytes actually read) should keep returning int/i32 rather than wrapping in exit_code_t. Confidence: 0.75
- Works on embedded C firmware for Raspberry Pi Pico / RP2350, using FreeRTOS (pico-sdk, CMake build), with kernel/port files vendored under src/third_party/. Confidence: 0.55
- When shown an upstream/official example (e.g. an official FreeRTOSConfig.h), wants an explicit comparison against the project's own version — what differs, what is functionally equivalent, and whether the official version would even compile against the project's kernel version/port — rather than blind adoption. Confidence: 0.5
