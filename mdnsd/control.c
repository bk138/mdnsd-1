/*
 * Copyright (c) 2010 Christiano F. Haesbaert <haesbaert@haesbaert.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mdnsd.h"
#include "mdns.h"
#include "log.h"
#include "control.h"

#define	CONTROL_BACKLOG	5

struct ctl_conn	*control_connbyfd(int);
struct ctl_conn	*control_connbypid(pid_t);
void		 control_close(int);
void		 control_lookup(struct ctl_conn *, struct imsg *);
void		 control_browse_add(struct ctl_conn *, struct imsg *);
void		 control_browse_del(struct ctl_conn *, struct imsg *);
void		 control_resolve(struct ctl_conn *, struct imsg *);

void
control_lookup(struct ctl_conn *c, struct imsg *imsg)
{
	struct rrset	 mlkup;
	struct rr	*rr;
	struct query 	*q;
	struct timeval	 tv;

	if ((imsg->hdr.len - IMSG_HEADER_SIZE) != sizeof(mlkup))
		return;

	memcpy(&mlkup, imsg->data, sizeof(mlkup));
	mlkup.dname[MAXHOSTNAMELEN - 1] = '\0'; /* assure clients are nice */

	switch (mlkup.type) {
	case T_A:		/* FALLTHROUGH */
	case T_HINFO:		/* FALLTHROUGH */
	case T_PTR:		/* FALLTHROUGH */
	case T_SRV:		/* FALLTHROUGH */
	case T_TXT:		/* FALLTHROUGH */
		break;
	default:
		log_warnx("Lookup type %d not supported/implemented",
		    mlkup.type);
		return;
	}

	if (mlkup.class != C_IN) {
		log_warnx("Lookup class %d not supported/implemented",
		    mlkup.class);
		return;
	}
	
	/* Check if control has this query already, if so don't do anything */
	LIST_FOREACH(q, &c->qlist, entry) {
		if (q->style != QUERY_LOOKUP)
			continue;
		LIST_FOREACH(rr, &q->rrlist, qentry)
		    if (rrset_cmp(&rr->rrs, &mlkup) == 0) {
			    log_debug("control already querying for %s",
				rrs_str(&rr->rrs));
			    return;
		    }
	}

	log_debug("looking up %s (%s %d)", mlkup.dname, rr_type_name(mlkup.type),
	    mlkup.class);

	rr = cache_lookup(&mlkup);
	/* cache hit */
	if (rr != NULL) {
		if (control_send_rr(c, rr, IMSG_CTL_LOOKUP) == -1)
			log_warnx("query_answer error");
		return;
	}

	if (question_add(&mlkup) == NULL) {
		log_warnx("Can't add question for %s (%s)", rr->rrs.dname,
		    rr_type_name(rr->rrs.type));
		return;
	}
	
	/* cache miss */
	if ((q = calloc(1, sizeof(*q))) == NULL)
		fatal("calloc");
	if ((rr = calloc(1, sizeof(*rr))) == NULL)
		fatal("calloc");
	LIST_INIT(&q->rrlist);
	q->style = QUERY_LOOKUP;
	q->ctl = c;
	rr->rrs = mlkup;
	LIST_INSERT_HEAD(&q->rrlist, rr, qentry);
	LIST_INSERT_HEAD(&c->qlist, q, entry);
	timerclear(&tv);
	tv.tv_usec = FIRST_QUERYTIME;
	evtimer_set(&q->timer, query_fsm, q);
	evtimer_add(&q->timer, &tv);
}

void
control_browse_add(struct ctl_conn *c, struct imsg *imsg)
{
	struct rrset	 mlkup;
	struct rr	*rr;
	struct query 	*q;
	struct timeval	 tv;

	if ((imsg->hdr.len - IMSG_HEADER_SIZE) != sizeof(mlkup))
		return;

	memcpy(&mlkup, imsg->data, sizeof(mlkup));
	mlkup.dname[MAXHOSTNAMELEN - 1] = '\0'; /* assure clients were nice */

	if (mlkup.type != T_PTR) {
		log_warnx("Browse type %d not supported/implemented",
		    mlkup.type);
		return;
	}

	if (mlkup.class != C_IN) {
		log_warnx("Browse class %d not supported/implemented",
		    mlkup.class);
		return;
	}
	
	/* Check if control has this query already, if so don't do anything */
	LIST_FOREACH(q, &c->qlist, entry) {
		if (q->style != QUERY_BROWSE)
			continue;
		LIST_FOREACH(rr, &q->rrlist, qentry)
		    if (rrset_cmp(&rr->rrs, &mlkup) == 0) {
			    log_debug("control already querying for %s",
				rrs_str(&rr->rrs));
			    return;
		    }
	}
	
	log_debug("Browse add %s (%s %d)", mlkup.dname, rr_type_name(mlkup.type),
	    mlkup.class);

	rr = cache_lookup(&mlkup);
	while (rr != NULL) {
		if (control_send_rr(c, rr, IMSG_CTL_BROWSE_ADD) == -1)
			log_warnx("control_send_rr error");
		rr = LIST_NEXT(rr, centry);
	}

	if (question_add(&mlkup) == NULL) {
		log_warnx("Can't add question for %s (%s)", rr->rrs.dname,
		    rr_type_name(rr->rrs.type));
		return;
	}
	
	if ((q = calloc(1, sizeof(*q))) == NULL)
		fatal("calloc");
	if ((rr = calloc(1, sizeof(*rr))) == NULL)
		fatal("calloc");
	LIST_INIT(&q->rrlist);
	q->style = QUERY_BROWSE;
	q->ctl = c;
	rr->rrs = mlkup;
	LIST_INSERT_HEAD(&q->rrlist, rr, qentry);
	LIST_INSERT_HEAD(&c->qlist, q, entry);
	timerclear(&tv);
	tv.tv_usec = FIRST_QUERYTIME;
	evtimer_set(&q->timer, query_fsm, q);
	evtimer_add(&q->timer, &tv);
}

void
control_browse_del(struct ctl_conn *c, struct imsg *imsg)
{
/* 	struct rrset	 mlkup; */
/* 	struct query 	*q; */

/* 	if ((imsg->hdr.len - IMSG_HEADER_SIZE) != sizeof(mlkup)) */
/* 		return; */

/* 	memcpy(&mlkup, imsg->data, sizeof(mlkup)); */
/* 	mlkup.dname[MAXHOSTNAMELEN - 1] = '\0'; /\* assure clients were nice *\/ */

/* 	if (mlkup.type != T_PTR) { */
/* 		log_warnx("Browse type %d not supported/implemented", */
/* 		    mlkup.type); */
/* 		return; */
/* 	} */

/* 	if (mlkup.class != C_IN) { */
/* 		log_warnx("Browse class %d not supported/implemented", */
/* 		    mlkup.class); */
/* 		return; */
/* 	} */
/* 	q = query_lookup(&mlkup); */
/* 	if (q != NULL) */
/* 		control_remq(c, q); */
}	

void
control_resolve(struct ctl_conn *c, struct imsg *imsg)
{
	struct mdns_service	 ms;
	char			 msg[MAXHOSTNAMELEN];
	struct rrset		 rrs;
	struct rr		*srv, *txt, *a, *rr;
	struct rr		*srv_cache, *txt_cache, *a_cache;
	struct query		*q;
	struct timeval		 tv;
	
	srv = txt = a = rr = NULL;
	srv_cache = txt_cache = a_cache = NULL;
	q = NULL;

	if ((imsg->hdr.len - IMSG_HEADER_SIZE) != sizeof(msg)) {
		log_warnx("control_resolve: Invalid msg len");
		return;
	}

	memcpy(msg, imsg->data, sizeof(msg));
	msg[sizeof(msg) - 1] = '\0';
	
	/* Check if control has this query already, if so don't do anything */
/* 	LIST_FOREACH(q, &c->qlist, entry) { */
/* 		if (q->style != QUERY_RESOLVE) */
/* 			continue; */
/* 		/\* TODO: use query_to_ms *\/ */
/* 		LIST_FOREACH(rr, &q->rrlist, qentry) { */
/* 			/\* compare the SRV only *\/ */
/* 			if (rr->rrs.type != T_SRV) */
/* 				continue; */
/* 			if (strcmp(msg, rr->rrs.dname) == 0) { */
/* 				log_debug("control already resolving %s", */
/* 				    rr->rrs.dname); */
/* 				return; */
/* 			} */
/* 		} */
/* 	} */
	
	log_debug("Resolve %s", msg);
	if (strlcpy(rrs.dname, msg, sizeof(rrs.dname)) >= sizeof(rrs.dname)) {
		log_warnx("control_resolve: msg too long, dropping");
		return;
	}

	/*
	 * Check what we have in cache.
	 */
	rrs.class = C_IN;
	rrs.type = T_SRV;
	srv_cache = cache_lookup(&rrs);
	rrs.type = T_TXT;
	txt_cache = cache_lookup(&rrs);
	
	/*
	 * Cool, we have the SRV, see if we have the address.
	 */
	if (srv_cache != NULL) {
		strlcpy(rrs.dname, srv_cache->rdata.SRV.dname, sizeof(rrs.dname));
		rrs.type  = T_A;
		rrs.class = C_IN;
		a_cache = cache_lookup(&rrs);
	}

	/*
	 * Now place a query for the records we don't have.
	 */
	log_debug("srv %p txt %p a %p", srv, txt, a);
	if (srv)
		log_debug_rr(srv);
	if (txt)
		log_debug_rr(txt);
	if (a)
		log_debug_rr(a);
	
	/*
	 * If all records are in cache, send ms and return.
	 */
	if (srv_cache != NULL && txt_cache != NULL && a_cache != NULL) {
		bzero(&ms, sizeof(ms));
		strlcpy(ms.name, srv->rrs.dname, sizeof(ms.name));
		strlcpy(ms.txt, txt->rdata.TXT, sizeof(ms.txt));
		ms.priority = srv->rdata.SRV.priority;
		ms.weight = srv->rdata.SRV.weight;
		ms.port = srv->rdata.SRV.port;
		ms.addr = a->rdata.A;
		control_send_ms(c, &ms, IMSG_CTL_RESOLVE);
		return;
	}

	/*
	 * If we got here we need to make a query.
	 */
	if ((q = calloc(1, sizeof(*q))) == NULL)
		fatal("calloc");
	LIST_INSERT_HEAD(&c->qlist, q, entry);
	LIST_INIT(&q->rrlist);
	q->style = QUERY_RESOLVE;
	q->ctl = c;
	timerclear(&tv);
	tv.tv_usec = FIRST_QUERYTIME;
	evtimer_set(&q->timer, query_fsm, q);
	evtimer_add(&q->timer, &tv);
	
	strlcpy(rrs.dname, msg, sizeof(rrs.dname));
	if ((srv = calloc(1, sizeof(*srv))) == NULL)
		fatal("calloc");
	if (srv_cache != NULL) {
		memcpy(srv, srv_cache, sizeof(*srv));
		srv->answered = 1;
	} else {
		rrs.type = T_SRV;
		srv->rrs = rrs;
	}
	LIST_INSERT_HEAD(&q->rrlist, srv, qentry);
	
	if ((txt = calloc(1, sizeof(*txt))) == NULL)
		fatal("calloc");
	if (txt_cache != NULL) {
		memcpy(txt, txt_cache, sizeof(*txt));
		txt->answered = 1;
	} else {
		rrs.type = T_TXT;
		txt->rrs = rrs;
	}
	LIST_INSERT_HEAD(&q->rrlist, txt, qentry);
	/*
	 * The T_A record we want is the dname of the T_SRV
	 */
	if (srv_cache != NULL && a_cache == NULL) {
		if ((a = calloc(1, sizeof(*a))) == NULL)
			fatal("calloc");
		a->rrs.type  = T_A;
		a->rrs.class = C_IN;
		strlcpy(a->rrs.dname, srv->rdata.SRV.dname,
		    sizeof(a->rrs.dname));
		LIST_INSERT_HEAD(&q->rrlist, a, qentry);
	}

	
	LIST_FOREACH(rr, &q->rrlist, qentry) {
		if (question_add(&rr->rrs) == NULL) {
			log_warnx("control_resolve: question_add error");
			query_remove(q);
			return;
			/* TODO: we may leak memory in srv, a or txt */
		}
	}
}
	
int
control_init(void)
{
	struct sockaddr_un	 sun;
	int			 fd;
	mode_t			 old_umask;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		log_warn("control_init: socket");
		return (-1);
	}

	bzero(&sun, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, MDNSD_SOCKET, sizeof(sun.sun_path));

	if (unlink(MDNSD_SOCKET) == -1)
		if (errno != ENOENT) {
			log_warn("control_init: unlink %s", MDNSD_SOCKET);
			close(fd);
			return (-1);
		}

	old_umask = umask(S_IXUSR|S_IXGRP|S_IWOTH|S_IROTH|S_IXOTH);
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		log_warn("control_init: bind: %s", MDNSD_SOCKET);
		close(fd);
		umask(old_umask);
		return (-1);
	}
	umask(old_umask);

	if (chmod(MDNSD_SOCKET, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) == -1) {
		log_warn("control_init: chmod");
		close(fd);
		(void)unlink(MDNSD_SOCKET);
		return (-1);
	}

	session_socket_blockmode(fd, BM_NONBLOCK);
	control_state.fd = fd;

	return (0);
}

int
control_listen(void)
{

	if (listen(control_state.fd, CONTROL_BACKLOG) == -1) {
		log_warn("control_listen: listen");
		return (-1);
	}

	event_set(&control_state.ev, control_state.fd, EV_READ | EV_PERSIST,
	    control_accept, NULL);
	event_add(&control_state.ev, NULL);

	return (0);
}

void
control_cleanup(void)
{
	unlink(MDNSD_SOCKET);
}

void
control_accept(int listenfd, short event, void *bula)
{
	int			 connfd;
	socklen_t		 len;
	struct sockaddr_un	 sun;
	struct ctl_conn		*c;

	len = sizeof(sun);
	if ((connfd = accept(listenfd,
	    (struct sockaddr *)&sun, &len)) == -1) {
		if (errno != EWOULDBLOCK && errno != EINTR)
			log_warn("control_accept: accept");
		return;
	}

	session_socket_blockmode(connfd, BM_NONBLOCK);

	if ((c = calloc(1, sizeof(struct ctl_conn))) == NULL) {
		log_warn("control_accept");
		close(connfd);
		return;
	}
	
	LIST_INIT(&c->qlist);
	imsg_init(&c->iev.ibuf, connfd);
	c->iev.handler = control_dispatch_imsg;
	c->iev.events = EV_READ;
	event_set(&c->iev.ev, c->iev.ibuf.fd, c->iev.events,
	    c->iev.handler, &c->iev);
	event_add(&c->iev.ev, NULL);

	TAILQ_INSERT_TAIL(&ctl_conns, c, entry);
}

struct ctl_conn *
control_connbyfd(int fd)
{
	struct ctl_conn	*c;

	for (c = TAILQ_FIRST(&ctl_conns); c != NULL && c->iev.ibuf.fd != fd;
	     c = TAILQ_NEXT(c, entry))
		;	/* nothing */

	return (c);
}

struct ctl_conn *
control_connbypid(pid_t pid)
{
	struct ctl_conn	*c;

	for (c = TAILQ_FIRST(&ctl_conns); c != NULL && c->iev.ibuf.pid != pid;
	     c = TAILQ_NEXT(c, entry))
		;	/* nothing */

	return (c);
}

void
control_close(int fd)
{
	struct ctl_conn	*c;
	struct query	*q;

	if ((c = control_connbyfd(fd)) == NULL) {
		log_warn("control_close: fd %d: not found", fd);
		return;
	}
	msgbuf_clear(&c->iev.ibuf.w);
	TAILQ_REMOVE(&ctl_conns, c, entry);

	event_del(&c->iev.ev);
	close(c->iev.ibuf.fd);
	while ((q = (LIST_FIRST(&c->qlist))) != NULL)
		query_remove(q);
	free(c);
}

void
control_dispatch_imsg(int fd, short event, void *bula)
{
	struct ctl_conn	*c;
	struct imsg	 imsg;
	ssize_t		 n;

	if ((c = control_connbyfd(fd)) == NULL) {
		log_warn("control_dispatch_imsg: fd %d: not found", fd);
		return;
	}

	if (event & EV_READ) {
		if ((n = imsg_read(&c->iev.ibuf)) == -1 || n == 0) {
			control_close(fd);
			return;
		}
	}
	if (event & EV_WRITE) {
		if (msgbuf_write(&c->iev.ibuf.w) == -1) {
			control_close(fd);
			return;
		}
	}

	for (;;) {
		if ((n = imsg_get(&c->iev.ibuf, &imsg)) == -1) {
			control_close(fd);
			return;
		}

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_CTL_LOOKUP:
			control_lookup(c, &imsg);
			break;
		case IMSG_CTL_BROWSE_ADD:
			control_browse_add(c, &imsg);
			break;
		case IMSG_CTL_BROWSE_DEL:
			control_browse_del(c, &imsg);
			break;
		case IMSG_CTL_RESOLVE:
			control_resolve(c, &imsg);
			break;
		default:
			log_debug("control_dispatch_imsg: "
			    "error handling imsg %d", imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}

	imsg_event_add(&c->iev);
}

void
session_socket_blockmode(int fd, enum blockmodes bm)
{
	int	flags;

	if ((flags = fcntl(fd, F_GETFL, 0)) == -1)
		fatal("fcntl F_GETFL");

	if (bm == BM_NONBLOCK)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	if ((flags = fcntl(fd, F_SETFL, flags)) == -1)
		fatal("fcntl F_SETFL");
}

int
control_send_rr(struct ctl_conn *c, struct rr *rr, int msgtype)
{
	log_debug("control_send_rr (%s) %s", rr_type_name(rr->rrs.type),
	    rr->rrs.dname);
	
	return (mdnsd_imsg_compose_ctl(c, msgtype, rr, sizeof(*rr)));
}

int
control_send_ms(struct ctl_conn *c, struct mdns_service *ms, int msgtype)
{
	log_debug("control_send_ms");

	return (mdnsd_imsg_compose_ctl(c, msgtype, ms, sizeof(*ms)));
}
