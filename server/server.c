/* Copyright 2019 the Age of Empires Free Sofware Remake authors. See LEGAL for legal info */

/**
 * Age of Empires server
 *
 * Provides a bare bones centralised server for AoE and AoE RoR.
 * Currently, it uses just one thread, to prevent any complicated data races
 * and also to make it easier to profile the performance bottlenecks.
 *
 * NOTE this server is linux only because it uses epoll, posix and other stuff
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <signal.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <xt/hashmap.h>
#include <xt/string.h>

#include "common.h"

#define CFG_NAME "settings"

// maximum number of users including gaia. the theoretical limit is the number
// of tcp ports, but making it this huge will decrease performance dramatically
#define MAX_USERS 1024
// I/O for epollwait
#define MAX_EVENTS (2*MAX_USERS)

// includes gaia
#define DEFAULT_MAX_USERS 9

#define HOSTSZ 120

// hint, ignored in most cases
#define EPOLL_SIZE 64

#define INIT_NET   0x01
#define INIT_SHEAP 0x02

unsigned init = 0;
int running = 0;

char path_cdrom[PATHSZ];
char motd[MOTD_SZ] = "Welcome to this AoE server. Be nice and adhere to the rules. ~FvV";

__attribute__((noreturn))
void panic(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	abort();
}

struct cfg {
	uint16_t port;
	uint16_t maxuser;
	char host[HOSTSZ];
} cfg = {
	DEFAULT_PORT, DEFAULT_MAX_USERS,
	"test server"
};

#define MAX_USERNAME 80

#define DEFAULT_SHEAP_PEERS 64
#define DEFAULT_SHEAP_USERS 16

// user state flags
#define USER_ACTIVE   0x01
#define USER_ALIVE    0x02
// for (usually) CPU-only users, e.g. gaia
#define USER_ASSIST   0x04
// can only die if it resigns, e.g. gaia
#define USER_IMMORTAL 0x08
// can trade with other users
#define USER_TRADE    0x10

struct res {
	uint32_t food, wood, gold, stone;
};

// in game user
struct user {
	// id is implicit, since the indices are all fixed
	unsigned state;
	unsigned ref; // player reference count
	struct res res;
};

#define PLAYER_ACTIVE 0x01
#define PLAYER_OP     0x02
#define PLAYER_HUMAN  0x04

/*
 * player cheats
 * NOTE the following cheats can crash the server and/or seriously degrade
 * performance (in order of seriousness, the first being the most dangerous):
 * - CHEAT_GOD
 * - CHEAT_INSTANT_ALL
 * - CHEAT_INSTANT_TRAIN
 * - CHEAT_INSTANT_BUILD
 * - CHEAT_REVEAL_MAP
 * - CHEAT_NO_FOG
 */
#define CHEAT_NO_FOG             0x0001
#define CHEAT_REVEAL_MAP         0x0002
/** see all player resources */
#define CHEAT_SPY_RESOURCES      0x0004
/** resources are instantly collected */
#define CHEAT_INSTANT_GATHER     0x0008
/** buildings are instantly completed */
#define CHEAT_INSTANT_BUILD      0x0010
/** units are instantly created */
#define CHEAT_INSTANT_TRAIN      0x0020
/** priests instantly convert units and buildings */
#define CHEAT_INSTANT_CONVERT    0x0040
/** player on steroids */
#define CHEAT_INSTANT_ALL        (CHEAT_INSTANT_GATHER|CHEAT_INSTANT_BUILD|CHEAT_INSTANT_TRAIN|CHEAT_INSTANT_CONVERT)
/** no resources need to be paid */
#define CHEAT_NO_RES_COSTS       0x0080
/** NOTE teleport only applies to the current player */
#define CHEAT_TELEPORT_UNITS     0x0100
#define CHEAT_TELEPORT_BUILDINGS 0x0200
#define CHEAT_TELEPORT_ALL       (CHEAT_TELEPORT_UNITS|CHEAT_TELEPORT_BUILDINGS)
// Don't add clip mode: this seriously breaks the quadtree world logic
/** all above handicaps combined */
#define CHEAT_GOD                0x03FF

/*
 * Player handicaps
 * These handicaps provide more novice users an initial or overall advantage
 * to make a match against more experienced players fairer. The exact bonus or
 * advantage is adjustable in the server settings. NOTE the bonus may also be
 * negative (i.e. positive bonus for novice players, negative bonus for expert
 * players).
 */
#define PH_RES             0x0001
#define PH_COST_UNITS      0x0002
#define PH_COST_BUILDINGS  0x0004
#define PH_COST_TECHS      0x0008
#define PH_GATHER          0x0010
#define PH_BUILD           0x0020
#define PH_TRAIN_UNITS     0x0040
#define PH_TRAIN_BUILDINGS 0x0080
#define PH_TRAIN_TECHS     0x0100
#define PH_TRAIN           (PH_TRAIN_UNITS|PH_TRAIN_BUILDINGS|PH_TRAIN_TECHS)
#define PH_CONVERT         0x0200
#define PH_REPAIR_WONDER   0x0400
#define PH_REPAIR          0x0800
// countdown for wonders and ruins victory is faster
#define PH_WINCOUNTER      0x1000
#define PH_TRADE           0x2000

#define PH_ALL             0x3FFF

/**
 * Virtual user, this makes it possible for multiple players to control the
 * same user. NOTE never ever cache a pointer or index to a player directly,
 * since it can change on the fly!
 */
struct player {
	uint16_t id; /**< unique */
	uint16_t uid; /**< index to user table, not unique */
	unsigned state;
	unsigned hc; /**< player handicap */
	unsigned cheats; /**< active OP actions */
	unsigned ai; /**< determines the AI mode, zero if human player without handicap/assistance */
	char name[MAX_USERNAME];
};

/**
 * Wrapper for epoll_event. NOTE never ever cache a pointer or index to a
 * slave/peer directly, since they can change on the fly!
 */
struct slave {
	int fd; // underlying socket connection
	uint16_t id; /**< unique identifier (is equal to modification counter at creation) */
	uint16_t pid; /**< virtual player unique identifier (also used to detect modification changes) */
	uint16_t bid; /**< netheap buffer identifier */
	struct sockaddr in_addr; // the underlying socket address
};

/**
 * The slaves heap. It uses a binary tree, which makes all operations in worst
 * case O(log n), except for merging, but we don't need that. It provides
 * multiple mappings in order to speed up searching for elements with different
 * key types. The first mapping sorts the peers on fd, the second on id, and
 * the last one on uid.
 * NOTE AI-only players are not stored in this heap, since AI-only players are
 * just virtual players and hence do not need a socket connection to the server.
 */
struct sheap {
	// NOTE data and pdata are binary heaps and therefore non-incrementally stored!
	struct slave *sdata; // fdmap is implicitly stored here
	struct player *pdata;
	// user data must be incrementally stored
	struct user *udata;
	//unsigned *pid, *uid; // mappings for player id and user id
	unsigned scount, scap; // no need for size_t, since maximum fits in unsigned anyway
	unsigned pcount, pcap;
	unsigned ucount, ucap; // fixed once the game is active
	/** modification counters */
	uint16_t smod, pmod;
	// network data: Cache for pending packets to receive
	struct netbuf {
		unsigned size;
		struct net_pkg pkg;
	} *ncache;
	/** used is the number of active slots, count the number of reserved slots. */
	unsigned nused, ncount, ncap;
	// keeps track of first available slots
	unsigned *rpop, rpopi;
} sheap;

static inline void player_reset(unsigned id, unsigned uid, unsigned state, unsigned handicap, unsigned ai, const char *name)
{
	struct player *p = &sheap.pdata[id];

	assert(!(p->state & PLAYER_ACTIVE));

	p->id = id;
	p->uid = uid;
	p->state = state;
	p->hc = handicap;
	p->ai = ai;

	xtstrncpy(p->name, name, MAX_USERNAME);
}

static inline void user_reset(unsigned id, unsigned state)
{
	sheap.udata[id].state = state;
}

static inline void user_init(unsigned id, unsigned state)
{
	assert(!(sheap.udata[id].state & USER_ACTIVE));
	user_reset(id, state);
}

void slave_init(struct slave *s, int fd)
{
	s->fd = fd;
	s->pid = sheap.pmod;
}

void sheap_free(const struct sheap *h)
{
	// network data
	free(h->rpop);
	free(h->ncache);
	// slave heap
	free(h->udata);
	free(h->pdata);
	free(h->sdata);
}

int sheap_init(void)
{
	int err = 1;
	struct sheap h = {0};

	assert(sizeof *h.pdata == sizeof(struct player));
	assert(!h.sdata && !h.pdata && !h.udata);

	if (!(h.sdata = malloc(DEFAULT_SHEAP_PEERS * sizeof *h.sdata))
		|| !(h.pdata = malloc(DEFAULT_SHEAP_PEERS * sizeof *h.pdata))
		|| !(h.udata = malloc(DEFAULT_SHEAP_USERS * sizeof *h.udata))
		|| !(h.ncache = malloc(DEFAULT_SHEAP_USERS * sizeof *h.ncache))
		|| !(h.rpop = malloc(DEFAULT_SHEAP_USERS * sizeof *h.rpop))) {
		goto fail;
	}

	sheap = h;
	sheap.ncap = sheap.pcap = sheap.scap = DEFAULT_SHEAP_PEERS;
	sheap.ucap = DEFAULT_SHEAP_USERS;
	// setup special users: gaia
	user_reset(0, USER_ACTIVE | USER_ALIVE | USER_ASSIST | USER_IMMORTAL);
	sheap.ucount = 1;

	err = 0;
fail:
	if (err)
		sheap_free(&h);

	return err;
}

#define heap_left(x) (2*(x)+1)
#define heap_right(x) (2*(x)+2)
#define heap_parent(x) (((x)-1)/2)

// slave heap errors
#define SHE_NOMEM  1
#define SHE_FULL   2
#define SHE_BADFD  3
#define SHE_BADPID 4

static const char *she_str[] = {
	"success",
	"out of memory",
	"server is full",
	"bad file descriptor",
	"bad player id",
};

static void netbuf_init(struct netbuf *b)
{
	b->size = 0;
}

static void netbuf_recv(struct netbuf *nbuf, const unsigned char *buf, unsigned n)
{
	unsigned rem = sizeof *nbuf - nbuf->size;
	unsigned copy = n > rem ? rem : n;

	if (copy) {
		memcpy(((unsigned char*)&nbuf->pkg) + nbuf->size, buf, copy);
		nbuf->size += copy;
	}
}

int sheap_new_slave(int fd, struct sockaddr *in_addr)
{
	// ensure both heaps are big enough
	if (sheap.scount == sheap.scap) {
		if (sheap.scap >= MAX_USERS >> 1)
			return SHE_FULL;

		size_t newcap = sheap.scap << 1;
		struct slave *data;

		if (!(data = realloc(sheap.sdata, newcap * sizeof *data)))
			return SHE_NOMEM;

		sheap.sdata = data;
		sheap.scap = newcap;
	}

	if (sheap.pcount == sheap.pcap) {
		if (sheap.pcap >= MAX_USERS >> 1)
			return SHE_FULL;

		size_t newcap = sheap.pcap << 1;
		struct player *data;

		if (!(data = realloc(sheap.pdata, newcap * sizeof *data)))
			return SHE_NOMEM;

		sheap.pdata = data;
		sheap.pcap = newcap;
	}

	// recycle empty slots
	unsigned slot = 0;

	if (sheap.rpopi) {
		slot = sheap.rpop[--sheap.rpopi];
		goto put;
	}

	if (sheap.ncount == sheap.ncap) {
		if (sheap.ncap >= MAX_USERS >> 1)
			return SHE_FULL;

		size_t newcap = sheap.ncap << 1;
		struct netbuf *ncache;

		if (!(ncache = realloc(sheap.ncache, newcap * sizeof *ncache)))
			return SHE_NOMEM;

		sheap.ncache = ncache;
		sheap.ncap = newcap;
	}

	slot = sheap.ncount++;
put:
	;
	// grab data
	struct slave *s = &sheap.sdata[sheap.scount];
	struct player *p = &sheap.pdata[sheap.pcount];

	// initialize data and update modification counters
	s->fd = fd;
	s->in_addr = *in_addr;
	s->id = sheap.smod++;
	s->pid = p->id = sheap.pmod++;
	p->uid = INVALID_USER_ID;
	p->state = PLAYER_ACTIVE | PLAYER_HUMAN;
	p->hc = p->ai = 0;
	p->name[0] = '\0';

	// restore heap properties
	for (unsigned pos = sheap.scount++; pos; pos = heap_parent(pos))
		if (sheap.sdata[pos].fd < sheap.sdata[heap_parent(pos)].fd) {
			struct slave tmp = sheap.sdata[pos];
			sheap.sdata[pos] = sheap.sdata[heap_parent(pos)];
			sheap.sdata[heap_parent(pos)] = tmp;
		}

	for (unsigned pos = sheap.pcount++; pos; pos = heap_parent(pos))
		if (sheap.pdata[pos].id < sheap.pdata[heap_parent(pos)].id) {
			struct player tmp = sheap.pdata[pos];
			sheap.pdata[pos] = sheap.pdata[heap_parent(pos)];
			sheap.pdata[heap_parent(pos)] = tmp;
		}

	// commit new slave
	++sheap.nused;
	s->bid = slot;
	netbuf_init(&sheap.ncache[slot]);

	return 0;
}

struct slave *sheap_find_slave(int fd)
{
	for (unsigned i = 0; i < sheap.scount;) {
		struct slave *s = &sheap.sdata[i];

		if (s->fd == fd)
			return s;
		else
			i = fd < s->fd ? heap_left(i) : heap_right(i);
	}

	return NULL;
}

struct player *sheap_find_player(uint16_t pid)
{
	for (unsigned i = 0; i < sheap.pcount;) {
		struct player *p = &sheap.pdata[i];

		if (p->id == pid)
			return p;
		else
			i = pid < p->id ? heap_left(i) : heap_right(i);
	}

	return NULL;
}

void sheap_delete_player(struct player *p)
{
	// first, purge the user reference
	if (p->uid != INVALID_USER_ID) {
		assert(sheap.udata[p->uid].ref);
		--sheap.udata[p->uid].ref;
	}

	if (!--sheap.pcount)
		return;

	// second, move last element to current player
	unsigned pos = (unsigned)(p - sheap.pdata);
	sheap.pdata[pos] = sheap.pdata[sheap.pcount];

	// restore heap property
	if (pos && sheap.pdata[pos].id >= sheap.pdata[heap_parent(pos)].id)
		do {
			struct player tmp = sheap.pdata[pos];
			sheap.pdata[pos] = sheap.pdata[heap_parent(pos)];
			sheap.pdata[heap_parent(pos)] = tmp;

			pos = heap_parent(pos);
		} while (pos && sheap.pdata[pos].id >= sheap.pdata[heap_parent(pos)].id);
	else
		for (unsigned l, r, m;; pos = m) {
			l = heap_left(pos);
			r = heap_right(pos);
			m = pos;

			if (l < sheap.pcount && sheap.pdata[l].id < sheap.pdata[m].id)
				m = l;
			if (r < sheap.pcount && sheap.pdata[r].id < sheap.pdata[m].id)
				m = r;

			if (m == pos)
				break;

			struct player tmp = sheap.pdata[pos];
			sheap.pdata[pos] = sheap.pdata[m];
			sheap.pdata[m] = tmp;
		}
}

void sheap_delete_slave(struct slave *s)
{
	// ensure the network data heap has enough room
	if (sheap.ncount == sheap.ncap) {
		assert(sheap.ncap < SIZE_MAX >> 1);

		size_t newcap = sheap.ncap << 1;
		unsigned *rpop;

		// if this fails, the server is unrecoverable anyway
		if (!(rpop = realloc(sheap.rpop, newcap * sizeof *rpop)))
			panic("sheap network heap overflow");
	}

	sheap.rpop[sheap.rpopi++] = s->bid;
	assert(sheap.nused);
	--sheap.nused;

	if (!--sheap.scount)
		return;

	// move last element to current slave
	unsigned pos = (unsigned)(s - sheap.sdata);
	sheap.sdata[pos] = sheap.sdata[sheap.scount];

	// restore heap property
	if (pos && sheap.sdata[pos].id >= sheap.pdata[heap_parent(pos)].id)
		do {
			struct slave tmp = sheap.sdata[pos];
			sheap.sdata[pos] = sheap.sdata[heap_parent(pos)];
			sheap.sdata[heap_parent(pos)] = tmp;

			pos = heap_parent(pos);
		} while (pos && sheap.sdata[pos].id >= sheap.sdata[heap_parent(pos)].id);
	else
		for (unsigned l, r, m;; pos = m) {
			l = heap_left(pos);
			r = heap_right(pos);
			m = pos;

			if (l < sheap.scount && sheap.sdata[l].id < sheap.pdata[m].id)
				m = l;
			if (r < sheap.scount && sheap.sdata[r].id < sheap.sdata[m].id)
				m = r;

			if (m == pos)
				break;

			struct slave tmp = sheap.sdata[pos];
			sheap.sdata[pos] = sheap.sdata[m];
			sheap.sdata[m] = tmp;
		}
}

/**
 * Remove the peer/slave associated with the specified fd.
 * Returns non-zero on failure.
 */
int sheap_close_fd(int fd)
{
	int err;
	struct slave *s;
	struct player *p;

	if (fd == -1)
		return SHE_BADFD;

	if ((err = close(fd)))
		perror("sheap_close_fd");

	if (!(s = sheap_find_slave(fd)))
		return SHE_BADFD;

	if (!(p = sheap_find_player(s->pid)))
		return SHE_BADPID;

	sheap_delete_player(p);
	sheap_delete_slave(s);

	return err ? SHE_BADFD : 0;
}

/** similar to xtStringTrim, but faster */
char *strtrim(char *str)
{
	char *start, *end;

	for (start = str; ; ++start)
		if (!isspace(*start))
			break;

	for (end = start + strlen(start); end > start; --end)
		if (!isspace(end[-1]))
			break;

	*end = '\0';

	return start;
}

/**
 * Read server configuration file (i.e. settings).
 * Blank lines or lines starting with `;' and `#' are ignored.
 * Options must be specified as: key=value
 * Returns zero on success.
 */
int cfg_init(void)
{
	int ret = 1;
	size_t lno = 0;
	FILE *f = NULL;
	char line[IO_BUFSZ];

	// read and apply settings

	if (!(f = fopen(CFG_NAME, "r"))) {
		perror(CFG_NAME);
		goto fail;
	}

	while (fgets(line, sizeof line, f)) {
		char *start, *sep, *key, *val;

		++lno;
		start = strtrim(line);

		// ignore blank lines and comments
		if (!*start || *start == ';' || *start == '#')
			continue;

		// key=value
		if (!(sep = strchr(start, '='))) {
			fprintf(stderr, "%s: garbage at line %zu\n", CFG_NAME, lno);
			goto fail;
		}

		*sep = '\0';
		key = strtrim(start);
		val = strtrim(sep + 1);

		// parse option
		if (!strcmp(key, "port")) {
			int port = atoi(val);

			if (port < 0 || port > UINT16_MAX) {
				fprintf(stderr, "%s: bogus port: %d\n", CFG_NAME, port);
				goto fail;
			}

			cfg.port = port;
		} else if (!strcmp(key, "maxuser")) {
			int count = atoi(val);

			if (count < 1 || count >= MAX_USERS) {
				fprintf(stderr, "%s: maxuser not in range [%d,%d)\n", CFG_NAME, 1, MAX_USERS);
				goto fail;
			}

			cfg.maxuser = count;
		} else if (!strcmp(key, "motd")) {
			xtstrncpy(motd, val, sizeof motd);
		} else if (!strcmp(key, "path")) {
			xtstrncpy(path_cdrom, val, sizeof path_cdrom);
		} else {
			fprintf(stderr, "%s: bad setting %s at line %zu\n", CFG_NAME, key, lno);
			goto fail;
		}
	}

	if (!path_cdrom[0]) {
		fprintf(stderr, "%s: path to CD-ROM not set\n", CFG_NAME);
		goto fail;
	}

	ret = 0;
fail:
	if (f)
		fclose(f);

	return ret;
}

struct sigaction sigpipe, sigpipe_old;
int sockfd = -1, efd = -1;

struct epoll_event events[MAX_EVENTS];

static int sock_nonblock(int fd)
{
	int flags;
	if ((flags = fcntl(fd, F_GETFL, 0)) == -1)
		return -1;
	flags |= O_NONBLOCK;
	return fcntl(fd, F_SETFL, flags) == -1;
}

static inline int sock_reuse(int fd)
{
	int val = 1;
	return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&val, sizeof val);
}

static inline int sock_keepalive(int fd)
{
	int val = 1;
	return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&val, sizeof val);
}

int net_init(void)
{
	int err = 1;
	struct epoll_event ev;
	struct sockaddr_in sa;

	// ignore boken pipes: this occurs when the server tries to write data to an already closed fd
	sigpipe.sa_handler = SIG_IGN;
	sigemptyset(&sigpipe.sa_mask);
	sigpipe.sa_flags = 0;

	if ((err = sigaction(SIGPIPE, &sigpipe, &sigpipe_old)))
		goto fail;

	// setup server socket
	if ((sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1) {
		perror("net_init: sock create");
		goto fail;
	}

	if ((err = sock_reuse(sockfd))) {
		perror("net_init: sock reuse");
		goto fail;
	}

	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(cfg.port);

	if ((err = bind(sockfd, (struct sockaddr*)&sa, sizeof sa)) < 0) {
		perror("net_init: sock bind");
		goto fail;
	}

	if ((err = listen(sockfd, SOMAXCONN))) {
		perror("net_init: sock listen");
		goto fail;
	}

	// setup epoll handler
	if ((efd = epoll_create(EPOLL_SIZE)) == -1) {
		perror("net_init: epoll create");
		goto fail;
	}

	memset(&ev, 0, sizeof ev);

	ev.data.fd = sockfd;
	ev.events = EPOLLIN | EPOLLET;

	if ((err = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &ev))) {
		perror("net_init: epoll_ctl");
		goto fail;
	}

	err = 0;
fail:
	if (err) {
		if (efd != -1)
			close(efd);
		if (sockfd != -1)
			close(sockfd);
	}

	return err;
}

void net_stop(void)
{
	if (efd != -1)
		close(efd);
	if (sockfd != -1)
		close(sockfd);

	sigaction(SIGPIPE, &sigpipe_old, NULL);
}

/** Accept any new clients. */
void incoming(void)
{
	while (1) {
		struct sockaddr in_addr;
		int err = 0, infd;
		socklen_t in_len = sizeof in_addr;

		if ((infd = accept(sockfd, &in_addr, &in_len)) == -1) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				perror("accept");
			break;
		}

		char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

		// setup incoming connection and drop if errors occur
		// XXX is getnameinfo still vulnerable?
		if (!getnameinfo(&in_addr, in_len,
				hbuf, sizeof hbuf,
				sbuf, sizeof sbuf,
				NI_NUMERICHOST | NI_NUMERICSERV))
			printf("incoming: fd %d from %s:%s\n", infd, hbuf, sbuf);
		else
			printf("incoming: fd %d from unknown\n", infd);

		if (sock_nonblock(infd)) {
			perror("nonblock");
			goto reject;
		}
		// XXX is this a good idea?
		if (sock_reuse(infd)) {
			perror("reuse");
			goto reject;
		}
		if (sock_keepalive(infd)) {
			perror("keep alive");
			goto reject;
		}

		// first register the event, then add the slave
		struct epoll_event ev;
		memset(&ev, 0, sizeof ev);

		ev.data.fd = infd;
		ev.events = EPOLLIN | EPOLLET;

		if (epoll_ctl(efd, EPOLL_CTL_ADD, infd, &ev) || (err = sheap_new_slave(infd, &in_addr))) {
			if (err)
				fprintf(stderr, "incoming: cannot accept peer: %s\n", she_str[err]);
			else
				perror("epoll_ctl");
reject:
			fprintf(stderr, "incoming: reject fd %d\n", infd);
			close(infd);
		}
	}
}

static int net_server_control(struct net_serverctl *ctl)
{
	switch (ctl->opcode) {
	case SC_STOP:
		running = 0;
		return 0;
	default:
		fprintf(stderr, "net_server_control: bad opcode: %04" PRIX16 "\n", ctl->opcode);
		return 1;
	}

	return 1;
}

static int net_pkg_process(struct net_pkg *pkg, struct slave *s)
{
	switch (pkg->type) {
	case NT_TEXT:
		// FIXME stub
		pkg->data.text.text[TEXT_BUFSZ - 1] = '\0';
		printf("%d: %s\n", s->fd, pkg->data.text.text);
		return 0;
	case NT_SERVER_CONTROL:
		return net_server_control(&pkg->data.serverctl);
	default:
		fprintf(stderr, "net_pkg_process: bad packet from fd %d\n", s->fd);
		return 1;
	}

	return 0;
}

// event processing errors
#define EPE_INVALID  1
#define EPE_READ     2
#define EPE_BADFD    3

static int peer_handle(int fd, unsigned char *buf, unsigned n)
{
	struct slave *s;

	if (!(s = sheap_find_slave(fd)))
		return EPE_BADFD;

	struct netbuf *nbuf = &sheap.ncache[s->bid];
	netbuf_recv(nbuf, buf, n);

	// process all complete packets
	while (1) {
		unsigned len = (unsigned)xtbe16toh(nbuf->pkg.length) + NET_HEADER_SIZE;

		if (nbuf->size >= NET_HEADER_SIZE && nbuf->pkg.length >= len) {
			int err;

			net_pkg_ntoh(&nbuf->pkg);

			if ((err = net_pkg_process(&nbuf->pkg, s)))
				return err;

			memmove(&nbuf->pkg, ((const unsigned char*)&nbuf->pkg) + nbuf->size, nbuf->size - len);
			nbuf->size -= len;
		} else {
			break;
		}
	}

	return 0;
}

/**
 * Read any pending data from the specified socket poll event.
 * Returns non-zero on error.
 */
int event_process(struct epoll_event *ev)
{
	// Filter invalid/error events
	if ((ev->events & EPOLLERR) || (ev->events & EPOLLHUP) || !(ev->events & EPOLLIN))
		return EPE_INVALID;

	// Process incoming events
	int fd = ev->data.fd;
	if (sockfd == fd) {
		incoming();
		return 0;
	}

	while (1) {
		int err;
		ssize_t n;
		unsigned char buf[NET_PACKET_SIZE];

		if ((n = read(fd, buf, sizeof buf)) < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fprintf(stderr,
					"event_process: read error fd %d: %s\n",
					fd, strerror(errno)
				);
				return EPE_READ;
			}
			break;
		} else if (!n) {
			printf("event_process: remote closed fd %d\n", fd);
			return EPE_READ;
		}
		if ((err = peer_handle(fd, buf, n)))
			return err;
	}

	return 0;
}

int event_loop(void)
{
	for (running = 1; running;) {
		int err, n;

		// XXX check if we need to implement a send_queue

		if ((n = epoll_wait(efd, events, MAX_EVENTS, -1)) == -1) {
			/*
			 * This case occurs only when the server itself has been
			 * suspended and resumed. We can just ignore this case.
			 */
			if (errno == EINTR)
				continue;

			perror("epoll_wait");
			return 1;
		}

		for (int i = 0; i < n; ++i)
			if ((err = event_process(&events[i]))) {
				if (err == EPE_INVALID)
					fprintf(stderr, "event_process: bad event (%d,%d): %s\n", i, events[i].data.fd, strerror(errno));

				if ((err = sheap_close_fd(events[i].data.fd))) {
					fprintf(stderr, "event_process: internal error: %s\n", she_str[err]);
					return 1;
				}
			}
	}

	return 0;
}

int main(void)
{
	int ret = 1;

	if ((ret = cfg_init()))
		goto fail;

	printf("Starting server \"%s\"...\n", cfg.host);
	printf("Max users: %" PRIu16 "\n", cfg.maxuser);

	// initialize subsystems

	if (net_init()) {
		perror("net_init");
		goto fail;
	}
	init |= INIT_NET;

	if ((ret = sheap_init()))
		goto fail;
	init |= INIT_SHEAP;

	// enter event main loop
	ret = event_loop();
fail:
	if (init & INIT_SHEAP)
		sheap_free(&sheap);
	if (init & INIT_NET)
		net_stop();

	return ret;
}
