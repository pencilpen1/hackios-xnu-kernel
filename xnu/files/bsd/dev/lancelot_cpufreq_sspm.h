/* Actual MT6768 SSPM-v1 CPU startup, portable for fault-injected host tests.
 * Sources: sspm_ipi_define/layout/mbox (6dd120fc), cpufreq_hybrid (224fe470),
 * and CPU_LEVEL_6 platform tables. This is not the PMIC IPI protocol.
 * No firmware reset/loading, PLL programming or PMIC voltage writes. */
#ifndef LANCELOT_CPUFREQ_SSPM_H
#define LANCELOT_CPUFREQ_SSPM_H
#include "lancelot_cpufreq.h"
#include "lancelot_cpufreq_tables.h"

#define LCS_EINVAL (-22)
#define LCS_EBUSY (-16)
#define LCS_EIO (-5)
#define LCS_EPROTO (-71)
#define LCS_ETIMEDOUT (-110)
#define LCS_CPU_IRQ 2u /* pin1 - mailbox0.start0 => bit1 */
#define LCS_CCI_OFFSET LCF_CCI_MAP
#define LCS_ACK_NS UINT64_C(2000000000)
#define LCS_MAX_POLLS 40000u
enum lcs_bank { LCS_REPO, LCS_MB0, LCS_CTRL0, LCS_MB1, LCS_CTRL1, LCS_MB2, LCS_CTRL2, LCS_BANKS };
struct lcs_io {
    void *cookie;
    int (*read32)(void *, enum lcs_bank, unsigned, uint32_t *);
    int (*write32)(void *, enum lcs_bank, unsigned, uint32_t);
    uint64_t (*now_ns)(void *);
    void (*wait_us)(void *, unsigned);
};
struct lcs_state {
    struct lcs_io io;
    bool backed_up, claimed, poisoned, published, possible_command, awaiting_command;
    unsigned stage, commands, rx_count, bad_bank, bad_slot;
    uint32_t bad_value, bad_expected, rx_begin, rx_end;
    int32_t ack;
};

static inline bool lcs_access(enum lcs_bank bank, unsigned off)
{
    if ((off & 3) || (unsigned)bank >= LCS_BANKS) return false;
    return bank == LCS_REPO ? off < LCF_SRAM_SIZE :
        bank == LCS_CTRL0 || bank == LCS_CTRL1 || bank == LCS_CTRL2 ? off <= 4 : off < 128;
}
static inline int lcs_read(struct lcs_state *s, enum lcs_bank bank, unsigned off, uint32_t *v)
{
    if (!s || !v || !s->io.read32 || !lcs_access(bank, off)) return LCS_EINVAL;
    return s->io.read32(s->io.cookie, bank, off, v);
}
static inline int lcs_write(struct lcs_state *s, enum lcs_bank bank, unsigned off, uint32_t v)
{
    if (!s || !s->claimed || s->poisoned || !s->io.write32 || !lcs_access(bank, off)) return LCS_EINVAL;
    /* Strict write scope, independent of the physical adapter's allowlist. */
    if (bank == LCS_REPO) {
        if (s->possible_command) return LCS_EINVAL;
    } else if (bank == LCS_MB0) {
        if (off < 12 || off > 24) return LCS_EINVAL;
    } else if ((bank != LCS_CTRL0 && bank != LCS_CTRL2) || off != 4 || v != LCS_CPU_IRQ) {
        if (bank != LCS_CTRL0 || off != 0 || v != LCS_CPU_IRQ) return LCS_EINVAL;
    }
    return s->io.write32(s->io.cookie, bank, off, v);
}
/* Layout tags identify the v1 slot geometry, NOT a running-image hash. All
 * used slots must still carry their startup tags before claiming cold state. */
static inline int lcs_layout(struct lcs_state *s)
{
    static const uint8_t sizes[3][7] = {{3,4,4,4,9,3,0},{2,8,5,7,4,4,2},{3,4,4,4,0,0,0}};
    static const unsigned counts[3] = {6,7,4}, first[3] = {0,6,0};
    static const enum lcs_bank mb[3] = {LCS_MB0,LCS_MB1,LCS_MB2};
    for (unsigned m=0;m<3;m++) {
        unsigned slot=0;
        for (unsigned p=0;p<counts[m];p++) {
            uint32_t expected=0x4d000000u | ((m==2?2u:1u)<<16) | ((p+first[m])<<8) | sizes[m][p];
            for (unsigned j=0;j<sizes[m][p];j++,slot++) {
                uint32_t value;int e=lcs_read(s,mb[m],slot*4,&value);
                if (e) return e;
                if (value!=expected) {
                    s->bad_bank=m;s->bad_slot=slot;s->bad_value=value;s->bad_expected=expected;
                    return LCS_EPROTO;
                }
            }
        }
    }
    return 0;
}
static inline int lcs_idle(struct lcs_state *s)
{
    const enum lcs_bank ctrl[2]={LCS_CTRL0,LCS_CTRL2};
    for (unsigned i=0;i<2;i++) for (unsigned off=0;off<=4;off+=4) {
        uint32_t v;int e=lcs_read(s,ctrl[i],off,&v);
        if (e) return e;
        if (v&LCS_CPU_IRQ) return LCS_EBUSY; /* Never clear a pre-existing ACK. */
    }
    return 0;
}
/* CPU log notifications use mailbox2 pin1. Linux's receive ISR only consumes
 * OUT_IRQ for this channel; unlike PLATFORM, the CPU thread sends no IN_IRQ ACK.
 * Preserve all other IRQ bits and all mailbox payload words. */
static inline int lcs_receive(struct lcs_state *s)
{
    if (!s || !s->claimed || s->poisoned) return LCS_EINVAL;
    if (!s->awaiting_command) {
        for (unsigned off=0;off<=4;off+=4) {
            uint32_t v;int e=lcs_read(s,LCS_CTRL0,off,&v);
            if (e || (v&LCS_CPU_IRQ)) {
                s->poisoned=true;s->bad_bank=LCS_CTRL0;s->bad_slot=off/4;
                s->bad_value=e?0:v;s->bad_expected=0;
                return e?e:LCS_EPROTO; /* Preserve unknown ACK/doorbell; no clear or retry. */
            }
        }
    }
    uint32_t irq, words[4];int e=lcs_read(s,LCS_CTRL2,4,&irq);
    if (e || !(irq&LCS_CPU_IRQ)) return e;
    for (unsigned i=0;i<4;i++) if ((e=lcs_read(s,LCS_MB2,12+i*4,&words[i]))) return e;
    for (unsigned i=0;i<2;i++)
        if (words[i]<LCF_LOG || words[i]>=LCF_LOG+LCF_LOG_COUNT*20 || (words[i]-LCF_LOG)%20) return LCS_EPROTO;
    e=lcs_write(s,LCS_CTRL2,4,LCS_CPU_IRQ);
    if (!e) {s->rx_count++;s->rx_begin=words[0];s->rx_end=words[1];}
    return e;
}
static inline int lcs_command(struct lcs_state *s, unsigned command, unsigned arg0, unsigned arg1)
{
    if (!s || !s->claimed || s->poisoned || !s->published || !s->io.now_ns || !s->io.wait_us ||
        command>2 || (command<2 && (arg0||arg1)) || (command==2 && (arg0>1||arg1!=1))) return LCS_EINVAL;
    uint32_t irq;int e=lcs_read(s,LCS_CTRL0,4,&irq);
    if (e) return e;
    if (irq&LCS_CPU_IRQ) return LCS_EBUSY;
    if ((e=lcs_read(s,LCS_CTRL0,0,&irq))) return e;
    if (irq&LCS_CPU_IRQ) return LCS_EBUSY;
    const uint32_t words[4]={command,arg0,arg1,0};
    for (unsigned i=0;i<4;i++) if ((e=lcs_write(s,LCS_MB0,12+i*4,words[i]))) goto failed;
    for (unsigned i=0;i<4;i++) {
        uint32_t v;
        if ((e=lcs_read(s,LCS_MB0,12+i*4,&v))) goto failed;
        if (v!=words[i]) {e=LCS_EPROTO;goto failed;}
    }
    /* Four fully initialized words; no invented wire sequence/length fields.
     * Once the doorbell MAY have been issued, an orphan/late ACK can never be
     * accepted by a retry. Keep this CPU channel poisoned for the whole boot. */
    s->possible_command=true;
    s->awaiting_command=true;
    uint64_t begin=s->io.now_ns(s->io.cookie);
    if ((e=lcs_write(s,LCS_CTRL0,0,LCS_CPU_IRQ))) goto failed;
    s->commands++;
    for (unsigned poll=0;poll<LCS_MAX_POLLS;poll++) {
        uint64_t now=s->io.now_ns(s->io.cookie);
        if (now<begin || now-begin>=LCS_ACK_NS) {e=LCS_ETIMEDOUT;goto failed;}
        if ((e=lcs_receive(s))) goto failed;
        if ((e=lcs_read(s,LCS_CTRL0,4,&irq))) goto failed;
        if (irq&LCS_CPU_IRQ) {
            uint32_t a,b;
            if ((e=lcs_read(s,LCS_MB0,12,&a)) || (e=lcs_read(s,LCS_MB0,12,&b))) goto failed;
            if (a!=b) {e=LCS_EPROTO;goto failed;}
            now=s->io.now_ns(s->io.cookie);
            if (now<begin || now-begin>=LCS_ACK_NS) {e=LCS_ETIMEDOUT;goto failed;}
            s->ack=(int32_t)a;
            if ((e=lcs_write(s,LCS_CTRL0,4,LCS_CPU_IRQ))) goto failed;
            if (s->ack!=0) {e=s->ack<0?s->ack:LCS_EPROTO;goto failed;}
            s->awaiting_command=false;
            return 0;
        }
        s->io.wait_us(s->io.cookie,50);
    }
    e=LCS_ETIMEDOUT;
failed:
    s->awaiting_command=false;
    s->poisoned=true;
    return e;
}

/* Initialize only fresh, unclaimed firmware startup slots. Never overwrite an
 * active/unknown repository merely because its guard or table fails admission.
 * The caller retains the complete old SRAM in RAM for diagnostic evidence.
 * Failed publication/startup is kept, not erased or advertised as success. */
static inline int lcs_cold_start(struct lcs_state *s, const unsigned initial[2], uint32_t *backup)
{
    if (!s || s->claimed || s->poisoned || !backup || !initial || !s->io.read32 ||
        !s->io.write32 || !s->io.now_ns || !s->io.wait_us ||
        initial[0]>=16 || initial[1]>=16) return LCS_EINVAL;
    int e=lcs_layout(s);
    if (!e) e=lcs_idle(s);
    if (e) return e;
    for (unsigned off=0;off<LCF_SRAM_SIZE;off+=4) if ((e=lcs_read(s,LCS_REPO,off,&backup[off/4]))) return e;
    s->backed_up=true;
    /* Even partly published guard words revoke cold ownership. Keep the old
     * state for diagnosis instead of resetting another CPU-DVFS instance. */
    for (unsigned i=0;i<4;i++) if (backup[i]==(i&1?0xaa55aa55u:0x55aa55aau)) return LCS_EBUSY;
    if (backup[0x2a0/4]==0x55aa55aa) return LCS_EBUSY;
    for (unsigned off=0;off<LCF_SRAM_SIZE;off+=4) {
        uint32_t value;
        if ((e=lcs_read(s,LCS_REPO,off,&value))) return e;
        if (value!=backup[off/4]) return LCS_EBUSY;
    }
    /* Fresh-tag recheck after the complete backup, before the first write. */
    if ((e=lcs_layout(s)) || (e=lcs_idle(s))) return e;
    s->claimed=true;s->stage=1;
    for (unsigned off=0x2a0;off<LCF_SRAM_SIZE;off+=4) if ((e=lcs_write(s,LCS_REPO,off,0))) goto failed;
    for (unsigned off=0x10;off<0x250;off+=4) if ((e=lcs_write(s,LCS_REPO,off,0))) goto failed;
    for (unsigned c=0;c<3;c++) {
        for (unsigned i=0;i<16;i++) {
            if ((e=lcs_write(s,LCS_REPO,0x10+c*0x90+i*4,(uint32_t)lcf_voltage[c][i]<<16|lcf_mhz[c][i]))) goto failed;
            if ((e=lcs_write(s,LCS_REPO,0x50+c*0x90+i*4,0x10000u|lcf_postdiv[c][i]))) goto failed;
        }
        if ((e=lcs_write(s,LCS_REPO,0x90+c*0x90,UINT32_MAX))) goto failed;
    }
    if ((e=lcs_write(s,LCS_REPO,0x240,UINT32_MAX))) goto failed; /* reference's reserved fourth sentinel */
    for (unsigned i=0;i<sizeof(lcf_cci_map);i+=4) {
        const uint8_t *p=(const uint8_t *)lcf_cci_map+i;
        uint32_t v=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        if ((e=lcs_write(s,LCS_REPO,LCS_CCI_OFFSET+i,v))) goto failed;
    }
    /* Full rated OPP range, while inherited divider0 still boots at OPP15.
     * Firmware owns every PLL/rail transition. Disable only schedule assist. */
    if ((e=lcs_write(s,LCS_REPO,LCF_LIMIT,15u)) ||
        (e=lcs_write(s,LCS_REPO,LCF_LIMIT+4,15u)) ||
        (e=lcs_write(s,LCS_REPO,LCF_REQUEST,initial[0])) ||
        (e=lcs_write(s,LCS_REPO,LCF_REQUEST+4,initial[1])) ||
        (e=lcs_write(s,LCS_REPO,0x2bc,1))) goto failed;
    for (unsigned i=0;i<4;i++) if ((e=lcs_write(s,LCS_REPO,i*4,i&1?0xaa55aa55:0x55aa55aa))) goto failed;
    if ((e=lcs_write(s,LCS_REPO,0x2a0,0x55aa55aa))) goto failed;
    /* Exact readback of the AP-owned published region before any IPI. */
    for (unsigned c=0;c<3;c++) for (unsigned i=0;i<16;i++) {
        uint32_t v;
        if ((e=lcs_read(s,LCS_REPO,0x10+c*0x90+i*4,&v))) goto failed;
        if (v!=((uint32_t)lcf_voltage[c][i]<<16|lcf_mhz[c][i])) {e=LCS_EIO;goto failed;}
        if ((e=lcs_read(s,LCS_REPO,0x50+c*0x90+i*4,&v))) goto failed;
        if (v!=(0x10000u|lcf_postdiv[c][i])) {e=LCS_EIO;goto failed;}
    }
    for (unsigned i=0;i<sizeof(lcf_cci_map);i+=4) {
        const uint8_t *p=(const uint8_t *)lcf_cci_map+i;uint32_t v;
        if ((e=lcs_read(s,LCS_REPO,LCS_CCI_OFFSET+i,&v))) goto failed;
        if (v!=((uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24)) {e=LCS_EIO;goto failed;}
    }
    for (unsigned i=0;i<4;i++) {
        uint32_t v;
        if ((e=lcs_read(s,LCS_REPO,i*4,&v))) goto failed;
        if (v!=(i&1?0xaa55aa55u:0x55aa55aau)) {e=LCS_EIO;goto failed;}
    }
    const unsigned check_off[9]={0x90,0x120,0x1b0,0x240,0x2a0,LCF_LIMIT,LCF_LIMIT+4,LCF_REQUEST,LCF_REQUEST+4};
    const uint32_t check_val[9]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,0x55aa55aa,15u,15u,initial[0],initial[1]};
    for (unsigned i=0;i<9;i++) {
        uint32_t v;
        if ((e=lcs_read(s,LCS_REPO,check_off[i],&v))) goto failed;
        if (v!=check_val[i]) {e=LCS_EIO;goto failed;}
    }
    uint32_t assist;
    if ((e=lcs_read(s,LCS_REPO,0x2bc,&assist))) goto failed;
    if (assist!=1) {e=LCS_EIO;goto failed;}
    /* CPU-DVFS records describe transitions, not a periodic heartbeat. Prove
     * the entire log is zero before starting firmware so a nonzero post-init
     * record demonstrates a NEW firmware event, even at a stable initial OPP. */
    for (unsigned off=LCF_LOG;off<LCF_LOG+LCF_LOG_COUNT*20;off+=4) {
        uint32_t value;
        if ((e=lcs_read(s,LCS_REPO,off,&value))) goto failed;
        if (value) {e=LCS_EIO;goto failed;}
    }
    /* Startup tags must still be untouched; layout checking is read-only. */
    if ((e=lcs_layout(s)) || (e=lcs_idle(s))) goto failed;
    s->published=true;s->stage=2;
    if ((e=lcs_command(s,0,0,0))) goto failed; /* INIT_PTBL: v1 seg argument is0, not CPU_LEVEL_6 */
    s->stage=3;
    if ((e=lcs_command(s,1,0,0))) goto failed; /* INIT */
    s->stage=4;
    if ((e=lcs_command(s,2,0,1))) goto failed;
    s->stage=5;
    if ((e=lcs_command(s,2,1,1))) goto failed;
    s->stage=6; /* ACKs complete, STILL NOT runtime/voltage/PLL admission. */
    return 0;
failed:
    s->poisoned=true;
    return e;
}
#endif
