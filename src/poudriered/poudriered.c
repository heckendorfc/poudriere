/*-
 * Copyright (c) 2014 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2014 Bryan Drewery <bdrewery@FreeBSD.org>
 * All rights reserved.
 *~
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *~
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <libutil.h>
#include <signal.h>
#include <spawn.h>
#define _WITH_DPRINTF
#include <stdio.h>
#include <histedit.h>
#include <syslog.h>
#include <stdlib.h>
#include <ucl.h>
#include <unistd.h>

#include "internal.h"

static ucl_object_t *conf;
static ucl_object_t *queue = NULL;
static int server_fd = -1;
static ucl_object_t *running = NULL;
extern char **environ;
static int kq;
static int nbevq = 0;
static struct kevent ke;
int conffd = -1;

struct client {
	int fd;
	struct sockaddr_storage ss;
	ucl_object_t *req;
	uid_t uid;
	gid_t gid;
};

static void
send_object(struct client *cl, ucl_object_t *o)
{
	if (o == NULL)
		o = ucl_object_typed_new(UCL_OBJECT);

	scgi_send(cl->fd, ucl_object_emit(o, UCL_EMIT_JSON_COMPACT));

	ucl_object_unref(o);
}

static void
send_error(struct client *cl, const char *msg) {
	ucl_object_t *umsg = NULL;
	ucl_object_t *o;
	
	o = ucl_object_fromstring_common("error", 5, 0);
	umsg = ucl_object_insert_key(umsg, o, "type", 4, true);
	o = ucl_object_fromstring_common(msg, strlen(msg),UCL_STRING_TRIM);
	umsg = ucl_object_insert_key(umsg, o, "message", 7, true);

	send_object(cl, umsg);
}

static ucl_object_t *
load_conf(void)
{
	struct ucl_parser *parser = NULL;
	ucl_object_t *obj;

	parser = ucl_parser_new(UCL_PARSER_KEY_LOWERCASE);

	if (!ucl_parser_add_file(parser, PREFIX"/etc/poudriered.conf")) {
		warnx("Failed to parse configuration file: %s",
		    ucl_parser_get_error(parser));
		return (NULL);
	}

	obj = ucl_parser_get_object(parser);

	ucl_parser_free(parser);

	return (obj);
}

ucl_object_t *
reload() {
	ucl_object_t *nconf;

	nconf = load_conf();
	if (nconf != NULL) {
		ucl_object_unref(conf);
		conf = nconf;
	}

	return (nconf);
}

static void
reload_signal() {
	(void)reload();
}

static void
close_socket(int dummy) {
	if (server_fd != -1)
		close(server_fd);

	ucl_object_t *o;
	o = ucl_object_find_key(conf, "socket");

	if (o == NULL || o->type != UCL_STRING) {
		ucl_object_unref(conf);
		exit(dummy);
	}

	unlink(ucl_object_tostring(o));
	ucl_object_unref(conf);

	exit(dummy);
}

void
client_free(struct client *cl)
{
	if (cl->req != NULL)
		ucl_object_unref(cl->req);
	if (cl->fd != -1)
		close(cl->fd);
	free(cl);
}

static bool
valid_user(ucl_object_t *o, struct client *cl)
{
	struct passwd *pw;

	switch (o->type) {
		case UCL_STRING:
			if (ucl_object_tostring(o)[0] == '*')
				return (true);
			pw = getpwnam(ucl_object_tostring(o));
			if (pw && pw->pw_uid == cl->uid)
				return (true);
			break;
		case UCL_INT:
			if (cl->uid == ucl_object_toint(o))
				return (true);
			break;
		default:
			break;
	}

	return (false);
}

static bool
valid_group(ucl_object_t *o, struct client *cl)
{
	struct group *gr;

	switch (o->type) {
		case UCL_STRING:
			if (ucl_object_tostring(o)[0] == '*')
				return (true);
			gr = getgrnam(ucl_object_tostring(o));
			if (gr && gr->gr_gid == cl->gid)
				return (true);
			break;
		case UCL_INT:
			if (cl->gid == ucl_object_toint(o))
				return (true);
			break;
		default:
			break;
	}

	return (false);
}

static int
check_argument(ucl_object_t *cmd, struct client *cl, const char *arg) {

	ucl_object_t *cred_cmds, *cred, *tmp, *wild, *o;
	ucl_object_iter_t it = NULL;

	cred_cmds = ucl_object_find_key(cmd, "argument");
	if (cred_cmds == NULL)
		return (0);

	cred = wild = NULL;
	while ((tmp = ucl_iterate_object(cred_cmds, &it, false))) {
		if ((cred = ucl_object_find_key(tmp, arg)))
			break;
		if (!wild)
			wild = ucl_object_find_key(tmp, "*");
	}
	
	if (cred == NULL && wild == NULL)
		return (0);

	/* check the groups */
	o = ucl_object_find_key(cred, "group");
	if (o != NULL) {
		it = NULL;
		while ((tmp = ucl_iterate_object(o, &it, false))) {
			if (valid_group(o, cl))
				return (1);
		}
	}

	o = ucl_object_find_key(cred, "user");
	if (o != NULL) {
		it = NULL;
		while ((tmp = ucl_iterate_object(o, &it, false))) {
			if (valid_user(o, cl))
				return (1);
		}
	}

	return (0);
}

static bool
is_arguments_allowed(ucl_object_t *a, ucl_object_t *cmd, struct client *cl)
{
	int nbargs, ok, argc, argvl, i;
	char **argv = NULL;
	char *buf, *tofree, *arg;

	nbargs = ok = argc = argvl = 0;

	if (a == NULL)
		return (false);

	buf = strdup(ucl_object_tostring(a));
	tofree = buf;
	while ((arg = strsep(&buf, "\t \n")) != NULL) {
		if (*arg == '\0')
			continue;
		if (argc > argvl - 2) {
			argvl += 1024;
			argv = reallocf(argv, argvl * sizeof(char *));
		}
		argv[argc++] = arg;
	}

	for (i = 0; i < argc; i++) {
		if (argv[i][0] != '-')
			continue;
		nbargs++;
		ok += check_argument(cmd, cl, argv[i]);
	}

	free(argv);
	free(tofree);

	return (ok == nbargs);
}

static bool
is_command_allowed(ucl_object_t *req, struct client *cl, ucl_object_t **ret)
{
	ucl_object_t *cred_cmds, *cred, *tmp, *wild, *o;
	ucl_object_iter_t it = NULL;

	*ret = NULL;
	cred_cmds = ucl_object_find_key(conf, "command");
	if (cred_cmds == NULL)
		return (false);

	cred = wild = NULL;
	while ((tmp = ucl_iterate_object(cred_cmds, &it, false))) {
		if ((cred = ucl_object_find_key(tmp, ucl_object_tostring(req))))
			break;
		if (!wild)
			wild = ucl_object_find_key(tmp, "*");
	}

	if (cred == NULL && wild == NULL)
		return (false);

	if (!cred)
		cred = wild;

	*ret = cred;

	/* Check the groups */
	o = ucl_object_find_key(cred, "group");
	if (o != NULL) {
		it = NULL;
		while ((tmp = ucl_iterate_object(o, &it, false))) {
			if (valid_group(tmp, cl))
				return (true);
		}
	}
	/* check the users */
	o = ucl_object_find_key(cred, "user");
	if (o != NULL) {
		it = NULL;
		while ((tmp = ucl_iterate_object(o, &it, false))) {
			if (valid_user(tmp, cl))
				return (true);
		}
	}

	return (false);
}

static bool
is_operation_allowed(ucl_object_t *o, struct client *cl)
{
	ucl_object_t *creds, *cred, *tmp, *wild, *obj;
	ucl_object_iter_t it = NULL;

	creds = ucl_object_find_key(conf, "operation");
	if (creds == NULL)
		return (false);

	cred = wild = NULL;
	while ((tmp = ucl_iterate_object(creds, &it, false))) {
		if ((cred = ucl_object_find_key(tmp, ucl_object_tostring(o))))
			break;
		if (!wild)
			wild = ucl_object_find_key(tmp, "*");
	}

	if (cred == NULL && wild == NULL)
		return (false);

	if (!cred)
		cred = wild;

	/* check groups */
	obj = ucl_object_find_key(cred, "group");
	if (obj != NULL) {
		it = NULL;
		while ((tmp = ucl_iterate_object(obj, &it, false))) {
			if (valid_group(tmp, cl))
				return (true);
		}
	}

	/* check users */
	obj = ucl_object_find_key(cred, "user");
	if (obj != NULL) {
		it = NULL;
		while ((tmp = ucl_iterate_object(obj, &it, false))) {
			if (valid_user(tmp, cl))
				return (true);
		}
	}

	return (false);
}

static int
mkdirs(const char *_path, bool lastisfile)
{
	char path[MAXPATHLEN];
	char *p;

	strlcpy(path, _path, sizeof(path));
	p = path;
	if (*p == '/')
		p++;

	if (lastisfile) {
		if ((p = strrchr(p, '/')) != NULL)
			*p = '\0';
		else
			return (0);
	}

	for (;;) {
		if ((p = strchr(p, '/')) != NULL)
			*p = '\0';

		if (mkdir(path, S_IRWXU | S_IRWXG | S_IRWXO) < 0)
			if (errno != EEXIST && errno != EISDIR)
				err(EXIT_FAILURE, "mkdir");

		/* that was the last element of the path */
		if (p == NULL)
			break;

		*p = '/';
		p++;
	}

	return (0);
}


static void
execute_cmd() {
	posix_spawn_file_actions_t action;
	int logfd;
	pid_t pid;
	int error;
	char **argv;
	char *buf, *tofree, *arg;
	int argc, argvl;
	ucl_object_t *o, *a, *l;

	if (running == NULL)
		return;

	l = ucl_object_find_key(running, "log");
	if (l != NULL)
		mkdirs(ucl_object_tostring(l), true);
	logfd = open(l != NULL ? ucl_object_tostring(l) : "/tmp/poudriered.log",
	    O_CREAT|O_RDWR|O_TRUNC,0644);
	if (logfd == -1)
		logfd = open("/dev/null", O_RDWR);

	posix_spawn_file_actions_init(&action);
	posix_spawn_file_actions_adddup2(&action, logfd, STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&action, logfd, STDERR_FILENO);

	argvl = 1024;
	argv = malloc(argvl * sizeof(char *));
	argv[0] = "poudriere";
	argv[1] = (char *)ucl_object_tostring(o);
	argc = 2;
	tofree = NULL;

	if (a != NULL) {
		buf = strdup(ucl_object_tostring(a));
		tofree = buf;
		while ((arg = strsep(&buf, "\t \n")) != NULL) {
			if (*arg == '\0')
				continue;
			if (argc > argvl -2 ) {
				argvl += 1024;
				argv = reallocf(argv, argvl * sizeof(char *));
			}
			argv[argc++] = arg;
		}
		argv[argc+1] = NULL;
	}

	if ((error = posix_spawn(&pid, PREFIX"/bin/poudriere",
		&action, NULL, argv, environ)) != 0) {
		errno = error;
		close(logfd);
		warn("Cannot run poudriere");
		goto done;
	}

	EV_SET(&ke, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, &logfd);
	kevent(kq, &ke, 1, NULL, 0, NULL);
	nbevq++;

done:
	free(tofree);
	free(argv);

	return;
}

static void
process_queue(void) {
	if (running != NULL)
		return;

	running = ucl_array_pop_first(queue);

	execute_cmd();
}

static bool
append_to_queue(ucl_object_t *cmd)
{
	queue = ucl_array_append(queue, cmd);
	syslog(LOG_INFO, "New command queued");

	return (true);
}

static void
client_exec(struct client *cl)
{
	ucl_object_t *cmd, *c, *cmd_cred;
	bool cmd_allowed = false;

        syslog(LOG_INFO, "uid(%d) sent request: %s", cl->uid,
	    ucl_object_emit(cl->req, UCL_EMIT_JSON_COMPACT));

	cmd = ucl_object_find_key(cl->req, "data");

	if ((c = ucl_object_find_key(cmd, "operation"))) {
		/* The user specified an operation not a command */
		if (is_operation_allowed(c, cl)) {
			if (!strcmp(ucl_object_tostring(c), "quit")) {
				close_socket(EXIT_SUCCESS);
			} else if (!strcmp(ucl_object_tostring(c), "reload")) {
				ucl_object_t *nconf = reload();
				send_object(cl,
				    ucl_object_insert_key(NULL,
				    ucl_object_frombool(nconf != NULL),
				    "reload", 6, true));
			} else if (!strcmp(ucl_object_tostring(c), "queue")) {
				send_object(cl, queue);
			} else if (!strcmp(ucl_object_tostring(c), "status")) {
				ucl_object_t *msg = NULL;
				msg = ucl_object_insert_key(msg,
				    ucl_object_fromstring(running ? "running" :
				    "idle"), "state", 5, true);
				msg = ucl_object_insert_key(msg, running ?
				    running : ucl_object_new(), "data", 4,
				    true);
				send_object(cl, msg);
			} else if (!strcmp(ucl_object_tostring(c), "jail")) {
				ucl_object_t *msg = NULL;
				ucl_object_t *o = jail_list();
				if (o == NULL)
					msg = ucl_object_insert_key(msg,
					    ucl_object_typed_new(UCL_ARRAY),
					    "jail", 4, true);
				else
					msg = ucl_object_insert_key(msg, o,
					    "jail", 4, true);
				send_object(cl, msg);
			} else if (!strcmp(ucl_object_tostring(c), "ports")) {
				ucl_object_t *msg = NULL;
				ucl_object_t *o = ports_list();
				if (o == NULL)
					msg = ucl_object_insert_key(msg,
					    ucl_object_new(),
					    "ports", 5, true);
				else
					msg = ucl_object_insert_key(msg, o,
					    "ports", 5, true);
				send_object(cl, msg);
			}
		} else {
			send_error(cl, "permission denied");
		}
		ucl_object_unref(cmd);
		return;
	}

	c = ucl_object_find_key(cmd, "command");
	if (c == NULL || c->type != UCL_STRING) {
		send_error(cl, "No command specified");
		ucl_object_unref(cmd);
		return;
	}
	/* validate credentials */
	cmd_allowed = is_command_allowed(c, cl, &cmd_cred);

	if (!cmd_allowed && cmd_cred != NULL) {
		c = ucl_object_find_key(cmd, "arguments");
		if (c && c->type != UCL_STRING)
			send_error(cl, "Expecting a string for the arguments");
		if (c && c->type == UCL_STRING)
			cmd_allowed = is_arguments_allowed(c, cmd_cred, cl);
	}

	if (!cmd_allowed) {
		/* still not allowed, let's check per args */
		send_error(cl, "Permission denied");
		ucl_object_unref(cmd);
		return;
	}

	/* ok just proceed */
	if (!append_to_queue(cmd)) {
		send_error(cl, "unknown, command not queued");
		ucl_object_unref(cmd);
		return;
	}
}

static void
client_read(struct client *cl, long len)
{
	int r;
	char buf[BUFSIZ];
	struct sbuf *b = sbuf_new_auto();

	r = read(cl->fd, buf, sizeof(buf));
	if (r < 0 && (errno == EINTR || errno == EAGAIN))
		return;

	sbuf_bcat(b, buf, r);

	if ((long)r == len) {
		cl->req = scgi_parse(sbuf_data(b));
		client_exec(cl);
	}
	sbuf_delete(b);
}

static struct client *
client_new(int fd)
{
	socklen_t sz;
	struct client *cl;
	int flags;

	if ((cl = malloc(sizeof(struct client))) == NULL)
		errx(EXIT_FAILURE, "Unable to allocate memory");

	sz = sizeof(cl->ss);
	cl->req = NULL;

	cl->fd = accept(fd, (struct sockaddr *)&(cl->ss), &sz);

	if (cl->fd < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EPROTO) {
			client_free(cl);
			return (NULL);
		}
		err(EXIT_FAILURE, "accept()");
	}
	
	if (getpeereid(cl->fd, &cl->uid, &cl->gid) != 0)
		err(EXIT_FAILURE, "getpeereid()");

	if (-1 == (flags = fcntl(cl->fd, F_GETFL, 0)))
		flags = 0;

	fcntl(cl->fd, F_SETFL, flags | O_NONBLOCK);

	return (cl);
}

static void
check_schedules() {
	struct tm *now;
	time_t now_t;
	ucl_object_t *o, *tmp, *cmd, *when, *dateformat;
	ucl_object_iter_t it = NULL;
	char datestr[BUFSIZ];

	now_t = time(NULL);
	now = gmtime(&now_t);

	if ((o = ucl_object_find_key(conf, "schedule")) == NULL)
		return;

	while ((tmp = ucl_iterate_object(o, &it, true))) {
		when = ucl_object_find_key(tmp, "when");
		dateformat = ucl_object_find_key(tmp, "format");
		cmd = ucl_object_find_key(tmp, "cmd");
		if (cmd == NULL ||
		    when == NULL ||
		    dateformat == NULL)
			continue;

		if (strftime_l(datestr, BUFSIZ, ucl_object_tostring(dateformat),
		    now, NULL) <= 0)
			continue;

		if (!strcmp(datestr, ucl_object_tostring(when))) {
			queue = ucl_array_append(queue, cmd);
			syslog(LOG_INFO, "New command queued");
		}
	}
}

static void
serve(void) {
	struct kevent *evlist = NULL;
	struct client *cl;
	int nev, i;
	int max_queues = 0;

	if ((kq = kqueue()) == -1)
		err(EXIT_FAILURE, "kqueue");

	if (ucl_object_find_key(conf, "schedule") != NULL) {
		EV_SET(&ke, 1, EVFILT_TIMER, EV_ADD, 0, 1000, NULL);
		kevent(kq, &ke, 1, NULL, 0, NULL);
		nbevq++;
	}
	EV_SET(&ke, server_fd,  EVFILT_READ, EV_ADD, 0, 0, NULL);
	kevent(kq, &ke, 1, NULL, 0, NULL);
	nbevq++;

	for (;;) {
		if (nbevq > max_queues) {
			max_queues += 1024;
			free(evlist);
			if ((evlist = malloc(max_queues *
			    sizeof(struct kevent))) == NULL)
				errx(EXIT_FAILURE, "Unable to allocate memory");
		}

		nev = kevent(kq, NULL, 0, evlist, max_queues, NULL);
		for (i = 0; i < nev; i++) {
			/* New client */
			if (evlist[i].udata == NULL && evlist[i].filter ==
			    EVFILT_READ) {
				/* We are in the listener */
				if ((cl = client_new(evlist[i].ident)) == NULL)
					continue;

				EV_SET(&ke, cl->fd, EVFILT_READ, EV_ADD, 0, 0,
				    cl);
				kevent(kq, &ke, 1, NULL, 0, NULL);
				nbevq++;
				continue;
			} 

			/* Reading from client */
			if (evlist[i].filter == EVFILT_READ) {
				if (evlist[i].flags & (EV_ERROR | EV_EOF)) {
					/* Do an extra read on EOF as kqueue
					 * will send this even if there is 
					 * data still available. */
					if (evlist[i].flags & EV_EOF)
						client_read(evlist[i].udata,
						    evlist[i].data);
					client_free(evlist[i].udata);
					nbevq--;
					continue;
				}
				client_read(evlist[i].udata, evlist[i].data);
				continue;
			}

			/* process died */
			if (evlist[i].filter == EVFILT_PROC) {
				int status = evlist[i].data;
				int fd = *(int *)evlist[i].udata;
				close(fd);
				ucl_object_unref(running);
				if (WIFEXITED(status))
					syslog(LOG_INFO, "Command exited with "
					    "status: %d", WEXITSTATUS(status));
				else if (WIFSIGNALED(status))
					syslog(LOG_INFO, "Command killed by "
					    "signal %d", WTERMSIG(status));
				else
					syslog(LOG_INFO, "Command terminated");

				running = NULL;
				continue;
			}

			if (evlist[i].filter == EVFILT_TIMER)
				check_schedules();

		}
		process_queue();
	}
}

int
main(void)
{
	struct sockaddr_un un;
	struct pidfh *pfh;
	pid_t otherpid;

	ucl_object_t *sock_path_o, *pidfile_path_o;

	if ((conf = load_conf()) == NULL)
		return (EXIT_FAILURE);

	if ((sock_path_o = ucl_object_find_key(conf, "socket")) == NULL) {
		warnx("'socket' not found in the configuration file");
		ucl_object_unref(conf);
		return (EXIT_FAILURE);
	}

	if ((pidfile_path_o = ucl_object_find_key(conf, "pidfile")) == NULL) {
		warnx("'pidfile' not found in the configuration file");
		ucl_object_unref(conf);
		return (EXIT_FAILURE);
	}

	if (mkdir(PREFIX"/etc/poudriere.d", 0644) == -1) {
		if (errno != EEXIST) {
			warn("unable to create configuration directory");
			ucl_object_unref(conf);
			return (EXIT_FAILURE);
		}
	}

	if ((conffd = open(PREFIX"/etc/poudriere.d", O_RDONLY|O_DIRECTORY)) == -1) {
		warn("unable to open the configuration directory");
		ucl_object_unref(conf);
		return (EXIT_FAILURE);
	}

	pfh = pidfile_open(ucl_object_tostring(pidfile_path_o), 0600,
	    &otherpid);
	if (pfh == NULL) {
		if (errno == EEXIST) {
			errx(EXIT_FAILURE, "Daemon already running, pid: %jd.",
			    (intmax_t)otherpid);
		}
		/* If we cannot create pidfile from other reasons, only warn. */
		warn("Cannot open or create pidfile");
	}

	memset(&un, 0, sizeof(struct sockaddr_un));
	if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		ucl_object_unref(conf);
		err(EXIT_FAILURE, "socket()");
	}

	/* SO_REUSEADDR does not prevent EADDRINUSE, since we are locked by
	 * a pid, just unlink the old socket if needed. */
	unlink(ucl_object_tostring(sock_path_o));
	un.sun_family = AF_UNIX;
	strlcpy(un.sun_path, ucl_object_tostring(sock_path_o),
	    sizeof(un.sun_path));
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (int[]){1},
	    sizeof(int)) < 0) {
		ucl_object_unref(conf);
		err(EXIT_FAILURE, "setsockopt()");
	}

	if (bind(server_fd, (struct sockaddr *) &un,
	    sizeof(struct sockaddr_un)) == -1) {
		ucl_object_unref(conf);
		err(EXIT_FAILURE, "bind()");
	}

	if (chmod(un.sun_path, 0666) != 0)
		err(EXIT_FAILURE, "chmod(socket)");

	signal(SIGINT, close_socket);
	signal(SIGKILL, close_socket);
	signal(SIGQUIT, close_socket);
	signal(SIGTERM, close_socket);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, reload_signal);

/*	if (daemon(0, 0) == -1) {
		pidfile_remove(pfh);
		err(EXIT_FAILURE, "Cannot daemonize");
	}*/

	pidfile_write(pfh);

	if (listen(server_fd, 1024) < 0) {
		warn("listen()");
		close_socket(EXIT_FAILURE);
	}

	openlog("poudriered", LOG_PID|LOG_NDELAY, LOG_DAEMON);

	serve();

	close_socket(EXIT_SUCCESS);
}