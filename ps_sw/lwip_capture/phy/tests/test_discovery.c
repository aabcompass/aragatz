/* Test the exact injected discovery code. No MDIO hardware is used. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32_t;
typedef uint16_t u16_t;
typedef struct { struct { uintptr_t BaseAddress; } Config; } XEmacPs;
#define XPAR_XEMACPS_0_BASEADDR 0xE000B000U
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define TRUE 1
#define FALSE 0
#define ARAGATZ_KSZ9031_ID1 0x0022U
#define ARAGATZ_KSZ9031_ID2 0x1620U
#define ARAGATZ_KSZ9031_ID2_MASK 0xFFF0U
#define xil_printf(...) ((void)0)
static u32_t phymapemac0[32], phymapemac1[32];
static u16_t ids[32][2];
static unsigned read_count;
static int failed_address;

static int XEmacPs_PhyRead(XEmacPs *mac, u32_t addr, u32_t reg, u16_t *out)
{
    (void)mac;
    assert(addr < 32 && (reg == 2 || reg == 3));
    ++read_count;
    if ((int)addr == failed_address) return XST_FAILURE;
    *out = ids[addr][reg-2];
    return XST_SUCCESS;
}

#include "../ksz9031_detect.inc"

static void reset(void)
{
    memset(ids, 0xff, sizeof(ids));
    memset(phymapemac0, 0xff, sizeof(phymapemac0));
    memset(phymapemac1, 0xff, sizeof(phymapemac1));
    read_count = 0;
    failed_address = -1;
}

static unsigned count(const u32_t *map)
{
    unsigned i, n = 0;
    for (i = 0; i < 32; ++i) n += map[i] != 0;
    return n;
}

int main(void)
{
    XEmacPs mac = {{XPAR_XEMACPS_0_BASEADDR}};
    unsigned addr;
    for (addr = 0; addr < 32; ++addr) {
        reset();
        ids[addr][0] = 0x0022;
        ids[addr][1] = 0x1622;
        detect_phy(&mac);
        assert(read_count == 64);
        assert(count(phymapemac0) == 1 && phymapemac0[addr] == TRUE);
        assert(count(phymapemac1) == 32); /* only selected GEM changes */
    }
    reset();
    detect_phy(&mac);
    assert(count(phymapemac0) == 0); /* absent address0 is not broadcast */
    reset();
    ids[0][0] = ids[31][0] = 0x0022;
    ids[0][1] = ids[31][1] = 0x162f;
    detect_phy(&mac);
    assert(count(phymapemac0) == 0); /* ambiguous wiring requires review */
    reset();
    ids[4][0] = 0x0022; ids[4][1] = 0x1612; /* different Micrel model */
    ids[5][0] = 0x0141; ids[5][1] = 0x1234; /* Marvell */
    detect_phy(&mac);
    assert(count(phymapemac0) == 0);
    ids[0][0] = 0x0022; ids[0][1] = 0x1621;
    failed_address = 7;
    detect_phy(&mac);
    assert(count(phymapemac0) == 0); /* bus error invalidates discovery */
    failed_address = -1;
    mac.Config.BaseAddress += 0x1000;
    detect_phy(&mac);
    assert(count(phymapemac1) == 1 && phymapemac1[0] == TRUE);
    puts("KSZ9031 discovery mock: PASS (32 addresses, absent/ambiguous/wrong PHY, MDIO error)");
    return 0;
}
