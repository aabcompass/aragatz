/* Host behavioral test of the exact injected PHY helper, not hardware proof.
 * cc -std=c11 -Wall -Wextra -Werror test_ksz9031_mock.c -o phy_mock && ./phy_mock
 * No socket, MDIO hardware, or BSP is involved.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef struct { struct { uintptr_t BaseAddress; } Config; } XEmacPs;
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define XPAR_XEMACPS_0_BASEADDR 0xE000B000U
#define CONFIG_LINKSPEED_AUTODETECT 1
#define xil_printf(...) ((void)0)

static u16_t registers[32];
static unsigned reads, writes, delays;
static int fail_read, fail_write, hold_restart, latched_low;
static unsigned fail_write_at;

static int XEmacPs_PhyRead(XEmacPs *mac, u32_t addr, u32_t reg, u16_t *value)
{
    (void)mac;
    assert(addr <= 31 && reg <= 31);
    ++reads;
    if (fail_read) return XST_FAILURE;
    if (reg == 0 && !hold_restart) registers[0] &= (u16_t)~0x0200U;
    if (reg == 1 && latched_low) {
        latched_low = 0;
        *value = 0;
        return XST_SUCCESS;
    }
    *value = registers[reg];
    return XST_SUCCESS;
}

static int XEmacPs_PhyWrite(XEmacPs *mac, u32_t addr, u32_t reg, u16_t value)
{
    (void)mac;
    assert(addr <= 31);
    /* The minimal overlay must never write reset/skew vendor registers. */
    assert(reg == 0 || reg == 4 || reg == 9);
    assert(reg != 0 || (value & 0x8000U) == 0U);
    ++writes;
    if (fail_write || writes == fail_write_at) return XST_FAILURE;
    registers[reg] = value;
    return XST_SUCCESS;
}

static void usleep(unsigned usec)
{
    assert(usec == 100000U);
    ++delays;
}

#include "../ksz9031_impl.inc"

static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    memset(aragatz_ksz9031_state, 0, sizeof(aragatz_ksz9031_state));
    registers[0] = 0x1000;
    registers[4] = 0x0de1;
    registers[9] = 0x0300;
    registers[1] = 0x0024;
    registers[31] = 0x0048;
    reads = writes = delays = 0;
    fail_read = fail_write = 0;
    hold_restart = latched_low = 0;
    fail_write_at = 0;
}

int main(void)
{
    XEmacPs mac = {{XPAR_XEMACPS_0_BASEADDR}};
    unsigned count, i;
    u32_t speed;
    reset();
    assert(get_KSZ9031_phy_speed(&mac, 0) == 1000);
    assert(writes == 3 && delays == 0);
    assert(registers[4] == 0x0541 && registers[9] == 0x0200);
    assert((registers[0] & 0x1000U) == 0x1000U);
    registers[31] = 0x0028;
    assert(get_KSZ9031_phy_speed(&mac, 0) == 100);
    registers[31] = 0x0018;
    assert(get_KSZ9031_phy_speed(&mac, 0) == 10);
    assert(writes == 3 && delays == 0);
    registers[1] = 0;
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
    assert(writes == 3 && delays == 0); /* hotplug never sleeps */

    reset();
    registers[1] = 0;
    assert(get_KSZ9031_phy_speed(&mac, 31) == XST_FAILURE);
    assert(delays == 100); /* bounded startup timeout */
    registers[1] = 0x24;
    assert(get_KSZ9031_phy_speed(&mac, 31) == 1000);
    assert(delays == 100 && writes == 3);

    reset();
    registers[31] = 0x40; /* half-duplex */
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
    assert(delays == 0);
    registers[31] = 0x78; /* impossible multiple speed bits */
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);

    reset();
    fail_read = 1;
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
    count = reads;
    fail_read = 0;
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
    assert(reads == count && writes == 0 && delays == 0);

    reset();
    fail_write = 1;
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
    assert(writes == 1 && delays == 0);
    fail_write = 0;
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
    assert(writes == 1); /* requires deliberate application restart */

    reset();
    registers[0] = 0x8000; /* reset in progress is not guessed away */
    assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
    assert(writes == 0 && delays == 0);
    assert(get_KSZ9031_phy_speed(&mac, 32) == XST_FAILURE);
    for (i=1; i<=3; ++i) {
        reset(); fail_write_at=i;
        assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
        assert(writes==i && delays==0);
        fail_write_at=0;
        assert(get_KSZ9031_phy_speed(&mac, 0) == XST_FAILURE);
        assert(writes==i); /* partial configuration stays failed */
    }
    reset();
    latched_low=1;
    assert(get_KSZ9031_phy_speed(&mac, 0)==1000); /* BMSR second read */
    hold_restart=1; registers[0]|=0x0200; speed=123;
    assert(aragatz_ksz9031_read_speed(&mac, 0, &speed)==0 && speed==0);
    hold_restart=0;
    registers[0]=0; /* forced mode with stale link bits */
    assert(aragatz_ksz9031_read_speed(&mac, 0, &speed)==-2);
    registers[0]=0x5000; /* loopback */
    assert(aragatz_ksz9031_read_speed(&mac, 0, &speed)==-2);
    registers[0]=0x1800; /* power-down */
    assert(aragatz_ksz9031_read_speed(&mac, 0, &speed)==-2);
    registers[0]=0x1400; /* isolate */
    assert(aragatz_ksz9031_read_speed(&mac, 0, &speed)==-2);
    registers[0]=0x1000; registers[31]=0x49; /* link check fail */
    assert(aragatz_ksz9031_read_speed(&mac, 0, &speed)==-2);
    assert(writes==3 && delays==0);
    puts("KSZ9031 helper mock: PASS (10/100/1000, hotplug, timeout, errors, duplex)");
    return 0;
}
