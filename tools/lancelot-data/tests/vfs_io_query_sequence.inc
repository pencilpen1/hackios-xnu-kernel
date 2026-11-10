/* Unmodified query-and-limit application excerpt from XNU 8796.121.2
 * bsd/vfs/vfs_subr.c, source SHA256 9e2f9734713f5e1cae06997c214b90575cea27483235736cdea742dd1d7231bf. */
	mp->mnt_maxreadcnt = MAX_UPL_SIZE_BYTES;
	mp->mnt_maxwritecnt = MAX_UPL_SIZE_BYTES;

	if (VNOP_IOCTL(devvp, DKIOCISVIRTUAL, (caddr_t)&isvirtual, 0, ctx) == 0) {
		if (isvirtual) {
			mp->mnt_kern_flag |= MNTK_VIRTUALDEV;
			mp->mnt_flag |= MNT_REMOVABLE;
		}
	}
	if (VNOP_IOCTL(devvp, DKIOCISSOLIDSTATE, (caddr_t)&isssd, 0, ctx) == 0) {
		if (isssd) {
			mp->mnt_kern_flag |= MNTK_SSD;
		}
	}
	if ((error = VNOP_IOCTL(devvp, DKIOCGETFEATURES,
	    (caddr_t)&features, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXBLOCKCOUNTREAD,
	    (caddr_t)&readblockcnt, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXBLOCKCOUNTWRITE,
	    (caddr_t)&writeblockcnt, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXBYTECOUNTREAD,
	    (caddr_t)&readmaxcnt, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXBYTECOUNTWRITE,
	    (caddr_t)&writemaxcnt, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXSEGMENTCOUNTREAD,
	    (caddr_t)&readsegcnt, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXSEGMENTCOUNTWRITE,
	    (caddr_t)&writesegcnt, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXSEGMENTBYTECOUNTREAD,
	    (caddr_t)&readsegsize, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMAXSEGMENTBYTECOUNTWRITE,
	    (caddr_t)&writesegsize, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETMINSEGMENTALIGNMENTBYTECOUNT,
	    (caddr_t)&alignment, 0, ctx))) {
		return error;
	}

	if ((error = VNOP_IOCTL(devvp, DKIOCGETCOMMANDPOOLSIZE,
	    (caddr_t)&ioqueue_depth, 0, ctx))) {
		return error;
	}

	if (readmaxcnt) {
		mp->mnt_maxreadcnt = (readmaxcnt > UINT32_MAX) ? UINT32_MAX :(uint32_t) readmaxcnt;
	}

	if (readblockcnt) {
		temp = readblockcnt * blksize;
		temp = (temp > UINT32_MAX) ? UINT32_MAX : temp;

		if (temp < mp->mnt_maxreadcnt) {
			mp->mnt_maxreadcnt = (u_int32_t)temp;
		}
	}

	if (writemaxcnt) {
		mp->mnt_maxwritecnt = (writemaxcnt > UINT32_MAX) ? UINT32_MAX : (uint32_t)writemaxcnt;
	}

	if (writeblockcnt) {
		temp = writeblockcnt * blksize;
		temp = (temp > UINT32_MAX) ? UINT32_MAX : temp;

		if (temp < mp->mnt_maxwritecnt) {
			mp->mnt_maxwritecnt = (u_int32_t)temp;
		}
	}
