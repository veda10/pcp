/*
 * Copyright (c) 2017,2021 Red Hat.
 * Copyright (c) 1995-2003 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "logger.h"
#include "pmda.h"

/*
 * this routine creates the "fake" pmResult records to be added to the
 * start (prologue) and end (epilogue) of the archive log to identify
 * information about the archive beyond what is in the archive label.
 */

/* encode the domain(x), cluster (y) and item (z) parts of the PMID */
#define PMID(x,y,z) ((x<<22)|(y<<10)|z)

/* encode the domain(x) and serial (y) parts of the pmInDom */
#define INDOM(x,y) ((x<<22)|y)

/*
 * Note: these pmDesc entries MUST correspond to the corrsponding
 *	 entries from the real PMDA ...
 *	 We fake it out here to accommodate logging from PCP 1.1
 *	 PMCD's and to avoid round-trip dependencies in setting up
 *	 the epilogue/prologue metadata
 */
static pmDesc	desc[] = {
/* pmcd.pmlogger.host */
    { PMID(2,3,3), PM_TYPE_STRING, INDOM(2,1), PM_SEM_DISCRETE, {0,0,0,0,0,0} },
/* pmcd.pmlogger.port */
    { PMID(2,3,0), PM_TYPE_U32, INDOM(2,1), PM_SEM_DISCRETE, {0,0,0,0,0,0} },
/* pmcd.pmlogger.archive */
    { PMID(2,3,2), PM_TYPE_STRING, INDOM(2,1), PM_SEM_DISCRETE, {0,0,0,0,0,0} },
/* pmcd.pid */
    { PMID(2,0,23), PM_TYPE_U64, PM_INDOM_NULL, PM_SEM_DISCRETE, PMDA_PMUNITS(0,0,0,0,0,0) },
/* pmcd.seqnum */
    { PMID(2,0,24), PM_TYPE_U32, PM_INDOM_NULL, PM_SEM_DISCRETE, PMDA_PMUNITS(0,0,0,0,0,0) },
};
/* names added for version 2 archives */
static char*	names[] = {
"pmcd.pmlogger.host",
"pmcd.pmlogger.port",
"pmcd.pmlogger.archive",
"pmcd.pid",
"pmcd.seqnum"
};

static int	n_metric = sizeof(desc) / sizeof(desc[0]);

#define PROLOGUE 1
#define EPILOGUE 2

int
do_logue(int type)
{
    int		sts;
    int		i;
    int		j;
    pid_t	mypid = getpid();
#if 0		// TODO when __pmEncodeResult => __pmResult
    __pmResult	*res;			/* the output pmResult */
#else
    pmResult	*res;			/* the output pmResult */
    __pmResult	*__res;			/* temporary pointer */
#endif
    pmResult	*res_pmcd = NULL;	/* for values from pmcd */
    __pmPDU	*pb;
    pmAtomValue	atom;
    char	path[MAXPATHLEN];
    char	host[MAXHOSTNAMELEN];
    long	offset;
    int		free_cp;
    __pmLogInDom_io	lid;

    /* start to build the pmResult */
    if ((__res = __pmAllocResult(n_metric)) == NULL)
	return -oserror();
    res = __pmOffsetResult(__res);

    res->numpmid = n_metric;
    if (type == PROLOGUE) {
	last_stamp = res->timestamp = epoch;	/* struct assignment */
	lid.stamp.sec = (__int32_t)epoch.tv_sec;
	lid.stamp.nsec = (__int32_t)epoch.tv_usec * 1000;
    }
    else {
	res->timestamp = last_stamp;	/* struct assignment */
	/*
	 * epilogue, last plus 1msec as the default ... hope pmFetch gives
	 * us a better answer below
	 */
	res->timestamp.tv_usec += 1000;
	if (res->timestamp.tv_usec > 999999) {
	    res->timestamp.tv_usec -= 1000000;
	    res->timestamp.tv_sec++;
	}
    }

    for (i = 0; i < n_metric; i++)
	res->vset[i] = NULL;

    for (i = 0; i < n_metric; i++) {
	res->vset[i] = (pmValueSet *)malloc(sizeof(pmValueSet));
	if (res->vset[i] == NULL) {
	    sts = -oserror();
	    goto done;
	}
	res->vset[i]->pmid = desc[i].pmid;
	res->vset[i]->numval = 1;
	/* special case for each value 0 .. n_metric-1 */
	free_cp = 0;
	if (desc[i].pmid == PMID(2,3,3)) {
	    __pmHostEnt *servInfo;
	    /* my fully qualified hostname, cloned from the pmcd PMDA */
	    (void)gethostname(host, MAXHOSTNAMELEN);
	    host[MAXHOSTNAMELEN-1] = '\0';
	    if ((servInfo = __pmGetAddrInfo(host)) == NULL)
		atom.cp = host;
	    else {
		atom.cp = __pmHostEntGetName(servInfo);
		__pmHostEntFree(servInfo);
		if (atom.cp == NULL)
		    atom.cp = host;
		else
		    free_cp = 1;
	    }
	    res->vset[i]->vlist[0].inst = (int)mypid;
	 }
	 else if (desc[i].pmid == PMID(2,3,0)) {
	    /* my control port number, from ports.c */
	    atom.l = ctlport;
	    res->vset[i]->vlist[0].inst = (int)mypid;
	 }
	 else if (desc[i].pmid == PMID(2,3,2)) {
	    /*
	     * the full pathname to the base of the archive, cloned
	     * from GetPort() in ports.c
	     */
	    if (__pmAbsolutePath(archName))
		atom.cp = archName;
	    else {
		if (getcwd(path, MAXPATHLEN) == NULL)
		    atom.cp = archName;
		else {
		    strncat(path, "/", 2);
		    strncat(path, archName, MAXPATHLEN-strlen(path));
		    atom.cp = path;
		}
	    }
	    res->vset[i]->vlist[0].inst = (int)mypid;
	}
	else if (desc[i].pmid == PMID(2,0,23)) {
	    pmID	pmid[2];
	    /*
	     * pmcd.pid and pmcd.seqnum we need from pmcd ...
	     */
	    pmid[0] = PMID(2,0,23);
	    pmid[1] = PMID(2,0,24);
	    // TODO needs to be pmHighResFetch()
	    sts = pmFetch(2, pmid, &res_pmcd);
	    if (sts >= 0 && type == EPILOGUE) {
		last_stamp = res->timestamp = res_pmcd->timestamp;	/* struct assignment */
	    }
	    if (sts >= 0 && res_pmcd->vset[0]->numval == 1 &&
	        (res_pmcd->vset[0]->valfmt == PM_VAL_SPTR || res_pmcd->vset[0]->valfmt == PM_VAL_DPTR))
		memcpy(&atom.ull, res_pmcd->vset[0]->vlist[0].value.pval->vbuf, sizeof(atom.ull));
	    else
		atom.ull = 0;
	    res->vset[i]->vlist[0].inst = PM_IN_NULL;
	}
	else if (desc[i].pmid == PMID(2,0,24)) {
	    if (res_pmcd != NULL && res_pmcd->vset[1]->numval == 1 && res_pmcd->vset[1]->valfmt == PM_VAL_INSITU)
		memcpy(&atom.ul, &res_pmcd->vset[1]->vlist[0].value.lval, sizeof(atom.ul));
	    else
		atom.ul = 0;
	    res->vset[i]->vlist[0].inst = PM_IN_NULL;
	}

	sts = __pmStuffValue(&atom, &res->vset[i]->vlist[0], desc[i].type);
	if (free_cp)
	    free(atom.cp);
	if (sts < 0)
	    goto done;
	res->vset[i]->valfmt = sts;
    }

    if ((sts = __pmEncodeResult(__pmFileno(archctl.ac_mfp), res, &pb)) < 0)
	goto done;

    __pmOverrideLastFd(__pmFileno(archctl.ac_mfp));	/* force use of log version */
    /* and write to the archive data file ... */
    last_log_offset = __pmFtell(archctl.ac_mfp);
    sts = __pmLogPutResult2(&archctl, pb);
    __pmUnpinPDUBuf(pb);
    if (sts < 0)
	goto done;

    if (type == PROLOGUE) {
	for (i = 0; i < n_metric; i++) {
	    if ((sts = __pmLogPutDesc(&archctl, &desc[i], 1, &names[i])) < 0)
		goto done;
	    if (desc[i].indom == PM_INDOM_NULL)
		continue;
	    for (j = 0; j < i; j++) {
		if (desc[i].indom == desc[j].indom)
		    break;
	    }
	    if (j == i) {
		/* need indom ... force one with my PID as the only instance */
		int		*instid;
		char		**instname;
		int		pdu_type;

		if ((instid = (int *)malloc(sizeof(*instid))) == NULL) {
		    sts = -oserror();
		    goto done;
		}
		*instid = (int)mypid;
		pmsprintf(path, sizeof(path), "%" FMT_PID, mypid);
		if ((instname = (char **)malloc(sizeof(char *)+strlen(path)+1)) == NULL) {
		    free(instid);
		    sts = -oserror();
		    goto done;
		}
		/*
		 * this _is_ correct ... instname[] is a one element array
		 * with the string value immediately following
		 */
		instname[0] = (char *)&instname[1];
		strcpy(instname[0], path);
		/*
		 * Note.	DO NOT free instid[] and instname[]... they
		 *		get hidden away in addindom() below
		 *		__pmLogPutInDom()
		 */
		if (archive_version == PM_LOG_VERS03)
		    pdu_type = TYPE_INDOM;
		else
		    pdu_type = TYPE_INDOM_V2;
		lid.indom = desc[i].indom;
		lid.numinst = 1;
		lid.instlist = instid;
		lid.namelist = instname;
		if ((sts = __pmLogPutInDom(&archctl, pdu_type, &lid)) < 0)
		    goto done;
	    }
	}

	/* fudge the temporal index */
	offset = __pmLogLabelSize(&logctl);
	__pmFseek(archctl.ac_mfp, offset, SEEK_SET);
	__pmFseek(logctl.mdfp, offset, SEEK_SET);
	__pmLogPutIndex(&archctl, &lid.stamp);
	__pmFseek(archctl.ac_mfp, 0L, SEEK_END);
	__pmFseek(logctl.mdfp, 0L, SEEK_END);
    }

    sts = 0;

    /*
     * and now free stuff
     */
done:
    for (i = 0; i < n_metric; i++) {
	if (res->vset[i] != NULL) {
	    if (res->vset[i]->valfmt == PM_VAL_DPTR)
		free(res->vset[i]->vlist[0].value.pval);
	    free(res->vset[i]);
	}
    }
    res->numpmid = 0;		/* don't free vset's */
    pmFreeResult(res);
    if (res_pmcd != NULL)
	pmFreeResult(res_pmcd);

    return sts;
}

int
do_prologue(void)
{
    return do_logue(PROLOGUE);
}

int
do_epilogue(void)
{
    return do_logue(EPILOGUE);
}
