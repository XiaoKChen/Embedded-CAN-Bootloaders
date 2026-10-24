/**
 * @file    i2c_bitbang.c
 * @brief   Bounded bit-banged I2C master (see i2c_bitbang.h for the contract).
 *
 * Implementation notes: every SCL rise goes through scl_rise(), which polls in 1 us steps and
 * draws from one stretch budget per public call (shared by the whole safe-write sequence), so
 * the total clock-stretch wait of a call never exceeds I2C_BB_STRETCH_BUDGET_US. Data changes
 * only while SCL is LOW; SDA is sampled at the end of the SCL-high half period. After a NACK or
 * a busy line the transaction still ends with a STOP; after a stretch timeout both lines are
 * just released, since a held SCL cannot clock a STOP.
 *
 * @see     i2c_bitbang.h, tests/test_i2c_bitbang.c
 */

#include "i2c_bitbang.h"

/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
typedef struct {
    const i2c_bb_hal_t *hal;
    uint32_t            stretch_left_us;
} bus_t;

static void half_period(const bus_t *bus)
{
    bus->hal->delay_us(bus->hal->ctx, I2C_BB_HALF_PERIOD_US);
}

static void release_both(const bus_t *bus)
{
    bus->hal->sda_release(bus->hal->ctx, true);
    bus->hal->scl_release(bus->hal->ctx, true);
}

/* Release SCL and wait, within the stretch budget, for a slave to let it rise. */
static bool scl_rise(bus_t *bus)
{
    bus->hal->scl_release(bus->hal->ctx, true);
    while (!bus->hal->scl_read(bus->hal->ctx)) {
        if (bus->stretch_left_us == 0u) {
            return false;
        }
        bus->hal->delay_us(bus->hal->ctx, 1u);
        --bus->stretch_left_us;
    }
    return true;
}

/* STOP from SCL LOW: SDA rises while SCL is high. */
static i2c_bb_status_t stop(bus_t *bus)
{
    bus->hal->sda_release(bus->hal->ctx, false);
    half_period(bus);
    if (!scl_rise(bus)) {
        release_both(bus);
        return I2C_BB_STRETCH_TIMEOUT;
    }
    half_period(bus);
    bus->hal->sda_release(bus->hal->ctx, true);
    half_period(bus);
    return I2C_BB_OK;
}

/* One bit from SCL LOW, back to SCL LOW. A released bit that reads LOW means another driver. */
static i2c_bb_status_t write_bit(bus_t *bus, bool bit)
{
    bus->hal->sda_release(bus->hal->ctx, bit);
    half_period(bus);
    if (!scl_rise(bus)) {
        return I2C_BB_STRETCH_TIMEOUT;
    }
    half_period(bus);
    const bool sda = bus->hal->sda_read(bus->hal->ctx);
    bus->hal->scl_release(bus->hal->ctx, false);
    return (bit && !sda) ? I2C_BB_BUS_BUSY : I2C_BB_OK;
}

/* Eight data bits MSB first, then the slave's ACK bit. */
static i2c_bb_status_t write_byte(bus_t *bus, uint8_t byte)
{
    for (uint32_t i = 0u; i < 8u; ++i) {
        const i2c_bb_status_t st = write_bit(bus, (byte & (0x80u >> i)) != 0u);
        if (st != I2C_BB_OK) {
            return st;
        }
    }
    bus->hal->sda_release(bus->hal->ctx, true);
    half_period(bus);
    if (!scl_rise(bus)) {
        return I2C_BB_STRETCH_TIMEOUT;
    }
    half_period(bus);
    const bool ack = !bus->hal->sda_read(bus->hal->ctx);
    bus->hal->scl_release(bus->hal->ctx, false);
    return ack ? I2C_BB_OK : I2C_BB_NACK;
}

static i2c_bb_status_t bus_clear(bus_t *bus)
{
    bus->hal->sda_release(bus->hal->ctx, true);
    if (!scl_rise(bus)) {
        release_both(bus);
        return I2C_BB_STRETCH_TIMEOUT;
    }
    half_period(bus);

    for (uint32_t pulses = 0u;
         pulses < I2C_BB_MAX_CLEAR_PULSES && !bus->hal->sda_read(bus->hal->ctx); ++pulses) {
        bus->hal->scl_release(bus->hal->ctx, false);
        half_period(bus);
        if (!scl_rise(bus)) {
            release_both(bus);
            return I2C_BB_STRETCH_TIMEOUT;
        }
        half_period(bus);
    }

    /* STOP even when no clocks were needed: resets every slave's interface. */
    bus->hal->scl_release(bus->hal->ctx, false);
    half_period(bus);
    const i2c_bb_status_t st = stop(bus);
    if (st != I2C_BB_OK) {
        return st;
    }
    return (bus->hal->sda_read(bus->hal->ctx) && bus->hal->scl_read(bus->hal->ctx))
               ? I2C_BB_OK : I2C_BB_BUS_BUSY;
}

static i2c_bb_status_t write_reg(bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value)
{
    release_both(bus);
    half_period(bus);
    if (!bus->hal->sda_read(bus->hal->ctx) || !bus->hal->scl_read(bus->hal->ctx)) {
        return I2C_BB_BUS_BUSY;
    }

    /* START: SDA falls while SCL is high. */
    bus->hal->sda_release(bus->hal->ctx, false);
    half_period(bus);
    bus->hal->scl_release(bus->hal->ctx, false);

    const uint8_t bytes[3] = {(uint8_t)(addr7 << 1), reg, value};
    i2c_bb_status_t st = I2C_BB_OK;
    for (uint32_t i = 0u; i < sizeof bytes && st == I2C_BB_OK; ++i) {
        st = write_byte(bus, bytes[i]);
    }
    if (st == I2C_BB_STRETCH_TIMEOUT) {
        release_both(bus);
        return st;
    }
    const i2c_bb_status_t stop_st = stop(bus);
    return (st != I2C_BB_OK) ? st : stop_st;
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
i2c_bb_status_t i2c_bb_bus_clear(const i2c_bb_hal_t *hal)
{
    bus_t bus = {hal, I2C_BB_STRETCH_BUDGET_US};
    return bus_clear(&bus);
}

i2c_bb_status_t i2c_bb_write_reg(const i2c_bb_hal_t *hal, uint8_t addr7, uint8_t reg,
                                 uint8_t value)
{
    bus_t bus = {hal, I2C_BB_STRETCH_BUDGET_US};
    return write_reg(&bus, addr7, reg, value);
}

uint8_t i2c_bb_apply_safe_writes(const i2c_bb_hal_t *hal, const board_profile_t *p)
{
    if (p->i2c_sda_gpio == BOARD_PROFILE_I2C_NONE || p->i2c_write_count == 0u) {
        return 0u;
    }
    bus_t bus = {hal, I2C_BB_STRETCH_BUDGET_US};
    if (bus_clear(&bus) != I2C_BB_OK) {
        return 0u;
    }
    uint8_t acked = 0u;
    for (uint32_t i = 0u; i < p->i2c_write_count && i < BOARD_PROFILE_I2C_MAX_WRITES; ++i) {
        const i2c_bb_status_t st =
            write_reg(&bus, p->i2c_addr7, p->i2c_writes[i].reg, p->i2c_writes[i].value);
        if (st == I2C_BB_OK) {
            ++acked;
        } else if (st != I2C_BB_NACK) {
            break;  /* a bus fault: later writes cannot succeed either */
        }
    }
    return acked;
}
