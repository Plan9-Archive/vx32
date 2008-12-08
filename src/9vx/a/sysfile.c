#include	"u.h"
#include	"lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"error.h"

/*
 * The sys*() routines needn't poperror() as they return directly to syscall().
 */

static void
unlockfgrp(Fgrp *f)
{
	int ex;

	ex = f->exceed;
	f->exceed = 0;
	unlock(&f->ref.lk);
	if(ex)
		pprint("warning: process exceeds %d file descriptors\n", ex);
}

int
growfd(Fgrp *f, int fd)	/* fd is always >= 0 */
{
	Chan **newfd, **oldfd;

	if(fd < f->nfd)
		return 0;
	if(fd >= f->nfd+DELTAFD)
		return -1;	/* out of range */
	/*
	 * Unbounded allocation is unwise; besides, there are only 16 bits
	 * of fid in 9P
	 */
	if(f->nfd >= 5000){
    Exhausted:
		print("no free file descriptors\n");
		return -1;
	}
	newfd = malloc((f->nfd+DELTAFD)*sizeof(Chan*));
	if(newfd == 0)
		goto Exhausted;
	oldfd = f->fd;
	memmove(newfd, oldfd, f->nfd*sizeof(Chan*));
	f->fd = newfd;
	free(oldfd);
	f->nfd += DELTAFD;
	if(fd > f->maxfd){
		if(fd/100 > f->maxfd/100)
			f->exceed = (fd/100)*100;
		f->maxfd = fd;
	}
	return 1;
}

/*
 *  this assumes that the fgrp is locked
 */
int
findfreefd(Fgrp *f, int start)
{
	int fd;

	for(fd=start; fd<f->nfd; fd++)
		if(f->fd[fd] == 0)
			break;
	if(fd >= f->nfd && growfd(f, fd) < 0)
		return -1;
	return fd;
}

int
newfd(Chan *c)
{
	int fd;
	Fgrp *f;

	f = up->fgrp;
	lock(&f->ref.lk);
	fd = findfreefd(f, 0);
	if(fd < 0){
		unlockfgrp(f);
		return -1;
	}
	if(fd > f->maxfd)
		f->maxfd = fd;
	f->fd[fd] = c;
	unlockfgrp(f);
	return fd;
}

int
newfd2(int fd[2], Chan *c[2])
{
	Fgrp *f;

	f = up->fgrp;
	lock(&f->ref.lk);
	fd[0] = findfreefd(f, 0);
	if(fd[0] < 0){
		unlockfgrp(f);
		return -1;
	}
	fd[1] = findfreefd(f, fd[0]+1);
	if(fd[1] < 0){
		unlockfgrp(f);
		return -1;
	}
	if(fd[1] > f->maxfd)
		f->maxfd = fd[1];
	f->fd[fd[0]] = c[0];
	f->fd[fd[1]] = c[1];
	unlockfgrp(f);

	return 0;
}

Chan*
fdtochan(int fd, int mode, int chkmnt, int iref)
{
	Chan *c;
	Fgrp *f;

	c = 0;
	f = up->fgrp;

	lock(&f->ref.lk);
	if(fd<0 || f->nfd<=fd || (c = f->fd[fd])==0) {
		unlock(&f->ref.lk);
		error(Ebadfd);
	}
	if(iref)
		incref(&c->ref);
	unlock(&f->ref.lk);

	if(chkmnt && (c->flag&CMSG)) {
		if(iref)
			cclose(c);
		error(Ebadusefd);
	}

	if(mode<0 || c->mode==ORDWR)
		return c;

	if((mode&OTRUNC) && c->mode==OREAD) {
		if(iref)
			cclose(c);
		error(Ebadusefd);
	}

	if((mode&~OTRUNC) != c->mode) {
		if(iref)
			cclose(c);
		error(Ebadusefd);
	}

	return c;
}

int
openmode(ulong o)
{
	o &= ~(OTRUNC|OCEXEC|ORCLOSE);
	if(o > OEXEC)
		error(Ebadarg);
	if(o == OEXEC)
		return OREAD;
	return o;
}

long
sysfd2path(u32int *arg)
{
	Chan *c;
	char *buf;
	
	buf = uvalidaddr(arg[1], arg[2], 1);

	c = fdtochan(arg[0], -1, 0, 1);
	snprint(buf, arg[2], "%s", chanpath(c));
	cclose(c);
	return 0;
}

long
syspipe(u32int *arg)
{
	int fd[2];
	Chan *c[2];
	Dev *d;
	static char *datastr[] = {"data", "data1"};
	int *ufd;
	
	ufd = uvalidaddr(arg[0], 2*BY2WD, 1);
	evenaddr(arg[0]);
	d = devtab[devno('|', 0)];
	c[0] = namec("#|", Atodir, 0, 0);
	c[1] = 0;
	fd[0] = -1;
	fd[1] = -1;

	if(waserror()){
		cclose(c[0]);
		if(c[1])
			cclose(c[1]);
		nexterror();
	}
	c[1] = cclone(c[0]);
	if(walk(&c[0], datastr+0, 1, 1, nil) < 0)
		error(Egreg);
	if(walk(&c[1], datastr+1, 1, 1, nil) < 0)
		error(Egreg);
	c[0] = d->open(c[0], ORDWR);
	c[1] = d->open(c[1], ORDWR);
	if(newfd2(fd, c) < 0)
		error(Enofd);
	poperror();

	ufd[0] = fd[0];
	ufd[1] = fd[1];
	return 0;
}

long
sysdup(u32int *arg)
{
	int fd;
	Chan *c, *oc;
	Fgrp *f = up->fgrp;

	/*
	 * Close after dup'ing, so date > #d/1 works
	 */
	c = fdtochan(arg[0], -1, 0, 1);
	fd = arg[1];
	if(fd != -1){
		lock(&f->ref.lk);
		if(fd<0 || growfd(f, fd)<0) {
			unlockfgrp(f);
			cclose(c);
			error(Ebadfd);
		}
		if(fd > f->maxfd)
			f->maxfd = fd;

		oc = f->fd[fd];
		f->fd[fd] = c;
		unlockfgrp(f);
		if(oc)
			cclose(oc);
	}else{
		if(waserror()) {
			cclose(c);
			nexterror();
		}
		fd = newfd(c);
		if(fd < 0)
			error(Enofd);
		poperror();
	}

	return fd;
}

long
sysopen(u32int *arg)
{
	int fd;
	Chan *c = 0;
	char *name;

	openmode(arg[1]);	/* error check only */
	name = uvalidaddr(arg[0], 1, 0);
	c = namec(name, Aopen, arg[1], 0);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	fd = newfd(c);
	if(fd < 0)
		error(Enofd);
	poperror();
	return fd;
}

void
fdclose(int fd, int flag)
{
	int i;
	Chan *c;
	Fgrp *f = up->fgrp;

	lock(&f->ref.lk);
	c = f->fd[fd];
	if(c == 0){
		/* can happen for users with shared fd tables */
		unlock(&f->ref.lk);
		return;
	}
	if(flag){
		if(c==0 || !(c->flag&flag)){
			unlock(&f->ref.lk);
			return;
		}
	}
	f->fd[fd] = 0;
	if(fd == f->maxfd)
		for(i=fd; --i>=0 && f->fd[i]==0; )
			f->maxfd = i;

	unlock(&f->ref.lk);
	cclose(c);
}

long
sysclose(u32int *arg)
{
	fdtochan(arg[0], -1, 0, 0);
	fdclose(arg[0], 0);

	return 0;
}

long
unionread(Chan *c, void *va, long n)
{
	int i;
	long nr;
	Mhead *m;
	Mount *mount;

	qlock(&c->umqlock);
	m = c->umh;
	rlock(&m->lock);
	mount = m->mount;
	/* bring mount in sync with c->uri and c->umc */
	for(i = 0; mount != nil && i < c->uri; i++)
		mount = mount->next;

	nr = 0;
	while(mount != nil){
		/* Error causes component of union to be skipped */
		if(mount->to && !waserror()){
			if(c->umc == nil){
				c->umc = cclone(mount->to);
				c->umc = devtab[c->umc->type]->open(c->umc, OREAD);
			}
	
			nr = devtab[c->umc->type]->read(c->umc, va, n, c->umc->offset);
			c->umc->offset += nr;
			poperror();
		}
		if(nr > 0)
			break;

		/* Advance to next element */
		c->uri++;
		if(c->umc){
			cclose(c->umc);
			c->umc = nil;
		}
		mount = mount->next;
	}
	runlock(&m->lock);
	qunlock(&c->umqlock);
	return nr;
}

static void
unionrewind(Chan *c)
{
	qlock(&c->umqlock);
	c->uri = 0;
	if(c->umc){
		cclose(c->umc);
		c->umc = nil;
	}
	qunlock(&c->umqlock);
}

static int
dirfixed(uchar *p, uchar *e, Dir *d)
{
	int len;

	len = GBIT16(p)+BIT16SZ;
	if(p + len > e)
		return -1;

	p += BIT16SZ;	/* ignore size */
	d->type = devno(GBIT16(p), 1);
	p += BIT16SZ;
	d->dev = GBIT32(p);
	p += BIT32SZ;
	d->qid.type = GBIT8(p);
	p += BIT8SZ;
	d->qid.vers = GBIT32(p);
	p += BIT32SZ;
	d->qid.path = GBIT64(p);
	p += BIT64SZ;
	d->mode = GBIT32(p);
	p += BIT32SZ;
	d->atime = GBIT32(p);
	p += BIT32SZ;
	d->mtime = GBIT32(p);
	p += BIT32SZ;
	d->length = GBIT64(p);

	return len;
}

static char*
dirname(uchar *p, int *n)
{
	p += BIT16SZ+BIT16SZ+BIT32SZ+BIT8SZ+BIT32SZ+BIT64SZ
		+ BIT32SZ+BIT32SZ+BIT32SZ+BIT64SZ;
	*n = GBIT16(p);
	return (char*)p+BIT16SZ;
}

static long
dirsetname(char *name, int len, uchar *p, long n, long maxn)
{
	char *oname;
	int olen;
	long nn;

	if(n == BIT16SZ)
		return BIT16SZ;

	oname = dirname(p, &olen);

	nn = n+len-olen;
	PBIT16(p, nn-BIT16SZ);
	if(nn > maxn)
		return BIT16SZ;

	if(len != olen)
		memmove(oname+len, oname+olen, p+n-(uchar*)(oname+olen));
	PBIT16((uchar*)(oname-2), len);
	memmove(oname, name, len);
	return nn;
}

/*
 * Mountfix might have caused the fixed results of the directory read
 * to overflow the buffer.  Catch the overflow in c->dirrock.
 */
static void
mountrock(Chan *c, uchar *p, uchar **pe)
{
	uchar *e, *r;
	int len, n;

	e = *pe;

	/* find last directory entry */
	for(;;){
		len = BIT16SZ+GBIT16(p);
		if(p+len >= e)
			break;
		p += len;
	}

	/* save it away */
	qlock(&c->rockqlock);
	if(c->nrock+len > c->mrock){
		n = ROUND(c->nrock+len, 1024);
		r = smalloc(n);
		memmove(r, c->dirrock, c->nrock);
		free(c->dirrock);
		c->dirrock = r;
		c->mrock = n;
	}
	memmove(c->dirrock+c->nrock, p, len);
	c->nrock += len;
	qunlock(&c->rockqlock);

	/* drop it */
	*pe = p;
}

/*
 * Satisfy a directory read with the results saved in c->dirrock.
 */
static int
mountrockread(Chan *c, uchar *op, long n, long *nn)
{
	long dirlen;
	uchar *rp, *erp, *ep, *p;

	/* common case */
	if(c->nrock == 0)
		return 0;

	/* copy out what we can */
	qlock(&c->rockqlock);
	rp = c->dirrock;
	erp = rp+c->nrock;
	p = op;
	ep = p+n;
	while(rp+BIT16SZ <= erp){
		dirlen = BIT16SZ+GBIT16(rp);
		if(p+dirlen > ep)
			break;
		memmove(p, rp, dirlen);
		p += dirlen;
		rp += dirlen;
	}

	if(p == op){
		qunlock(&c->rockqlock);
		return 0;
	}

	/* shift the rest */
	if(rp != erp)
		memmove(c->dirrock, rp, erp-rp);
	c->nrock = erp - rp;

	*nn = p - op;
	qunlock(&c->rockqlock);
	return 1;
}

static void
mountrewind(Chan *c)
{
	c->nrock = 0;
}

/*
 * Rewrite the results of a directory read to reflect current 
 * name space bindings and mounts.  Specifically, replace
 * directory entries for bind and mount points with the results
 * of statting what is mounted there.  Except leave the old names.
 */
static long
mountfix(Chan *c, uchar *op, long n, long maxn)
{
	char *name;
	int nbuf, nname;
	Chan *nc;
	Mhead *mh;
	Mount *m;
	uchar *p;
	int dirlen, rest;
	long l;
	uchar *buf, *e;
	Dir d;

	p = op;
	buf = nil;
	nbuf = 0;
	for(e=&p[n]; p+BIT16SZ<e; p+=dirlen){
		dirlen = dirfixed(p, e, &d);
		if(dirlen < 0)
			break;
		nc = nil;
		mh = nil;
		if(findmount(&nc, &mh, d.type, d.dev, d.qid)){
			/*
			 * If it's a union directory and the original is
			 * in the union, don't rewrite anything.
			 */
			for(m=mh->mount; m; m=m->next)
				if(eqchantdqid(m->to, d.type, d.dev, d.qid, 1))
					goto Norewrite;

			name = dirname(p, &nname);
			/*
			 * Do the stat but fix the name.  If it fails, leave old entry.
			 * BUG: If it fails because there isn't room for the entry,
			 * what can we do?  Nothing, really.  Might as well skip it.
			 */
			if(buf == nil){
				buf = smalloc(4096);
				nbuf = 4096;
			}
			if(waserror())
				goto Norewrite;
			l = devtab[nc->type]->stat(nc, buf, nbuf);
			l = dirsetname(name, nname, buf, l, nbuf);
			if(l == BIT16SZ)
				error("dirsetname");
			poperror();

			/*
			 * Shift data in buffer to accomodate new entry,
			 * possibly overflowing into rock.
			 */
			rest = e - (p+dirlen);
			if(l > dirlen){
				while(p+l+rest > op+maxn){
					mountrock(c, p, &e);
					if(e == p){
						dirlen = 0;
						goto Norewrite;
					}
					rest = e - (p+dirlen);
				}
			}
			if(l != dirlen){
				memmove(p+l, p+dirlen, rest);
				dirlen = l;
				e = p+dirlen+rest;
			}

			/*
			 * Rewrite directory entry.
			 */
			memmove(p, buf, l);

		    Norewrite:
			cclose(nc);
			putmhead(mh);
		}
	}
	if(buf)
		free(buf);

	if(p != e)
		error("oops in rockfix");

	return e-op;
}

static long
doread(u32int *arg, vlong *offp)
{
	int dir;
	long n, nn, nnn;
	uchar *p;
	Chan *c;
	vlong off;

	n = arg[2];
	p = uvalidaddr(arg[1], n, 1);
	c = fdtochan(arg[0], OREAD, 1, 1);

	if(waserror()){
		cclose(c);
		nexterror();
	}

	/*
	 * The offset is passed through on directories, normally.
	 * Sysseek complains, but pread is used by servers like exportfs,
	 * that shouldn't need to worry about this issue.
	 *
	 * Notice that c->devoffset is the offset that c's dev is seeing.
	 * The number of bytes read on this fd (c->offset) may be different
	 * due to rewritings in rockfix.
	 */
	if(offp == nil)	/* use and maintain channel's offset */
		off = c->offset;
	else
		off = *offp;
	if(off < 0)
		error(Enegoff);

	if(off == 0){	/* rewind to the beginning of the directory */
		if(offp == nil){
			c->offset = 0;
			c->devoffset = 0;
		}
		mountrewind(c);
		unionrewind(c);
	}

	dir = c->qid.type&QTDIR;
	if(dir && mountrockread(c, p, n, &nn)){
		/* do nothing: mountrockread filled buffer */
	}else{
		if(dir && c->umh)
			nn = unionread(c, p, n);
		else
			nn = devtab[c->type]->read(c, p, n, off);
	}
	if(dir)
		nnn = mountfix(c, p, nn, n);
	else
		nnn = nn;

	lock(&c->ref.lk);
	c->devoffset += nn;
	c->offset += nnn;
	unlock(&c->ref.lk);

	poperror();
	cclose(c);

	return nnn;
}

long
sys_read(u32int *arg)
{
	return doread(arg, nil);
}

long
syspread(u32int *arg)
{
	vlong v;

	// Plan 9 VX replaced dodgy varargs code
	v = *(vlong*)&arg[3];

	if(v == ~0ULL)
		return doread(arg, nil);

	return doread(arg, &v);
}

static long
dowrite(u32int *arg, vlong *offp)
{
	Chan *c;
	long m, n;
	vlong off;
	uchar *p;

	p = uvalidaddr(arg[1], arg[2], 0);
	n = 0;
	c = fdtochan(arg[0], OWRITE, 1, 1);
	if(waserror()) {
		if(offp == nil){
			lock(&c->ref.lk);
			c->offset -= n;
			unlock(&c->ref.lk);
		}
		cclose(c);
		nexterror();
	}

	if(c->qid.type & QTDIR)
		error(Eisdir);

	n = arg[2];

	if(offp == nil){	/* use and maintain channel's offset */
		lock(&c->ref.lk);
		off = c->offset;
		c->offset += n;
		unlock(&c->ref.lk);
	}else
		off = *offp;

	if(off < 0)
		error(Enegoff);

	m = devtab[c->type]->write(c, p, n, off);

	if(offp == nil && m < n){
		lock(&c->ref.lk);
		c->offset -= n - m;
		unlock(&c->ref.lk);
	}

	poperror();
	cclose(c);

	return m;
}

long
sys_write(u32int *arg)
{
	return dowrite(arg, nil);
}

long
syspwrite(u32int *arg)
{
	vlong v;

	// Plan 9 VX replaced dodgy varargs code
	v = *(vlong*)&arg[3];

	if(v == ~0ULL)
		return dowrite(arg, nil);

	return dowrite(arg, &v);
}

static void
sseek(vlong *ret, u32int *arg)
{
	Chan *c;
	uchar buf[sizeof(Dir)+100];
	Dir dir;
	int n;
	vlong off;
	union {
		vlong v;
		u32int u[2];
	} o;

	c = fdtochan(arg[1], -1, 1, 1);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	if(devtab[c->type]->dc == '|')
		error(Eisstream);

	off = 0;
	o.u[0] = arg[2];
	o.u[1] = arg[3];
	switch(arg[4]){
	case 0:
		off = o.v;
		if((c->qid.type & QTDIR) && off != 0)
			error(Eisdir);
		if(off < 0)
			error(Enegoff);
		c->offset = off;
		break;

	case 1:
		if(c->qid.type & QTDIR)
			error(Eisdir);
		lock(&c->ref.lk);	/* lock for read/write update */
		off = o.v + c->offset;
		if(off < 0){
			unlock(&c->ref.lk);
			error(Enegoff);
		}
		c->offset = off;
		unlock(&c->ref.lk);
		break;

	case 2:
		if(c->qid.type & QTDIR)
			error(Eisdir);
		n = devtab[c->type]->stat(c, buf, sizeof buf);
		if(convM2D(buf, n, &dir, nil) == 0)
			error("internal error: stat error in seek");
		off = dir.length + o.v;
		if(off < 0)
			error(Enegoff);
		c->offset = off;
		break;

	default:
		error(Ebadarg);
	}
	*ret = off;	/* caller translated arg[0] already */
	c->uri = 0;
	c->dri = 0;
	cclose(c);
	poperror();
}

long
sysseek(u32int *arg)
{
	sseek(uvalidaddr(arg[0], BY2V, 1), arg);
	return 0;
}

long
sysoseek(u32int *arg)
{
	union {
		vlong v;
		u32int u[2];
	} o;
	u32int a[5];

	o.v = (long)arg[1];
	a[0] = 0;
	a[1] = arg[0];
	a[2] = o.u[0];
	a[3] = o.u[1];
	a[4] = arg[2];
	sseek(&o.v, a);
	return o.v;
}

void
validstat(uchar *s, int n)
{
	int m;
	char buf[64];

	if(statcheck(s, n) < 0)
		error(Ebadstat);
	/* verify that name entry is acceptable */
	s += STATFIXLEN - 4*BIT16SZ;	/* location of first string */
	/*
	 * s now points at count for first string.
	 * if it's too long, let the server decide; this is
	 * only for his protection anyway. otherwise
	 * we'd have to allocate and waserror.
	 */
	m = GBIT16(s);
	s += BIT16SZ;
	if(m+1 > sizeof buf)
		return;
	memmove(buf, s, m);
	buf[m] = '\0';
	/* name could be '/' */
	if(strcmp(buf, "/") != 0)
		validname(buf, 0);
}

static char*
pathlast(Path *p)
{
	char *s;

	if(p == nil)
		return nil;
	if(p->len == 0)
		return nil;
	s = strrchr(p->s, '/');
	if(s)
		return s+1;
	return p->s;
}

long
sysfstat(u32int *arg)
{
	Chan *c;
	uint l;
	uchar *p;

	l = arg[2];
	p = uvalidaddr(arg[1], l, 1);
	c = fdtochan(arg[0], -1, 0, 1);
	if(waserror()) {
		cclose(c);
		nexterror();
	}
	l = devtab[c->type]->stat(c, p, l);
	poperror();
	cclose(c);
	return l;
}

long
sysstat(u32int *arg)
{
	char *name;
	Chan *c;
	uint l;
	uchar *p;

	l = arg[2];
	p = uvalidaddr(arg[1], l, 1);
	name = uvalidaddr(arg[0], 1, 0);
	c = namec(name, Aaccess, 0, 0);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	l = devtab[c->type]->stat(c, p, l);
	name = pathlast(c->path);
	if(name)
		l = dirsetname(name, strlen(name), p, l, arg[2]);

	poperror();
	cclose(c);
	return l;
}

long
syschdir(u32int *arg)
{
	Chan *c;
	char *name;

	name = uvalidaddr(arg[0], 1, 0);

	c = namec(name, Atodir, 0, 0);
	cclose(up->dot);
	up->dot = c;
	return 0;
}

long
bindmount(int ismount, int fd, int afd, char* arg0, char* arg1, ulong flag, char* spec)
{
	int ret;
	Chan *c0, *c1, *ac, *bc;
	struct{
		Chan	*chan;
		Chan	*authchan;
		char	*spec;
		int	flags;
	}bogus;

	if((flag&~MMASK) || (flag&MORDER)==(MBEFORE|MAFTER))
		error(Ebadarg);

	bogus.flags = flag & MCACHE;

	if(ismount){
		if(up->pgrp->noattach)
			error(Enoattach);

		ac = nil;
		bc = fdtochan(fd, ORDWR, 0, 1);
		if(waserror()) {
			if(ac)
				cclose(ac);
			cclose(bc);
			nexterror();
		}

		if(afd >= 0)
			ac = fdtochan(afd, ORDWR, 0, 1);

		bogus.chan = bc;
		bogus.authchan = ac;

		bogus.spec = spec;
		if(waserror())
			error(Ebadspec);
		spec = validnamedup(spec, 1);
		poperror();
		
		if(waserror()){
			free(spec);
			nexterror();
		}

		ret = devno('M', 0);
		c0 = devtab[ret]->attach((char*)&bogus);

		poperror();	/* spec */
		free(spec);
		poperror();	/* ac bc */
		if(ac)
			cclose(ac);
		cclose(bc);
	}else{
		bogus.spec = 0;
		c0 = namec(arg0, Abind, 0, 0);
	}

	if(waserror()){
		cclose(c0);
		nexterror();
	}

	c1 = namec(arg1, Amount, 0, 0);
	if(waserror()){
		cclose(c1);
		nexterror();
	}

	ret = cmount(&c0, c1, flag, bogus.spec);

	poperror();
	cclose(c1);
	poperror();
	cclose(c0);
	if(ismount)
		fdclose(fd, 0);

	return ret;
}

long
sysbind(u32int *arg)
{
	return bindmount(0, -1, -1, uvalidaddr(arg[0], 1, 0), uvalidaddr(arg[1], 1, 0), arg[2], nil);
}

long
sysmount(u32int *arg)
{
	return bindmount(1, arg[0], arg[1], nil, uvalidaddr(arg[2], 1, 0), arg[3], uvalidaddr(arg[4], 1, 0));
}

long
sys_mount(u32int *arg)
{
	return bindmount(1, arg[0], -1, nil, uvalidaddr(arg[1], 1, 0), arg[2], uvalidaddr(arg[3], 1, 0));
}

long
sysunmount(u32int *arg)
{
	Chan *cmount, *cmounted;
	char *mount, *mounted;

	cmounted = 0;

	mount = uvalidaddr(arg[1], 1, 0);
	cmount = namec(mount, Amount, 0, 0);

	if(arg[0]) {
		if(waserror()) {
			cclose(cmount);
			nexterror();
		}
		mounted = uvalidaddr(arg[0], 1, 0);
		/*
		 * This has to be namec(..., Aopen, ...) because
		 * if arg[0] is something like /srv/cs or /fd/0,
		 * opening it is the only way to get at the real
		 * Chan underneath.
		 */
		cmounted = namec(mounted, Aopen, OREAD, 0);
		poperror();
	}

	if(waserror()) {
		cclose(cmount);
		if(cmounted)
			cclose(cmounted);
		nexterror();
	}

	cunmount(cmount, cmounted);
	cclose(cmount);
	if(cmounted)
		cclose(cmounted);
	poperror();
	return 0;
}

long
syscreate(u32int *arg)
{
	int fd;
	Chan *c = 0;
	char *name;

	openmode(arg[1]&~OEXCL);	/* error check only; OEXCL okay here */
	if(waserror()) {
		if(c)
			cclose(c);
		nexterror();
	}
	name = uvalidaddr(arg[0], 1, 0);
	c = namec(name, Acreate, arg[1], arg[2]);
	fd = newfd(c);
	if(fd < 0)
		error(Enofd);
	poperror();
	return fd;
}

long
sysremove(u32int *arg)
{
	Chan *c;
	char *name;

	name = uvalidaddr(arg[0], 1, 0);
	c = namec(name, Aremove, 0, 0);
	/*
	 * Removing mount points is disallowed to avoid surprises
	 * (which should be removed: the mount point or the mounted Chan?).
	 */
	if(c->ismtpt){
		cclose(c);
		error(Eismtpt);
	}
	if(waserror()){
		c->type = 0;	/* see below */
		cclose(c);
		nexterror();
	}
	devtab[c->type]->remove(c);
	/*
	 * Remove clunks the fid, but we need to recover the Chan
	 * so fake it up.  rootclose() is known to be a nop.
	 */
	c->type = 0;
	poperror();
	cclose(c);
	return 0;
}

static long
wstat(Chan *c, uchar *d, int nd)
{
	long l;
	int namelen;

	if(waserror()){
		cclose(c);
		nexterror();
	}
	if(c->ismtpt){
		/*
		 * Renaming mount points is disallowed to avoid surprises
		 * (which should be renamed? the mount point or the mounted Chan?).
		 */
		dirname(d, &namelen);
		if(namelen)
			nameerror(chanpath(c), Eismtpt);
	}
	l = devtab[c->type]->wstat(c, d, nd);
	poperror();
	cclose(c);
	return l;
}

long
syswstat(u32int *arg)
{
	Chan *c;
	uint l;
	char *name;
	uchar *p;

	l = arg[2];
	p = uvalidaddr(arg[1], l, 0);
	validstat(p, l);
	name = uvalidaddr(arg[0], 1, 0);
	c = namec(name, Aaccess, 0, 0);
	return wstat(c, p, l);
}

long
sysfwstat(u32int *arg)
{
	Chan *c;
	uint l;
	uchar *p;

	l = arg[2];
	p = uvalidaddr(arg[1], l, 0);
	validstat(p, l);
	c = fdtochan(arg[0], -1, 1, 1);
	return wstat(c, p, l);
}

static void
packoldstat(uchar *buf, Dir *d)
{
	uchar *p;
	ulong q;

	/* lay down old stat buffer - grotty code but it's temporary */
	p = buf;
	strncpy((char*)p, d->name, 28);
	p += 28;
	strncpy((char*)p, d->uid, 28);
	p += 28;
	strncpy((char*)p, d->gid, 28);
	p += 28;
	q = d->qid.path & ~DMDIR;	/* make sure doesn't accidentally look like directory */
	if(d->qid.type & QTDIR)	/* this is the real test of a new directory */
		q |= DMDIR;
	PBIT32(p, q);
	p += BIT32SZ;
	PBIT32(p, d->qid.vers);
	p += BIT32SZ;
	PBIT32(p, d->mode);
	p += BIT32SZ;
	PBIT32(p, d->atime);
	p += BIT32SZ;
	PBIT32(p, d->mtime);
	p += BIT32SZ;
	PBIT64(p, d->length);
	p += BIT64SZ;
	PBIT16(p, d->type);
	p += BIT16SZ;
	PBIT16(p, d->dev);
}

long
sys_stat(u32int *arg)
{
	Chan *c;
	uint l;
	uchar buf[128];	/* old DIRLEN plus a little should be plenty */
	char strs[128], *name, *elem;
	Dir d;
	char old[] = "old stat system call - recompile";
	uchar *p;

	p = uvalidaddr(arg[1], 116, 1);
	name = uvalidaddr(arg[0], 1, 0);
	c = namec(name, Aaccess, 0, 0);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	l = devtab[c->type]->stat(c, buf, sizeof buf);
	/* buf contains a new stat buf; convert to old. yuck. */
	if(l <= BIT16SZ)	/* buffer too small; time to face reality */
		error(old);
	elem = pathlast(c->path);
	if(elem)
		l = dirsetname(elem, strlen(elem), buf, l, sizeof buf);
	l = convM2D(buf, l, &d, strs);
	if(l == 0)
		error(old);
	packoldstat(p, &d);
	
	poperror();
	cclose(c);
	return 0;
}

long
sys_fstat(u32int *arg)
{
	Chan *c;
	char *name;
	uint l;
	uchar buf[128];	/* old DIRLEN plus a little should be plenty */
	char strs[128];
	Dir d;
	char old[] = "old fstat system call - recompile";
	uchar *p;

	p = uvalidaddr(arg[1], 116, 1);
	c = fdtochan(arg[0], -1, 0, 1);
	if(waserror()){
		cclose(c);
		nexterror();
	}
	l = devtab[c->type]->stat(c, buf, sizeof buf);
	/* buf contains a new stat buf; convert to old. yuck. */
	if(l <= BIT16SZ)	/* buffer too small; time to face reality */
		error(old);
	name = pathlast(c->path);
	if(name)
		l = dirsetname(name, strlen(name), buf, l, sizeof buf);
	l = convM2D(buf, l, &d, strs);
	if(l == 0)
		error(old);
	packoldstat(p, &d);
	
	poperror();
	cclose(c);
	return 0;
}

long
sys_wstat(u32int *u)
{
	error("old wstat system call - recompile");
	return -1;
}

long
sys_fwstat(u32int *u)
{
	error("old fwstat system call - recompile");
	return -1;
}

// Plan 9 VX additions
long
kbind(char *new, char *old, int flag)
{
	return bindmount(0, -1, -1, new, old, flag, nil);
}

long
syspassfd(u32int *u)
{
	error("passfd unimplemented");
	return -1;
}

