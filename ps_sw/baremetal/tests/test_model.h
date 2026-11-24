#ifndef ARAGATZ_TEST_MODEL_H
#define ARAGATZ_TEST_MODEL_H

#include <stdint.h>

/* Pure reference calculations. Cursors are sampled after GEN STOP/PAUSE;
 * FRAME_INDEX has not wrapped and statistics were cleared before START. */
static inline uint32_t ar_expected_events(uint32_t first, uint32_t period,
                                         uint32_t trigger_word,
                                         uint32_t completed, uint32_t next_word)
{
    uint32_t count = 0U;
    if (completed > first)
        count = period == 0U ? 1U : 1U + (completed - 1U - first) / period;
    if (completed >= first && next_word > trigger_word &&
        ((period == 0U && completed == first) ||
         (period != 0U && (completed - first) % period == 0U)))
        ++count;
    return count;
}

static inline int ar_layout(uint32_t frames, uint32_t fragments,
                            uint32_t capacity, uint32_t guard,
                            uint32_t *fragment_bytes, uint32_t *total_bytes)
{
    uint64_t one, total;
    if (frames < 2U || frames > 131072U || fragments == 0U)
        return -1;
    one = 128U + (uint64_t)frames * 768U;
    total = one * fragments;
    if (total + guard > capacity)
        return -1;
    *fragment_bytes = (uint32_t)one;
    *total_bytes = (uint32_t)total;
    return 0;
}

/* PAUSE holds the next-word cursor; STOP must finish any begun frame. */
static inline int ar_partial_frames(uint32_t first_saved, uint32_t frame,
                                    uint32_t word, uint32_t goal,
                                    uint32_t *actual)
{
    uint64_t n;
    if (frame < first_saved || word >= 96U)
        return -1;
    n = (uint64_t)frame - first_saved + (word != 0U ? 1U : 0U);
    if (n == 0U || n >= goal)
        return -1;
    *actual = (uint32_t)n;
    return 0;
}

#endif
