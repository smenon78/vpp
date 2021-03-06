/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * cli.c: Unix stdin/socket CLI.
 *
 * Copyright (c) 2008 Eliot Dresselhaus
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <vlib/vlib.h>
#include <vlib/unix/unix.h>
#include <vppinfra/timer.h>

#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/telnet.h>
#include <sys/ioctl.h>

/* ANSI Escape code. */
#define ESC "\x1b"

/* ANSI Control Sequence Introducer. */
#define CSI ESC "["

/* ANSI sequences. */
#define ANSI_CLEAR      CSI "2J" CSI "1;1H"
#define ANSI_RESET      CSI "0m"
#define ANSI_BOLD       CSI "1m"
#define ANSI_DIM        CSI "2m"
#define ANSI_DRED       ANSI_DIM CSI "31m"
#define ANSI_BRED       ANSI_BOLD CSI "31m"
#define ANSI_CLEAR      CSI "2J" CSI "1;1H"
#define ANSI_CLEARLINE  CSI "2K"
#define ANSI_SCROLLDN   CSI "1T"
#define ANSI_SAVECURSOR CSI "s"
#define ANSI_RESTCURSOR CSI "u"

/** Maximum depth into a byte stream from which to compile a Telnet
 * protocol message. This is a saftey measure. */
#define UNIX_CLI_MAX_DEPTH_TELNET 24

/** Unix standard in */
#define UNIX_CLI_STDIN_FD 0


typedef struct {
  u8 * line;
  u32 length;
} unix_cli_banner_t;

#define _(a) { .line = (u8 *)(a), .length = sizeof(a) - 1 }
/** Plain welcome banner. */
static unix_cli_banner_t unix_cli_banner[] = {
_("    _______    _        _   _____  ___ \n"),
_(" __/ __/ _ \\  (_)__    | | / / _ \\/ _ \\\n"),
_(" _/ _// // / / / _ \\   | |/ / ___/ ___/\n"),
_(" /_/ /____(_)_/\\___/   |___/_/  /_/    \n"),
_("\n")
};
/** ANSI color welcome banner. */
static unix_cli_banner_t unix_cli_banner_color[] = {
_(ANSI_BRED "    _______    _     " ANSI_RESET "   _   _____  ___ \n"),
_(ANSI_BRED " __/ __/ _ \\  (_)__ " ANSI_RESET "   | | / / _ \\/ _ \\\n"),
_(ANSI_BRED " _/ _// // / / / _ \\" ANSI_RESET "   | |/ / ___/ ___/\n"),
_(ANSI_BRED " /_/ /____(_)_/\\___/" ANSI_RESET "   |___/_/  /_/    \n"),
_("\n")
};
#undef _


/** Unix CLI session. */
typedef struct {
  u32 unix_file_index;

  /* Vector of output pending write to file descriptor. */
  u8 * output_vector;

  /* Vector of input saved by Unix input node to be processed by
     CLI process. */
  u8 * input_vector;

  u8 has_history;
  u8 ** command_history;
  u8 * current_command;
  i32 excursion;

  /* Maximum number of history entries this session will store. */
  u32 history_limit;

  /* Current command line counter */
  u32 command_number;

  u8 * search_key;
  int search_mode;

  /* Position of the insert cursor on the current input line */
  u32 cursor;

  /* Line mode or char mode */
  u8 line_mode;

  /* Set if the CRLF mode wants CR + LF */
  u8 crlf_mode;

  /* Can we do ANSI output? */
  u8 ansi_capable;

  /* Has the session started? */
  u8 started;

  /* Disable the pager? */
  u8 no_pager;

  /* Pager buffer */
  u8 ** pager_vector;

  /* Lines currently displayed */
  u32 pager_lines;

  /* Line number of top of page */
  u32 pager_start;

  /* Terminal size */
  u32 width, height;

  u32 process_node_index;
} unix_cli_file_t;

always_inline void
unix_cli_pager_reset (unix_cli_file_t *f)
{
  u8 ** p;

  f->pager_lines = f->pager_start = 0;
  vec_foreach (p, f->pager_vector)
    {
      vec_free(*p);
    }
  vec_free(f->pager_vector);
  f->pager_vector = 0;
}

always_inline void
unix_cli_file_free (unix_cli_file_t * f)
{
  vec_free (f->output_vector);
  vec_free (f->input_vector);
  unix_cli_pager_reset(f);
}

/* CLI actions */
typedef enum {
  UNIX_CLI_PARSE_ACTION_NOACTION = 0,
  UNIX_CLI_PARSE_ACTION_CRLF,
  UNIX_CLI_PARSE_ACTION_TAB,
  UNIX_CLI_PARSE_ACTION_ERASE,
  UNIX_CLI_PARSE_ACTION_ERASERIGHT,
  UNIX_CLI_PARSE_ACTION_UP,
  UNIX_CLI_PARSE_ACTION_DOWN,
  UNIX_CLI_PARSE_ACTION_LEFT,
  UNIX_CLI_PARSE_ACTION_RIGHT,
  UNIX_CLI_PARSE_ACTION_HOME,
  UNIX_CLI_PARSE_ACTION_END,
  UNIX_CLI_PARSE_ACTION_WORDLEFT,
  UNIX_CLI_PARSE_ACTION_WORDRIGHT,
  UNIX_CLI_PARSE_ACTION_ERASELINELEFT,
  UNIX_CLI_PARSE_ACTION_ERASELINERIGHT,
  UNIX_CLI_PARSE_ACTION_CLEAR,
  UNIX_CLI_PARSE_ACTION_REVSEARCH,
  UNIX_CLI_PARSE_ACTION_FWDSEARCH,
  UNIX_CLI_PARSE_ACTION_YANK,
  UNIX_CLI_PARSE_ACTION_TELNETIAC,

  UNIX_CLI_PARSE_ACTION_PAGER_CRLF,
  UNIX_CLI_PARSE_ACTION_PAGER_QUIT,
  UNIX_CLI_PARSE_ACTION_PAGER_NEXT,
  UNIX_CLI_PARSE_ACTION_PAGER_DN,
  UNIX_CLI_PARSE_ACTION_PAGER_UP,
  UNIX_CLI_PARSE_ACTION_PAGER_TOP,
  UNIX_CLI_PARSE_ACTION_PAGER_BOTTOM,
  UNIX_CLI_PARSE_ACTION_PAGER_PGDN,
  UNIX_CLI_PARSE_ACTION_PAGER_PGUP,
  UNIX_CLI_PARSE_ACTION_PAGER_SEARCH,

  UNIX_CLI_PARSE_ACTION_PARTIALMATCH,
  UNIX_CLI_PARSE_ACTION_NOMATCH
} unix_cli_parse_action_t;

/** \brief Mapping of input buffer strings to action values.
 * @note This won't work as a hash since we need to be able to do
 *       partial matches on the string.
 */
typedef struct {
  u8 *input;                        /**< Input string to match. */
  u32 len;                          /**< Length of input without final NUL. */
  unix_cli_parse_action_t action;   /**< Action to take when matched. */
} unix_cli_parse_actions_t;

/** \brief Given a capital ASCII letter character return a NUL terminated
 * string with the control code for that letter.
 * \example CTL('A') returns { 0x01, 0x00 } as a u8[].
 */
#define CTL(c) (u8[]){ (c) - '@', 0 }

#define _(a,b) { .input = (u8 *)(a), .len = sizeof(a) - 1, .action = (b) }
static unix_cli_parse_actions_t unix_cli_parse_strings[] = {
 /* Line handling */
 _( "\r\n",   UNIX_CLI_PARSE_ACTION_CRLF ),       /* Must be before '\r' */
 _( "\n",     UNIX_CLI_PARSE_ACTION_CRLF ),
 _( "\r\0",   UNIX_CLI_PARSE_ACTION_CRLF ),       /* Telnet does this */
 _( "\r",     UNIX_CLI_PARSE_ACTION_CRLF ),

 /* Unix shell control codes */
 _( CTL('B'), UNIX_CLI_PARSE_ACTION_LEFT ),
 _( CTL('F'), UNIX_CLI_PARSE_ACTION_RIGHT ),
 _( CTL('P'), UNIX_CLI_PARSE_ACTION_UP ),
 _( CTL('N'), UNIX_CLI_PARSE_ACTION_DOWN ),
 _( CTL('A'), UNIX_CLI_PARSE_ACTION_HOME ),
 _( CTL('E'), UNIX_CLI_PARSE_ACTION_END ),
 _( CTL('D'), UNIX_CLI_PARSE_ACTION_ERASERIGHT ),
 _( CTL('U'), UNIX_CLI_PARSE_ACTION_ERASELINELEFT ),
 _( CTL('K'), UNIX_CLI_PARSE_ACTION_ERASELINERIGHT ),
 _( CTL('Y'), UNIX_CLI_PARSE_ACTION_YANK ),
 _( CTL('L'), UNIX_CLI_PARSE_ACTION_CLEAR ),
 _( ESC "b",  UNIX_CLI_PARSE_ACTION_WORDLEFT ),   /* Alt-B */
 _( ESC "f",  UNIX_CLI_PARSE_ACTION_WORDRIGHT ),  /* Alt-F */
 _( "\b",     UNIX_CLI_PARSE_ACTION_ERASE ),      /* ^H */
 _( "\x7f",   UNIX_CLI_PARSE_ACTION_ERASE ),      /* Backspace */
 _( "\t",     UNIX_CLI_PARSE_ACTION_TAB ),        /* ^I */

 /* VT100 Normal mode - Broadest support */
 _( CSI "A",  UNIX_CLI_PARSE_ACTION_UP ),
 _( CSI "B",  UNIX_CLI_PARSE_ACTION_DOWN ),
 _( CSI "C",  UNIX_CLI_PARSE_ACTION_RIGHT ),
 _( CSI "D",  UNIX_CLI_PARSE_ACTION_LEFT ),
 _( CSI "H",  UNIX_CLI_PARSE_ACTION_HOME ),
 _( CSI "F",  UNIX_CLI_PARSE_ACTION_END ),
 _( CSI "3~", UNIX_CLI_PARSE_ACTION_ERASERIGHT ), /* Delete */
 _( CSI "1;5D", UNIX_CLI_PARSE_ACTION_WORDLEFT ), /* C-Left */
 _( CSI "1;5C", UNIX_CLI_PARSE_ACTION_WORDRIGHT ),/* C-Right */

  /* VT100 Application mode - Some Gnome Terminal functions use these */
 _( ESC "OA", UNIX_CLI_PARSE_ACTION_UP ),
 _( ESC "OB", UNIX_CLI_PARSE_ACTION_DOWN ),
 _( ESC "OC", UNIX_CLI_PARSE_ACTION_RIGHT ),
 _( ESC "OD", UNIX_CLI_PARSE_ACTION_LEFT ),
 _( ESC "OH", UNIX_CLI_PARSE_ACTION_HOME ),
 _( ESC "OF", UNIX_CLI_PARSE_ACTION_END ),

 /* ANSI X3.41-1974 - sent by Microsoft Telnet and PuTTY */
 _( CSI "1~", UNIX_CLI_PARSE_ACTION_HOME ),
 _( CSI "4~", UNIX_CLI_PARSE_ACTION_END ),

 /* Emacs-ish history search */
 _( CTL('S'), UNIX_CLI_PARSE_ACTION_FWDSEARCH ),
 _( CTL('R'), UNIX_CLI_PARSE_ACTION_REVSEARCH ),

 /* Other protocol things */
 _( "\xff",   UNIX_CLI_PARSE_ACTION_TELNETIAC ),  /* IAC */
 _( "\0",     UNIX_CLI_PARSE_ACTION_NOACTION ),   /* NUL */
 _( NULL,     UNIX_CLI_PARSE_ACTION_NOMATCH )
};
static unix_cli_parse_actions_t unix_cli_parse_pager[] = {
 /* Line handling */
 _( "\r\n",   UNIX_CLI_PARSE_ACTION_PAGER_CRLF ),       /* Must be before '\r' */
 _( "\n",     UNIX_CLI_PARSE_ACTION_PAGER_CRLF ),
 _( "\r\0",   UNIX_CLI_PARSE_ACTION_PAGER_CRLF ),       /* Telnet does this */
 _( "\r",     UNIX_CLI_PARSE_ACTION_PAGER_CRLF ),

 /* Pager commands */
 _( " ",      UNIX_CLI_PARSE_ACTION_PAGER_NEXT ),
 _( "q",      UNIX_CLI_PARSE_ACTION_PAGER_QUIT ),
 _( "/",      UNIX_CLI_PARSE_ACTION_PAGER_SEARCH ),

 /* VT100 */
 _( CSI "A",  UNIX_CLI_PARSE_ACTION_PAGER_UP ),
 _( CSI "B",  UNIX_CLI_PARSE_ACTION_PAGER_DN ),
 _( CSI "H",  UNIX_CLI_PARSE_ACTION_PAGER_TOP ),
 _( CSI "F",  UNIX_CLI_PARSE_ACTION_PAGER_BOTTOM ),

 /* VT100 Application mode */
 _( ESC "OA", UNIX_CLI_PARSE_ACTION_PAGER_UP ),
 _( ESC "OB", UNIX_CLI_PARSE_ACTION_PAGER_DN ),
 _( ESC "OH", UNIX_CLI_PARSE_ACTION_PAGER_TOP ),
 _( ESC "OF", UNIX_CLI_PARSE_ACTION_PAGER_BOTTOM ),

 /* ANSI X3.41-1974 */
 _( CSI "1~", UNIX_CLI_PARSE_ACTION_PAGER_TOP ),
 _( CSI "4~", UNIX_CLI_PARSE_ACTION_PAGER_BOTTOM ),
 _( CSI "5~", UNIX_CLI_PARSE_ACTION_PAGER_PGUP ),
 _( CSI "6~", UNIX_CLI_PARSE_ACTION_PAGER_PGDN ),

 /* Other protocol things */
 _( "\xff",   UNIX_CLI_PARSE_ACTION_TELNETIAC ),  /* IAC */
 _( "\0",     UNIX_CLI_PARSE_ACTION_NOACTION ),   /* NUL */
 _( NULL,     UNIX_CLI_PARSE_ACTION_NOMATCH )
};
#undef _

typedef enum {
  UNIX_CLI_PROCESS_EVENT_READ_READY,
  UNIX_CLI_PROCESS_EVENT_QUIT,
} unix_cli_process_event_type_t;

typedef struct {
  /* Prompt string for CLI. */
  u8 * cli_prompt;

  unix_cli_file_t * cli_file_pool;

  u32 * unused_cli_process_node_indices;

  /* The session index of the stdin cli */
  u32 stdin_cli_file_index;

  /* File pool index of current input. */
  u32 current_input_file_index;
} unix_cli_main_t;

static unix_cli_main_t unix_cli_main;

/**
 * \brief Search for a byte sequence in the action list.
 *
 * Searches unix_cli_parse_actions[] for a match with the bytes in \c input
 * of maximum length \c ilen . When a match is made \c *matched indicates how
 * many bytes were matched. Returns a value from the enum
 * \c unix_cli_parse_action_t to indicate whether no match was found, a
 * partial match was found or a complete match was found and what action,
 * if any, should be taken.
 *
 * @param a       Actions list to search within.
 * @param input   String fragment to search for.
 * @param ilen    Length of the string in 'input'.
 * @param matched Pointer to an integer that will contain the number of
 *                bytes matched when a complete match is found.
 *
 * @return Action from \v unix_cli_parse_action_t that the string fragment
 *         matches.
 *         \c UNIX_CLI_PARSE_ACTION_PARTIALMATCH is returned when the whole
 *         input string matches the start of at least one action.
 *         \c UNIX_CLI_PARSE_ACTION_NOMATCH is returned when there is no
 *         match at all.
 */
static unix_cli_parse_action_t
unix_cli_match_action(unix_cli_parse_actions_t *a,
          u8 *input, u32 ilen, i32 *matched)
{
  u8 partial = 0;

  while (a->input)
    {
        if (ilen >= a->len)
          {
            /* see if the start of the input buffer exactly matches the current
             * action string. */
            if (memcmp(input, a->input, a->len) == 0)
              {
                *matched = a->len;
                return a->action;
              }
          }
        else
          {
            /* if the first ilen characters match, flag this as a partial -
             * meaning keep collecting bytes in case of a future match */
            if (memcmp(input, a->input, ilen) == 0)
                partial = 1;
          }

        /* check next action */
        a ++;
    }

  return partial ?
        UNIX_CLI_PARSE_ACTION_PARTIALMATCH :
        UNIX_CLI_PARSE_ACTION_NOMATCH;
}


static void
unix_cli_add_pending_output (unix_file_t * uf,
			     unix_cli_file_t * cf,
			     u8 * buffer,
			     uword buffer_bytes)
{
  unix_main_t * um = &unix_main;

  vec_add (cf->output_vector, buffer, buffer_bytes);
  if (vec_len (cf->output_vector) > 0)
    {
      int skip_update = 0 != (uf->flags & UNIX_FILE_DATA_AVAILABLE_TO_WRITE);
      uf->flags |= UNIX_FILE_DATA_AVAILABLE_TO_WRITE;
      if (! skip_update)
	um->file_update (uf, UNIX_FILE_UPDATE_MODIFY);
    }
}

static void
unix_cli_del_pending_output (unix_file_t * uf,
			     unix_cli_file_t * cf,
			     uword n_bytes)
{
  unix_main_t * um = &unix_main;

  vec_delete (cf->output_vector, n_bytes, 0);
  if (vec_len (cf->output_vector) <= 0)
    {
      int skip_update = 0 == (uf->flags & UNIX_FILE_DATA_AVAILABLE_TO_WRITE);
      uf->flags &= ~UNIX_FILE_DATA_AVAILABLE_TO_WRITE;
      if (! skip_update)
	um->file_update (uf, UNIX_FILE_UPDATE_MODIFY);
    }
}

/** \brief A bit like strchr with a buffer length limit.
 * Search a buffer for the first instance of a character up to the limit of
 * the buffer length. If found then return the position of that character.
 *
 * The key departure from strchr is that if the character is not found then
 * return the buffer length.
 *
 * @param chr The byte value to search for.
 * @param str The buffer in which to search for the value.
 * @param len The depth into the buffer to search.
 *
 * @return The index of the first occurence of \c chr. If \c chr is not
 *          found then \c len instead.
 */
always_inline word unix_vlib_findchr(u8 chr, u8 *str, word len)
{
    word i = 0;
    for (i = 0; i < len; i++, str++)
      {
        if (*str == chr)
          return i;
      }
    return len;
}

/** \brief Send a buffer to the CLI stream if possible, enqueue it otherwise.
 * Attempts to write given buffer to the file descriptor of the given
 * Unix CLI session. If that session already has data in the output buffer
 * or if the write attempt tells us to try again later then the given buffer
 * is appended to the pending output buffer instead.
 *
 * This is typically called only from \c unix_vlib_cli_output_cooked since
 * that is where CRLF handling occurs or from places where we explicitly do
 * not want cooked handling.
 *
 * @param cf Unix CLI session of the desired stream to write to.
 * @param uf The Unix file structure of the desired stream to write to.
 * @param buffer Pointer to the buffer that needs to be written.
 * @param buffer_bytes The number of bytes from \c buffer to write.
 */
static void unix_vlib_cli_output_raw(unix_cli_file_t * cf,
          unix_file_t * uf,
          u8 * buffer,
          uword buffer_bytes)
{
  int n = 0;

  if (vec_len (cf->output_vector) == 0)
      n = write (uf->file_descriptor, buffer, buffer_bytes);

  if (n < 0 && errno != EAGAIN)
    {
      clib_unix_warning ("write");
    }
  else if ((word) n < (word) buffer_bytes)
    {
      /* We got EAGAIN or we already have stuff in the buffer;
       * queue up whatever didn't get sent for later. */
      if (n < 0) n = 0;
      unix_cli_add_pending_output (uf, cf, buffer + n, buffer_bytes - n);
    }
}

/** \brief Process a buffer for CRLF handling before outputting it to the CLI.
 *
 * @param cf Unix CLI session of the desired stream to write to.
 * @param uf The Unix file structure of the desired stream to write to.
 * @param buffer Pointer to the buffer that needs to be written.
 * @param buffer_bytes The number of bytes from \c buffer to write.
 */
static void unix_vlib_cli_output_cooked(unix_cli_file_t * cf,
          unix_file_t * uf,
          u8 * buffer,
          uword buffer_bytes)
{
  word end = 0, start = 0;

  while (end < buffer_bytes)
    {
      if (cf->crlf_mode)
        {
          /* iterate the line on \n's so we can insert a \r before it */
          end = unix_vlib_findchr('\n',
                                  buffer + start,
                                  buffer_bytes - start) + start;
        }
      else
        {
          /* otherwise just send the whole buffer */
          end = buffer_bytes;
        }

      unix_vlib_cli_output_raw(cf, uf, buffer + start, end - start);

      if (cf->crlf_mode)
        {
          if (end < buffer_bytes)
            {
              unix_vlib_cli_output_raw(cf, uf, (u8 *)"\r\n", 2);
              end ++; /* skip the \n that we already sent */
            }
          start = end;
        }
    }
}

/** \brief Output the CLI prompt */
static void unix_cli_cli_prompt(unix_cli_file_t * cf, unix_file_t * uf)
{
  unix_cli_main_t * cm = &unix_cli_main;

  unix_vlib_cli_output_raw (cf, uf, cm->cli_prompt, vec_len (cm->cli_prompt));
}

/** \brief Output a pager prompt and show number of buffered lines */
static void unix_cli_pager_prompt(unix_cli_file_t * cf, unix_file_t * uf)
{
  u8 * prompt;

  prompt = format(0, "\r%s-- more -- (%d-%d/%d)%s",
    cf->ansi_capable ? ANSI_BOLD : "",
    cf->pager_start + 1,
    cf->pager_start + cf->height,
    cf->pager_lines,
    cf->ansi_capable ? ANSI_RESET: "");

  unix_vlib_cli_output_cooked(cf, uf, prompt, vec_len(prompt));

  vec_free(prompt);
}

/** \brief Output a pager "skipping" message */
static void unix_cli_pager_message(unix_cli_file_t * cf, unix_file_t * uf,
    char *message, char *postfix)
{
  u8 * prompt;

  prompt = format(0, "\r%s-- %s --%s%s",
    cf->ansi_capable ? ANSI_BOLD : "",
    message,
    cf->ansi_capable ? ANSI_RESET: "",
    postfix);

  unix_vlib_cli_output_cooked(cf, uf, prompt, vec_len(prompt));

  vec_free(prompt);
}

/** \brief Erase the printed pager prompt */
static void unix_cli_pager_prompt_erase(unix_cli_file_t * cf, unix_file_t * uf)
{
  if (cf->ansi_capable)
    {
      unix_vlib_cli_output_cooked(cf, uf, (u8 *)"\r", 1);
      unix_vlib_cli_output_cooked(cf, uf,
          (u8 *)ANSI_CLEARLINE, sizeof(ANSI_CLEARLINE) - 1);
    }
  else
    {
      int i;

      unix_vlib_cli_output_cooked(cf, uf, (u8 *)"\r", 1);
      for (i = 0; i < cf->width - 1; i ++)
        unix_vlib_cli_output_cooked(cf, uf, (u8 *)" ", 1);
      unix_vlib_cli_output_cooked(cf, uf, (u8 *)"\r", 1);
    }
}

/** \brief Uses an ANSI escape sequence to move the cursor */
static void unix_cli_ansi_cursor(unix_cli_file_t * cf, unix_file_t * uf,
      u16 x, u16 y)
{
  u8 * str;

  str = format(0, "%s%d;%dH", CSI, y, x);

  unix_vlib_cli_output_cooked(cf, uf, str, vec_len(str));

  vec_free(str);
}

/** \brief VLIB CLI output function. */
static void unix_vlib_cli_output (uword cli_file_index,
				  u8 * buffer,
				  uword buffer_bytes)
{
  unix_main_t * um = &unix_main;
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  unix_file_t * uf;

  cf = pool_elt_at_index (cm->cli_file_pool, cli_file_index);
  uf = pool_elt_at_index (um->file_pool, cf->unix_file_index);

  if (cf->no_pager || um->cli_pager_buffer_limit == 0 || cf->height == 0)
    {
      unix_vlib_cli_output_cooked(cf, uf, buffer, buffer_bytes);
    }
  else
    {
      /* At each \n add the line to the pager buffer.
       * If we've not yet displayed a whole page in this command then
       * also output the line.
       * If we have displayed a whole page then display a pager prompt
       * and count the lines we've buffered.
       */
      u8 * p = buffer;
      u8 * line;
      uword i, len = buffer_bytes;

      while (len)
        {
          i = unix_vlib_findchr('\n', p, len);
          if (i < len)
            i ++; /* include the '\n' */

          /* make a vec out of this substring */
          line = vec_new(u8, i);
          memcpy(line, p, i);

          /* store in pager buffer */
          vec_add1(cf->pager_vector, line);
          cf->pager_lines ++;

          if (cf->pager_lines < cf->height)
            {
              /* output this line */
              unix_vlib_cli_output_cooked(cf, uf, p, i);
              /* TODO: stop if line longer than cf->width and would
               * overflow cf->height */
            }
          else
            {
              /* Display the pager prompt every 10 lines */
              if (!(cf->pager_lines % 10))
                unix_cli_pager_prompt(cf, uf);
            }

          p += i;
          len -= i;
        }

        /* Check if we went over the pager buffer limit */
        if (cf->pager_lines > um->cli_pager_buffer_limit)
          {
            /* Stop using the pager for the remainder of this CLI command */
            cf->no_pager = 2;

            /* If we likely printed the prompt, erase it */
            if (cf->pager_lines > cf->height - 1)
              unix_cli_pager_prompt_erase (cf, uf);

            /* Dump out the contents of the buffer */
            for (i = cf->pager_start + (cf->height - 1);
                          i < cf->pager_lines; i ++)
              unix_vlib_cli_output_cooked (cf, uf,
                cf->pager_vector[i],
                vec_len(cf->pager_vector[i]));

            unix_cli_pager_reset (cf);
          }
    }
}

/** \brief Identify whether a terminal type is ANSI capable. */
static u8 unix_cli_terminal_type(u8 * term, uword len)
{
  /* This may later be better done as a hash of some sort. */
#define _(a) do { \
    if (strncasecmp(a, (char *)term, (size_t)len) == 0) return 1; \
  } while(0)

  _("xterm");
  _("xterm-color");
  _("xterm-256color"); /* iTerm on Mac */
  _("screen");
  _("ansi"); /* Microsoft Telnet */
#undef _

  return 0;
}

/** \brief Emit initial welcome banner and prompt on a connection. */
static void unix_cli_file_welcome(unix_cli_main_t * cm, unix_cli_file_t * cf)
{
  unix_main_t * um = &unix_main;
  unix_file_t * uf = pool_elt_at_index (um->file_pool, cf->unix_file_index);
  unix_cli_banner_t *banner;
  int i, len;

  /*
   * Put the first bytes directly into the buffer so that further output is
   * queued until everything is ready. (oterwise initial prompt can appear
   * mid way through VPP initialization)
   */
  unix_cli_add_pending_output (uf, cf, (u8 *)"\r", 1);

  if (!um->cli_no_banner)
    {
      if (cf->ansi_capable)
        {
          banner = unix_cli_banner_color;
          len = ARRAY_LEN(unix_cli_banner_color);
        }
      else
        {
          banner = unix_cli_banner;
          len = ARRAY_LEN(unix_cli_banner);
        }

      for (i = 0; i < len; i++)
        {
          unix_vlib_cli_output_cooked(cf, uf,
            banner[i].line,
            banner[i].length);
        }
      }

  /* Prompt. */
  unix_cli_cli_prompt (cf, uf);

  cf->started = 1;
}

/** \brief A failsafe triggered on a timer to ensure we send the prompt
 * to telnet sessions that fail to negotiate the terminal type. */
static void unix_cli_file_welcome_timer(any arg, f64 delay)
{
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  (void)delay;

  /* Check the connection didn't close already */
  if (pool_is_free_index (cm->cli_file_pool, (uword)arg))
    return;

  cf = pool_elt_at_index (cm->cli_file_pool, (uword)arg);

  if (!cf->started)
    unix_cli_file_welcome(cm, cf);
}

/** \brief A mostly no-op Telnet state machine.
 * Process Telnet command bytes in a way that ensures we're mostly
 * transparent to the Telnet protocol. That is, it's mostly a no-op.
 *
 * @return -1 if we need more bytes, otherwise a positive integer number of
 *          bytes to consume from the input_vector, not including the initial
 *          IAC byte.
 */
static i32 unix_cli_process_telnet(unix_main_t * um,
        unix_cli_file_t * cf,
        unix_file_t * uf,
        u8 * input_vector,
        uword len)
{
  /* Input_vector starts at IAC byte.
   * See if we have a complete message; if not, return -1 so we wait for more.
   * if we have a complete message, consume those bytes from the vector.
   */
  i32 consume = 0;

  if (len == 1)
    return -1; /* want more bytes */

  switch (input_vector[1])
    {
      case IAC:
        /* two IAC's in a row means to pass through 0xff.
         * since that makes no sense here, just consume it.
         */
        consume = 1;
        break;

      case WILL:
      case WONT:
      case DO:
      case DONT:
        /* Expect 3 bytes */
        if (vec_len(input_vector) < 3)
          return -1; /* want more bytes */

        consume = 2;
        break;

      case SB:
        {
          /* Sub option - search ahead for IAC SE to end it */
          i32 i;
          for (i = 3; i < len && i < UNIX_CLI_MAX_DEPTH_TELNET; i++)
            {
              if (input_vector[i - 1] == IAC && input_vector[i] == SE)
                {
                  /* We have a complete message; see if we care about it */
                  switch (input_vector[2])
                    {
                      case TELOPT_TTYPE:
                        if (input_vector[3] != 0)
                          break;
                        /* See if the terminal type is ANSI capable */
                        cf->ansi_capable =
                            unix_cli_terminal_type(input_vector + 4, i - 5);
                        /* If session not started, we can release the pause */
                        if (!cf->started)
                          /* Send the welcome banner and initial prompt */
                          unix_cli_file_welcome(&unix_cli_main, cf);
                        break;

                      case TELOPT_NAWS:
                        /* Window size */
                        if (i != 8) /* check message is correct size */
                          break;
                        cf->width = clib_net_to_host_u16(*((u16 *)(input_vector + 3)));
                        cf->height = clib_net_to_host_u16(*((u16 *)(input_vector + 5)));
                        break;

                      default:
                        break;
                    }
                  /* Consume it all */
                  consume = i;
                  break;
                }
            }

          if (i == UNIX_CLI_MAX_DEPTH_TELNET)
            consume = 1; /* hit max search depth, advance one byte */

          if (consume == 0)
            return -1; /* want more bytes */

          break;
        }

      case GA:
      case EL:
      case EC:
      case AO:
      case IP:
      case BREAK:
      case DM:
      case NOP:
      case SE:
      case EOR:
      case ABORT:
      case SUSP:
      case xEOF:
        /* Simple one-byte messages */
        consume = 1;
        break;

      case AYT:
        /* Are You There - trigger a visible response */
        consume = 1;
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) "fd.io VPP\n", 10);
        break;

      default:
        /* Unknown command! Eat the IAC byte */
        break;
    }

    return consume;
}

/** \brief Process actionable input.
 * Based on the \c action process the input; this typically involves
 * searching the command history or editing the current command line.
 */
static int unix_cli_line_process_one(unix_cli_main_t * cm,
        unix_main_t * um,
        unix_cli_file_t * cf,
        unix_file_t * uf,
        u8 input,
        unix_cli_parse_action_t action)
{
  u8 * prev;
  int j, delta;

  switch (action)
    {
    case UNIX_CLI_PARSE_ACTION_NOACTION:
      break;

    case UNIX_CLI_PARSE_ACTION_REVSEARCH:
    case UNIX_CLI_PARSE_ACTION_FWDSEARCH:
      if (!cf->has_history || !cf->history_limit)
        break;
      if (cf->search_mode == 0)
        {
          /* Erase the current command (if any) */
          for (j = 0; j < (vec_len (cf->current_command)); j++)
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b \b", 3);

          vec_reset_length (cf->search_key);
          vec_reset_length (cf->current_command);
          if (action == UNIX_CLI_PARSE_ACTION_REVSEARCH)
              cf->search_mode = -1;
          else
              cf->search_mode = 1;
          cf->cursor = 0;
        }
      else
        {
          if (action == UNIX_CLI_PARSE_ACTION_REVSEARCH)
            cf->search_mode = -1;
          else
            cf->search_mode = 1;

          cf->excursion += cf->search_mode;
          goto search_again;
        }
      break;

    case UNIX_CLI_PARSE_ACTION_ERASELINELEFT:
      /* Erase the command from the cursor to the start */

      /* Shimmy forwards to the new end of line position */
      delta = vec_len (cf->current_command) - cf->cursor;
      for (j = cf->cursor; j > delta; j--)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
      /* Zap from here to the end of what is currently displayed */
      for (; j < (vec_len (cf->current_command)); j++)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) " ", 1);
      /* Get back to the start of the line */
      for (j = 0; j < (vec_len (cf->current_command)); j++)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);

      j = vec_len(cf->current_command) - cf->cursor;
      memmove(cf->current_command,
              cf->current_command + cf->cursor,
              j);
      _vec_len(cf->current_command) = j;

      /* Print the new contents */
      unix_vlib_cli_output_cooked (cf, uf, cf->current_command, j);
      /* Shimmy back to the start */
      for (j = 0; j < (vec_len (cf->current_command)); j++)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
      cf->cursor = 0;

      cf->search_mode = 0;
      break;

    case UNIX_CLI_PARSE_ACTION_ERASELINERIGHT:
      /* Erase the command from the cursor to the end */

      /* Zap from cursor to end of what is currently displayed */
      for (j = cf->cursor; j < (vec_len (cf->current_command)); j++)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) " ", 1);
      /* Get back to where we were */
      for (j = cf->cursor; j < (vec_len (cf->current_command)); j++)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);

      /* Truncate the line at the cursor */
      _vec_len(cf->current_command) = cf->cursor;

      cf->search_mode = 0;
      break;

    case UNIX_CLI_PARSE_ACTION_LEFT:
      if (cf->cursor > 0)
        {
          unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
          cf->cursor --;
        }

      cf->search_mode = 0;
      break;

    case UNIX_CLI_PARSE_ACTION_RIGHT:
      if (cf->cursor < vec_len(cf->current_command))
        {
          /* have to emit the character under the cursor */
          unix_vlib_cli_output_cooked (cf, uf, cf->current_command + cf->cursor, 1);
          cf->cursor ++;
        }

      cf->search_mode = 0;
      break;

    case UNIX_CLI_PARSE_ACTION_UP:
    case UNIX_CLI_PARSE_ACTION_DOWN:
      if (!cf->has_history || !cf->history_limit)
        break;
      cf->search_mode = 0;
      /* Erase the command */
      for (j = cf->cursor; j < (vec_len (cf->current_command)); j++)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) " ", 1);
      for (j = 0; j < (vec_len (cf->current_command)); j++)
        unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b \b", 3);
      vec_reset_length (cf->current_command);
      if (vec_len (cf->command_history))
        {
          if (action == UNIX_CLI_PARSE_ACTION_UP)
            delta = -1;
          else
            delta = 1;

          cf->excursion += delta;

          if (cf->excursion == vec_len (cf->command_history))
            {
              /* down-arrowed to last entry - want a blank line */
              _vec_len (cf->current_command) = 0;
            }
          else if (cf->excursion < 0)
            {
              /* up-arrowed over the start to the end, want a blank line */
              cf->excursion = vec_len (cf->command_history);
              _vec_len (cf->current_command) = 0;
            }
          else
            {
              if (cf->excursion > (i32) vec_len (cf->command_history) -1)
                /* down-arrowed past end - wrap to start */
                cf->excursion = 0;

              /* Print the command at the current position */
              prev = cf->command_history [cf->excursion];
              vec_validate (cf->current_command, vec_len(prev)-1);

              clib_memcpy (cf->current_command, prev, vec_len(prev));
              _vec_len (cf->current_command) = vec_len(prev);
              unix_vlib_cli_output_cooked (cf, uf, cf->current_command,
                                           vec_len (cf->current_command));
            }
          cf->cursor = vec_len(cf->current_command);

          break;
        }
      break;

    case UNIX_CLI_PARSE_ACTION_HOME:
      if (vec_len (cf->current_command) && cf->cursor > 0)
        {
          while (cf->cursor)
            {
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
              cf->cursor --;
            }
        }

      cf->search_mode = 0;
      break;

    case UNIX_CLI_PARSE_ACTION_END:
      if (vec_len (cf->current_command) &&
              cf->cursor < vec_len(cf->current_command))
        {
          unix_vlib_cli_output_cooked (cf, uf,
                cf->current_command + cf->cursor,
                vec_len(cf->current_command) - cf->cursor);
          cf->cursor = vec_len(cf->current_command);
        }

      cf->search_mode = 0;
      break;

    case UNIX_CLI_PARSE_ACTION_WORDLEFT:
      if (vec_len (cf->current_command) && cf->cursor > 0)
        {
          j = cf->cursor;

          unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
          j --;

          while (j && isspace(cf->current_command[j]))
            {
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
              j --;
            }
          while (j && !isspace(cf->current_command[j]))
            {
              if (isspace(cf->current_command[j - 1]))
                break;
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
              j --;
            }

          cf->cursor = j;
        }

      cf->search_mode = 0;
      break;

    case UNIX_CLI_PARSE_ACTION_WORDRIGHT:
      if (vec_len (cf->current_command) &&
              cf->cursor < vec_len(cf->current_command))
        {
          int e = vec_len(cf->current_command);
          j = cf->cursor;
          while (j < e && !isspace(cf->current_command[j]))
            j ++;
          while (j < e && isspace(cf->current_command[j]))
            j ++;
          unix_vlib_cli_output_cooked (cf, uf,
                cf->current_command + cf->cursor,
                j - cf->cursor);
          cf->cursor = j;
        }

      cf->search_mode = 0;
      break;


    case UNIX_CLI_PARSE_ACTION_ERASE:
      if (vec_len (cf->current_command))
        {
          if (cf->cursor == vec_len(cf->current_command))
            {
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b \b", 3);
              _vec_len (cf->current_command)--;
              cf->cursor --;
            }
          else if (cf->cursor > 0)
            {
              /* shift everything at & to the right of the cursor left by 1 */
              j = vec_len (cf->current_command) - cf->cursor;
              memmove (cf->current_command + cf->cursor - 1,
                cf->current_command + cf->cursor,
                j);
              _vec_len (cf->current_command)--;
              cf->cursor --;
              /* redraw the rest of the line */
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
              unix_vlib_cli_output_cooked (cf, uf,
                    cf->current_command + cf->cursor, j);
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) " \b\b", 3);
              /* and shift the terminal cursor back where it should be */
              while (-- j)
                unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
            }
        }
      cf->search_mode = 0;
      cf->excursion = 0;
      vec_reset_length (cf->search_key);
      break;

    case UNIX_CLI_PARSE_ACTION_ERASERIGHT:
      if (vec_len (cf->current_command))
        {
          if (cf->cursor < vec_len(cf->current_command))
            {
              /* shift everything to the right of the cursor left by 1 */
              j = vec_len (cf->current_command) - cf->cursor - 1;
              memmove (cf->current_command + cf->cursor,
                cf->current_command + cf->cursor + 1,
                j);
              _vec_len (cf->current_command)--;
              /* redraw the rest of the line */
              unix_vlib_cli_output_cooked (cf, uf,
                    cf->current_command + cf->cursor, j);
              unix_vlib_cli_output_cooked (cf, uf, (u8 *) " \b", 2);
              /* and shift the terminal cursor back where it should be */
              if (j)
                {
                  unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
                  while (-- j)
                    unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);
                }
            }
        }
      else if (input == 'D' - '@')
        {
          /* ^D with no command entered = quit */
          unix_vlib_cli_output_cooked (cf, uf, (u8 *) "quit\n", 5);
          vlib_process_signal_event (um->vlib_main,
                vlib_current_process (um->vlib_main),
                UNIX_CLI_PROCESS_EVENT_QUIT,
                cf - cm->cli_file_pool);
        }
      cf->search_mode = 0;
      cf->excursion = 0;
      vec_reset_length (cf->search_key);
      break;

    case UNIX_CLI_PARSE_ACTION_CLEAR:
      /* If we're in ANSI mode, clear the screen.
       * Then redraw the prompt and any existing command input, then put
       * the cursor back where it was in that line.
       */
      if (cf->ansi_capable)
          unix_vlib_cli_output_cooked (cf, uf,
                    (u8 *) ANSI_CLEAR,
                    sizeof(ANSI_CLEAR)-1);
      else
          unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\n", 1);

      unix_vlib_cli_output_raw (cf, uf,
                cm->cli_prompt,
                vec_len (cm->cli_prompt));
      unix_vlib_cli_output_raw (cf, uf,
                cf->current_command,
                vec_len (cf->current_command));
      for (j = cf->cursor; j < vec_len(cf->current_command); j++)
         unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b", 1);

      break;

    case UNIX_CLI_PARSE_ACTION_TAB:
    case UNIX_CLI_PARSE_ACTION_YANK:
      /* TODO */
      break;


    case UNIX_CLI_PARSE_ACTION_PAGER_QUIT:
    pager_quit:
      unix_cli_pager_prompt_erase (cf, uf);
      unix_cli_pager_reset (cf);
      unix_cli_cli_prompt (cf, uf);
      break;

    case UNIX_CLI_PARSE_ACTION_PAGER_NEXT:
    case UNIX_CLI_PARSE_ACTION_PAGER_PGDN:
      /* show next page of the buffer */
      if (cf->height + cf->pager_start < cf->pager_lines)
        {
          u8 * line;
          int m = cf->pager_start + (cf->height - 1);
          unix_cli_pager_prompt_erase (cf, uf);
          for (j = m;
                j < cf->pager_lines && cf->pager_start < m;
                j ++, cf->pager_start ++)
            {
              line = cf->pager_vector[j];
              unix_vlib_cli_output_cooked (cf, uf, line, vec_len(line));
            }
          unix_cli_pager_prompt (cf, uf);
        }
      else
        {
          if (action == UNIX_CLI_PARSE_ACTION_PAGER_NEXT)
            /* no more in buffer, exit, but only if it was <space> */
            goto pager_quit;
        }
      break;

    case UNIX_CLI_PARSE_ACTION_PAGER_DN:
    case UNIX_CLI_PARSE_ACTION_PAGER_CRLF:
      /* display the next line of the buffer */
      if (cf->pager_start < cf->pager_lines - (cf->height - 1))
        {
          u8 * line;
          unix_cli_pager_prompt_erase (cf, uf);
          line = cf->pager_vector[cf->pager_start + (cf->height - 1)];
          unix_vlib_cli_output_cooked (cf, uf, line, vec_len(line));
          cf->pager_start ++;
          unix_cli_pager_prompt (cf, uf);
        }
      else
        {
          if (action == UNIX_CLI_PARSE_ACTION_PAGER_CRLF)
            /* no more in buffer, exit, but only if it was <enter> */
            goto pager_quit;
        }

      break;

    case UNIX_CLI_PARSE_ACTION_PAGER_UP:
      /* scroll the page back one line */
      if (cf->pager_start > 0)
        {
          u8 * line;
          cf->pager_start --;
          if (cf->ansi_capable)
            {
              unix_cli_pager_prompt_erase (cf, uf);
              unix_vlib_cli_output_cooked (cf, uf, (u8 *)ANSI_SCROLLDN, sizeof(ANSI_SCROLLDN) - 1);
              unix_vlib_cli_output_cooked (cf, uf, (u8 *)ANSI_SAVECURSOR, sizeof(ANSI_SAVECURSOR) - 1);
              unix_cli_ansi_cursor(cf, uf, 1, 1);
              unix_vlib_cli_output_cooked (cf, uf, (u8 *)ANSI_CLEARLINE, sizeof(ANSI_CLEARLINE) - 1);
              unix_vlib_cli_output_cooked (cf, uf, cf->pager_vector[cf->pager_start], vec_len(cf->pager_vector[cf->pager_start]));
              unix_vlib_cli_output_cooked (cf, uf, (u8 *)ANSI_RESTCURSOR, sizeof(ANSI_RESTCURSOR) - 1);
              unix_cli_pager_prompt_erase (cf, uf);
              unix_cli_pager_prompt (cf, uf);
            }
          else
            {
              int m = cf->pager_start + (cf->height - 1);
              unix_cli_pager_prompt_erase (cf, uf);
              for (j = cf->pager_start; j < cf->pager_lines && j < m; j ++)
                {
                  line = cf->pager_vector[j];
                  unix_vlib_cli_output_cooked (cf, uf, line, vec_len(line));
                }
              unix_cli_pager_prompt (cf, uf);
            }
        }
      break;

    case UNIX_CLI_PARSE_ACTION_PAGER_TOP:
      /* back to the first page of the buffer */
      if (cf->pager_start > 0)
        {
          u8 * line;
          cf->pager_start = 0;
          int m = cf->pager_start + (cf->height - 1);
          unix_cli_pager_prompt_erase (cf, uf);
          for (j = cf->pager_start; j < cf->pager_lines && j < m; j ++)
            {
              line = cf->pager_vector[j];
              unix_vlib_cli_output_cooked (cf, uf, line, vec_len(line));
            }
          unix_cli_pager_prompt (cf, uf);
        }
      break;

    case UNIX_CLI_PARSE_ACTION_PAGER_BOTTOM:
      /* skip to the last page of the buffer */
      if (cf->pager_start < cf->pager_lines - (cf->height - 1))
        {
          u8 * line;
          cf->pager_start = cf->pager_lines - (cf->height - 1);
          unix_cli_pager_prompt_erase (cf, uf);
          unix_cli_pager_message (cf, uf, "skipping", "\n");
          for (j = cf->pager_start; j < cf->pager_lines; j ++)
            {
              line = cf->pager_vector[j];
              unix_vlib_cli_output_cooked (cf, uf, line, vec_len(line));
            }
          unix_cli_pager_prompt (cf, uf);
        }
      break;

    case UNIX_CLI_PARSE_ACTION_PAGER_PGUP:
      /* wander back one page in the buffer */
      if (cf->pager_start > 0)
        {
          u8 * line;
          int m;
          if (cf->pager_start >= cf->height)
            cf->pager_start -= cf->height - 1;
          else
            cf->pager_start = 1;
          m = cf->pager_start + cf->height - 1;
          unix_cli_pager_prompt_erase (cf, uf);
          for (j = cf->pager_start; j < cf->pager_lines && j < m; j ++)
            {
              line = cf->pager_vector[j];
              unix_vlib_cli_output_cooked (cf, uf, line, vec_len(line));
            }
          unix_cli_pager_prompt (cf, uf);
        }
      break;

    case UNIX_CLI_PARSE_ACTION_PAGER_SEARCH:
      /* search forwards in the buffer */
      break;


    case UNIX_CLI_PARSE_ACTION_CRLF:
    crlf:
      vec_add1 (cf->current_command, '\r');
      vec_add1 (cf->current_command, '\n');
      unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\n", 1);

      vec_validate (cf->input_vector, vec_len(cf->current_command)-1);
      clib_memcpy (cf->input_vector, cf->current_command,
              vec_len(cf->current_command));
      _vec_len(cf->input_vector) = _vec_len (cf->current_command);

      if (cf->has_history && cf->history_limit)
        {
          if (cf->command_history && vec_len(cf->command_history) >= cf->history_limit)
            {
              vec_free (cf->command_history[0]);
              vec_delete (cf->command_history, 1, 0);
            }
          /* Don't add blank lines to the cmd history */
          if (vec_len (cf->current_command) > 2)
            {
              /* Don't duplicate the previous command */
              _vec_len (cf->current_command) -= 2;
              j = vec_len(cf->command_history);
              if (j == 0 ||
                (vec_len (cf->current_command) != vec_len (cf->command_history[j - 1]) ||
                    memcmp(cf->current_command, cf->command_history[j - 1],
                           vec_len (cf->current_command)) != 0))
                {
                  vec_add1 (cf->command_history, cf->current_command);
                  cf->current_command = 0;
                  cf->command_number ++;
                }
              else
                vec_reset_length (cf->current_command);
            }
          else
            vec_reset_length (cf->current_command);
          cf->excursion = vec_len (cf->command_history);
        }
      else /* history disabled */
        vec_reset_length (cf->current_command);
      cf->search_mode = 0;
      vec_reset_length (cf->search_key);
      cf->cursor = 0;

      return 0;

    case UNIX_CLI_PARSE_ACTION_PARTIALMATCH:
    case UNIX_CLI_PARSE_ACTION_NOMATCH:
      if (cf->pager_lines)
        {
          /* no-op for now */
        }
      else if (cf->has_history && cf->search_mode && isprint(input))
        {
          int k, limit, offset;
          u8 * item;

          vec_add1 (cf->search_key, input);

        search_again:
          for (j = 0; j < vec_len(cf->command_history); j++)
            {
              if (cf->excursion > (i32) vec_len (cf->command_history) -1)
                cf->excursion = 0;
              else if (cf->excursion < 0)
                cf->excursion = vec_len (cf->command_history) -1;

              item = cf->command_history[cf->excursion];

              limit = (vec_len(cf->search_key) > vec_len (item)) ?
                vec_len(item) : vec_len (cf->search_key);

              for (offset = 0; offset <= vec_len(item) - limit; offset++)
                {
                  for (k = 0; k < limit; k++)
                    {
                      if (item[k+offset] != cf->search_key[k])
                        goto next_offset;
                    }
                  goto found_at_offset;

                next_offset:
                  ;
                }
              goto next;

            found_at_offset:
              for (j = 0; j < vec_len (cf->current_command); j++)
                unix_vlib_cli_output_cooked (cf, uf, (u8 *) "\b \b", 3);

              vec_validate (cf->current_command, vec_len(item)-1);

              clib_memcpy (cf->current_command, item, vec_len(item));
              _vec_len (cf->current_command) = vec_len(item);
              unix_vlib_cli_output_cooked (cf, uf, cf->current_command,
                                           vec_len (cf->current_command));
              cf->cursor = vec_len (cf->current_command);
              goto found;

            next:
              cf->excursion += cf->search_mode;
            }

          unix_vlib_cli_output_cooked (cf, uf, (u8 *)"\nNo match...", 12);
          vec_reset_length (cf->search_key);
          vec_reset_length (cf->current_command);
          cf->search_mode = 0;
          cf->cursor = 0;
          goto crlf;
        }
      else if (isprint(input)) /* skip any errant control codes */
        {
          if (cf->cursor == vec_len(cf->current_command))
            {
              /* Append to end */
              vec_add1 (cf->current_command, input);
              cf->cursor ++;

              /* Echo the character back to the client */
              unix_vlib_cli_output_raw (cf, uf, &input, 1);
            }
          else
            {
              /* Insert at cursor: resize +1 byte, move everything over */
              j = vec_len (cf->current_command) - cf->cursor;
              vec_add1 (cf->current_command, (u8)'A');
              memmove (cf->current_command + cf->cursor + 1,
                cf->current_command + cf->cursor,
                j);
              cf->current_command[cf->cursor] = input;
              /* Redraw the line */
              j ++;
              unix_vlib_cli_output_raw (cf, uf,
                    cf->current_command + cf->cursor, j);
              /* Put terminal cursor back */
              while (-- j)
                unix_vlib_cli_output_raw (cf, uf, (u8 *)"\b", 1);
              cf->cursor ++;
            }
        }
      else
        {
          /* no-op - not printable or otherwise not actionable */
        }

    found:

      break;

    case UNIX_CLI_PARSE_ACTION_TELNETIAC:
      break;
    }
    return 1;
}

/** \brief Process input bytes on a stream to provide line editing and
 * command history in the CLI. */
static int unix_cli_line_edit (unix_cli_main_t * cm,
                      unix_main_t * um,
                      unix_cli_file_t * cf)
{
  unix_file_t * uf = pool_elt_at_index (um->file_pool, cf->unix_file_index);
  int i;

  for (i = 0; i < vec_len (cf->input_vector); i++)
    {
      unix_cli_parse_action_t action;
      /* See if the input buffer is some sort of control code */
      i32 matched = 0;
      unix_cli_parse_actions_t *a;

      /* If we're in the pager mode, search the pager actions */
      a = cf->pager_lines ? unix_cli_parse_pager : unix_cli_parse_strings;

      action = unix_cli_match_action(a, &cf->input_vector[i],
        vec_len (cf->input_vector) - i, &matched);

      switch (action)
        {
        case UNIX_CLI_PARSE_ACTION_PARTIALMATCH:
          if (i)
            {
              /* There was a partial match which means we need more bytes
               * than the input buffer currently has.
               * Since the bytes before here have been processed, shift
               * the current contents to the start of the input buffer.
               */
              int j = vec_len (cf->input_vector) - i;
              memmove(cf->input_vector, cf->input_vector + i, j);
              _vec_len(cf->input_vector) = j;
            }
          return 1; /* wait for more */

        case UNIX_CLI_PARSE_ACTION_TELNETIAC:
          /* process telnet options */
          matched = unix_cli_process_telnet(um, cf, uf,
                cf->input_vector + i, vec_len(cf->input_vector) - i);
          if (matched < 0)
            {
              if (i)
                {
                  /* There was a partial match which means we need more bytes
                   * than the input buffer currently has.
                   * Since the bytes before here have been processed, shift
                   * the current contents to the start of the input buffer.
                   */
                  int j = vec_len (cf->input_vector) - i;
                  memmove(cf->input_vector, cf->input_vector + i, j);
                  _vec_len(cf->input_vector) = j;
                }
              return 1; /* wait for more */
            }
          break;

        default:
          /* process the action */
          if (!unix_cli_line_process_one(cm, um, cf, uf,
                cf->input_vector[i], action))
            return 0; /* CRLF found */
        }

      i += matched;
    }

  vec_reset_length(cf->input_vector);
  return 1;
}

/** \brief Process input to a CLI session. */
static void unix_cli_process_input (unix_cli_main_t * cm,
                        uword cli_file_index)
{
  unix_main_t * um = &unix_main;
  unix_file_t * uf;
  unix_cli_file_t * cf = pool_elt_at_index (cm->cli_file_pool, cli_file_index);
  unformat_input_t input;
  int vlib_parse_eval (u8 *);

  /* Try vlibplex first.  Someday... */
  if (0 && vlib_parse_eval (cf->input_vector) == 0)
      goto done;

  /* Line edit, echo, etc. */
  if (!cf->line_mode  && unix_cli_line_edit (cm, um, cf))
    return;

  if (um->log_fd)
    {
      static u8 * lv;
      vec_reset_length (lv);
      lv = format (lv, "%U[%d]: %v", 
                   format_timeval,
                   0 /* current bat-time */,
                   0 /* current bat-format */,
                   cli_file_index,
                   cf->input_vector);
      {
	int rv __attribute__((unused)) = 
	  write (um->log_fd, lv, vec_len(lv));
      }
    }

  unformat_init_vector (&input, cf->input_vector);

  /* Remove leading white space from input. */
  (void) unformat (&input, "");

  cm->current_input_file_index = cli_file_index;
  cf->pager_lines = 0; /* start a new pager session */
  cf->pager_start = 0;

  if (unformat_check_input (&input) != UNFORMAT_END_OF_INPUT)
    vlib_cli_input (um->vlib_main, &input, unix_vlib_cli_output, cli_file_index);

  /* Re-fetch pointer since pool may have moved. */
  cf = pool_elt_at_index (cm->cli_file_pool, cli_file_index);
  uf = pool_elt_at_index (um->file_pool, cf->unix_file_index);

  /* Zero buffer since otherwise unformat_free will call vec_free on it. */
  input.buffer = 0;

  unformat_free (&input);

  /* Re-use input vector. */
done:
  _vec_len (cf->input_vector) = 0;

  if (cf->no_pager == 2)
    {
      /* Pager was programmatically disabled */
      unix_cli_pager_message (cf, uf, "pager buffer overflowed", "\n");
      cf->no_pager = um->cli_no_pager;
    }

  if (cf->pager_lines == 0 || cf->pager_lines < cf->height)
    {
      /* There was no need for the pager */
      unix_cli_pager_reset (cf);

      /* Prompt. */
      unix_cli_cli_prompt (cf, uf);
    }
  else
    {
      /* Display the pager prompt */
      unix_cli_pager_prompt(cf, uf);
    }
}

static void unix_cli_kill (unix_cli_main_t * cm, uword cli_file_index)
{
  unix_main_t * um = &unix_main;
  unix_cli_file_t * cf;
  unix_file_t * uf;
  int i;

  cf = pool_elt_at_index (cm->cli_file_pool, cli_file_index);
  uf = pool_elt_at_index (um->file_pool, cf->unix_file_index);

  /* Quit/EOF on stdin means quit program. */
  if (uf->file_descriptor == UNIX_CLI_STDIN_FD)
    clib_longjmp (&um->vlib_main->main_loop_exit, VLIB_MAIN_LOOP_EXIT_CLI);

  vec_free (cf->current_command);
  vec_free (cf->search_key);

  for (i = 0; i < vec_len (cf->command_history); i++)
      vec_free (cf->command_history[i]);

  vec_free (cf->command_history);

  unix_file_del (um, uf);

  unix_cli_file_free (cf);
  pool_put (cm->cli_file_pool, cf);
}

static uword
unix_cli_process (vlib_main_t * vm,
		  vlib_node_runtime_t * rt,
		  vlib_frame_t * f)
{
  unix_cli_main_t * cm = &unix_cli_main;
  uword i, * data = 0;

  while (1)
    {
      unix_cli_process_event_type_t event_type;
      vlib_process_wait_for_event (vm);
      event_type = vlib_process_get_events (vm, &data);

      switch (event_type)
	{
	case UNIX_CLI_PROCESS_EVENT_READ_READY:
	  for (i = 0; i < vec_len (data); i++)
	    unix_cli_process_input (cm, data[i]);
	  break;

	case UNIX_CLI_PROCESS_EVENT_QUIT:
	  /* Kill this process. */
	  for (i = 0; i < vec_len (data); i++)
	    unix_cli_kill (cm, data[i]);
	  goto done;
	}

      if (data)
	_vec_len (data) = 0;
    }

 done:
  vec_free (data);

  vlib_node_set_state (vm, rt->node_index, VLIB_NODE_STATE_DISABLED);

  /* Add node index so we can re-use this process later. */
  vec_add1 (cm->unused_cli_process_node_indices, rt->node_index);

  return 0;
}

static clib_error_t * unix_cli_write_ready (unix_file_t * uf)
{
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  int n;

  cf = pool_elt_at_index (cm->cli_file_pool, uf->private_data);

  /* Flush output vector. */
  n = write (uf->file_descriptor,
	     cf->output_vector, vec_len (cf->output_vector));

  if (n < 0 && errno != EAGAIN)
    return clib_error_return_unix (0, "write");

  else if (n > 0)
    unix_cli_del_pending_output (uf, cf, n);

  return /* no error */ 0;
}

static clib_error_t * unix_cli_read_ready (unix_file_t * uf)
{
  unix_main_t * um = &unix_main;
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  uword l;
  int n, n_read, n_try;

  cf = pool_elt_at_index (cm->cli_file_pool, uf->private_data);

  n = n_try = 4096;
  while (n == n_try) {
      l = vec_len (cf->input_vector);
      vec_resize (cf->input_vector, l + n_try);

      n = read (uf->file_descriptor, cf->input_vector + l, n_try);

      /* Error? */
      if (n < 0 && errno != EAGAIN)
          return clib_error_return_unix (0, "read");
  
      n_read = n < 0 ? 0 : n;
      _vec_len (cf->input_vector) = l + n_read;
  }

  if (! (n < 0))
    vlib_process_signal_event (um->vlib_main,
			       cf->process_node_index,
			       (n_read == 0
				? UNIX_CLI_PROCESS_EVENT_QUIT
				: UNIX_CLI_PROCESS_EVENT_READ_READY),
			       /* event data */ uf->private_data);

  return /* no error */ 0;
}

static u32 unix_cli_file_add (unix_cli_main_t * cm, char * name, int fd)
{
  unix_main_t * um = &unix_main;
  unix_cli_file_t * cf;
  unix_file_t template = {0};
  vlib_main_t * vm = um->vlib_main;
  vlib_node_t * n;

  name = (char *) format (0, "unix-cli-%s", name);

  if (vec_len (cm->unused_cli_process_node_indices) > 0)
    {
      uword l = vec_len (cm->unused_cli_process_node_indices);

      /* Find node and give it new name. */
      n = vlib_get_node (vm, cm->unused_cli_process_node_indices[l - 1]);
      vec_free (n->name);
      n->name = (u8 *) name;

      vlib_node_set_state (vm, n->index, VLIB_NODE_STATE_POLLING);

      _vec_len (cm->unused_cli_process_node_indices) = l - 1;
    }
  else
    {
      static vlib_node_registration_t r = {
	.function = unix_cli_process,
	.type = VLIB_NODE_TYPE_PROCESS,
	.process_log2_n_stack_bytes = 14,
      };

      r.name = name;
      vlib_register_node (vm, &r);
      vec_free (name);

      n = vlib_get_node (vm, r.index);
    }

  pool_get (cm->cli_file_pool, cf);
  memset (cf, 0, sizeof (*cf));

  template.read_function = unix_cli_read_ready;
  template.write_function = unix_cli_write_ready;
  template.file_descriptor = fd;
  template.private_data = cf - cm->cli_file_pool;

  cf->process_node_index = n->index;
  cf->unix_file_index = unix_file_add (um, &template);
  cf->output_vector = 0;
  cf->input_vector = 0;

  vlib_start_process (vm, n->runtime_index);
  return cf - cm->cli_file_pool;
}

static clib_error_t * unix_cli_listen_read_ready (unix_file_t * uf)
{
  unix_main_t * um = &unix_main;
  unix_cli_main_t * cm = &unix_cli_main;
  clib_socket_t * s = &um->cli_listen_socket;
  clib_socket_t client;
  char * client_name;
  clib_error_t * error;
  unix_cli_file_t * cf;
  u32 cf_index;

  error = clib_socket_accept (s, &client);
  if (error)
    return error;

  client_name = (char *) format (0, "%U%c", format_sockaddr, &client.peer, 0);

  cf_index = unix_cli_file_add (cm, client_name, client.fd);
  cf = pool_elt_at_index (cm->cli_file_pool, cf_index);

  /* No longer need CLIB version of socket. */
  clib_socket_free (&client);

  vec_free (client_name);

  /* if we're supposed to run telnet session in character mode (default) */
  if (um->cli_line_mode == 0)
    {
      /*
       * Set telnet client character mode, echo on, suppress "go-ahead".
       * Technically these should be negotiated, but this works.
       */
      u8 charmode_option[] = {
        IAC, WONT, TELOPT_LINEMODE, /* server will do char-by-char */
        IAC, DONT, TELOPT_LINEMODE, /* client should do char-by-char */
        IAC, WILL, TELOPT_SGA,      /* server willl supress GA */
        IAC, DO,   TELOPT_SGA,      /* client should supress Go Ahead */
        IAC, WILL, TELOPT_ECHO,     /* server will do echo */
        IAC, DONT, TELOPT_ECHO,     /* client should not echo */
        IAC, DO,   TELOPT_TTYPE,    /* client should tell us its term type */
        IAC, SB,   TELOPT_TTYPE, 1, IAC, SE, /* now tell me ttype */
        IAC, DO,   TELOPT_NAWS,     /* client should tell us its window sz */
        IAC, SB,   TELOPT_NAWS, 1, IAC, SE,  /* now tell me window size */
      };

      /* Enable history on this CLI */
      cf->history_limit = um->cli_history_limit;
      cf->has_history = cf->history_limit != 0;

      /* Make sure this session is in line mode */
      cf->line_mode = 0;

      /* We need CRLF */
      cf->crlf_mode = 1;

      /* Setup the pager */
      cf->no_pager = um->cli_no_pager;

      uf = pool_elt_at_index (um->file_pool, cf->unix_file_index);

      /* Send the telnet options */
      unix_vlib_cli_output_raw (cf, uf, charmode_option,
                                   ARRAY_LEN(charmode_option));

      /* In case the client doesn't negotiate terminal type, use
       * a timer to kick off the initial prompt. */
      timer_call (unix_cli_file_welcome_timer, cf_index, 1);
    }

  return error;
}

static void
unix_cli_resize_interrupt (int signum)
{
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf = pool_elt_at_index (cm->cli_file_pool,
                                cm->stdin_cli_file_index);
  struct winsize ws;
  (void)signum;

  /* Terminal resized, fetch the new size */
  ioctl(UNIX_CLI_STDIN_FD, TIOCGWINSZ, &ws);
  cf->width = ws.ws_col;
  cf->height = ws.ws_row;
}

static clib_error_t *
unix_cli_config (vlib_main_t * vm, unformat_input_t * input)
{
  unix_main_t * um = &unix_main;
  unix_cli_main_t * cm = &unix_cli_main;
  int flags;
  clib_error_t * error = 0;
  unix_cli_file_t * cf;
  u32 cf_index;
  struct termios tio;
  struct sigaction sa;
  struct winsize ws;
  u8 * term;

  /* We depend on unix flags being set. */
  if ((error = vlib_call_config_function (vm, unix_config)))
    return error;

  if (um->flags & UNIX_FLAG_INTERACTIVE)
    {
      /* Set stdin to be non-blocking. */
      if ((flags = fcntl (UNIX_CLI_STDIN_FD, F_GETFL, 0)) < 0)
        flags = 0;
      fcntl (UNIX_CLI_STDIN_FD, F_SETFL, flags | O_NONBLOCK);

      cf_index = unix_cli_file_add (cm, "stdin", UNIX_CLI_STDIN_FD);
      cf = pool_elt_at_index (cm->cli_file_pool, cf_index);
      cm->stdin_cli_file_index = cf_index;

      /* If stdin is a tty and we are using chacracter mode, enable
       * history on the CLI and set the tty line discipline accordingly. */
      if (isatty(UNIX_CLI_STDIN_FD) && um->cli_line_mode == 0)
        {
          /* Capture terminal resize events */
          sa.sa_handler = unix_cli_resize_interrupt;
          sa.sa_flags = 0;
          if (sigaction (SIGWINCH, &sa, 0) < 0)
              clib_panic ("sigaction");

          /* Retrieve the current terminal size */
          ioctl(UNIX_CLI_STDIN_FD, TIOCGWINSZ, &ws);
          cf->width = ws.ws_col;
          cf->height = ws.ws_row;

          if (cf->width == 0 || cf->height == 0)
            /* We have a tty, but no size. Stick to line mode. */
            goto notty;

          /* Setup the history */
          cf->history_limit = um->cli_history_limit;
          cf->has_history = cf->history_limit != 0;

          /* Setup the pager */
          cf->no_pager = um->cli_no_pager;

          /* We're going to be in char by char mode */
          cf->line_mode = 0;

          /* Save the original tty state so we can restore it later */
          tcgetattr(UNIX_CLI_STDIN_FD, &um->tio_stdin);
          um->tio_isset = 1;

          /* Tweak the tty settings */
          tio = um->tio_stdin;
          /* echo off, canonical mode off, ext'd input processing off */
          tio.c_lflag &= ~(ECHO | ICANON | IEXTEN);
          tio.c_cc[VMIN] = 1; /* 1 byte at a time */
          tio.c_cc[VTIME] = 0; /* no timer */
          tcsetattr(UNIX_CLI_STDIN_FD, TCSAFLUSH, &tio);

          /* See if we can do ANSI/VT100 output */
          term = (u8 *)getenv("TERM");
          if (term != NULL)
            cf->ansi_capable = unix_cli_terminal_type(term,
                        strlen((char *)term));
        }
      else
        {
        notty:
          /* No tty, so make sure these things are off */
          cf->no_pager = 1;
          cf->history_limit = 0;
          cf->has_history = 0;
          cf->line_mode = 1;
        }

      /* Send banner and initial prompt */
      unix_cli_file_welcome(cm, cf);
    }

  /* If we have socket config, LISTEN, otherwise, don't */
  clib_socket_t * s = &um->cli_listen_socket;
  if(s->config && s->config[0] != 0) {
    /* CLI listen. */
    unix_file_t template = {0};

    s->flags = SOCKET_IS_SERVER; /* listen, don't connect */
    error = clib_socket_init (s);

    if (error)
      return error;

    template.read_function = unix_cli_listen_read_ready;
    template.file_descriptor = s->fd;

    unix_file_add (um, &template);
  }

  /* Set CLI prompt. */
  if (! cm->cli_prompt)
    cm->cli_prompt = format (0, "VLIB: ");

  return 0;
}

VLIB_CONFIG_FUNCTION (unix_cli_config, "unix-cli");

static clib_error_t *
unix_cli_exit (vlib_main_t * vm)
{
  unix_main_t * um = &unix_main;

  /* If stdin is a tty and we saved the tty state, reset the tty state */
  if (isatty(UNIX_CLI_STDIN_FD) && um->tio_isset)
    tcsetattr(UNIX_CLI_STDIN_FD, TCSAFLUSH, &um->tio_stdin);

  return 0;
}

VLIB_MAIN_LOOP_EXIT_FUNCTION (unix_cli_exit);

void vlib_unix_cli_set_prompt (char * prompt)
{
  char * fmt = (prompt[strlen(prompt)-1] == ' ') ? "%s" : "%s ";
  unix_cli_main_t * cm = &unix_cli_main;
  if (cm->cli_prompt)
    vec_free (cm->cli_prompt);
  cm->cli_prompt = format (0, fmt, prompt);
}

static clib_error_t *
unix_cli_quit (vlib_main_t * vm,
	       unformat_input_t * input,
	       vlib_cli_command_t * cmd)
{
  unix_cli_main_t * cm = &unix_cli_main;

  vlib_process_signal_event (vm,
			     vlib_current_process (vm),
			     UNIX_CLI_PROCESS_EVENT_QUIT,
			     cm->current_input_file_index);
  return 0;
}

VLIB_CLI_COMMAND (unix_cli_quit_command, static) = {
  .path = "quit",
  .short_help = "Exit CLI",
  .function = unix_cli_quit,
};

static clib_error_t *
unix_cli_exec (vlib_main_t * vm,
	       unformat_input_t * input,
	       vlib_cli_command_t * cmd)
{
  char * file_name;
  int fd;
  unformat_input_t sub_input;
  clib_error_t * error;

  file_name = 0;
  fd = -1;
  error = 0;

  if (! unformat (input, "%s", &file_name))
    {
      error = clib_error_return (0, "expecting file name, got `%U'",
				 format_unformat_error, input);
      goto done;
    }

  fd = open (file_name, O_RDONLY);
  if (fd < 0)
    {
      error = clib_error_return_unix (0, "failed to open `%s'", file_name);
      goto done;
    }

  /* Make sure its a regular file. */
  {
    struct stat s;

    if (fstat (fd, &s) < 0)
      {
	error = clib_error_return_unix (0, "failed to stat `%s'", file_name);
	goto done;
      }
    
    if (! (S_ISREG (s.st_mode) || S_ISLNK (s.st_mode)))
      {
	error = clib_error_return (0, "not a regular file `%s'", file_name);
	goto done;
      }
  }

  unformat_init_unix_file (&sub_input, fd);

  vlib_cli_input (vm, &sub_input, 0, 0);
  unformat_free (&sub_input);

 done:
  if (fd > 0)
    close (fd);
  vec_free (file_name);

  return error;
}

VLIB_CLI_COMMAND (cli_exec, static) = {
  .path = "exec",
  .short_help = "Execute commands from file",
  .function = unix_cli_exec,
  .is_mp_safe = 1,
};

static clib_error_t *
unix_show_errors (vlib_main_t * vm,
		  unformat_input_t * input,
		  vlib_cli_command_t * cmd)
{
  unix_main_t * um = &unix_main;
  clib_error_t * error = 0;
  int i, n_errors_to_show;
  unix_error_history_t * unix_errors = 0;

  n_errors_to_show = 1 << 30;

  if (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (! unformat (input, "%d", &n_errors_to_show))
	{
	  error = clib_error_return (0, "expecting integer number of errors to show, got `%U'",
				     format_unformat_error, input);
	  goto done;
	}
    }

  n_errors_to_show = clib_min (ARRAY_LEN (um->error_history), n_errors_to_show);

  i = um->error_history_index > 0 ? um->error_history_index - 1 : ARRAY_LEN (um->error_history) - 1;

  while (n_errors_to_show > 0)
    {
      unix_error_history_t * eh = um->error_history + i;

      if (! eh->error)
	break;

      vec_add1 (unix_errors, eh[0]);
      n_errors_to_show -= 1;
      if (i == 0)
	i = ARRAY_LEN (um->error_history) - 1;
      else
	i--;
    }

  if (vec_len (unix_errors) == 0)
    vlib_cli_output (vm, "no Unix errors so far");
  else
    {
      vlib_cli_output (vm, "%Ld total errors seen", um->n_total_errors);
      for (i = vec_len (unix_errors) - 1; i >= 0; i--)
	{
	  unix_error_history_t * eh = vec_elt_at_index (unix_errors, i);
	  vlib_cli_output (vm, "%U: %U",
			   format_time_interval, "h:m:s:u", eh->time,
			   format_clib_error, eh->error);
	}
      vlib_cli_output (vm, "%U: time now",
		       format_time_interval, "h:m:s:u", vlib_time_now (vm));
    }

 done:
  vec_free (unix_errors);
  return error;
}

VLIB_CLI_COMMAND (cli_unix_show_errors, static) = {
  .path = "show unix-errors",
  .short_help = "Show Unix system call error history",
  .function = unix_show_errors,
};

static clib_error_t *
unix_cli_show_history (vlib_main_t * vm,
      unformat_input_t * input,
      vlib_cli_command_t * cmd)
{
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  int i, j;

  cf = pool_elt_at_index (cm->cli_file_pool, cm->current_input_file_index);

  if (cf->has_history && cf->history_limit)
    {
      i = 1 + cf->command_number - vec_len(cf->command_history);
      for (j = 0; j < vec_len (cf->command_history); j++)
          vlib_cli_output (vm, "%d  %v\n", i + j, cf->command_history[j]);
    }
  else
    {
      vlib_cli_output (vm, "History not enabled.\n");
    }

  return 0;
}

VLIB_CLI_COMMAND (cli_unix_cli_show_history, static) = {
  .path = "history",
  .short_help = "Show current session command history",
  .function = unix_cli_show_history,
};

static clib_error_t *
unix_cli_show_terminal (vlib_main_t * vm,
      unformat_input_t * input,
      vlib_cli_command_t * cmd)
{
  unix_main_t * um = &unix_main;
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  vlib_node_t * n;

  cf = pool_elt_at_index (cm->cli_file_pool, cm->current_input_file_index);
  n = vlib_get_node (vm, cf->process_node_index);

  vlib_cli_output (vm, "Terminal name:   %v\n", n->name);
  vlib_cli_output (vm, "Terminal mode:   %s\n", cf->line_mode ? 
                                                "line-by-line" :
                                                "char-by-char");
  vlib_cli_output (vm, "Terminal width:  %d\n", cf->width);
  vlib_cli_output (vm, "Terminal height: %d\n", cf->height);
  vlib_cli_output (vm, "ANSI capable:    %s\n", cf->ansi_capable ? "yes" : "no");
  vlib_cli_output (vm, "History enabled: %s%s\n",
        cf->has_history ? "yes" : "no",
        !cf->has_history || cf->history_limit ? "" : " (disabled by history limit)");
  if (cf->has_history)
    vlib_cli_output (vm, "History limit:   %d\n", cf->history_limit);
  vlib_cli_output (vm, "Pager enabled:   %s%s%s\n",
        cf->no_pager ? "no" : "yes",
        cf->no_pager || cf->height ? "" : " (disabled by terminal height)",
        cf->no_pager || um->cli_pager_buffer_limit ? "" : " (disabled by buffer limit)");
  if (!cf->no_pager)
    vlib_cli_output (vm, "Pager limit:     %d\n", um->cli_pager_buffer_limit);
  vlib_cli_output (vm, "CRLF mode:       %s\n", cf->crlf_mode ? "CR+LF" : "LF");

  return 0;
}

VLIB_CLI_COMMAND (cli_unix_cli_show_terminal, static) = {
  .path = "show terminal",
  .short_help = "Show current session terminal settings",
  .function = unix_cli_show_terminal,
};

static clib_error_t *
unix_cli_set_terminal_pager (vlib_main_t * vm,
      unformat_input_t * input,
      vlib_cli_command_t * cmd)
{
  unix_main_t * um = &unix_main;
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  unformat_input_t _line_input, * line_input = &_line_input;

  if (! unformat_user (input, unformat_line_input, line_input))
    return 0;

  cf = pool_elt_at_index (cm->cli_file_pool, cm->current_input_file_index);

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
    if (unformat (line_input, "on"))
      cf->no_pager = 0;
    else if (unformat (line_input, "off"))
      cf->no_pager = 1;
    else if (unformat (line_input, "limit %u", &um->cli_pager_buffer_limit))
      vlib_cli_output (vm, "Pager limit set to %u lines; note, this is global.\n",
        um->cli_pager_buffer_limit);
    else
      return clib_error_return (0, "unknown parameter: `%U`",
                                format_unformat_error, line_input);
  }

  unformat_free(line_input);

  return 0;
}

VLIB_CLI_COMMAND (cli_unix_cli_set_terminal_pager, static) = {
  .path = "set terminal pager",
  .short_help = "set terminal pager [on|off] [limit <lines>]",
  .function = unix_cli_set_terminal_pager,
};

static clib_error_t *
unix_cli_set_terminal_history (vlib_main_t * vm,
      unformat_input_t * input,
      vlib_cli_command_t * cmd)
{
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;
  unformat_input_t _line_input, * line_input = &_line_input;
  u32 limit;

  if (! unformat_user (input, unformat_line_input, line_input))
    return 0;

  cf = pool_elt_at_index (cm->cli_file_pool, cm->current_input_file_index);

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
    if (unformat (line_input, "on"))
      cf->has_history = 1;
    else if (unformat (line_input, "off"))
      cf->has_history = 0;
    else if (unformat (line_input, "limit %u", &cf->history_limit))
      ;
    else
      return clib_error_return (0, "unknown parameter: `%U`",
                                format_unformat_error, line_input);

    /* If we reduced history size, or turned it off, purge the history */
    limit = cf->has_history ? cf->history_limit : 0;

    while (cf->command_history && vec_len(cf->command_history) >= limit)
      {
        vec_free (cf->command_history[0]);
        vec_delete (cf->command_history, 1, 0);
      }
  }

  unformat_free(line_input);

  return 0;
}

VLIB_CLI_COMMAND (cli_unix_cli_set_terminal_history, static) = {
  .path = "set terminal history",
  .short_help = "set terminal history [on|off] [limit <lines>]",
  .function = unix_cli_set_terminal_history,
};

static clib_error_t *
unix_cli_set_terminal_ansi (vlib_main_t * vm,
      unformat_input_t * input,
      vlib_cli_command_t * cmd)
{
  unix_cli_main_t * cm = &unix_cli_main;
  unix_cli_file_t * cf;

  cf = pool_elt_at_index (cm->cli_file_pool, cm->current_input_file_index);

  if (unformat (input, "on"))
    cf->ansi_capable = 1;
  else if (unformat (input, "off"))
    cf->ansi_capable = 0;
  else
    return clib_error_return (0, "unknown parameter: `%U`",
                              format_unformat_error, input);

  return 0;
}

VLIB_CLI_COMMAND (cli_unix_cli_set_terminal_ansi, static) = {
  .path = "set terminal ansi",
  .short_help = "set terminal ansi [on|off]",
  .function = unix_cli_set_terminal_ansi,
};

static clib_error_t *
unix_cli_init (vlib_main_t * vm)
{
  return 0;
}

VLIB_INIT_FUNCTION (unix_cli_init);
