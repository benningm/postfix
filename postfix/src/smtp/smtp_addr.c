/*++
/* NAME
/*	smtp_addr 3
/* SUMMARY
/*	SMTP server address lookup
/* SYNOPSIS
/*	#include "smtp_addr.h"
/*
/*	DNS_RR *smtp_domain_addr(name, misc_flags, why, found_myself)
/*	char	*name;
/*	int	misc_flags;
/*	DSN_VSTRING *why;
/*	int	*found_myself;
/*
/*	DNS_RR *smtp_host_addr(name, misc_flags, why)
/*	char	*name;
/*	int	misc_flags;
/*	DSN_VSTRING *why;
/* DESCRIPTION
/*	This module implements Internet address lookups. By default,
/*	lookups are done via the Internet domain name service (DNS).
/*	A reasonable number of CNAME indirections is permitted. When
/*	DNS lookups are disabled, host address lookup is done with
/*	gethostbyname().
/*
/*	smtp_domain_addr() looks up the network addresses for mail
/*	exchanger hosts listed for the named domain. Addresses are
/*	returned in most-preferred first order. The result is truncated
/*	so that it contains only hosts that are more preferred than the
/*	local mail server itself. The found_myself result parameter
/*	is updated when the local MTA is MX host for the specified
/*	destination.
/*
/*	When no mail exchanger is listed in the DNS for \fIname\fR, the
/*	request is passed to smtp_host_addr().
/*
/*	It is an error to call smtp_domain_addr() when DNS lookups are
/*	disabled.
/*
/*	smtp_host_addr() looks up all addresses listed for the named
/*	host.  The host can be specified as a numerical Internet network
/*	address, or as a symbolic host name.
/*
/*	Results from smtp_domain_addr() or smtp_host_addr() are
/*	destroyed by dns_rr_free(), including null lists.
/* DIAGNOSTICS
/*	Panics: interface violations. For example, calling smtp_domain_addr()
/*	when DNS lookups are explicitly disabled.
/*
/*	All routines either return a DNS_RR pointer, or return a null
/*	pointer and set the \fIsmtp_errno\fR global variable accordingly:
/* .IP SMTP_ERR_RETRY
/*	The request failed due to a soft error, and should be retried later.
/* .IP SMTP_ERR_FAIL
/*	The request attempt failed due to a hard error.
/* .IP SMTP_ERR_LOOP
/*	The local machine is the best mail exchanger.
/* .PP
/*	In addition, a description of the problem is made available
/*	via the \fIwhy\fR argument.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netdb.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Utility library. */

#include <msg.h>
#include <vstring.h>
#include <mymalloc.h>
#include <inet_addr_list.h>
#include <stringops.h>
#include <myaddrinfo.h>
#include <inet_proto.h>

/* Global library. */

#include <mail_params.h>
#include <own_inet_addr.h>
#include <dsn_util.h>

/* DNS library. */

#include <dns.h>

/* Application-specific. */

#include "smtp.h"
#include "smtp_addr.h"

/* smtp_print_addr - print address list */

static void smtp_print_addr(char *what, DNS_RR *addr_list)
{
    DNS_RR *addr;
    MAI_HOSTADDR_STR hostaddr;

    msg_info("begin %s address list", what);
    for (addr = addr_list; addr; addr = addr->next) {
	if (dns_rr_to_pa(addr, &hostaddr) == 0) {
	    msg_warn("skipping record type %s: %m", dns_strtype(addr->type));
	} else {
	    msg_info("pref %4d host %s/%s",
		     addr->pref, addr->name,
		     hostaddr.buf);
	}
    }
    msg_info("end %s address list", what);
}

/* smtp_addr_one - address lookup for one host name */

static DNS_RR *smtp_addr_one(DNS_RR *addr_list, char *host, unsigned pref,
			             DSN_VSTRING *why)
{
    char   *myname = "smtp_addr_one";
    DNS_RR *addr = 0;
    DNS_RR *rr;
    int     aierr;
    struct addrinfo *res0;
    struct addrinfo *res;
    INET_PROTO_INFO *proto_info = inet_proto_info();
    int     found;

    if (msg_verbose)
	msg_info("%s: host %s", myname, host);

    /*
     * Interpret a numerical name as an address.
     */
    if (hostaddr_to_sockaddr(host, (char *) 0, 0, &res0) == 0
     && strchr((char *) proto_info->sa_family_list, res0->ai_family) != 0) {
	if ((addr = dns_sa_to_rr(host, pref, res0->ai_addr)) == 0)
	    msg_fatal("host %s: conversion error for address family %d: %m",
		    host, ((struct sockaddr *) (res0->ai_addr))->sa_family);
	addr_list = dns_rr_append(addr_list, addr);
	freeaddrinfo(res0);
	return (addr_list);
    }

    /*
     * Use DNS lookup, but keep the option open to use native name service.
     */
    if (smtp_host_lookup_mask & SMTP_HOST_FLAG_DNS) {
	switch (dns_lookup_v(host, RES_DEFNAMES, &addr, (VSTRING *) 0,
			     why->vstring, DNS_REQ_FLAG_ALL,
			     proto_info->dns_atype_list)) {
	case DNS_OK:
	    for (rr = addr; rr; rr = rr->next)
		rr->pref = pref;
	    addr_list = dns_rr_append(addr_list, addr);
	    return (addr_list);
	default:
	    dsn_vstring_update(why, "4.4.3", "");
	    smtp_errno = SMTP_ERR_RETRY;
	    return (addr_list);
	case DNS_FAIL:
	    dsn_vstring_update(why, "4.4.3", "");
	    if (smtp_errno != SMTP_ERR_RETRY)
		smtp_errno = SMTP_ERR_FAIL;
	    return (addr_list);
	case DNS_NOTFOUND:
	    dsn_vstring_update(why, "4.4.4", "");
	    if (smtp_errno != SMTP_ERR_RETRY)
		smtp_errno = SMTP_ERR_FAIL;
	    /* maybe native naming service will succeed */
	    break;
	}
    }

    /*
     * Use the native name service which also looks in /etc/hosts.
     */
#define RETRY_AI_ERROR(e) \
        ((e) == EAI_AGAIN || (e) == EAI_MEMORY || (e) == EAI_SYSTEM)
#ifdef EAI_NODATA
#define DSN_NOHOST(e) \
	((e) == EAI_AGAIN || (e) == EAI_NODATA || (e) == EAI_NONAME)
#else
#define DSN_NOHOST(e) \
	((e) == EAI_AGAIN || (e) == EAI_NONAME)
#endif

    if (smtp_host_lookup_mask & SMTP_HOST_FLAG_NATIVE) {
	if ((aierr = hostname_to_sockaddr(host, (char *) 0, 0, &res0)) != 0) {
	    dsn_vstring_update(why, DSN_NOHOST(aierr) ? "4.4.4" : "4.3.0",
			       "unable to look up host %s: %s",
			       host, MAI_STRERROR(aierr));
	    if (smtp_errno != SMTP_ERR_RETRY)
		smtp_errno =
		    (RETRY_AI_ERROR(aierr) ? SMTP_ERR_RETRY : SMTP_ERR_FAIL);
	} else {
	    for (found = 0, res = res0; res != 0; res = res->ai_next) {
		if (strchr((char *) proto_info->sa_family_list, res->ai_family) == 0) {
		    msg_info("skipping address family %d for host %s",
			     res->ai_family, host);
		    continue;
		}
		found++;
		if ((addr = dns_sa_to_rr(host, pref, res->ai_addr)) == 0)
		    msg_fatal("host %s: conversion error for address family %d: %m",
		    host, ((struct sockaddr *) (res0->ai_addr))->sa_family);
		addr_list = dns_rr_append(addr_list, addr);
	    }
	    freeaddrinfo(res0);
	    if (found == 0) {
		dsn_vstring_update(why, "5.4.4", "%s: host not found", host);
		if (smtp_errno != SMTP_ERR_RETRY)
		    smtp_errno = SMTP_ERR_FAIL;
	    }
	    return (addr_list);
	}
    }

    /*
     * No further alternatives for host lookup.
     */
    return (addr_list);
}

/* smtp_addr_list - address lookup for a list of mail exchangers */

static DNS_RR *smtp_addr_list(DNS_RR *mx_names, DSN_VSTRING *why)
{
    DNS_RR *addr_list = 0;
    DNS_RR *rr;

    /*
     * As long as we are able to look up any host address, we ignore problems
     * with DNS lookups (except if we're backup MX, and all the better MX
     * hosts can't be found).
     * 
     * XXX 2821: update smtp_errno (0->FAIL upon unrecoverable lookup error,
     * any->RETRY upon temporary lookup error) so that we can correctly
     * handle the case of no resolvable MX host. Currently this is always
     * treated as a soft error. RFC 2821 wants a more precise response.
     */
    for (rr = mx_names; rr; rr = rr->next) {
	if (rr->type != T_MX)
	    msg_panic("smtp_addr_list: bad resource type: %d", rr->type);
	addr_list = smtp_addr_one(addr_list, (char *) rr->data, rr->pref, why);
    }
    return (addr_list);
}

/* smtp_find_self - spot myself in a crowd of mail exchangers */

static DNS_RR *smtp_find_self(DNS_RR *addr_list)
{
    char   *myname = "smtp_find_self";
    INET_ADDR_LIST *self;
    INET_ADDR_LIST *proxy;
    DNS_RR *addr;
    int     i;

    self = own_inet_addr_list();
    proxy = proxy_inet_addr_list();

    for (addr = addr_list; addr; addr = addr->next) {

	/*
	 * Find out if this mail system is listening on this address.
	 */
	for (i = 0; i < self->used; i++)
	    if (DNS_RR_EQ_SA(addr, (struct sockaddr *) (self->addrs + i))) {
		if (msg_verbose)
		    msg_info("%s: found self at pref %d", myname, addr->pref);
		return (addr);
	    }

	/*
	 * Find out if this mail system has a proxy listening on this
	 * address.
	 */
	for (i = 0; i < proxy->used; i++)
	    if (DNS_RR_EQ_SA(addr, (struct sockaddr *) (proxy->addrs + i))) {
		if (msg_verbose)
		    msg_info("%s: found proxy at pref %d", myname, addr->pref);
		return (addr);
	    }
    }

    /*
     * Didn't find myself, or my proxy.
     */
    if (msg_verbose)
	msg_info("%s: not found", myname);
    return (0);
}

/* smtp_truncate_self - truncate address list at self and equivalents */

static DNS_RR *smtp_truncate_self(DNS_RR *addr_list, unsigned pref)
{
    DNS_RR *addr;
    DNS_RR *last;

    for (last = 0, addr = addr_list; addr; last = addr, addr = addr->next) {
	if (pref == addr->pref) {
	    if (msg_verbose)
		smtp_print_addr("truncated", addr);
	    dns_rr_free(addr);
	    if (last == 0) {
		addr_list = 0;
	    } else {
		last->next = 0;
	    }
	    break;
	}
    }
    return (addr_list);
}

/* smtp_compare_pref - compare resource records by preference */

static int smtp_compare_pref(DNS_RR *a, DNS_RR *b)
{
    if (a->pref != b->pref)
	return (a->pref - b->pref);
#ifdef HAS_IPV6
    if (a->type == b->type)			/* 200412 */
	return 0;
    if (a->type == T_AAAA)
	return (-1);
    if (b->type == T_AAAA)
	return (+1);
#endif
    return 0;
}

/* smtp_domain_addr - mail exchanger address lookup */

DNS_RR *smtp_domain_addr(char *name, int misc_flags, DSN_VSTRING *why,
			         int *found_myself)
{
    DNS_RR *mx_names;
    DNS_RR *addr_list = 0;
    DNS_RR *self = 0;
    unsigned best_pref;
    unsigned best_found;

    smtp_errno = SMTP_ERR_NONE;			/* Paranoia */

    /*
     * Preferences from DNS use 0..32767, fall-backs use 32768+.
     */
#define IMPOSSIBLE_PREFERENCE	(~0)

    /*
     * Sanity check.
     */
    if (var_disable_dns)
	msg_panic("smtp_domain_addr: DNS lookup is disabled");

    /*
     * Look up the mail exchanger hosts listed for this name. Sort the
     * results by preference. Look up the corresponding host addresses, and
     * truncate the list so that it contains only hosts that are more
     * preferred than myself. When no MX resource records exist, look up the
     * addresses listed for this name.
     * 
     * According to RFC 974: "It is possible that the list of MXs in the
     * response to the query will be empty.  This is a special case.  If the
     * list is empty, mailers should treat it as if it contained one RR, an
     * MX RR with a preference value of 0, and a host name of REMOTE.  (I.e.,
     * REMOTE is its only MX).  In addition, the mailer should do no further
     * processing on the list, but should attempt to deliver the message to
     * REMOTE."
     * 
     * Normally it is OK if an MX host cannot be found in the DNS; we'll just
     * use a backup one, and silently ignore the better MX host. However, if
     * the best backup that we can find in the DNS is the local machine, then
     * we must remember that the local machine is not the primary MX host, or
     * else we will claim that mail loops back.
     * 
     * XXX Optionally do A lookups even when the MX lookup didn't complete.
     * Unfortunately with some DNS servers this is not a transient problem.
     * 
     * XXX Ideally we would perform A lookups only as far as needed. But as long
     * as we're looking up all the hosts, it would be better to look up the
     * least preferred host first, so that DNS lookup error messages make
     * more sense.
     * 
     * XXX 2821: RFC 2821 says that the sender must shuffle equal-preference MX
     * hosts, whereas multiple A records per hostname must be used in the
     * order as received. They make the bogus assumption that a hostname with
     * multiple A records corresponds to one machine with multiple network
     * interfaces.
     * 
     * XXX 2821: Postfix recognizes the local machine by looking for its own IP
     * address in the list of mail exchangers. RFC 2821 says one has to look
     * at the mail exchanger hostname as well, making the bogus assumption
     * that an IP address is listed only under one hostname. However, looking
     * at hostnames provides a partial solution for MX hosts behind a NAT
     * gateway.
     */
    switch (dns_lookup(name, T_MX, 0, &mx_names, (VSTRING *) 0, why->vstring)) {
    default:
	dsn_vstring_update(why, "4.4.3", "");
	smtp_errno = SMTP_ERR_RETRY;
	if (var_ign_mx_lookup_err)
	    addr_list = smtp_host_addr(name, misc_flags, why);
	break;
    case DNS_FAIL:
	dsn_vstring_update(why, "5.4.3", "");
	smtp_errno = SMTP_ERR_FAIL;
	if (var_ign_mx_lookup_err)
	    addr_list = smtp_host_addr(name, misc_flags, why);
	break;
    case DNS_OK:
	mx_names = dns_rr_sort(mx_names, smtp_compare_pref);
	best_pref = (mx_names ? mx_names->pref : IMPOSSIBLE_PREFERENCE);
	addr_list = smtp_addr_list(mx_names, why);
	dns_rr_free(mx_names);
	if (addr_list == 0) {
	    /* DSN and text does not change. */
	    if (var_smtp_defer_mxaddr)
		smtp_errno = SMTP_ERR_RETRY;
	    msg_warn("no MX host for %s has a valid address record", name);
	    break;
	}
	best_found = (addr_list ? addr_list->pref : IMPOSSIBLE_PREFERENCE);
	if (msg_verbose)
	    smtp_print_addr(name, addr_list);
	if ((misc_flags & SMTP_MISC_FLAG_LOOP_DETECT)
	    && (self = smtp_find_self(addr_list)) != 0) {
	    addr_list = smtp_truncate_self(addr_list, self->pref);
	    if (addr_list == 0) {
		if (best_pref != best_found) {
		    dsn_vstring_update(why, "4.4.4",
				       "unable to find primary relay for %s",
				       name);
		    smtp_errno = SMTP_ERR_RETRY;
		} else {
		    dsn_vstring_update(why, "5.3.5",
				       "mail for %s loops back to myself",
				       name);
		    smtp_errno = SMTP_ERR_LOOP;
		}
	    }
	}
	if (addr_list && addr_list->next && var_smtp_rand_addr) {
	    addr_list = dns_rr_shuffle(addr_list);
	    addr_list = dns_rr_sort(addr_list, smtp_compare_pref);
	}
	break;
    case DNS_NOTFOUND:
	addr_list = smtp_host_addr(name, misc_flags, why);
	break;
    }

    /*
     * Clean up.
     */
    *found_myself |= (self != 0);
    return (addr_list);
}

/* smtp_host_addr - direct host lookup */

DNS_RR *smtp_host_addr(char *host, int misc_flags, DSN_VSTRING *why)
{
    DNS_RR *addr_list;

    smtp_errno = SMTP_ERR_NONE;			/* Paranoia */

    /*
     * If the host is specified by numerical address, just convert the
     * address to internal form. Otherwise, the host is specified by name.
     */
#define PREF0	0
    addr_list = smtp_addr_one((DNS_RR *) 0, host, PREF0, why);
    if (addr_list
	&& (misc_flags & SMTP_MISC_FLAG_LOOP_DETECT)
	&& smtp_find_self(addr_list) != 0) {
	dns_rr_free(addr_list);
	dsn_vstring_update(why, "5.3.5",
			   "mail for %s loops back to myself", host);
	smtp_errno = SMTP_ERR_LOOP;
	return (0);
    }
    if (addr_list && addr_list->next) {
	if (var_smtp_rand_addr)
	    addr_list = dns_rr_shuffle(addr_list);
	/* The following changes the order of equal-preference hosts. */
	if (inet_proto_info()->ai_family_list[1] != 0)
	    addr_list = dns_rr_sort(addr_list, smtp_compare_pref);
    }
    if (msg_verbose)
	smtp_print_addr(host, addr_list);
    return (addr_list);
}
