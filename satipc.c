/*
 * Copyright (C) 2014-2020 Catalin Toda <catalinii@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 */


#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/ca.h>
#include <fcntl.h>
#include <ctype.h>
#include "dvbapi.h"
#include "satipc.h"
#include "ca.h"
#include "minisatip.h"
#include "dvb.h"

extern char *fe_delsys[];
extern struct struct_opts opts;

char servers[100];
uint16_t apid[MAX_ADAPTERS][MAX_PIDS]; // pids to add 
uint16_t dpid[MAX_ADAPTERS][MAX_PIDS]; // pids to delete

int lap[MAX_ADAPTERS], ldp[MAX_ADAPTERS]; // number of pids to add, number of pids to delete
int http_request (adapter *ad, char *url, char *method);

#define SATIPC_ITEM 0x3000000000000
#define MAKE_ITEM(a,b) (SATIPC_ITEM | (a<<24) | (b))
void satipc_commit(adapter *ad);

int satipc_reply(sockets * s)
{
	int rlen = s->rlen;
	adapter *ad;
	char *arg[50], *sess, *es, *sid;
	int la, i;
	s->rlen = 0;
	LOG("satipc_reply (sock %d) handle %d, adapter %d:\n%s", s->id, s->sock, s->sid, s->buf);
	if(!(ad = get_adapter(s->sid)))
		return 0;
	sess = NULL;

	la = split (arg, s->buf, 50, ' ');

        if(map_int(arg[1], NULL) != 200)
                ad->err = 1;
	sid = NULL;
	
	for(i = 0; i< la; i++)
		if(strncasecmp("Session:", arg[i], 8) == 0)
			sess = header_parameter(arg, i);
		else if(strncasecmp("com.ses.streamID:", arg[i], 17) == 0)
			sid = header_parameter(arg, i);
		
	if(!ad->err && !ad->session[0] && sess)
	{
		if(es = strchr(sess, ';'))
			*es = 0;
		strncpy(ad->session, sess, sizeof(ad->session));
		ad->session[sizeof(ad->session) - 1 ] = 0;
		LOG("satipc: session set for adapter %d to %s", ad->id, ad->session);
	}
	
	if(sid && ad->stream_id==-1)
		ad->stream_id = map_int(sid, NULL);
	
	if(ad->wp >= ad->qp)
		ad->expect_reply = 0;
	else 
	{
		char *np = (char *)getItem(MAKE_ITEM(ad->id, ad->wp));
		if(np)
		{
			int len = strlen(np);
			if(ad->session[0] && !strstr(np, "Session:"))
				sprintf(np + len - 2, "Session: %s\r\n\r\n", ad->session );
				
			LOG("satipc_reply: sending next packet:\n%s", np);
			write(s->sock, np, strlen(np));
			delItem(MAKE_ITEM(ad->id, ad->wp++));
		}
	}
	if(!ad->expect_reply && (ad->wp >= ad->qp) && ad->want_commit )  // we do not expect reply and no other events in the queue, we commit a
	{
		satipc_commit(ad);
	}
}

int satipc_timeout(sockets *s)
{
	char buf[500];
	char sess[100];
	adapter *ad = get_adapter(s->sid);
	LOG("satipc: Sent keep-alive to the satip server %s:%d, adapter %d, socket_id %d, handle %d", ad?ad->sip:NULL, ad?ad->sport:0, s->sid, s->id, s->sock);
	if(!ad)
		return 1;
	http_request(ad, NULL, "OPTIONS");
	
	s->rtime = getTick();
	return 0;
}

int satipc_close(sockets * s)
{
	LOG("satip_close called for adapter %d, socket_id %d, handle %d", s->sid, s->id, s->sock);
	close_adapter(s->sid);
}



int satipc_rtcp_reply(sockets * s)
{
	char *b = s->buf, *ver, *tun, *signal;
	int i, rlen = s->rlen;
	adapter *ad = get_adapter(s->sid);
	int strength, status, snr;
	if(!ad)
		return 0;
//	LOG("satip_rtcp_reply called");
	for(i = 0; i< rlen - 4; i++)
		if(b[i]=='v' && b[i+1] == 'e' && b[i+2]=='r' && b[i+3]=='=')
		{
			ver = b + i;
			tun = strstr(ver, "tuner=");
			if(tun)
				signal = strchr(tun, ',');
			if(signal)
			{
				sscanf(signal + 1,"%d,%d,%d", &strength, &status, &snr);
				ad->strength = strength;
				ad->status = status;
				ad->snr = snr;
			}
		}
}


int satipc_open_device(adapter *ad)
{
	int sock;
	int ctime;
	if(!ad->sip)
		return 1;
	
	ctime = getTick();
	if((ad->last_connect > 0) && (ctime - ad->last_connect < 30000))
		return 3;
		
	ad->last_connect = ctime;
	ad->fe = tcp_connect(ad->sip, ad->sport, NULL, 0); // non-blockin socket
	if(ad->fe < 0)
		return 2;
	
	LOG("satipc: connected to SAT>IP server %s port %d, handle %d", ad->sip, ad->sport, ad->fe);
	ad->listen_rtp = opts.start_rtp + 1000 + ad->id*2;
	ad->dvr = udp_bind(NULL, ad->listen_rtp);
	ad->rtcp = udp_bind(NULL, ad->listen_rtp + 1);
	
	ad->fe_sock = sockets_add (ad->fe, NULL, ad->id, TYPE_TCP, (socket_action) satipc_reply, (socket_action) satipc_close, (socket_action )satipc_timeout);
	ad->rtcp_sock = sockets_add (ad->rtcp, NULL, ad->id, TYPE_TCP, (socket_action) satipc_rtcp_reply, (socket_action) satipc_close, NULL);
	sockets_timeout(ad->fe_sock, 25000); // 25s
	set_socket_receive_buffer(ad->dvr, opts.output_buffer);
	if(ad->fe_sock < 0 || ad->dvr < 0 || ad->rtcp<0 || ad->rtcp_sock<0)
	{
		sockets_del(ad->rtcp_sock);
		sockets_del(ad->fe_sock);
		close(ad->rtcp);
		close(ad->dvr);
		close(ad->fe);
	}

	ad->session[0] = 0;
	lap[ad->id] = 0;
	ldp[ad->id] = 0;
	ad->cseq = 1;
	ad->err = 0;
	ad->expect_reply = 0;
	ad->last_connect = 0;
	ad->sent_transport = 0;
	ad->session[0] = 0;
	ad->stream_id = -1;
	ad->wp = ad->qp = ad->want_commit = 0;
	return 0;

}

void satip_close_device(adapter *ad)
{
	http_request(ad, NULL, "TEARDOWN");
	ad->session[0] = 0;
	ad->sent_transport = 0;
}

int satipc_read(int socket, void *buf, int len, sockets *ss)
{	
	char buf1[20];
	struct iovec iov[3] = {{ .iov_base = buf1, .iov_len  = 12}, {.iov_base = buf, .iov_len = len }, {NULL, 0}}; 
	return readv(socket, iov, 2);  // stripping rtp header
}

void satip_post_init(adapter *ad)
{
	sockets_setread(ad->sock, satipc_read);
}

int satipc_set_pid (adapter *ad, uint16_t pid)
{
	int aid = ad->id;
	if(ad->err) // error reported, return error
		return 0;
	LOG("satipc: set_pid for adapter %d, pid %d, err %d", aid, pid, ad->err);
	if(ad->err) // error reported, return error
		return 0;
	apid[aid][lap[aid]] = pid;
	lap[aid]++;
	return aid + 100;
}

int satipc_del_filters (int fd, int pid)
{
	adapter *ad;
	fd -= 100;
	ad = get_adapter(fd);
	if(!ad)
		return 0;
	LOG("satipc: del_pid for aid %d, pid %d, err %d", fd, pid, ad->err);
	if(ad->err) // error reported, return error
		return 0;
	dpid[fd][ldp[fd]] = pid;
	ldp[fd]++;	
	return 0;
}


void get_s2_url(adapter *ad, char *url)
{
#define FILL(req, val, def, met) if((val != def) && (val!=-1)) len +=sprintf(url + len, req, met);
	int len = 0;
	transponder * tp = &ad->tp;
	url[0] = 0;
	FILL("freq=%d", tp->freq, 0, tp->freq / 1000);
	FILL("&pol=%s", tp->pol, -1, get_pol(tp->pol));
	FILL("&sr=%d", tp->sr, -1, tp->sr / 1000);
	FILL("&fec=%s", tp->fec, FEC_AUTO, get_fec(tp->fec));
	FILL("&fe=%d", tp->fe, 0, tp->fe);
	FILL("&src=%d", tp->diseqc, 0, tp->diseqc);
	FILL("&mtype=%s", tp->mod, 0, get_modulation(tp->mod));
	FILL("&msys=%s", tp->sys, 0, get_delsys(tp->sys));
	if(ad->tp.sys == SYS_DVBS2)	
	{
		FILL("&ro=%s", tp->ro, ROLLOFF_AUTO, get_rolloff(tp->ro));
		FILL("&plts=%s", tp->plts, PILOT_AUTO, get_pilot(tp->sys));
	}
	sprintf(url+len, "&pids=");
	return ;
}

void get_c2_url(adapter *ad, char *url)
{
	int len = 0;
	transponder * tp = &ad->tp;
	url[0] = 0;
	FILL("freq=%.1f", tp->freq, 0, tp->freq / 1000.0);
	FILL("&sr=%d", tp->sr, -1, tp->sr / 1000);
	FILL("&bw=%d", tp->bw, BANDWIDTH_AUTO, tp->bw/1000000);
	FILL("&gi=%s", tp->gi, GUARD_INTERVAL_AUTO, get_gi(tp->gi));
	FILL("&fec=%s", tp->fec, FEC_AUTO, get_fec(tp->fec));
	FILL("&tmode=%s", tp->tmode, TRANSMISSION_MODE_AUTO, get_tmode(tp->tmode));
	FILL("&specinv=%d", tp->inversion, INVERSION_AUTO, tp->inversion); 
	FILL("&t2id=%d", tp->t2id, 0, tp->t2id); 
	FILL("&sm=%d", tp->sm, 0, tp->sm); 
	FILL("&plp=%d", tp->plp, 0, tp->plp); 	
	FILL("&mtype=%s", tp->mod, 0, get_modulation(tp->mod));
	FILL("&msys=%s", tp->sys, 0, get_delsys(tp->sys));
	sprintf(url+len, "&pids=");
	return ;
}

void get_t2_url(adapter *ad, char *url)
{
	int len = 0;
	transponder * tp = &ad->tp;
	url[0] = 0;
	FILL("freq=%.1f", tp->freq, 0, tp->freq / 1000.0);
	FILL("&bw=%d", tp->bw, BANDWIDTH_AUTO, tp->bw/1000000);
	FILL("&gi=%s", tp->gi, GUARD_INTERVAL_AUTO, get_gi(tp->gi));
	FILL("&tmode=%s", tp->tmode, TRANSMISSION_MODE_AUTO, get_tmode(tp->tmode));
	FILL("&specinv=%d", tp->inversion, INVERSION_AUTO, tp->inversion); 
	FILL("&c2tft=%d", tp->c2tft, 0, tp->c2tft); 
	FILL("&ds=%d", tp->ds, 0, tp->ds); 
	FILL("&plp=%d", tp->plp, 0, tp->plp); 	
	FILL("&mtype=%s", tp->mod, 0, get_modulation(tp->mod));
	FILL("&msys=%s", tp->sys, 0, get_delsys(tp->sys));
	sprintf(url+len, "&pids=");
	return ;
}


int http_request (adapter *ad, char *url, char *method)
{
	char session[200];
	char buf[2048];
	char sid[40];
	int lb;
	char *qm;
	char format[] = "%s rtsp://%s:%d/%s%s%s RTSP/1.0\r\nCSeq: %d\r\n%s\r\n\r\n";
	session[0] = 0;
	sid[0] = 0;
	int ctime = getTick();

	if(!method && ad->sent_transport == 0)
		method = "SETUP";
		
	if(!method)
		method = "PLAY";
		
	if(ad->sent_transport == 0 && method[0]=='S')
	{
		ad->sent_transport = 1;
		sprintf(session, "Transport:RTP/AVP;unicast;client_port=%d-%d\r\n", ad->listen_rtp, ad->listen_rtp + 1);
	}
	else 
	{
		if(ad->session[0])
			sprintf(session, "Session: %s\r\n", ad->session);
		else session[0] = 0;
	}
	
	if(strcmp(method, "OPTIONS") == 0)
	{
			char *public = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN";
			sprintf(session + strlen(session), "%s\r\n", public);
	}
	qm = "?";
	if(!url)
		qm = "";
	
	if(!url)
		url = "";
	
	if(ad->stream_id != -1)
		sprintf(sid, "stream=%d", ad->stream_id);
	
	lb = snprintf(buf, sizeof(buf), format, method, ad->sip, ad->sport, sid, qm, url, ad->cseq++, session);
	
	LOG("dvbapi_http_request: %s to handle %d: \n%s", ad->expect_reply?"queueing":"sending", ad->fe, buf);
	if(ad->expect_reply)
	{
		setItem(MAKE_ITEM(ad->id,ad->qp++), buf, lb + 1, 0);
	}
	else 
	{
		ad->wp = ad->qp = 0;
		write(ad->fe, buf, lb);
	}
	ad->expect_reply = 1;

}

void tune_url(adapter *ad, char *url)
{
	if(ad->sys[0] == SYS_DVBS2 || ad->sys[0] == SYS_DVBS)
		get_s2_url(ad, url);
	else if(ad->sys[0] == SYS_DVBC2 || ad->sys[0] == SYS_DVBC_ANNEX_A)
		get_c2_url(ad, url);
	else if(	ad->sys[0] == SYS_DVBT2 || ad->sys[0] == SYS_DVBT)
		get_t2_url(ad, url);
}

void satipc_commit(adapter *ad)
{
	char url[400];
	int len = 0, i;
	LOG("satipc: commit for adapter %d pids to add %d, pids to delete %d, expect_reply %d", ad->id, lap[ad->id], ldp[ad->id], ad->expect_reply);

	if(lap[ad->id] + ldp[ad->id] == 0)
		return ;
	
	if(ad->expect_reply)
	{
		ad->want_commit = 1;
		return ;
	}
	
	ad->want_commit = 0;
	if(ad->want_tune)
	{
		tune_url(ad, url);
		len = strlen(url);
	} else if(lap[ad->id] > 0)
		len += sprintf(url + len, "addpids=");
	for ( i = 0; i < lap[ad->id]; i++)
		if(apid[ad->id][i]==8192)
			len += sprintf(url + len, "all,");
		else 
			len += sprintf(url + len, "%d,", apid[ad->id][i]);
	if(len > 0)
		url[len - 1] = '&';		
	
	if(!ad->want_tune)
	{
		if(ldp[ad->id] > 0)
			len += sprintf(url + len, "delpids=");
		for ( i = 0; i < ldp[ad->id]; i++)
		if(dpid[ad->id][i]==8192)
			len += sprintf(url + len, "all,");
		else 
			len += sprintf(url + len, "%d,", dpid[ad->id][i]);	
	}
	url[len - 1] = 0;
		
	http_request(ad, url, NULL);
	lap[ad->id] = 0;
	ldp[ad->id] = 0;
	ad->want_tune = 0;
	return ;
}


int satipc_tune (int aid, transponder * tp)
{
	char url[400];
	adapter *ad = get_adapter(aid);
	LOG("called tune for adapter %d, freq %d, sys %d", aid, ad->tp.freq/1000, ad->tp.sys);
	if(!ad)
		return 1;
	ad->err = 0;	
	ad->strength = 0;
	ad->status = 0;
	ad->snr = 0;
	ad->want_commit = 0;
	ad->want_tune = 1;
	lap[ad->id] = 0;
	ldp[ad->id] = 0;
	if(ad->sent_transport == 0)
	{
		tune_url(ad, url);
		url[strlen(url) - 6] = 0;
//		sprintf(url + strlen(url), "0");
		http_request(ad, url, "SETUP");
		ad->want_tune = 0;
	}
	return 0;
}


fe_delivery_system_t satipc_delsys (int aid, int fd, fe_delivery_system_t *sys)
{
}



void find_satip_adapter(adapter *a)
{
	int i, la, j, k;
	char *sep1, *sep2, *sep;
	char *arg[50];
	if(!opts.satipc)
		return;
	strncpy(servers, opts.satipc, sizeof(servers));
	la = split (arg, servers, 50, ',');
	j = 0;
	for(i=0;i<MAX_ADAPTERS;i++)
		if(a[i].pa == -1 && a[i].fn == -1 && j<la)
		{
			a[i].open = (Open_device )satipc_open_device;
			a[i].set_pid = (Set_pid ) satipc_set_pid;
			a[i].del_filters = (Del_filters )satipc_del_filters;
			a[i].commit = (Adapter_commit )satipc_commit;
			a[i].tune = (Tune )satipc_tune;
			a[i].delsys = (Dvb_delsys )satipc_delsys;
			a[i].post_init = (Adapter_commit )satip_post_init;
			a[i].close = (Adapter_commit )satip_close_device;
			
			sep1 = NULL;
			sep2 = NULL;
			sep1 = strchr(arg[j], ':');
			if(sep1)
				sep2 = strchr(sep1 + 1, ':');
			sep = sep1;
			a[i].sip = arg[j];
			for(k = 0; k < 10; k++)
				a[i].sys[k] = 0;
			if(sep2)
			{
				sep = sep2;
				a[i].sip = sep1 + 1;
				a[i].sys[0] = a[i].tp.sys = map_int(arg[j], fe_delsys);
				if(a[i].sys[0] == SYS_DVBS2)
					a[i].sys[1] = SYS_DVBS;
				if(a[i].sys[0] == SYS_DVBT2)
					a[i].sys[1] = SYS_DVBT;
				if(a[i].sys[0] == SYS_DVBC2)
					a[i].sys[1] = SYS_DVBC_ANNEX_A;
				
			} else
			{
				a[i].sys[0] = SYS_DVBS2;
				a[i].sys[1] = SYS_DVBS;
			
			}
				
			a[i].sport = 554;
			if(sep)
			{
				a[i].sport = map_intd(sep + 1, NULL, 554);
				*sep = 0;
			}			
			j++;
			LOG("Satip device %s port %d delsys %d: %s", a[i].sip, a[i].sport, a[i].sys[0], get_delsys(a[i].sys[0]));
		}
}

