#ifndef DISABLE_DVBCSA
#ifndef DVBAPI_H
#define DVBAPI_H

#include "adapter.h"
#include "stream.h"
#include <dvbcsa/dvbcsa.h>


#define DVBAPI_PROTOCOL_VERSION         1

#define DVBAPI_FILTER_DATA     0xFFFF0000
#define DVBAPI_CLIENT_INFO     0xFFFF0001
#define DVBAPI_SERVER_INFO     0xFFFF0002
#define DVBAPI_DMX_SET_FILTER  	   0x403c6f2b
#define DVBAPI_CA_SET_PID      0x40086f87
#define DVBAPI_CA_SET_DESCR    0x40106f86
#define DVBAPI_DMX_STOP        0x00006f2a



#define AOT_CA_PMT 0x9F803282

#define CAPMT_LIST_MORE    0x00    // append a 'MORE' CAPMT object the list and start receiving the next object
#define CAPMT_LIST_FIRST   0x01    // clear the list when a 'FIRST' CAPMT object is received, and start receiving the next object
#define CAPMT_LIST_LAST    0x02    // append a 'LAST' CAPMT object to the list and start working with the list
#define CAPMT_LIST_ONLY    0x03    // clear the list when an 'ONLY' CAPMT object is received, and start working with the object
#define CAPMT_LIST_ADD     0x04    // append an 'ADD' CAPMT object to the current list and start working with the updated list
#define CAPMT_LIST_UPDATE  0x05    // replace an entry in the list with an 'UPDATE' CAPMT object, and start working with the updated list


#define MAX_KEYS 255
#define MAX_PMT_DATA 1880
typedef struct struct_key
{
	void *key[2];
	int key_ok[2];
	int sid;
	int pmt_pid;
	int enabled;
	int id;
	int adapter;
	int demux;
	int ver;
	unsigned char *pi;
	int pi_len;
	struct dvbcsa_bs_batch_s batch[129];
	int parity;
	int blen;
	int enabled_channels;
	int last_parity_change;
	struct struct_key *next_key;
} SKey;

int init_dvbapi();
int have_dvbapi();
int dvbapi_enabled();
int send_ecm(unsigned char *b, adapter *ad);
int batch_size();
int decrypt_stream(adapter *ad,int rlen);
int keys_add(int adapter, int sid, int pmt);
int keys_del(int i);
SKey *get_key(int i);
int dvbapi_process_pmt(unsigned char *b, adapter *ad);
void dvbapi_pid_add(adapter *a,int pid, SPid *cp);
void dvbapi_pid_del(adapter *a,int pid, SPid *cp);

#endif
#endif
