/*
 * tcpack.{cc,hh} -- provides TCP like acknowledgement service
 * Benjie Chen
 *
 * Copyright (c) 2001 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/confparse.hh>
#include <click/click_ip.h>
#include <click/click_tcp.h>
#include <click/elemfilter.hh>
#include <click/router.hh>
#include <click/error.hh>
#include "tcpbuffer.hh"
#include "tcpack.hh"

TCPAck::TCPAck()
  : Element(2, 3), _timer(this)
{
  MOD_INC_USE_COUNT;
}

TCPAck::~TCPAck()
{
  MOD_DEC_USE_COUNT;
}

int
TCPAck::configure(const Vector<String> &conf, ErrorHandler *errh)
{
  _ackdelay_ms = 20;
  return cp_va_parse(conf, this, errh, 
                     cpOptional, 
		     cpUnsigned, "ack delay (ms)", &_ackdelay_ms, 0);
}


int
TCPAck::initialize(ErrorHandler *errh)
{
  CastElementFilter filter("TCPBuffer");
  Vector<Element*> tcpbuffers;
  
  if (router()->downstream_elements(this, 0, &filter, tcpbuffers) < 0)
    return errh->error("flow-based router context failure");
  if (tcpbuffers.size() < 1) 
    return errh->error
      ("%d downstream elements found, expecting at least 1", tcpbuffers.size());

  for(int i=0; i<tcpbuffers.size(); i++) {
    _tcpbuffer = reinterpret_cast<TCPBuffer*>(tcpbuffers[i]->cast("TCPBuffer"));
    if (_tcpbuffer)
      break;
  }
  if (!_tcpbuffer)
    return errh->error("no TCPBuffer element found!", tcpbuffers[0]->id().cc());

  _synack = false;
  _needack = false;
  _copyhdr = true;
  _timer.initialize(this);
  _timer.schedule_after_ms(_ackdelay_ms);
  return 0;
}

void
TCPAck::uninitialize()
{
}

void
TCPAck::push(int port, Packet *p)
{
  bool forward;
  if (port == 0)
    forward = iput(p);
  else
    forward = oput(p);
  if (forward)
    output(port).push(p);
}

Packet *
TCPAck::pull(int port)
{
  bool forward;
  Packet *p = input(port).pull();
  if (port == 0)
    forward = iput(p);
  else
    forward = oput(p);
  if (forward) 
    return p;
  else {
    p->kill();
    return 0;
  }
}

bool
TCPAck::iput(Packet *p)
{
  const click_ip *iph = p->ip_header();
  const click_tcp *tcph =
    reinterpret_cast<const click_tcp *>(p->transport_header());
  if (!_synack && (tcph->th_flags&(TH_SYN|TH_ACK))==(TH_SYN|TH_ACK)) {
    /* synack on input, meaning next seqn to use is the ackn
     * in packet and next ackn is the seqn in packet */
    _seq_nxt = ntohl(tcph->th_ack);
    _ack_nxt = ntohl(tcph->th_seq)+1;
    _synack = true;
  }
  if (!_synack)
    return false;
  else if (_copyhdr) {
    memmove(&_iph_in, iph, sizeof(click_ip));
    memmove(&_tcph_in, tcph, sizeof(click_tcp));
    _copyhdr = false;
  }

  if (tcph->th_flags & (TH_SYN|TH_FIN|TH_RST))
    return true;

  if (TCPBuffer::seqno(p) == _ack_nxt) {
    _ack_nxt += TCPBuffer::seqlen(p);
    bool v = _tcpbuffer->next_missing_seq_no(_ack_nxt, _ack_nxt);
    assert(v);
  } else {
    click_chatter("seqno < ack_nxt: out of order or retransmitted packet");
  }

  _needack = true;
  if (!_timer.scheduled()) 
    _timer.schedule_after_ms(_ackdelay_ms);
 
  click_chatter("next ack: got %u, ack_nxt %u, seq_nxt %u", 
                TCPBuffer::seqno(p), _ack_nxt, _seq_nxt);
  return true;
}

bool
TCPAck::oput(Packet *p)
{
  const click_tcp *tcph =
    reinterpret_cast<const click_tcp *>(p->transport_header());
  if (tcph->th_flags&(TH_SYN|TH_ACK) == (TH_SYN|TH_ACK)) {
    /* synack on output, meaning next seqn to use is the seqn 
     * in packet and next ackn is the ackn in packet */
    _seq_nxt = ntohl(tcph->th_seq)+1;
    _ack_nxt = ntohl(tcph->th_ack);
    _synack = true;
  }
  if (!_synack)
    return false;
  _seq_nxt = TCPBuffer::seqno(p)+TCPBuffer::seqlen(p);
  _needack = false;
  click_tcp *tcph_new =
    reinterpret_cast<click_tcp *>(p->uniqueify()->transport_header());
  tcph_new->th_ack = htonl(_ack_nxt);
  return true;
}

void
TCPAck::run_scheduled()
{
  if (_needack) {
    send_ack();
    _needack = false;
  }
}

void
TCPAck::send_ack()
{
  struct click_ip *ip;
  struct click_tcp *tcp;
  WritablePacket *q = Packet::make(sizeof(*ip) + sizeof(*tcp));
  if (q == 0) {
    click_chatter("TCPAck: cannot make packet");
    return;
  } 
  memset(q->data(), '\0', q->length());
  ip = (struct click_ip *) q->data();
  tcp = (struct click_tcp *) (ip + 1);
  
  ip->ip_v = 4;
  ip->ip_hl = 5;
  ip->ip_tos = _iph_in.ip_tos;
  ip->ip_len = htons(q->length());
  ip->ip_id = htons(0); // what is this used for exactly?
  ip->ip_off = htons(IP_DF);
  ip->ip_ttl = 255;
  ip->ip_p = IP_PROTO_TCP;
  ip->ip_sum = 0;
  memmove((void *) &(ip->ip_src), (void *) &(_iph_in.ip_dst), 4);
  memmove((void *) &(ip->ip_dst), (void *) &(_iph_in.ip_src), 4);

  tcp->th_sport = _tcph_in.th_dport;
  tcp->th_dport = _tcph_in.th_sport;
  tcp->th_seq = htonl(_seq_nxt);
  tcp->th_ack = htonl(_ack_nxt);
  tcp->th_off = 5;
  tcp->th_flags = TH_ACK;
  tcp->th_win = htons(32120); // when and where should this be set?
  tcp->th_sum = htons(0);
  tcp->th_urp = htons(0);

  output(2).push(q);
}


EXPORT_ELEMENT(TCPAck)

