/**
*  This file is part of rmlint.
*
*  rmlint is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  rmlint is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with rmlint.  If not, see <http://www.gnu.org/licenses/>.
*
** Author: Christopher Pahl <sahib@online.de>:
** Hosted at the time of writing (Do 30. Sep 18:32:19 CEST 2010):
*  on http://github.com/sahib/rmlint
*
**/

/* Needed for nftw() */
#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <regex.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>

#include "rmlint.h"
#include "filter.h"
#include "mode.h"
#include "defs.h"
#include "list.h"


uint32 dircount = 0;
short iinterrupt = 0;
bool db_done = false; 

/*
 * Callbock from signal()
 * Counts number of CTRL-Cs and reacts
 */
static void interrupt(int p)
{
        /** This was connected with SIGINT (CTRL-C) in recurse_dir() **/
		if(db_done)  {
			warning(YEL"\nINFO: "NCO"Received Interrupt.\n");
			die(0);
		}
 
        switch(iinterrupt) {
        case 0:
                warning(GRE"\nINFO: "NCO"Received Interrupt.\n");
                break;
        case 1:
                die(1);
        }
        iinterrupt++;
}

/*
 * grep the string "string" to see if it contains the pattern.
 * Will return 0 if yes, 1 otherwise.
 */
int regfilter(const char* input, const char *pattern)
{
        int status;
        int flags = REG_EXTENDED|REG_NOSUB;
        const char *string;

        if(!pattern) {
                return 0;
        } else {
                regex_t re;
				string = basename(input);
				
                if(!set.casematch)
                        flags |= REG_ICASE;

                if(regcomp(&re, pattern, flags))
                        return 0;

                if( (status = regexec(&re, string, (size_t)0, NULL, 0)) != 0) {
                        if(status != REG_NOMATCH) {
                                char err_buff[100];
                                regerror(status, &re, err_buff, 100);
                                warning(YEL"WARN: "NCO" Invalid regex pattern: '%s'\n", err_buff);
                        }
                }

                regfree(&re);
                return (set.invmatch) ? !(status) : (status);
        }
}

/*
 * Callbock from nftw()
 * If the file given from nftw by "path":
 * - is a directory: recurs into it and catch the files there,
 * 	as long the depth doesnt get bigger than max_depth and contains the pattern  cmp_pattern
 * - a file: Push it back to the list, if it has "cmp_pattern" in it. (if --regex was given)
 * If the user interrupts, nftw() stops, and the program will do it'S magic.
 * 
 * 
 * Appendum: rmlint used the Inode to sort the contents before doing any I/O to speed up things. 
 * 			 This is nevertheless limited to Unix filesystems like ext*, reiserfs.. (might work in MacOSX - don't know) 
 * 			 If someone likes to port this to Windows he would to replace the inode number by the MFT entry point, or simply disable it 
 * 			 I personally don't have a Windowsmachine and wn't port it, as I don't found many users knowing what that black window with white 
 *           lines can do ;-) 
 */
static int eval_file(const char *path, const struct stat *ptr, int flag, struct FTW *ftwbuf)
{
        if(set.depth && ftwbuf->level > set.depth) {
                /* Do not recurse in this subdir */
                return FTW_SKIP_SIBLINGS;
        }
        if(iinterrupt) {
                return FTW_STOP;
        }
        if(flag == FTW_SLN) {
				list_append(path, 0,ptr->st_dev,ptr->st_ino,42);
				return FTW_CONTINUE;
        }
        if(path) {
				/* Check this to be a valid file and NOT a blockfile (reading /dev/null does funny things) */
                if(flag == FTW_F && ptr->st_rdev == 0) {
                        if(!regfilter(path, set.fpattern) && !access(path,R_OK)) {
								if(set.ignore_hidden) {
										char *base = basename(path); 
										if(*base != '.') { 
												dircount++;
												list_append(path, ptr->st_size,ptr->st_dev,ptr->st_ino,1);
										}
								} else {
										dircount++;
										list_append(path, ptr->st_size,ptr->st_dev,ptr->st_ino,1);
								}
                        }
                        return FTW_CONTINUE;
                }
        }
        if(flag == FTW_D) {
                if((regfilter(path,set.dpattern) && strcmp(path,set.paths[get_cpindex()]) != 0)) {
                        return FTW_SKIP_SUBTREE;
                }
                if(set.ignore_hidden && path) 
                {
					char *base = basename(path); 
					if(*base == '.') { 
						return FTW_SKIP_SUBTREE;
					}
				}
        }
        return FTW_CONTINUE;
}

/* This calls basically nftw() and sets some options */
int recurse_dir(const char *path)
{
        /* Set options */
        int flags = FTW_ACTIONRETVAL;
        if(!set.followlinks)
                flags |= FTW_PHYS;

        if(USE_DEPTH_FIRST)
                flags |= FTW_DEPTH;

        if(set.samepart)
                flags |= FTW_MOUNT;

        /* Handle SIGINT */
        signal(SIGINT, interrupt);

        /* Start recurse */
        if( nftw(path, eval_file, _XOPEN_SOURCE, flags) == -1) {
                warning(YEL"FATAL: "NCO"nftw() failed with: %s\n", strerror(errno));
                return EXIT_FAILURE;
        }

        return dircount;
}


/* If we have more than one path, several iFiles  *
 *  may point to the same (physically same file.  *
 *  This woud result in false positves - Kick'em  */
static uint32 rm_double_paths(file_group *fp)
{
        iFile *b = fp->grp_stp;
        uint32 c = 0;

        if(b) {
                while(b->next) {
                        if( (b->node == b->next->node) &&
                            (b->dev  == b->next->dev )  ) {
                                iFile *tmp = b;
                                b = list_remove(b);

                                if(tmp == fp->grp_stp)
                                        fp->grp_stp = b;
                                if(tmp == fp->grp_enp)
                                        fp->grp_enp = b;

                                c++;
                        } else {
                                b=b->next;
                        }
                }
        }
        return c;
}

/* Sort criteria for sorting by dev and inode */
static long cmp_nd(iFile *a, iFile *b)
{
        return ((long)(a->dev*a->node) - (long)(b->dev*b->node));
}

/* Compares the "fp" array of the iFiles a and b */
static int cmp_fingerprints(iFile *a,iFile *b)
{
        int i,j;
        for(i=0; i<2; i++) {
                for(j=0; j<MD5_LEN; j++) {
                        if(a->fp[i][j] != b->fp[i][j]) {
                                return  0;
                        }
                }
        }
        
#if BYTE_IN_THE_MIDDLE 
        for(i=0; i<BYTE_MIDDLE_SIZE; i++) {
				if(a->bim[i] != b->bim[i]) {
						return 0; 
			    }
		}
#endif 

        return 1;
}

/* Performs a fingerprint check on the group fp */
static uint32 group_filter(file_group *fp)
{
        iFile *p = fp->grp_stp;
        iFile *i,*j;

        uint32 remove_count = 0;
        uint32 fp_sz;

        if(!fp || fp->grp_stp == NULL)
                return 0;

        /* The size read in to build a fingerprint */
        fp_sz = MD5_FPSIZE_FORM(fp->grp_stp->fsize); 

        /* Clamp it to some maximum (4KB) */
        fp_sz = (fp_sz > MD5_FP_MAX_RSZ) ? MD5_FP_MAX_RSZ : fp_sz;

        while(p) {
                md5_fingerprint(p, fp_sz);
                p=p->next;
        }

        /* Compare each other */
        i = fp->grp_stp;

        while(i) {
                if(i->filter) {
                        j=i->next;
                        while(j) {
                                if(j->filter) {
                                        if(cmp_fingerprints(i,j)) {
                                                /* i 'similiar' to j */
                                                j->filter = false;
                                                i->filter = false;
                                        }
                                }
                                j = j->next;
                        }
                        if(i->filter) {
                                iFile *tmp = i;
                                fp->len--;
                                fp->size -= i->fsize;

                                i = list_remove(i);

                                /* Update start / end */
                                if(tmp == fp->grp_stp)
                                        fp->grp_stp = i;

                                if(tmp == fp->grp_enp)
                                        fp->grp_enp = i;

                                remove_count++;
                                continue;
                        } else {
                                i=i->next;
                                continue;
                        }
                }
                i=i->next;
        }
        return remove_count;
}

/* Callback from build_checksums */
static void *cksum_cb(void * vp)
{
        file_group *gp = vp;
        iFile *file = gp->grp_stp;

        int i = 0;
        for(; i < gp->size && file != NULL; i++) {
                md5_file(file);
                file=file->next;
        }
        return NULL;
}

static void build_checksums(file_group *grp)
{
        if(grp == NULL || grp->grp_stp == NULL)
                return;

        if(set.threads == 1 ||  grp->size < MD5_MTHREAD_SIZE) {
                /* Just loop through this group and built the checksum */
                iFile *p = grp->grp_stp;
                while(p) {
                        md5_file(p);
                        p=p->next;
                }
        } else {
                /* The Group is a bigger one (over 4MB by default),
                 * so it may be more efficient wo work this on more
                 * than one thread (especially with serial IO */

                iFile *ptr = grp->grp_stp;
                int ii = 0, jj = 0;
                int packsize = grp->size / MD5_MTHREAD_SIZE;

                pthread_t *tt = malloc( (grp->len / packsize + 2) * sizeof(pthread_t));
               
                while(ptr) {
                        if(ii == 0 ||  (ii % packsize) == 0) {
                                file_group group_pass;
                                group_pass.grp_stp = ptr;
                                group_pass.size = packsize;

                                if(pthread_create(&tt[jj], NULL, cksum_cb, (void*)&group_pass))
                                        perror(RED"ERROR: "NCO"pthread_create in build_checksums()");

                                jj++;
                        }

                        ii++;
                        ptr=ptr->next;
                }

                for(ii = 0; ii < jj; ii++) {
                        if(pthread_join(tt[ii],NULL))
                                perror(RED"ERROR: "NCO"pthread_join in build_checksums()");
                }
                
                if(tt) free(tt); 
        }
}

/* Callback that actually does the work for ONE group */
static void* sheduler_cb(void *gfp)
{
        file_group *group = gfp;
        if(group == NULL || group->grp_stp == NULL)
                return NULL;

        group_filter(group);

        if(group->len == 1) {
                list_clear(group->grp_stp);
                return NULL;
        }

        build_checksums(group);
        findmatches(group);
        list_clear(group->grp_stp);
        return NULL;
}

/* Joins the threads launched by sheduler */
static void sheduler_jointhreads(pthread_t *tt, uint32 n)
{
        uint32 ii = 0;
        for(ii=0; ii < n; ii++) {
                if(pthread_join(tt[ii],NULL))
                        perror(RED"ERROR: "NCO"pthread_join in sheduler()");
        }
}

/* Distributes the groups on the ressources */
static void start_sheduler(file_group *fp, uint32 nlistlen)
{
        uint32 ii;
        pthread_t *tt = malloc(sizeof(pthread_t)*(nlistlen+1));

        if(set.threads == 1 || THREAD_SHEDULER == 0) {
                for(ii = 0; ii < nlistlen; ii++) {
                        sheduler_cb(&fp[ii]);
                }
        } else if(THREAD_SHEDULER == 1) {
			
				/* Run max set.threads at the same time. */ 
                int nrun = 0;
                for(ii = 0; ii < nlistlen; ii++) {
					
					if(fp[ii].size >  THREAD_SHEDULER_MTLIMIT) {
							if(pthread_create(&tt[nrun],NULL,sheduler_cb,(void*)&fp[ii]))
									perror(RED"ERROR: "NCO"pthread_create in sheduler()");

							if(nrun >= set.threads-1) {
									sheduler_jointhreads(tt, nrun + 1);
									nrun = 0;
									continue;
							}
					
							nrun++;
					} else { 
					        sheduler_cb(&fp[ii]);
				    }
					
                }
                sheduler_jointhreads(tt, nrun);
        } else if(THREAD_SHEDULER == 2) { 
				
				/* experimental.. may be faster quite often, but crahses also quite often */
				int nrun = 0; 
		        for(ii = 0; ii < nlistlen; ii++) {
						if(fp[ii].size >  THREAD_SHEDULER_MTLIMIT) 
						{
							if(pthread_create(&tt[nrun++],NULL,sheduler_cb,(void*)&fp[ii]))
                                perror(RED"ERROR: "NCO"pthread_create in sheduler()");
						
						} else {
						        sheduler_cb(&fp[ii]);
						}
				}
				sheduler_jointhreads(tt, nrun); 
				
		} 
        if(tt) free(tt);
}

/* Sort group array via qsort() */
static int cmp_grplist_bynodes(const void *a,const void *b)
{
        const file_group *ap = a;
        const file_group *bp = b;
        if(ap && bp) {
                if(ap->grp_stp && bp->grp_stp) {
                        return ap->grp_stp->node * ap->grp_stp->dev -
                                bp->grp_stp->node * bp->grp_stp->dev ;
                }

        }
        return -1;
}

/* Takes num and converts into some human readable string. 1024 -> 1KB */
static void size_to_human_readable(uint32 num, char *in, int sz)
{
        if(num < 1024) {
                snprintf(in,sz,"%ld B",(unsigned long)num);
        } else if(num >= 1024 && num < 1048576.0) {
                snprintf(in,sz,"%.2f KB",(float)(num/1024.0));
        } else if(num >= 1024*1024 && num < 1073741824) {
                snprintf(in,sz,"%.2f MB",(float)(num/1048576.0));
        } else {
                snprintf(in,sz,"%.2f GB",(float)(num/1073741824.0));
        }
}


static void find_double_bases(iFile *starting) 
{
	iFile *i = starting; 
	iFile *j = NULL;  
	
	int  c = 1; 
	
	while(i)
	{
		bool pr = false;  
		j=i->next;
	    while(j) 
	    {
			if(!strcmp(basename(i->path), basename(j->path)))
			{
				char *tmp2 = canonicalize_file_name(j->path); 
				if(!pr) 
				{
					char * tmp = canonicalize_file_name(i->path); 
					i->dupflag = false;
					printf("%d %s %u\n",c,tmp,i->fsize); 
					pr = true;
					if(tmp) free(tmp); 
				}
				
				j->dupflag = false; 
				printf("%d %s %u\n",c,tmp2,i->fsize);
				if(tmp2) free(tmp2); 
			}
			j=j->next; 
		}
		if(pr) c++; 
		i=i->next; 
    }
        
	die(-1); 
}


/* Splits into groups, and does some serious presortage */
void start_processing(iFile *b)
{
        file_group *fglist = NULL,
                      emptylist; 

        /* Logvariables - not relevant to algorithm */
        char lintbuf[128];
        uint32 ii = 0,
                lint = 0,
                spelen = 0,
                remaining = 0,
                rem_counter = 0,
                path_doubles = 0,
                original = list_len();

		db_done = true;
		
		if(ABS(set.dump) == 1) { 
				error("Double basenames: \n");
				error("----------------\n"); 
				find_double_bases(b); 
				if(set.dump == -1) die(0); 
		}
        emptylist.len = 0;

		if(ABS(set.dump) == 2) {
			error("Groups sorted by size:\n"); 
			error("---------------------\n"); 
		}
        while(b) {
                iFile *q = b, *prev = NULL;
                uint32 glen = 0, gsize = 0;

                while(b && q->fsize == b->fsize) {
                        prev = b;
                        gsize += b->fsize;
                        glen++;
                        b = b->next;
                }

                if(glen == 1) {
                        /* This is only a single element        */
                        /* We can remove it without feelind bad */
                        q = list_remove(q);
                        if(b != NULL) b = q;
                        rem_counter++;
                } else {
                        /* Mark this isle as 'sublist' by setting next/last pointers */
                        if(b != NULL) {
                                prev->next = NULL;
                                b->last = NULL;
                        }

                        if(q->fsize != 0) {
                                
                                
                                /* Sort by inode (speeds up IO on normal HDs [not SSDs]) */
                                q = list_sort(q,cmp_nd);

                                /* Add this group to the list array */
                                fglist = realloc(fglist, (spelen+1) * sizeof(file_group));
                                fglist[spelen].grp_stp = q;
                                fglist[spelen].grp_enp = prev;
                                fglist[spelen].len     = glen;
                                fglist[spelen].size    = gsize;
                               
								if(ABS(set.dump) == 2) {
									iFile *la = q;
									while(la) 
									{
										char *tmp = canonicalize_file_name(la->path);
										fprintf(stdout,"%u %s\n",la->fsize, tmp); 
										la=la->next; 
										free(tmp); 
									}
								}
                                /* Remove 'path-doubles' (files pointing to the physically SAME file) - this requires a node sorted list */
                                if(get_cpindex() > 1 || set.followlinks)
                                        path_doubles += rm_double_paths(&fglist[spelen]);

                                spelen++;
                        } else {
                                iFile *ptr;
                                q = list_sort(q,cmp_nd);
                                emptylist.grp_stp = q;
                                if(get_cpindex() > 1 || set.followlinks)
                                        rm_double_paths(&emptylist);

                                ptr = emptylist.grp_stp;
                                emptylist.len = 0;
                                while(ptr) {
										if(set.output) write_to_log(ptr, false, get_logstream());
                                        ptr = list_remove(ptr);
                                        emptylist.len++;
                                }
                        }

                }
        }
        
        if(set.dump == -2) die(0); 
        
        info("%ld files less in list",rem_counter);
        if(path_doubles) {
	           info(" (removed %ld pathzombies)", path_doubles);  
	    } 
        info("\nBy ignoring %ld empty files, list was split in %ld parts.\n", emptylist.len, spelen);
        info("Now sorting groups based on their location on the drive.. ");

        /* Now make sure groups are sorted by their location on the disk*/
        qsort(fglist, spelen, sizeof(file_group), cmp_grplist_bynodes);

        info(" done \nNow doing fingerprints and full checksums:\n\n");

        /* Grups are splitted, now give it to the sheduler */
        /* The sheduler will do another filterstep, build checkusm
         *  and compare 'em. The result is printed afterwards */
        start_sheduler(fglist, spelen);

		if(set.mode == 5 && set.output) { 
			fprintf(get_logstream(), SCRIPT_LAST);
			fprintf(get_logstream(), "\n# End of script. Have a nice day."); 
		}
        /* Gather the total size of removeable data */
        for(ii=0; ii < spelen; ii++) {
                lint += fglist[ii].size - ((fglist[ii].len > 0) ? (fglist[ii].size / fglist[ii].len) : 0);
                remaining +=  (fglist[ii].len) ? (fglist[ii].len - 1) : 0;
        }

        size_to_human_readable(lint, lintbuf, 127);
        info("\nIn total %ld (of %ld files as input) files are duplicates.\n",remaining, original);
        info("This means: %s [%ld Bytes] seems to be useless lint.\n", lintbuf, lint);
        
        if(set.output)
				info("A log has been written to %s.\n", set.output);

        /* End of actual program. Now do some file handling / frees */
        if(fglist) free(fglist);
}
