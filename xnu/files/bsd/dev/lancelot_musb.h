/* SPDX-License-Identifier: GPL-2.0-only
 * Bounded, polling MT6768 MUSB peripheral core. Register/EP0 sequencing follows
 * the board's MusbDiagnosticDxe (GPL-2.0-only); no firmware callbacks are used.
 * The adapter supplies ordered MMIO and an immutable console snapshot.
 */
#ifndef LANCELOT_MUSB_H
#define LANCELOT_MUSB_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define LU_SNAPSHOT 16384u
#define LU_HEADER 40u
enum { LU_ADDR=0, LU_POWER=1, LU_INTRTX=2, LU_INTRRX=4, LU_TXE=6, LU_RXE=8,
       LU_INTRUSB=10, LU_USBE=11, LU_INDEX=14, LU_TXMAX=16, LU_TXCSR=18,
       LU_RXMAX=20, LU_RXCSR=22, LU_COUNT=24, LU_FIFO0=32, LU_FIFO1=36 };
enum { LU_IDLE, LU_SETUP_ACK, LU_DATA_IN, LU_STATUS_IN, LU_STATUS_OUT };
enum { LU_NONE, LU_LOG_HEADER, LU_LOG_DATA, LU_LOG_END, LU_UPLOAD_DATA, LU_UPLOAD_END };
struct lancelot_musb {
	void *ctx;
	uint8_t (*r8)(void *, unsigned);
	uint16_t (*r16)(void *, unsigned);
	void (*w8)(void *, unsigned, uint8_t);
	void (*w16)(void *, unsigned, uint16_t);
	size_t (*snapshot)(uint64_t, uint64_t *, uint8_t *, size_t);
	uint64_t boot, cursor, start;
	uint8_t address, configuration, control[128], bytes[LU_HEADER + LU_SNAPSHOT];
	unsigned state, reply, length, offset, control_length, control_offset, ack_polls;
	int pending_address;
	bool zlp, halt_in, halt_out;
	uint64_t polls, resets, setups, commands, stalls, sent;
};
#define LU_R8(r) u->r8(u->ctx, (r))
#define LU_R16(r) u->r16(u->ctx, (r))
#define LU_W8(r,v) u->w8(u->ctx, (r), (v))
#define LU_W16(r,v) u->w16(u->ctx, (r), (v))
static void lu_fifo_out(struct lancelot_musb *u, unsigned fifo, const void *data, unsigned n)
{
	const uint8_t *p = data;
	for (unsigned i = 0; i < n; i++) LU_W8(fifo, p[i]);
}
static void lu_bulk_reset(struct lancelot_musb *u)
{
	LU_W8(LU_INDEX, 1);
	LU_W8(0x62, 3); LU_W8(0x63, 3);
	LU_W16(0x64, 8); LU_W16(0x66, 16);
	LU_W16(LU_TXMAX, 64); LU_W16(LU_RXMAX, 64);
	LU_W16(LU_TXCSR, 0x2048); LU_W16(LU_TXCSR, 0x2048);
	LU_W16(LU_RXCSR, 0x90); LU_W16(LU_RXCSR, 0x90);
	LU_W16(LU_TXCSR, 0x2000); LU_W16(LU_RXCSR, 0);
	u->halt_in = u->halt_out = false; u->reply = LU_NONE;
	LU_W8(LU_INDEX, 0);
}
static void lu_reset(struct lancelot_musb *u)
{
	u->resets++; u->configuration = u->address = 0;
	u->pending_address = -1; u->state = LU_IDLE;
	u->control_length = u->control_offset = u->ack_polls = 0; u->zlp = false;
	LU_W8(LU_ADDR, 0); lu_bulk_reset(u); LU_W16(LU_TXCSR, 0x100);
}
static void lu_stall(struct lancelot_musb *u)
{
	u->stalls++; u->state = LU_IDLE; u->pending_address = -1;
	LU_W16(LU_TXCSR, 0x60);
}
static void lu_control_packet(struct lancelot_musb *u)
{
	unsigned n = u->control_length - u->control_offset;
	if (n > 64) n = 64;
	lu_fifo_out(u, LU_FIFO0, u->control + u->control_offset, n);
	u->control_offset += n;
	bool last = u->control_offset == u->control_length && (!u->zlp || !n);
	u->state = last ? LU_STATUS_OUT : LU_DATA_IN;
	LU_W16(LU_TXCSR, 2 | (last ? 8 : 0));
}
static void lu_control_reply(struct lancelot_musb *u, const void *data, unsigned length, unsigned requested)
{
	u->control_length = length < requested ? length : requested;
	if (u->control_length > sizeof(u->control)) { lu_stall(u); return; }
	u->control_offset = 0;
	u->zlp = length < requested && u->control_length % 64 == 0;
	if (data != u->control) memcpy(u->control, data, u->control_length);
	u->state = LU_SETUP_ACK; u->ack_polls = 0;
	LU_W16(LU_TXCSR, 0x40); /* release shared SETUP FIFO before writing IN */
}
static void lu_ack(struct lancelot_musb *u)
{
	u->state = LU_STATUS_IN; LU_W16(LU_TXCSR, 0x48);
}
static void lu_setup(struct lancelot_musb *u, const uint8_t *s)
{
	/* Retain firmware VID/PID, serial, interface and FS endpoints for Windows. */
	static const uint8_t device[] = {18,1,0,2,0,0,0,64,0xd1,0x18,0x0d,0xd0,3,0,1,2,3,1};
	static const uint8_t config[] = {9,2,32,0,1,1,0,0xc0,1,9,4,0,0,2,0xff,0x42,3,0,
	    7,5,0x81,2,64,0,0,7,5,1,2,64,0,0};
	unsigned type = s[0], req = s[1], value = s[2] | s[3] << 8;
	unsigned index = s[4] | s[5] << 8, len = s[6] | s[7] << 8;
	uint8_t reply[2] = {0, 0};
	u->setups++; u->pending_address = -1;
	if (type == 0x80 && req == 6 && len) {
		unsigned kind = value >> 8, number = value & 255;
		if (kind == 1 && !number && !index) { lu_control_reply(u, device, sizeof(device), len); return; }
		if (kind == 2 && !number && !index) { lu_control_reply(u, config, sizeof(config), len); return; }
		if (kind == 3) {
			const char *text = NULL;
			if (!number) { const uint8_t lang[] = {4,3,9,4}; lu_control_reply(u, lang, 4, len); return; }
			if (!index || index == 0x409) {
				if (number == 1) text = "Lancelot UEFI";
				if (number == 2) text = "MUSB diagnostics";
				if (number == 3) text = "lancelot-uefi";
			}
			if (text) {
				unsigned n = (unsigned)strlen(text);
				u->control[0] = (uint8_t)(2 + n * 2); u->control[1] = 3;
				for (unsigned i = 0; i < n; i++) { u->control[2+i*2] = text[i]; u->control[3+i*2] = 0; }
				lu_control_reply(u, u->control, u->control[0], len); return;
			}
		}
	} else if (!type && req == 5 && value <= 127 && !index && !len && !u->configuration) {
		u->pending_address = (int)value; lu_ack(u); return;
	} else if (!type && req == 9 && value <= 1 && !index && !len && u->address) {
		u->configuration = (uint8_t)value; lu_bulk_reset(u); lu_ack(u); return;
	} else if (type == 0x80 && req == 8 && !value && !index && len == 1) {
		lu_control_reply(u, &u->configuration, 1, len); return;
	} else if (!req && !value && len == 2) {
		if (type == 0x80 && !index) reply[0] = 1;
		else if (type == 0x81 && !index && u->configuration) reply[0] = 0;
		else if (type == 0x82 && (index == 0 || index == 0x80)) reply[0] = 0;
		else if (type == 0x82 && u->configuration && (index == 1 || index == 0x81))
			reply[0] = index == 1 ? u->halt_out : u->halt_in;
		else { lu_stall(u); return; }
		lu_control_reply(u, reply, 2, len); return;
	} else if (type == 0x81 && req == 10 && !value && !index && len == 1 && u->configuration) {
		lu_control_reply(u, reply, 1, len); return;
	} else if (type == 1 && req == 11 && !value && !index && !len && u->configuration) {
		lu_bulk_reset(u); lu_ack(u); return;
	} else if (type == 2 && (req == 1 || req == 3) && !value && !len && u->configuration &&
	    (index == 1 || index == 0x81)) {
		bool halt = req == 3;
		LU_W8(LU_INDEX, 1);
		if (index == 1) { u->halt_out = halt; LU_W16(LU_RXCSR, halt ? 0x20 : 0x90); }
		else { u->halt_in = halt; LU_W16(LU_TXCSR, halt ? 0x2010 : 0x2048); }
		/* A cleared halt abandons the old transaction; the host retries its cursor. */
		if (!halt) u->reply = LU_NONE;
		LU_W8(LU_INDEX, 0); lu_ack(u); return;
	}
	lu_stall(u);
}
static void lu_control_poll(struct lancelot_musb *u, bool tx)
{
	LU_W8(LU_INDEX, 0);
	uint16_t csr = LU_R16(LU_TXCSR);
	if (csr & 4) { LU_W16(LU_TXCSR, 0); u->state = LU_IDLE; u->pending_address = -1; csr = LU_R16(LU_TXCSR); }
	if (csr & 0x10) { LU_W16(LU_TXCSR, 0x80); u->state = LU_IDLE; u->pending_address = -1; csr = LU_R16(LU_TXCSR); }
	if (csr & 8) return; /* DATAEND, not an interrupt, determines status completion */
	if (u->state == LU_SETUP_ACK) {
		if (csr & 1) { if (++u->ack_polls >= 100) lu_stall(u); return; }
		if (!(csr & 2)) lu_control_packet(u);
		return;
	}
	if (tx && u->state == LU_STATUS_IN && !(csr & 2)) {
		if (u->pending_address >= 0) { u->address = (uint8_t)u->pending_address; LU_W8(LU_ADDR, u->address); u->pending_address = -1; }
		u->state = LU_IDLE;
	} else if (tx && u->state == LU_STATUS_OUT && !(csr & 2)) {
		u->state = LU_IDLE;
		if ((csr & 1) && !LU_R8(LU_COUNT)) { LU_W16(LU_TXCSR, 0x40); return; }
	}
	if (csr & 1) {
		unsigned n = LU_R8(LU_COUNT);
		if (u->state == LU_STATUS_OUT && !n) { LU_W16(LU_TXCSR, 0x40); u->state = LU_IDLE; }
		else if (n == 8) { uint8_t setup[8]; for (unsigned i=0;i<8;i++) setup[i]=LU_R8(LU_FIFO0); lu_setup(u, setup); }
		else lu_stall(u);
	} else if (tx && u->state == LU_DATA_IN && !(csr & 2)) lu_control_packet(u);
}
static void lu_hex(char *p, uint64_t v, unsigned digits)
{
	static const char hex[] = "0123456789abcdef";
	for (unsigned i=0;i<digits;i++) p[i]=hex[(v >> ((digits-1-i)*4)) & 15];
}
static bool lu_cursor(const char *cmd, unsigned n, const char *prefix, uint64_t *value)
{
	unsigned len = (unsigned)strlen(prefix);
	if (n != len + 16 || memcmp(cmd, prefix, len)) return false;
	uint64_t v = 0;
	for (unsigned i=len;i<n;i++) {
		unsigned c=(unsigned char)cmd[i], d;
		if (c >= '0' && c <= '9') d=c-'0';
		else if (c >= 'a' && c <= 'f') d=c-'a'+10;
		else if (c >= 'A' && c <= 'F') d=c-'A'+10;
		else return false;
		v=(v<<4)|d;
	}
	*value=v; return true;
}
static uint32_t lu_crc32(const uint8_t *p, unsigned n)
{
	uint32_t c=~0u;
	for (unsigned i=0;i<n;i++) { c^=p[i]; for (unsigned b=0;b<8;b++) c=(c>>1)^((0u-(c&1))&0xedb88320u); }
	return ~c;
}
static void lu_le(uint8_t *p, uint64_t v, unsigned n) { for (unsigned i=0;i<n;i++) p[i]=(uint8_t)(v>>(i*8)); }
static void lu_send(struct lancelot_musb *u, const void *p, unsigned n)
{
	lu_fifo_out(u, LU_FIFO1, p, n); LU_W16(LU_TXCSR, 0x2001); u->sent += n;
}
static void lu_next(struct lancelot_musb *u)
{
	char p[64]; unsigned n;
	if (u->reply == LU_LOG_HEADER) {
		memcpy(p,"INFOLLOG1 ",10); lu_hex(p+10,u->boot,16); p[26]=' ';
		lu_hex(p+27,u->start,16); p[43]=' '; lu_hex(p+44,u->start+u->length,16);
		n=60; u->reply = u->length ? LU_LOG_DATA : LU_LOG_END;
	} else if (u->reply == LU_LOG_DATA) {
		unsigned count=u->length-u->offset; if(count>24)count=24;
		memcpy(p,"INFOLLOGD ",10);
		for(unsigned i=0;i<count;i++)lu_hex(p+10+i*2,u->bytes[u->offset++],2);
		n=10+count*2; if(u->offset==u->length)u->reply=LU_LOG_END;
	} else if (u->reply == LU_UPLOAD_DATA) {
		n=u->length-u->offset; if(n>64)n=64;
		lu_send(u,u->bytes+u->offset,n); u->offset+=n;
		if(u->offset==u->length)u->reply=LU_UPLOAD_END;
		return;
	} else { memcpy(p,"OKAY",4); n=4; u->reply=LU_NONE; }
	lu_send(u,p,n);
}
static void lu_command(struct lancelot_musb *u, const char *cmd, unsigned n)
{
	uint64_t cursor;
	const char *reply="FAILdiagnostic logs only";
	u->commands++;
	if (lu_cursor(cmd,n,"oem log ",&cursor)) {
		u->length=(unsigned)u->snapshot(cursor,&u->start,u->bytes,4096);
		u->offset=0; u->reply=LU_LOG_HEADER; lu_next(u); return;
	}
	if (lu_cursor(cmd,n,"oem klog ",&cursor)) { u->cursor=cursor; reply="OKAY"; }
	else if (n == 6 && !memcmp(cmd,"upload",6)) {
		unsigned count=(unsigned)u->snapshot(u->cursor,&u->start,u->bytes+LU_HEADER,LU_SNAPSHOT);
		memcpy(u->bytes,"KLOG2\0\0\0",8); lu_le(u->bytes+8,u->boot,8);
		lu_le(u->bytes+16,u->start,8); lu_le(u->bytes+24,u->start+count,8);
		lu_le(u->bytes+32,lu_crc32(u->bytes+LU_HEADER,count),4); lu_le(u->bytes+36,count,4);
		u->length=count+LU_HEADER; u->offset=0; u->reply=LU_UPLOAD_DATA;
		char p[12]; memcpy(p,"DATA",4); lu_hex(p+4,u->length,8); lu_send(u,p,12); return;
	} else if (!strcmp(cmd,"getvar:version")) reply="OKAY0.4";
	else if (!strcmp(cmd,"getvar:product")) reply="OKAYlancelot-xnu";
	else if (!strcmp(cmd,"getvar:serialno")) reply="OKAYlancelot-uefi";
	else if (!strcmp(cmd,"getvar:version-bootloader")) reply="OKAYxnu-musb-log-1";
	else if (!strcmp(cmd,"getvar:log-version")) reply="OKAYKLOG2";
	else if (!strcmp(cmd,"getvar:max-download-size")) reply="OKAY0x00000000";
	lu_send(u,reply,(unsigned)strlen(reply));
}
/* One bounded service step, including EP0 while a bulk snapshot is in flight. */
static void lu_poll(struct lancelot_musb *u)
{
	u->polls++;
	uint8_t usb=LU_R8(LU_INTRUSB); uint16_t tx=LU_R16(LU_INTRTX),rx=LU_R16(LU_INTRRX);
	/* MTK status is W1C, not read-to-clear. */
	LU_W8(LU_INTRUSB,usb); LU_W16(LU_INTRTX,tx); LU_W16(LU_INTRRX,rx);
	if(usb&4){lu_reset(u);return;}
	lu_control_poll(u,(tx&1)!=0);
	LU_W8(LU_INDEX,1);
	if(!u->configuration || u->halt_in || u->halt_out || (LU_R16(LU_TXCSR)&1)) goto done;
	if(u->reply!=LU_NONE){lu_next(u);goto done;}
	if(LU_R16(LU_RXCSR)&1){
		unsigned n=LU_R16(LU_COUNT); char cmd[65];
		if(n>64){LU_W16(LU_RXCSR,0x90);goto done;}
		for(unsigned i=0;i<n;i++)cmd[i]=(char)LU_R8(LU_FIFO1);
		cmd[n]=0; LU_W16(LU_RXCSR,0);
		if(n && strlen(cmd)==n)lu_command(u,cmd,n);
	}
done:
	LU_W8(LU_INDEX,0);
}
#undef LU_R8
#undef LU_R16
#undef LU_W8
#undef LU_W16
#endif
