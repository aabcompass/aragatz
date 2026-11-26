/* Adapter integration helper, not a replacement for the entire Xilinx BSP.
 * MDIO callback returns 0 on successful read/write. The implementation
 * must itself bound each MDIO transaction (including GEM idle waits).
 * Caller supplies board-specific reset/skew setup and GEM clock selection.
 */
#ifndef ARAGATZ_KSZ9031_H
#define ARAGATZ_KSZ9031_H
#include <stdint.h>
typedef int (*ksz_read)(void *,unsigned,unsigned,uint16_t *);
typedef int (*ksz_write)(void *,unsigned,unsigned,uint16_t);
typedef void (*ksz_delay)(unsigned);
static inline int ksz9031_find(void *ctx,ksz_read rd,unsigned *addr)
{
    unsigned a; uint16_t hi,lo;
    for(a=0;a<32;a++)
        if(!rd(ctx,a,2,&hi) && !rd(ctx,a,3,&lo) &&
           hi==0x0022 && (lo&0xfff0)==0x1620) {
            *addr=a; return 0;
        }
    return -1;
}
/* Return negotiated link mode from common advertisements, not BMCR speed.
 * BMSR link status is latch-low, therefore read it twice.
 * Full duplex required for this initial application.
 */
static inline int ksz9031_negotiate(void *ctx,ksz_read rd,ksz_write wr,
                                  ksz_delay delay,unsigned a,unsigned *speed)
{
    uint16_t bmcr,bmsr,adv,partner,gctl,gstat;
    unsigned i;
    if(rd(ctx,a,0,&bmcr)||rd(ctx,a,4,&adv))return -1;
    /* Disable asymmetric pause advertisement (see applicable PHY errata). */
    if(wr(ctx,a,4,adv & ~(1U<<11)) ||
       wr(ctx,a,0,(bmcr & ~((1U<<15)|(1U<<14)|(1U<<11)|(1U<<10))) |
                   (1U<<12)|(1U<<9)))return -1;
    for(i=0;i<100;i++) {
        if(rd(ctx,a,1,&bmsr)||rd(ctx,a,1,&bmsr))return -1;
        if((bmsr&0x24)==0x24)break;
        delay(100);
    }
    if(i==100)return -1;
    if(rd(ctx,a,4,&adv)||rd(ctx,a,5,&partner)||
       rd(ctx,a,9,&gctl)||rd(ctx,a,10,&gstat))return -1;
    if((gctl&0x200)&&(gstat&0x800)) *speed=1000;
    else if((gctl&0x100)&&(gstat&0x400)) return -1;
    else if(adv&partner&0x100) *speed=100;
    else if(adv&partner&0x80) return -1;
    else if(adv&partner&0x40) *speed=10;
    else return -1;
    return 0;
}
#endif
