#ifdef UWSGI_SPOOLER
#include "uwsgi.h"

extern struct uwsgi_server uwsgi;

static void spooler_readdir(struct uwsgi_spooler *, char *dir);
#ifdef __linux__
static void spooler_scandir(struct uwsgi_spooler *, char *dir);
#endif
static void spooler_manage_task(struct uwsgi_spooler *, char *, char *);

// fake function to allow waking the spooler
void spooler_wakeup() {}

void uwsgi_opt_add_spooler(char *opt, char *directory, void *none) {

	int i;

        if (access(directory, R_OK | W_OK | X_OK)) {
                uwsgi_error("[spooler directory] access()");
                exit(1);
        }

	if (uwsgi.spooler_numproc > 0) {
		for(i=0;i<uwsgi.spooler_numproc;i++) {
			uwsgi_new_spooler(directory);
		}
	}
	else {
        	uwsgi_new_spooler(directory);
	}

}


struct uwsgi_spooler *uwsgi_new_spooler(char *dir) {

        struct uwsgi_spooler *uspool = uwsgi.spoolers;

        if (!uspool) {
                uwsgi.spoolers = uwsgi_malloc_shared(sizeof(struct uwsgi_spooler));
                uspool = uwsgi.spoolers;
        }
        else {
                while(uspool) {
                        if (uspool->next == NULL) {
                                uspool->next = uwsgi_malloc_shared(sizeof(struct uwsgi_spooler));
                                uspool = uspool->next;
                                break;
                        }
                        uspool = uspool->next;
                }
        }

        if (!realpath(dir, uspool->dir)) {
                uwsgi_error("[spooler] realpath()");
                exit(1);
        }

        uspool->next = NULL;

        return uspool;
}


struct uwsgi_spooler *uwsgi_get_spooler_by_name(char *name) {

	struct uwsgi_spooler *uspool = uwsgi.spoolers;

	while(uspool) {
		if (!strcmp(uspool->dir, name)) {
			return uspool;
		}
		uspool = uspool->next;
	}
	
	return NULL;
}

pid_t spooler_start(struct uwsgi_spooler *uspool) {
	
	int i;

	pid_t pid = uwsgi_fork("uWSGI spooler");
	if (pid < 0) {
		uwsgi_error("fork()");
		exit(1);
	}
	else if (pid == 0) {
		// USR1 will be used to wake up the spooler
		signal(SIGUSR1, spooler_wakeup);
		uwsgi.mywid = -1;
		uwsgi.mypid = getpid();
		uspool->pid = uwsgi.mypid;
		// avoid race conditions !!!
		uwsgi.i_am_a_spooler = uspool;

		uwsgi_fixup_fds(0, 0, NULL);
		uwsgi_close_all_sockets();

		for (i = 0; i < 0xFF; i++) {
                	if (uwsgi.p[i]->post_fork) {
                        	uwsgi.p[i]->post_fork();
                	}
        	}

		uwsgi.signal_socket = uwsgi.shared->spooler_signal_pipe[1];

		for (i = 0; i < 0xFF; i++) {
                	if (uwsgi.p[i]->spooler_init) {
                        	uwsgi.p[i]->spooler_init();
                	}
        	}

        	for (i = 0; i < uwsgi.gp_cnt; i++) {
                	if (uwsgi.gp[i]->spooler_init) {
                        	uwsgi.gp[i]->spooler_init();
                	}
        	}

		spooler(uspool);
	}
	else if (pid > 0) {
		uwsgi_log("spawned the uWSGI spooler on dir %s with pid %d\n", uspool->dir, pid);
	}

	return pid;
}

void destroy_spool(char *dir, char *file) {

	if (chdir(dir)) {
		uwsgi_error("chdir()");
                uwsgi_log("[spooler] something horrible happened to the spooler. Better to kill it.\n");
		exit(1);
	}

	if (unlink(file)) {
        	uwsgi_error("unlink()");
                uwsgi_log("[spooler] something horrible happened to the spooler. Better to kill it.\n");
                exit(1);
	}

}


int spool_request(struct uwsgi_spooler *uspool, char *filename, int rn, int core_id, char *buffer, int size, char *priority, time_t at, char *body, size_t body_len) {

	struct timeval tv;
	int fd;
	struct uwsgi_header uh;

	if (!uspool) {
		uspool = uwsgi.spoolers;
	}

	// this lock is for threads, the pid value in filename will avoid multiprocess races
	uwsgi_lock(uspool->lock);

	gettimeofday(&tv, NULL);

	if (priority) {
		if (snprintf(filename, 1024, "%s/%s", uspool->dir, priority) <= 0) {
			uwsgi_unlock(uspool->lock);
			return 0;
		}
		// no need to check for errors...
		(void) mkdir(filename, 0777);

		if (snprintf(filename, 1024, "%s/%s/uwsgi_spoolfile_on_%s_%d_%d_%d_%llu_%llu", uspool->dir, priority, uwsgi.hostname, (int) getpid(), rn, core_id, (unsigned long long) tv.tv_sec, (unsigned long long) tv.tv_usec) <= 0) {
			uwsgi_unlock(uspool->lock);
			return 0;
		}
	}
	else {
		if (snprintf(filename, 1024, "%s/uwsgi_spoolfile_on_%s_%d_%d_%d_%llu_%llu", uspool->dir, uwsgi.hostname, (int) getpid(), rn, core_id, (unsigned long long) tv.tv_sec, (unsigned long long) tv.tv_usec) <= 0) {
			uwsgi_unlock(uspool->lock);
			return 0;
		}
	}

	fd = open(filename, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		uwsgi_error_open(filename);
		uwsgi_unlock(uspool->lock);
		return 0;
	}

	// now lock the file, it will no be runnable, until the lock is not removed
	// a race could come if the spooler take the file before fcntl is called
        // in such case the spooler will detect a zeroed file and will retry later
	if (uwsgi_fcntl_lock(fd)) {
		close(fd);
		uwsgi_unlock(uspool->lock);
		return 0;
	}

	uh.modifier1 = 17;
	uh.modifier2 = 0;
	uh.pktsize = (uint16_t) size;
#ifdef __BIG_ENDIAN__
	uh.pktsize = uwsgi_swap16(uh.pktsize);
#endif

	if (write(fd, &uh, 4) != 4) {
		goto clear;
	}

	if (write(fd, buffer, size) != size) {
		goto clear;
	}

	if (body && body_len > 0) {
		if ((size_t)write(fd, body, body_len) != body_len) {
			goto clear;	
		}
	}

	if (at > 0) {
		struct timeval tv[2];
		tv[0].tv_sec = at;
		tv[0].tv_usec = 0;
		tv[1].tv_sec = at;
		tv[1].tv_usec = 0;
#ifdef __sun__
		if (futimesat(fd, NULL, tv)) {
#else
		if (futimes(fd, tv)) {
#endif
			uwsgi_error("futimes()");
		}
	}

	// here the file will be unlocked too
	close(fd);

	uwsgi_log("[spooler] written %d bytes to file %s\n", size + body_len + 4, filename);
	
	// and here waiting threads can continue
	uwsgi_unlock(uspool->lock);

/*	wake up the spoolers attached to the specified dir ... (HACKY) 
	no need to fear races, as USR1 is harmless an all of the uWSGI processes...
	it could be a problem if a new process takes the old pid, but modern systems should avoid that
*/

	struct uwsgi_spooler *spoolers = uwsgi.spoolers;
	while(spoolers) {
		if (!strcmp(spoolers->dir, uspool->dir)) {
			if (spoolers->pid > 0 && spoolers->running == 0) {
				(void) kill(spoolers->pid, SIGUSR1);
			}
		}
		spoolers = spoolers->next;
	}

	return 1;


      clear:
	uwsgi_unlock(uspool->lock);
	uwsgi_error("write()");
	if (unlink(filename)) {
		uwsgi_error("unlink()");
	}
	// unlock the file too
	close(fd);
	return 0;
}



void spooler(struct uwsgi_spooler *uspool) {

	// prevent process blindly reading stdin to make mess
	int nullfd;

	// asked by Marco Beri
#ifdef __HAIKU__
#ifdef UWSGI_DEBUG
	uwsgi_log("lowering spooler priority to %d\n", B_LOW_PRIORITY);
#endif
	set_thread_priority(find_thread(NULL), B_LOW_PRIORITY);
#else
#ifdef UWSGI_DEBUG
	uwsgi_log("lowering spooler priority to %d\n", PRIO_MAX);
#endif
	setpriority(PRIO_PROCESS, getpid(), PRIO_MAX);
#endif

	nullfd = open("/dev/null", O_RDONLY);
	if (nullfd < 0) {
		uwsgi_error_open("/dev/null");
		exit(1);
	}

	if (nullfd != 0) {
		dup2(nullfd, 0);
		close(nullfd);
	}

	int spooler_event_queue = event_queue_init();
	int interesting_fd = -1;

	if (uwsgi.master_process) {
		event_queue_add_fd_read(spooler_event_queue, uwsgi.shared->spooler_signal_pipe[1]);
	}

	for (;;) {


		if (chdir(uspool->dir)) {
			uwsgi_error("chdir()");
			exit(1);
		}

		if (uwsgi.spooler_ordered) {
#ifdef __linux__
			spooler_scandir(uspool, NULL);
#else
			spooler_readdir(uspool, NULL);
#endif
		}
		else {
			spooler_readdir(uspool, NULL);
		}

		if (event_queue_wait(spooler_event_queue, uwsgi.shared->spooler_frequency, &interesting_fd) > 0) {
			if (uwsgi.master_process) {
				if (interesting_fd == uwsgi.shared->spooler_signal_pipe[1]) {
					uint8_t uwsgi_signal;
					if (read(interesting_fd, &uwsgi_signal, 1) <= 0) {
                                                	uwsgi_log_verbose("uWSGI spooler screams: UAAAAAAH my master died, i will follow him...\n");
                                               		end_me(0); 
                                	}
                                	else {
#ifdef UWSGI_DEBUG
                                        	uwsgi_log_verbose("master sent signal %d to the spooler\n", uwsgi_signal);
#endif
                                        	if (uwsgi_signal_handler(uwsgi_signal)) {
                                                	uwsgi_log_verbose("error managing signal %d on the spooler\n", uwsgi_signal);
                                        	}
                                	}
				}
			}
		}

	}
}

#ifdef __linux__
static void spooler_scandir(struct uwsgi_spooler *uspool, char *dir) {

	struct dirent **tasklist;
	int n;

	if (!dir) dir = uspool->dir;

	n = scandir(dir, &tasklist, 0, versionsort);
	if (n < 0) {
		uwsgi_error("scandir()");
		return;
	}
	
	while(n--) {
		spooler_manage_task(uspool, dir, tasklist[n]->d_name);
		free(tasklist[n]);	
	}

	free(tasklist);
}
#endif


static void spooler_readdir(struct uwsgi_spooler *uspool, char *dir) {

	DIR *sdir;
	struct dirent *dp;

	if (!dir) dir = uspool->dir;

	sdir = opendir(dir);
	if (sdir) {
		while ((dp = readdir(sdir)) != NULL) {
			spooler_manage_task(uspool, dir, dp->d_name);
		}
		closedir(sdir);
	}
	else {
		uwsgi_error("opendir()");
	}
}

void spooler_manage_task(struct uwsgi_spooler *uspool, char *dir, char *task) {

	int i, ret;

	char spool_buf[0xffff];
	struct uwsgi_header uh;
	char *body = NULL;
	size_t body_len = 0;

	int spool_fd;

	if (!dir) dir = uspool->dir;

	if (!strncmp("uwsgi_spoolfile_on_", task, 19) || (uwsgi.spooler_ordered && is_a_number(task))) {
		struct stat sf_lstat;
		if (lstat(task, &sf_lstat)) {
			return;
		}

		// a spool request for the future
		if (sf_lstat.st_mtime > time(NULL)) {
			return;
		}

#ifdef __linux__
		if (S_ISDIR(sf_lstat.st_mode) && uwsgi.spooler_ordered) {
			if (chdir(task)) {
				uwsgi_error("chdir()");
				return;
			}
			char *prio_path = realpath(".", NULL);
			spooler_scandir(uspool, prio_path);
			free(prio_path);
			if (chdir(dir)) {
				uwsgi_error("chdir()");
			}
			return;
		}
#endif
		if (!S_ISREG(sf_lstat.st_mode)) {
			return;
		}
		if (!access(task, R_OK | W_OK)) {

			spool_fd = open(task, O_RDWR);

			if (spool_fd < 0) {
				uwsgi_error_open(task);
				return;
			}

			// check if the file is locked by anther process
			if (uwsgi_fcntl_is_locked(spool_fd)) {
				close(spool_fd);
				return;
			}

			if (read(spool_fd, &uh, 4) != 4) {
				// it could be here for broken file or just opened one
				uwsgi_error("read()");
				close(spool_fd);
				return;
			}

#ifdef __BIG_ENDIAN__
			uh.pktsize = uwsgi_swap16(uh.pktsize);
#endif

			if (read(spool_fd, spool_buf, uh.pktsize) != uh.pktsize) {
				uwsgi_error("read()");
				destroy_spool(dir, task);	
				close(spool_fd);
				return;
			}			
					
			// body available ?
			if (sf_lstat.st_size > (uh.pktsize+4)) {
				body_len = sf_lstat.st_size - (uh.pktsize+4);
				body = uwsgi_malloc(body_len);
				if ((size_t)read(spool_fd, body, body_len) != body_len) {
					uwsgi_error("read()");
					destroy_spool(dir, task);
					close(spool_fd);
					free(body);
					return;
				}
			}

			// not the task is running and should not be waken
			uspool->running = 1;

			uwsgi_log("[spooler %s pid: %d] managing request %s ...\n", uspool->dir, (int) uwsgi.mypid, task);


			// chdir before running the task (if requested)
			if (uwsgi.spooler_chdir) {
				if (chdir(uwsgi.spooler_chdir)) {
					uwsgi_error("chdir()");
				}
			}

			int callable_found = 0;
			for(i=0;i<0xff;i++) {
				if (uwsgi.p[i]->spooler) {
					time_t now = time(NULL);
					if(uwsgi.shared->options[UWSGI_OPTION_SPOOLER_HARAKIRI] > 0) {
                        			set_spooler_harakiri(uwsgi.shared->options[UWSGI_OPTION_SPOOLER_HARAKIRI]);
                			}
					ret = uwsgi.p[i]->spooler(task, spool_buf, uh.pktsize, body, body_len);
					if(uwsgi.shared->options[UWSGI_OPTION_SPOOLER_HARAKIRI] > 0) {
                        			set_spooler_harakiri(0);
                			}
					if (ret == 0) continue;
					callable_found = 1;
					if (ret == -2) {
						uwsgi_log("[spooler %s pid: %d] done with task %s after %d seconds\n", uspool->dir, (int) uwsgi.mypid, task, time(NULL)-now);
						destroy_spool(dir, task);	
					}
					// re-spool it
					break;	
				}
			}

			if (body)
				free(body);

			// here we free and unlock the task
			close(spool_fd);
			uspool->running = 0;

			if (!callable_found) {
				uwsgi_log("unable to find the spooler function, have you loaded it into the spooler process ?\n");
			}

		}
	}
}

#else
#warning "*** Spooler support is disabled ***"
#endif
