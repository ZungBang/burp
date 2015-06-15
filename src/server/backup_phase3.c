#include "include.h"
#include "manio.h"
#include "sdirs.h"

static const char *get_rmanifest_relative(struct sdirs *sdirs,
	struct conf **confs)
{
	const char *cp;
	cp=sdirs->rmanifest+strlen(get_string(confs[OPT_DIRECTORY]));
	while(cp && *cp=='/') cp++;
	return cp;
}

// Combine the phase1 and phase2 files into a new manifest.
int backup_phase3_server_all(struct sdirs *sdirs, struct conf **confs)
{
	int ret=-1;
	int pcmp=0;
	struct blk *blk=NULL;
	struct sbuf *usb=NULL;
	struct sbuf *csb=NULL;
	char *manifesttmp=NULL;
	struct manio *newmanio=NULL;
	struct manio *chmanio=NULL;
	struct manio *unmanio=NULL;
	enum protocol protocol=get_protocol(confs);
	const char *rmanifest_relative=NULL;

	logp("Begin phase3 (merge manifests)\n");

	if(protocol==PROTO_2)
		rmanifest_relative=get_rmanifest_relative(sdirs, confs);

	if(!(manifesttmp=get_tmp_filename(sdirs->manifest))
	  || !(newmanio=manio_open_phase3(manifesttmp, comp_level(confs),
		protocol, rmanifest_relative))
	  || !(chmanio=manio_open_phase2(sdirs->changed, "rb", protocol))
	  || !(unmanio=manio_open_phase2(sdirs->unchanged, "rb", protocol))
	  || !(usb=sbuf_alloc(confs))
	  || !(csb=sbuf_alloc(confs)))
		goto end;

	while(chmanio || unmanio)
	{
		if(!blk && !(blk=blk_alloc())) goto end;

		if(unmanio
		  && !usb->path.buf)
		{
			switch(manio_read(unmanio, usb, confs))
			{
				case -1: goto end;
				case 1: manio_close(&unmanio);
			}
		}

		if(chmanio
		  && !csb->path.buf)
		{
			switch(manio_read(chmanio, csb, confs))
			{
				case -1: goto end;
				case 1: manio_close(&chmanio);
			}
		}

		if(usb->path.buf && !csb->path.buf)
		{
			if(write_status(CNTR_STATUS_MERGING,
				usb->path.buf, confs)) goto end;
			switch(manio_copy_entry(NULL /* no async */,
				usb, usb, &blk, unmanio, newmanio, confs))
			{
				case -1: goto end;
				case 1: manio_close(&unmanio);
			}
		}
		else if(!usb->path.buf && csb->path.buf)
		{
			if(write_status(CNTR_STATUS_MERGING,
				csb->path.buf, confs)) goto end;
			switch(manio_copy_entry(NULL /* no async */,
				csb, csb, &blk, chmanio, newmanio, confs))
			{
				case -1: goto end;
				case 1: manio_close(&chmanio);
			}
		}
		else if(!usb->path.buf && !csb->path.buf)
		{
			continue;
		}
		else if(!(pcmp=sbuf_pathcmp(usb, csb)))
		{
			// They were the same - write one.
			if(write_status(CNTR_STATUS_MERGING,
				csb->path.buf, confs)) goto end;
			switch(manio_copy_entry(NULL /* no async */,
				csb, csb, &blk, chmanio, newmanio, confs))
			{
				case -1: goto end;
				case 1: manio_close(&chmanio);
			}
		}
		else if(pcmp<0)
		{
			if(write_status(CNTR_STATUS_MERGING,
				usb->path.buf, confs)) goto end;
			switch(manio_copy_entry(NULL /* no async */,
				usb, usb, &blk, unmanio, newmanio, confs))
			{
				case -1: goto end;
				case 1: manio_close(&unmanio);
			}
		}
		else
		{
			if(write_status(CNTR_STATUS_MERGING,
				csb->path.buf, confs)) goto end;
			switch(manio_copy_entry(NULL /* no async */,
				csb, csb, &blk, chmanio, newmanio, confs))
			{
				case -1: goto end;
				case 1: manio_close(&chmanio);
			}
		}
	}

	// Flush to disk.
	if(manio_close(&newmanio))
	{
		logp("error gzclosing %s in backup_phase3_server\n",
			manifesttmp);
		goto end;
	}

	// Rename race condition should be of no consequence here, as the
	// manifest should just get recreated automatically.
	if(do_rename(manifesttmp, sdirs->manifest))
		goto end;
	else
	{
		recursive_delete(sdirs->changed, NULL, 1);
		recursive_delete(sdirs->unchanged, NULL, 1);
	}

	logp("End phase3 (merge manifests)\n");
	ret=0;
end:
	manio_close(&newmanio);
	manio_close(&chmanio);
	manio_close(&unmanio);
	sbuf_free(&csb);
	sbuf_free(&usb);
	blk_free(&blk);
	free_w(&manifesttmp);
	return ret;
}