/**
 * @file    test_i2c_bitbang.c
 * @brief   Host-side Unity tests for the bounded bit-banged I2C master (i2c_bitbang.h).
 *
 * A fake open-drain bus wires the master's pin callbacks to a slave model: SDA and SCL are the
 * wired-AND of both sides, the slave decodes START/STOP and bytes on SCL edges, ACKs its own
 * address and every later byte, and can hold SDA LOW for a number of clocks (a slave interrupted
 * mid-byte by a reset), stretch SCL, or hold SCL forever. Every delay is summed, so time bounds
 * are asserted exactly.
 *
 * Covers:       The exact line sequence of one register write; NACK of an absent device; the
 *               UM10204 bus clear with an idle bus, an SDA stuck for 3 clocks and a permanently
 *               stuck SDA; clock stretching within and beyond the budget; the safe-write
 *               sequence for the SOLO ODU profile, with no writes, an absent device, a stuck
 *               SDA and a held SCL; lines released after every call; the worst-case time bound.
 * Not covered:  Real pad timing, pull-up rise times and the EMC2305 itself (bench test).
 *
 * @see     i2c_bitbang.h, rules/testing.md
 */

#include <string.h>

#include "unity.h"

#include "board_profile.h"
#include "i2c_bitbang.h"

/* -----------------------------------------------------------------------------
 * Fake open-drain bus and slave
 * ----------------------------------------------------------------------------- */
#define FOREVER 1000000000u

typedef struct {
    /* Configuration */
    uint8_t  addr7;
    bool     present;
    uint32_t sda_stuck_clocks;   /* slave holds SDA LOW for this many SCL falls       */
    uint32_t stretch_us;         /* SCL held LOW this long after each master release  */
    uint32_t stretch_after_fall; /* stretching starts after this many SCL falls       */

    /* Master side */
    bool m_sda;                  /* true = released */
    bool m_scl;

    /* Slave side */
    bool     s_ack;
    uint32_t stretch_left_us;
    bool     in_txn;
    bool     pending;            /* a bit sampled on the SCL rise, decoded on the fall */
    bool     pending_sda;
    uint32_t bit_count;
    uint8_t  shift;
    uint32_t byte_index;
    bool     addressed;

    /* Observations */
    uint32_t scl_falls;
    uint32_t elapsed_us;
    uint32_t hal_calls;
    char     trace[1024];
    size_t   trace_len;
} fake_bus_t;

static fake_bus_t s_bus;

static bool sda_level(const fake_bus_t *b)
{
    return b->m_sda && !b->s_ack && b->sda_stuck_clocks == 0u;
}

static bool scl_level(const fake_bus_t *b)
{
    return b->m_scl && b->stretch_left_us == 0u;
}

static void trace_char(fake_bus_t *b, char c)
{
    if (b->trace_len + 1u < sizeof b->trace) {
        b->trace[b->trace_len++] = c;
        b->trace[b->trace_len]   = '\0';
    }
}

/* A slave samples SDA on the SCL rise but acts on the fall: a START or STOP in between
 * (SDA moving while SCL is high) cancels the sampled bit, as on a real bus. */
static void on_scl_rise(fake_bus_t *b)
{
    b->pending     = b->in_txn;
    b->pending_sda = sda_level(b);
}

static void on_scl_fall(fake_bus_t *b)
{
    ++b->scl_falls;
    if (b->sda_stuck_clocks > 0u && b->sda_stuck_clocks != FOREVER) {
        --b->sda_stuck_clocks;
    }
    if (!b->pending) {
        return;
    }
    b->pending = false;
    if (b->bit_count < 8u) {
        b->shift = (uint8_t)((b->shift << 1) | (b->pending_sda ? 1u : 0u));
        trace_char(b, b->pending_sda ? '1' : '0');
        if (++b->bit_count == 8u) {
            if (b->byte_index == 0u) {
                b->addressed = b->present && b->shift == (uint8_t)(b->addr7 << 1);
            }
            b->s_ack = b->addressed;  /* drive the ninth clock's ACK */
        }
        return;
    }
    trace_char(b, b->pending_sda ? 'N' : 'A');
    b->bit_count = 0u;
    ++b->byte_index;
    b->s_ack = false;
}

/* Apply one master line change and let the slave react to the resulting edges. */
static void apply(fake_bus_t *b, bool new_sda, bool new_scl)
{
    const bool sda0 = sda_level(b);
    const bool scl0 = scl_level(b);
    b->m_sda = new_sda;
    b->m_scl = new_scl;
    const bool sda1 = sda_level(b);
    const bool scl1 = scl_level(b);

    if (scl0 && scl1 && sda0 && !sda1) {
        trace_char(b, 'S');
        b->in_txn = true; b->bit_count = 0u; b->byte_index = 0u; b->addressed = false;
        b->pending = false;
    } else if (scl0 && scl1 && !sda0 && sda1) {
        trace_char(b, 'P');
        b->in_txn = false; b->s_ack = false; b->pending = false;
    } else if (!scl0 && scl1) {
        on_scl_rise(b);
    } else if (scl0 && !scl1) {
        on_scl_fall(b);
    }
}

static void fake_sda(void *ctx, bool release)
{
    fake_bus_t *b = ctx;
    ++b->hal_calls;
    apply(b, release, b->m_scl);
}

static void fake_scl(void *ctx, bool release)
{
    fake_bus_t *b = ctx;
    ++b->hal_calls;
    if (release && !b->m_scl && b->stretch_us != 0u && b->scl_falls >= b->stretch_after_fall) {
        b->stretch_left_us = b->stretch_us;  /* slave holds SCL after the master lets go */
    }
    apply(b, b->m_sda, release);
}

static bool fake_sda_read(void *ctx)
{
    fake_bus_t *b = ctx;
    ++b->hal_calls;
    return sda_level(b);
}

static bool fake_scl_read(void *ctx)
{
    fake_bus_t *b = ctx;
    ++b->hal_calls;
    return scl_level(b);
}

static void fake_delay(void *ctx, uint32_t us)
{
    fake_bus_t *b = ctx;
    ++b->hal_calls;
    b->elapsed_us += us;
    if (b->stretch_left_us == 0u || b->stretch_left_us == FOREVER) {
        return;
    }
    b->stretch_left_us = (us >= b->stretch_left_us) ? 0u : b->stretch_left_us - us;
    if (scl_level(b)) {
        on_scl_rise(b);  /* the slave let SCL go */
    }
}

static const i2c_bb_hal_t HAL = {
    &s_bus, fake_sda, fake_scl, fake_sda_read, fake_scl_read, fake_delay,
};

static board_profile_t odu_profile(void)
{
    board_profile_t p;
    board_profile_defaults(&p);
    p.i2c_sda_gpio    = 8u;
    p.i2c_scl_gpio    = 9u;
    p.i2c_addr7       = 0x2Cu;
    p.i2c_write_count = 5u;
    for (uint8_t i = 0u; i < 5u; ++i) {
        p.i2c_writes[i].reg   = (uint8_t)(0x30u + 0x10u * i);
        p.i2c_writes[i].value = 0x00u;
    }
    return p;
}

static uint32_t count_char(const char *s, char c)
{
    uint32_t n = 0u;
    for (; *s != '\0'; ++s) {
        n += (*s == c) ? 1u : 0u;
    }
    return n;
}

static void assert_released(void)
{
    TEST_ASSERT_TRUE(s_bus.m_sda);
    TEST_ASSERT_TRUE(s_bus.m_scl);
}

void setUp(void)
{
    memset(&s_bus, 0, sizeof s_bus);
    s_bus.addr7   = 0x2Cu;
    s_bus.present = true;
    s_bus.m_sda   = true;
    s_bus.m_scl   = true;
}

void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * One register write
 * ----------------------------------------------------------------------------- */
static void test_write_reg_bit_sequence(void)
{
    TEST_ASSERT_EQUAL_INT(I2C_BB_OK, i2c_bb_write_reg(&HAL, 0x2Cu, 0x30u, 0x00u));
    /* START, 0x58 (0x2C + W), ACK, 0x30, ACK, 0x00, ACK, STOP. */
    TEST_ASSERT_EQUAL_STRING("S01011000A00110000A00000000AP", s_bus.trace);
    TEST_ASSERT_EQUAL_UINT32(I2C_BB_WRITE_HALVES * I2C_BB_HALF_PERIOD_US, s_bus.elapsed_us);
    assert_released();
}

static void test_write_reg_value_bits(void)
{
    TEST_ASSERT_EQUAL_INT(I2C_BB_OK, i2c_bb_write_reg(&HAL, 0x2Cu, 0xA5u, 0xFFu));
    TEST_ASSERT_EQUAL_STRING("S01011000A10100101A11111111AP", s_bus.trace);
}

static void test_write_reg_nack_absent_device(void)
{
    s_bus.present = false;
    TEST_ASSERT_EQUAL_INT(I2C_BB_NACK, i2c_bb_write_reg(&HAL, 0x2Cu, 0x30u, 0x00u));
    TEST_ASSERT_EQUAL_STRING("S01011000NP", s_bus.trace);
    assert_released();
}

static void test_write_reg_refuses_busy_bus(void)
{
    s_bus.sda_stuck_clocks = FOREVER;
    TEST_ASSERT_EQUAL_INT(I2C_BB_BUS_BUSY, i2c_bb_write_reg(&HAL, 0x2Cu, 0x30u, 0x00u));
    TEST_ASSERT_EQUAL_STRING("", s_bus.trace);
    TEST_ASSERT_EQUAL_UINT32(0u, s_bus.scl_falls);
    assert_released();
}

static void test_write_reg_short_stretch_succeeds(void)
{
    s_bus.stretch_us = 10u;  /* 29 rises x 10 us = 290 us, inside the budget */
    TEST_ASSERT_EQUAL_INT(I2C_BB_OK, i2c_bb_write_reg(&HAL, 0x2Cu, 0x30u, 0x00u));
    TEST_ASSERT_EQUAL_STRING("S01011000A00110000A00000000AP", s_bus.trace);
    assert_released();
}

static void test_write_reg_stretch_timeout(void)
{
    s_bus.stretch_us         = FOREVER;
    s_bus.stretch_after_fall = 1u;  /* SCL held from the first data clock on */
    TEST_ASSERT_EQUAL_INT(I2C_BB_STRETCH_TIMEOUT, i2c_bb_write_reg(&HAL, 0x2Cu, 0x30u, 0x00u));
    TEST_ASSERT_TRUE(s_bus.elapsed_us <= I2C_BB_WRITE_HALVES * I2C_BB_HALF_PERIOD_US +
                                         I2C_BB_STRETCH_BUDGET_US);
    assert_released();
}

/* -----------------------------------------------------------------------------
 * Bus clear (UM10204 §3.1.16)
 * ----------------------------------------------------------------------------- */
static void test_bus_clear_idle_bus_sends_stop_only(void)
{
    TEST_ASSERT_EQUAL_INT(I2C_BB_OK, i2c_bb_bus_clear(&HAL));
    TEST_ASSERT_EQUAL_STRING("P", s_bus.trace);
    TEST_ASSERT_EQUAL_UINT32(1u, s_bus.scl_falls);  /* the STOP's own SCL low */
    assert_released();
}

static void test_bus_clear_frees_stuck_sda(void)
{
    s_bus.sda_stuck_clocks = 3u;
    TEST_ASSERT_EQUAL_INT(I2C_BB_OK, i2c_bb_bus_clear(&HAL));
    TEST_ASSERT_EQUAL_UINT32(3u + 1u, s_bus.scl_falls);  /* 3 pulses, then the STOP */
    TEST_ASSERT_EQUAL_STRING("P", s_bus.trace);
    assert_released();
}

static void test_bus_clear_gives_up_after_nine_pulses(void)
{
    s_bus.sda_stuck_clocks = FOREVER;
    TEST_ASSERT_EQUAL_INT(I2C_BB_BUS_BUSY, i2c_bb_bus_clear(&HAL));
    TEST_ASSERT_EQUAL_UINT32(I2C_BB_MAX_CLEAR_PULSES + 1u, s_bus.scl_falls);
    TEST_ASSERT_EQUAL_UINT32(I2C_BB_CLEAR_HALVES * I2C_BB_HALF_PERIOD_US, s_bus.elapsed_us);
    assert_released();
}

static void test_bus_clear_scl_held(void)
{
    s_bus.m_scl              = false;  /* SCL low when the clear starts... */
    s_bus.stretch_us         = FOREVER;
    s_bus.stretch_after_fall = 0u;     /* ...and a slave never lets it rise */
    TEST_ASSERT_EQUAL_INT(I2C_BB_STRETCH_TIMEOUT, i2c_bb_bus_clear(&HAL));
    TEST_ASSERT_EQUAL_UINT32(I2C_BB_STRETCH_BUDGET_US, s_bus.elapsed_us);
    assert_released();
}

/* -----------------------------------------------------------------------------
 * Profile safe-write sequence
 * ----------------------------------------------------------------------------- */
static void test_safe_writes_odu_profile(void)
{
    const board_profile_t p = odu_profile();
    TEST_ASSERT_EQUAL_UINT8(5u, i2c_bb_apply_safe_writes(&HAL, &p));
    TEST_ASSERT_EQUAL_STRING("P"
                             "S01011000A00110000A00000000AP"
                             "S01011000A01000000A00000000AP"
                             "S01011000A01010000A00000000AP"
                             "S01011000A01100000A00000000AP"
                             "S01011000A01110000A00000000AP",
                             s_bus.trace);
    TEST_ASSERT_TRUE(s_bus.elapsed_us <= I2C_BB_WORST_CASE_US);
    assert_released();
}

static void test_safe_writes_none_touches_nothing(void)
{
    board_profile_t p;
    board_profile_defaults(&p);
    TEST_ASSERT_EQUAL_UINT8(0u, i2c_bb_apply_safe_writes(&HAL, &p));
    TEST_ASSERT_EQUAL_UINT32(0u, s_bus.hal_calls);
}

static void test_safe_writes_absent_device_tries_each(void)
{
    const board_profile_t p = odu_profile();
    s_bus.present = false;
    TEST_ASSERT_EQUAL_UINT8(0u, i2c_bb_apply_safe_writes(&HAL, &p));
    TEST_ASSERT_EQUAL_UINT32(5u, count_char(s_bus.trace, 'S'));
    TEST_ASSERT_EQUAL_UINT32(5u, count_char(s_bus.trace, 'N'));
    assert_released();
}

static void test_safe_writes_skipped_on_stuck_sda(void)
{
    const board_profile_t p = odu_profile();
    s_bus.sda_stuck_clocks = FOREVER;
    TEST_ASSERT_EQUAL_UINT8(0u, i2c_bb_apply_safe_writes(&HAL, &p));
    TEST_ASSERT_EQUAL_UINT32(0u, count_char(s_bus.trace, 'S'));
    assert_released();
}

static void test_safe_writes_recover_interrupted_byte(void)
{
    const board_profile_t p = odu_profile();
    s_bus.sda_stuck_clocks = 5u;  /* the app was reset mid-read */
    TEST_ASSERT_EQUAL_UINT8(5u, i2c_bb_apply_safe_writes(&HAL, &p));
    assert_released();
}

static void test_safe_writes_abort_on_held_scl(void)
{
    const board_profile_t p = odu_profile();
    s_bus.stretch_us         = FOREVER;
    s_bus.stretch_after_fall = 3u;  /* SCL held during the first write */
    TEST_ASSERT_EQUAL_UINT8(0u, i2c_bb_apply_safe_writes(&HAL, &p));
    TEST_ASSERT_TRUE(count_char(s_bus.trace, 'S') <= 1u);
    TEST_ASSERT_TRUE(s_bus.elapsed_us <= I2C_BB_WORST_CASE_US);
    assert_released();
}

static void test_worst_case_bound_is_under_10_ms(void)
{
    TEST_ASSERT_EQUAL_UINT32(4475u, I2C_BB_WORST_CASE_US);
    TEST_ASSERT_TRUE(I2C_BB_WORST_CASE_US < 10000u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_write_reg_bit_sequence);
    RUN_TEST(test_write_reg_value_bits);
    RUN_TEST(test_write_reg_nack_absent_device);
    RUN_TEST(test_write_reg_refuses_busy_bus);
    RUN_TEST(test_write_reg_short_stretch_succeeds);
    RUN_TEST(test_write_reg_stretch_timeout);
    RUN_TEST(test_bus_clear_idle_bus_sends_stop_only);
    RUN_TEST(test_bus_clear_frees_stuck_sda);
    RUN_TEST(test_bus_clear_gives_up_after_nine_pulses);
    RUN_TEST(test_bus_clear_scl_held);
    RUN_TEST(test_safe_writes_odu_profile);
    RUN_TEST(test_safe_writes_none_touches_nothing);
    RUN_TEST(test_safe_writes_absent_device_tries_each);
    RUN_TEST(test_safe_writes_skipped_on_stuck_sda);
    RUN_TEST(test_safe_writes_recover_interrupted_byte);
    RUN_TEST(test_safe_writes_abort_on_held_scl);
    RUN_TEST(test_worst_case_bound_is_under_10_ms);
    return UNITY_END();
}
