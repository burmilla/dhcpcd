/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Privilege Separation BPF Initiator
 * Copyright (c) 2006-2020 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/socket.h>
#include <sys/types.h>

/* Need these headers just for if_ether on some OS. */
#ifndef __NetBSD__
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#endif
#include <netinet/if_ether.h>

#include <assert.h>
#include <pwd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arp.h"
#include "bpf.h"
#include "dhcp.h"
#include "dhcp6.h"
#include "eloop.h"
#include "ipv6nd.h"
#include "logerr.h"
#include "privsep.h"

#ifdef HAVE_CAPSICUM
#include <sys/capsicum.h>
#endif

static void
ps_bpf_recvbpf(void *arg)
{
	struct ps_process *psp = arg;
	struct bpf *bpf = psp->psp_bpf;
	uint8_t buf[sizeof(bpf->bpf_flags) + FRAMELEN_MAX];
	ssize_t len;
	struct ps_msghdr psm = {
		.ps_id = psp->psp_id,
		.ps_cmd = psp->psp_id.psi_cmd,
	};

	bpf->bpf_flags &= ~BPF_EOF;
	/* A BPF read can read more than one filtered packet at time.
	 * This mechanism allows us to read each packet from the buffer. */
	while (!(bpf->bpf_flags & BPF_EOF)) {
		len = bpf_read(bpf,
		    buf + sizeof(bpf->bpf_flags),
		    sizeof(buf) - sizeof(bpf->bpf_flags));
		if (len == -1)
			logerr(__func__);
		if (len == -1 || len == 0)
			break;
		memcpy(buf, &bpf->bpf_flags, sizeof(bpf->bpf_flags));
		len = ps_sendpsmdata(psp->psp_ctx, psp->psp_ctx->ps_data_fd,
		    &psm, buf, (size_t)len + sizeof(bpf->bpf_flags));
		if (len == -1 && errno != ECONNRESET)
			logerr(__func__);
		if (len == -1 || len == 0)
			break;
	}
}

static ssize_t
ps_bpf_recvmsgcb(void *arg, struct ps_msghdr *psm, struct msghdr *msg)
{
	struct ps_process *psp = arg;
	struct iovec *iov = msg->msg_iov;

#ifdef PRIVSEP_DEBUG
	logerrx("%s: IN cmd %x, psp %p", __func__, psm->ps_cmd, psp);
#endif

	switch(psm->ps_cmd) {
#ifdef ARP
	case PS_BPF_ARP:	/* FALLTHROUGH */
#endif
	case PS_BPF_BOOTP:
		break;
	default:
		/* IPC failure, we should not be processing any commands
		 * at this point!/ */
		errno = EINVAL;
		return -1;
	}

	return bpf_send(psp->psp_bpf, psp->psp_proto,
	    iov->iov_base, iov->iov_len);
}

static void
ps_bpf_recvmsg(void *arg)
{
	struct ps_process *psp = arg;

	if (ps_recvpsmsg(psp->psp_ctx, psp->psp_fd,
	    ps_bpf_recvmsgcb, arg) == -1)
		logerr(__func__);
}

static int
ps_bpf_start_bpf(void *arg)
{
	struct ps_process *psp = arg;
	struct dhcpcd_ctx *ctx = psp->psp_ctx;
	char *addr;
	struct in_addr *ia = &psp->psp_id.psi_addr.psa_in_addr;
#ifdef HAVE_CAPSICUM
	cap_rights_t rights;

	/* We need CAP_IOCTL so we can change the BPF filter when we
	 * need to. */
	cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_EVENT, CAP_IOCTL);
#endif

	if (ia->s_addr == INADDR_ANY) {
		ia = NULL;
		addr = NULL;
	} else
		addr = inet_ntoa(*ia);
	setproctitle("[BPF %s] %s%s%s", psp->psp_protostr, psp->psp_ifname,
	    addr != NULL ? " " : "", addr != NULL ? addr : "");
	ps_freeprocesses(ctx, psp);

	psp->psp_bpf = bpf_open(&psp->psp_ifp, psp->psp_filter, ia);
	if (psp->psp_bpf == NULL)
		logerr("%s: bpf_open",__func__);
#ifdef HAVE_CAPSICUM
	else if (cap_rights_limit(psp->psp_bpf->bpf_fd, &rights) == -1 &&
	    errno != ENOSYS)
		logerr("%s: cap_rights_limit", __func__);
#endif
	else if (eloop_event_add(ctx->eloop,
	    psp->psp_bpf->bpf_fd, ps_bpf_recvbpf, psp) == -1)
		logerr("%s: eloop_event_add", __func__);
	else {
		psp->psp_work_fd = psp->psp_bpf->bpf_fd;
		return 0;
	}

	eloop_exit(ctx->eloop, EXIT_FAILURE);
	return -1;
}

static void
ps_bpf_signal_bpfcb(int sig, void *arg)
{
	struct dhcpcd_ctx *ctx = arg;

	eloop_exit(ctx->eloop, sig == SIGTERM ? EXIT_SUCCESS : EXIT_FAILURE);
}

ssize_t
ps_bpf_cmd(struct dhcpcd_ctx *ctx, struct ps_msghdr *psm, struct msghdr *msg)
{
	uint16_t cmd;
	struct ps_process *psp;
	pid_t start;
	struct iovec *iov = msg->msg_iov;
	struct interface *ifp;

	cmd = (uint16_t)(psm->ps_cmd & ~(PS_START | PS_STOP));
	psp = ps_findprocess(ctx, &psm->ps_id);

#ifdef PRIVSEP_DEBUG
	logerrx("%s: IN cmd %x, psp %p", __func__, psm->ps_cmd, psp);
#endif

	switch (cmd) {
#ifdef ARP
	case PS_BPF_ARP:	/* FALLTHROUGH */
#endif
	case PS_BPF_BOOTP:
		break;
	default:
		logerrx("%s: unknown command %x", __func__, psm->ps_cmd);
		errno = ENOTSUP;
		return -1;
	}

	if (!(psm->ps_cmd & PS_START)) {
		errno = EINVAL;
		return -1;
	}

	if (psp != NULL)
		return 1;

	psp = ps_newprocess(ctx, &psm->ps_id);
	if (psp == NULL)
		return -1;

	ifp = &psp->psp_ifp;
	assert(msg->msg_iovlen == 1);
	assert(iov->iov_len == sizeof(*ifp));
	memcpy(ifp, iov->iov_base, sizeof(*ifp));
	ifp->ctx = psp->psp_ctx;
	ifp->options = NULL;
	memset(ifp->if_data, 0, sizeof(ifp->if_data));

	memcpy(psp->psp_ifname, ifp->name, sizeof(psp->psp_ifname));

	switch (cmd) {
#ifdef ARP
	case PS_BPF_ARP:
		psp->psp_proto = ETHERTYPE_ARP;
		psp->psp_protostr = "ARP";
		psp->psp_filter = bpf_arp;
		break;
#endif
	case PS_BPF_BOOTP:
		psp->psp_proto = ETHERTYPE_IP;
		psp->psp_protostr = "BOOTP";
		psp->psp_filter = bpf_bootp;
		break;
	}

	start = ps_dostart(ctx,
	    &psp->psp_pid, &psp->psp_fd,
	    ps_bpf_recvmsg, NULL, psp,
	    ps_bpf_start_bpf, ps_bpf_signal_bpfcb,
	    PSF_DROPPRIVS);
	switch (start) {
	case -1:
		ps_freeprocess(psp);
		return -1;
	case 0:
#ifdef HAVE_CAPSICUM
		if (cap_enter() == -1 && errno != ENOSYS)
			logerr("%s: cap_enter", __func__);
#endif
#ifdef HAVE_PLEDGE
		if (pledge("stdio", NULL) == -1)
			logerr("%s: pledge", __func__);
#endif
		break;
	default:
#ifdef PRIVSEP_DEBUG
		logdebugx("%s: spawned BPF %s on PID %d",
		    psp->psp_ifname, psp->psp_protostr, start);
#endif
		break;
	}
	return start;
}

ssize_t
ps_bpf_dispatch(struct dhcpcd_ctx *ctx,
    struct ps_msghdr *psm, struct msghdr *msg)
{
	struct iovec *iov = msg->msg_iov;
	struct interface *ifp;
	uint8_t *bpf;
	size_t bpf_len;
	unsigned int bpf_flags;

	ifp = if_findindex(ctx->ifaces, psm->ps_id.psi_ifindex);
	bpf = iov->iov_base;
	bpf_len = iov->iov_len;
	memcpy(&bpf_flags, bpf, sizeof(bpf_flags));
	bpf += sizeof(bpf_flags);
	bpf_len -= sizeof(bpf_flags);

	switch (psm->ps_cmd) {
#ifdef ARP
	case PS_BPF_ARP:
		arp_packet(ifp, bpf, bpf_len, bpf_flags);
		break;
#endif
	case PS_BPF_BOOTP:
		dhcp_packet(ifp, bpf, bpf_len, bpf_flags);
		break;
	default:
		errno = ENOTSUP;
		return -1;
	}

	return 1;
}

static ssize_t
ps_bpf_send(const struct interface *ifp, const struct in_addr *ia,
    uint16_t cmd, const void *data, size_t len)
{
	struct dhcpcd_ctx *ctx = ifp->ctx;
	struct ps_msghdr psm = {
		.ps_cmd = cmd,
		.ps_id = {
			.psi_ifindex = ifp->index,
			.psi_cmd = (uint8_t)(cmd & ~(PS_START | PS_STOP)),
		},
	};

	if (ia != NULL)
		psm.ps_id.psi_addr.psa_in_addr = *ia;

	return ps_sendpsmdata(ctx, ctx->ps_root_fd, &psm, data, len);
}

#ifdef ARP
ssize_t
ps_bpf_openarp(const struct interface *ifp, const struct in_addr *ia)
{

	assert(ia != NULL);
	return ps_bpf_send(ifp, ia, PS_BPF_ARP | PS_START,
	    ifp, sizeof(*ifp));
}

ssize_t
ps_bpf_closearp(const struct interface *ifp, const struct in_addr *ia)
{

	return ps_bpf_send(ifp, ia, PS_BPF_ARP | PS_STOP, NULL, 0);
}

ssize_t
ps_bpf_sendarp(const struct interface *ifp, const struct in_addr *ia,
    const void *data, size_t len)
{

	assert(ia != NULL);
	return ps_bpf_send(ifp, ia, PS_BPF_ARP, data, len);
}
#endif

ssize_t
ps_bpf_openbootp(const struct interface *ifp)
{

	return ps_bpf_send(ifp, NULL, PS_BPF_BOOTP | PS_START,
	    ifp, sizeof(*ifp));
}

ssize_t
ps_bpf_closebootp(const struct interface *ifp)
{

	return ps_bpf_send(ifp, NULL, PS_BPF_BOOTP | PS_STOP, NULL, 0);
}

ssize_t
ps_bpf_sendbootp(const struct interface *ifp, const void *data, size_t len)
{

	return ps_bpf_send(ifp, NULL, PS_BPF_BOOTP, data, len);
}