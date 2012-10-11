/* $Id$ */

/*
 * Copyright (c) 2012 Nicholas Marriott <nicm@users.sourceforge.net>
 * Copyright (c) 2012 George Nachman <tmux@georgester.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <event.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

void printflike2 control_msg_error(struct cmd_ctx *, const char *, ...);
void printflike2 control_msg_print(struct cmd_ctx *, const char *, ...);
void printflike2 control_msg_info(struct cmd_ctx *, const char *, ...);

/* Command error callback. */
void printflike2
control_msg_error(struct cmd_ctx *ctx, const char *fmt, ...)
{
	struct client	*c = ctx->curclient;
	va_list		 ap;

	va_start(ap, fmt);
	evbuffer_add_vprintf(c->stdout_data, fmt, ap);
	va_end(ap);

	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
}

/* Command print callback. */
void printflike2
control_msg_print(struct cmd_ctx *ctx, const char *fmt, ...)
{
	struct client	*c = ctx->curclient;
	va_list		 ap;

	va_start(ap, fmt);
	evbuffer_add_vprintf(c->stdout_data, fmt, ap);
	va_end(ap);

	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
}

/* Command info callback. */
void printflike2
control_msg_info(unused struct cmd_ctx *ctx, unused const char *fmt, ...)
{
}

/* Write a line. */
void printflike2
control_write(struct client *c, const char *fmt, ...)
{
	va_list		 ap;

	va_start(ap, fmt);
	evbuffer_add_vprintf(c->stdout_data, fmt, ap);
	va_end(ap);

	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
}

/* Write a buffer, adding a terminal newline. Empties buffer. */
void
control_write_buffer(struct client *c, struct evbuffer *buffer)
{
	evbuffer_add_buffer(c->stdout_data, buffer);
	evbuffer_add(c->stdout_data, "\n", 1);
	server_push_stdout(c);
}

/** Used to tell evbuffer_readln what kind of line-ending to look for.
 */
enum evbuffer_eol_style {
	/** Any sequence of CR and LF characters is acceptable as an EOL. */
	EVBUFFER_EOL_ANY,
	/** An EOL is an LF, optionally preceded by a CR.  This style is
	 * most useful for implementing text-based internet protocols. */
	EVBUFFER_EOL_CRLF,
	/** An EOL is a CR followed by an LF. */
	EVBUFFER_EOL_CRLF_STRICT,
	/** An EOL is a LF. */
        EVBUFFER_EOL_LF
};

/* Copied from libevent 2.x
 * Copyright (c) 2002-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright (c) 2007-2012 Niels Provos and Nick Mathewson
 */

char *
evbuffer_readln(struct evbuffer *buffer, size_t *n_read_out,
		enum evbuffer_eol_style eol_style)
{
	u_char *data = EVBUFFER_DATA(buffer);
	u_char *start_of_eol, *end_of_eol;
	size_t len = EVBUFFER_LENGTH(buffer);
	char *line;
	unsigned int i, n_to_copy, n_to_drain;

	/* depending on eol_style, set start_of_eol to the first character
	 * in the newline, and end_of_eol to one after the last character. */
	switch (eol_style) {
	case EVBUFFER_EOL_ANY:
		for (i = 0; i < len; i++) {
			if (data[i] == '\r' || data[i] == '\n')
				break;
		}
		if (i == len)
			return (NULL);
		start_of_eol = data+i;
		++i;
		for ( ; i < len; i++) {
			if (data[i] != '\r' && data[i] != '\n')
				break;
		}
		end_of_eol = data+i;
		break;
	case EVBUFFER_EOL_CRLF:
		end_of_eol = memchr(data, '\n', len);
		if (!end_of_eol)
			return (NULL);
		if (end_of_eol > data && *(end_of_eol-1) == '\r')
			start_of_eol = end_of_eol - 1;
		else
			start_of_eol = end_of_eol;
		end_of_eol++; /*point to one after the LF. */
		break;
	case EVBUFFER_EOL_CRLF_STRICT: {
		u_char *cp = data;
		while ((cp = memchr(cp, '\r', len-(cp-data)))) {
			if (cp < data+len-1 && *(cp+1) == '\n')
				break;
			if (++cp >= data+len) {
				cp = NULL;
				break;
			}
		}
		if (!cp)
			return (NULL);
		start_of_eol = cp;
		end_of_eol = cp+2;
		break;
	}
	case EVBUFFER_EOL_LF:
		start_of_eol = memchr(data, '\n', len);
		if (!start_of_eol)
			return (NULL);
		end_of_eol = start_of_eol + 1;
		break;
	default:
		return (NULL);
	}

	n_to_copy = start_of_eol - data;
	n_to_drain = end_of_eol - data;

	if ((line = malloc(n_to_copy+1)) == NULL) {
		fprintf(stderr, "%s: out of memory\n", __func__);
		evbuffer_drain(buffer, n_to_drain);
		return (NULL);
	}

	memcpy(line, data, n_to_copy);
	line[n_to_copy] = '\0';

	evbuffer_drain(buffer, n_to_drain);
	if (n_read_out)
		*n_read_out = (size_t)n_to_copy;

	return (line);
}

/* Control input callback. Read lines and fire commands. */
void
control_callback(struct client *c, int closed, unused void *data)
{
	char		*line, *cause;
	struct cmd_ctx	 ctx;
	struct cmd_list	*cmdlist;

	if (closed)
		c->flags |= CLIENT_EXIT;

	for (;;) {
		line = evbuffer_readln(c->stdin_data, NULL, EVBUFFER_EOL_LF);
		if (line == NULL)
			break;
		if (*line == '\0') { /* empty line exit */
			c->flags |= CLIENT_EXIT;
			break;
		}

		ctx.msgdata = NULL;
		ctx.cmdclient = NULL;
		ctx.curclient = c;

		ctx.error = control_msg_error;
		ctx.print = control_msg_print;
		ctx.info = control_msg_info;

		if (cmd_string_parse(line, &cmdlist, &cause) != 0) {
			control_write(c, "%%error in line \"%s\": %s", line,
			    cause);
			free(cause);
		} else {
			cmd_list_exec(cmdlist, &ctx);
			cmd_list_free(cmdlist);
		}

		free(line);
	}
}
