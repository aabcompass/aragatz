/* Fixed V1 profile; call capture_poll from the raw lwIP main loop. */
#include "aragatz_hw.h"
#include "xil_cache.h"
#include "xtime_l.h"
#include <stdint.h>
#define ONE (128u+64u*768u)
#define SIZE (16u*ONE)
#ifndef ARAGATZ_ENABLE_CAPTURE
#define ARAGATZ_ENABLE_CAPTURE 0
#endif
enum { EMPTY, RESETTING, ARMING, RUNNING, STOPPING, READY, FAULT };
static unsigned state;
static XTime deadline;
static uint32_t id, crc;
uint32_t ar_crc(const void *buf, unsigned n) {
    const uint8_t *p=buf; uint32_t c=~0u;
    while(n--) { c^=*p++; for(unsigned j=0;j<8;j++) c=(c>>1)^((0u-(c&1u))&0xedb88320u); }
    return ~c;
}
unsigned capture_state(void){return state;}
uint32_t capture_id(void){return id;}
uint32_t capture_crc(void){return crc;}
unsigned capture_size(void){return SIZE;}
const void *capture_data(void){return (const void *)(UINTPTR)ARAGATZ_TEST_DDR_BASE;}
static void timeout(void){XTime_GetTime(&deadline); deadline+=2ull*COUNTS_PER_SECOND;}
static void fault(void){
    unsigned s=cap_rd(CAP_STATUS)&15u;
    if(s==CAP_STATE_FILL||s==CAP_STATE_WAIT||s==CAP_STATE_XFER) cap_wr(CAP_COMMAND,CAP_CMD_STOP);
    if(s==CAP_STATE_IDLE||s==CAP_STATE_END||s==CAP_STATE_ERROR) gen_wr(GEN_COMMAND,GEN_CMD_STOP);
    state=FAULT;
}
int capture_start(void){
    if(!ARAGATZ_ENABLE_CAPTURE || state!=EMPTY) return -1;
    if(ARAGATZ_TEST_DDR_BASE!=0x10000000u || ARAGATZ_TEST_DDR_SIZE!=0x100000u ||
       ARAGATZ_TEST_DDR_BASE<XPAR_PS7_DDR_0_S_AXI_BASEADDR ||
       ARAGATZ_TEST_DDR_BASE+ARAGATZ_TEST_DDR_SIZE-1>XPAR_PS7_DDR_0_S_AXI_HIGHADDR ||
       cap_rd(CAP_VERSION)!=ARAGATZ_IP_VERSION || gen_rd(GEN_VERSION)!=ARAGATZ_IP_VERSION) return -1;
    unsigned s=cap_rd(CAP_STATUS)&15u;
    if((s!=CAP_STATE_IDLE&&s!=CAP_STATE_END)||cap_rd(CAP_ERRORS)) return -1;
    gen_wr(GEN_COMMAND,GEN_CMD_STOP);
    if(s==CAP_STATE_END) cap_wr(CAP_COMMAND,CAP_CMD_STOP);
    state=RESETTING; timeout(); return 0;
}
void capture_poll(void){
    if(state==EMPTY||state==READY||state==FAULT) return;
    XTime now; XTime_GetTime(&now);
    unsigned status=cap_rd(CAP_STATUS), s=status&15u;
    if(now>=deadline||cap_rd(CAP_ERRORS)||s==CAP_STATE_ERROR){fault();return;}
    if(state==RESETTING && s==CAP_STATE_IDLE && gen_rd(GEN_STATUS)==0){
        cap_wr(CAP_COMMAND,CAP_CMD_CLEAR_STATS); gen_wr(GEN_COMMAND,GEN_CMD_CLEAR_STATS);
        for(unsigned i=0;i<(SIZE+256u)/4;i++) ddr_wr(i*4,ARAGATZ_SENTINEL(i));
        Xil_DCacheFlushRange((INTPTR)ARAGATZ_TEST_DDR_BASE,SIZE+256u);
        cap_wr(CAP_TRIGGER_ENABLE,1); cap_wr(CAP_N_FRAMES,64); cap_wr(CAP_N_PREV,16);
        cap_wr(CAP_N_FRAGMENTS,16); cap_wr(CAP_MEM_ADDR,ARAGATZ_TEST_DDR_BASE);
        cap_wr(CAP_HEADER_ID,ARAGATZ_HEADER_ID); cap_wr(CAP_AUX0,ARAGATZ_AUX0);
        cap_wr(CAP_AUX0+4,ARAGATZ_AUX1);
        for(unsigned i=2;i<8;i++) cap_wr(CAP_AUX0+4*i,0);
        cap_wr(CAP_INT_PERIOD,0); cap_wr(CAP_INT_CTRL,0); cap_wr(CAP_BUFFER_BYTES,SIZE);
        gen_wr(GEN_PAUSE,0); gen_wr(GEN_MODE,0); gen_wr(GEN_FRAME_WORDS,96);
        gen_wr(GEN_RATE_NUM,24); gen_wr(GEN_RATE_DEN,125); gen_wr(GEN_TRIG_MODE,1);
        gen_wr(GEN_TRIG_FIRST,128); gen_wr(GEN_TRIG_PERIOD,128); gen_wr(GEN_TRIG_WORD,47); gen_wr(GEN_EXT_WIDTH,4);
        cap_wr(CAP_COMMAND,CAP_CMD_START); state=ARMING; timeout();
    } else if(state==ARMING && s==CAP_STATE_FILL){
        gen_wr(GEN_COMMAND,GEN_CMD_START); state=RUNNING; timeout();
    } else if(state==RUNNING && s==CAP_STATE_END && !(status&(1u<<8))){
        gen_wr(GEN_COMMAND,GEN_CMD_STOP); state=STOPPING; timeout();
    } else if(state==STOPPING && gen_rd(GEN_STATUS)==0){
        if(s!=CAP_STATE_END || (status&(1u<<8)) || cap_rd(CAP_RUN_FRAGMENTS)!=16 ||
           cap_rd(CAP_FRAGMENTS_TOTAL)!=16 || cap_rd(CAP_LAST_ADDR)!=ARAGATZ_TEST_DDR_BASE+15*ONE ||
           cap_rd(CAP_LAST_BYTES)!=ONE || cap_rd(CAP_LAST_FRAMES)!=64 || cap_rd(CAP_LAST_STATUS)!=1 ||
           cap_rd(CAP_TRIG_ACCEPTED)!=16 || cap_rd(CAP_TRIG_ACCEPTED_HI) ||
           cap_rd(CAP_TRIG_MISSED)||cap_rd(CAP_TRIG_MISSED_HI)||cap_rd(CAP_AXI_ERRORS)||cap_rd(CAP_FIFO_OVERFLOWS)){fault();return;}
        Xil_DCacheInvalidateRange((INTPTR)ARAGATZ_TEST_DDR_BASE,SIZE+256u);
        for(unsigned i=SIZE/4;i<(SIZE+256u)/4;i++) if(ddr_rd(i*4)!=ARAGATZ_SENTINEL(i)){fault();return;}
        crc=ar_crc(capture_data(),SIZE); ++id; state=READY;
    }
}
int capture_ack(uint32_t i,uint32_t n,uint32_t c){
    if(state!=READY||i!=id||n!=SIZE||c!=crc)return -1;
    state=EMPTY;return 0;
}
