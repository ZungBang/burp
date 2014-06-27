#include "include.h"
#include "champ_chooser/include.h"
#include "../auth.h"
#include "../backup_phase1.h"
#include "../bu.h"

static int write_incexc(const char *realworking, const char *incexc)
{
	int ret=-1;
	FILE *fp=NULL;
	char *path=NULL;
	if(!(path=prepend_s(realworking, "incexc")))
		goto end;
	if(!(fp=open_file(path, "wb")))
		goto end;
	fprintf(fp, "%s", incexc);
	ret=0;
end:
	if(close_fp(&fp))
	{
		logp("error writing to %s in write_incexc\n", path);
		ret=-1;
	}
	if(path) free(path);
	return ret;
}

// Also used by the burp1 stuff.
int open_log(struct asfd *asfd, const char *realworking, struct conf *cconf)
{
	char *logpath=NULL;

	if(!(logpath=prepend_s(realworking, "log")))
	{
		log_and_send_oom(asfd, __func__);
		return -1;
	}
	if(set_logfp(logpath, cconf))
	{
		char msg[256]="";
		snprintf(msg, sizeof(msg),
			"could not open log file: %s", logpath);
		log_and_send(asfd, msg);
		free(logpath);
		return -1;
	}
	free(logpath);

	logp("Client version: %s\n", cconf->peer_version?:"");
	logp("Protocol: %d\n", cconf->protocol);
	// Make sure a warning appears in the backup log.
	// The client will already have been sent a message with logw.
	// This time, prevent it sending a logw to the client by specifying
	// NULL for cntr.
	if(cconf->version_warn) version_warn(asfd, NULL, cconf);

	return 0;
}

static struct asfd *setup_champ_chooser(struct async *as,
	struct sdirs *sdirs, struct conf *conf)
{
	int champsock=-1;
	char *champname=NULL;
	struct asfd *chfd=NULL;

	// Connect to champ chooser now.
	// This may start up a new champ chooser. On a machine with multiple
	// cores, it may be faster to do now, way before it is actually needed
	// in phase2.
	if((champsock=connect_to_champ_chooser(sdirs, conf))<0)
	{
		logp("could not connect to champ chooser\n");
		goto error;
	}

	if(!(chfd=asfd_alloc())
	  || chfd->init(chfd, "champ chooser socket",
		as, champsock, NULL /* no SSL */, conf))
			goto error;
	as->asfd_add(as, chfd);

	if(!(champname=prepend("cname",
		conf->cname, strlen(conf->cname), ":")))
			goto error;

	if(chfd->write_str(chfd, CMD_GEN, champname)
	  || chfd->read_expect(chfd, CMD_GEN, "cname ok"))
		goto error;

	free(champname);
	return chfd;
error:
	free(champname);
	as->asfd_remove(as, chfd);
	asfd_free(&chfd);
	close_fd(&champsock);
	return NULL;
}

int do_backup_server_burp2(struct async *as, struct sdirs *sdirs,
	struct conf *cconf, const char *incexc, int resume)
{
	int ret=0;
	char msg[256]="";
	// Real path to the working directory
	char *realworking=NULL;
	// Real path to the manifest directory
	char *manifest_dir=NULL;
	char tstmp[64]="";
	struct asfd *chfd=NULL;
	struct asfd *asfd=as->asfd;

	logp("in do_backup_server\n");

	if(timestamp_get_new(asfd, sdirs, cconf, tstmp, sizeof(tstmp)))
		goto error;
	if(!(realworking=prepend_s(sdirs->client, tstmp))
	 || !(manifest_dir=prepend_s(realworking, "manifest")))
	{
		log_and_send_oom(asfd, __func__);
		goto error;
	}

	// Add the working symlink before creating the directory.
	// This is because bedup checks the working symlink before
	// going into a directory. If the directory got created first,
	// bedup might go into it in the moment before the symlink
	// gets added.
	if(symlink(tstmp, sdirs->working)) // relative link to the real work dir
	{
		snprintf(msg, sizeof(msg),
		  "could not point working symlink to: %s",
		  realworking);
		log_and_send(asfd, msg);
		goto error;
	}
	else if(mkdir(realworking, 0777))
	{
		snprintf(msg, sizeof(msg),
		  "could not mkdir for next backup: %s", sdirs->working);
		log_and_send(asfd, msg);
		unlink(sdirs->working);
		goto error;
	}
	else if(open_log(asfd, realworking, cconf))
	{
		goto error;
	}
	else if(timestamp_write(sdirs->timestamp, tstmp))
	{
		snprintf(msg, sizeof(msg),
		  "unable to write timestamp %s", sdirs->timestamp);
		log_and_send(asfd, msg);
		goto error;
	}
	else if(incexc && *incexc && write_incexc(realworking, incexc))
	{
		snprintf(msg, sizeof(msg), "unable to write incexc");
		log_and_send(asfd, msg);
		goto error;
	}

	if(!(chfd=setup_champ_chooser(as, sdirs, cconf)))
	{
		log_and_send(asfd, "problem connecting to champ chooser");
		goto error;
	}

	if(backup_phase1_server(asfd, sdirs, cconf))
	{
		logp("error in phase1\n");
		goto error;
	}

	if(backup_phase2_server(as, sdirs, manifest_dir, resume, cconf))
	{
		logp("error in phase2\n");
		goto error;
	}

	// Close the connection with the client, the rest of the job
	// we can do by ourselves.
	as->asfd_remove(as, asfd);
	asfd_close(asfd);

	if(backup_phase3_server(sdirs, manifest_dir, cconf))
	{
		logp("error in phase3\n");
		goto end;
	}

	cntr_stats_to_file(cconf->cntr, sdirs->working, ACTION_BACKUP);

	// Move the symlink to indicate that we are now finished.
	// FIX THIS: check whether the race condition here means that the
	// backup is not automatically recoverable.
	if(do_rename(sdirs->working, sdirs->current)) goto error;

	cntr_print(cconf->cntr, ACTION_BACKUP);

	logp("Backup completed.\n");

	set_logfp(NULL, cconf); // does an fclose on logfp.
	compress_filename(sdirs->current, "log", "log.gz", cconf);

	if(cconf->keep>0)
	{
		//ret=delete_old_backups(sdirs, cconf);
		// FIX THIS: Need to figure out which data files can be
		// deleted.
	}

	goto end;
error:
	ret=-1;
end:
	set_logfp(NULL, cconf);
	if(manifest_dir) free(manifest_dir);
	if(realworking) free(realworking);
	as->asfd_remove(as, chfd);
	asfd_free(&chfd);
	return ret;
}
