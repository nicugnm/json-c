/*
 * $Id: json_tokener.c,v 1.20 2006/07/25 03:24:50 mclark Exp $
 *
 * Copyright (c) 2004, 2005 Metaparadigm Pte. Ltd.
 * Michael Clark <michael@metaparadigm.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 *
 * Copyright (c) 2008-2009 Yahoo! Inc.  All rights reserved.
 * The copyrights to the contents of this file are licensed under the MIT License
 * (http://www.opensource.org/licenses/mit-license.php)
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>

#include "bits.h"
#include "debug.h"
#include "printbuf.h"
#include "arraylist.h"
#include "json_inttypes.h"
#include "json_object.h"
#include "json_tokener.h"
#include "json_util.h"

#if !HAVE_STRNCASECMP && defined(_MSC_VER)
  /* MSC has the version as _strnicmp */
# define strncasecmp _strnicmp
#elif !HAVE_STRNCASECMP
# error You do not have strncasecmp on your system.
#endif /* HAVE_STRNCASECMP */


static const char* json_null_str = "null";
static const char* json_true_str = "true";
static const char* json_false_str = "false";

const char* json_tokener_errors[] = {
  "success",
  "continue",
  "nesting to deep",
  "unexpected end of data",
  "unexpected character",
  "null expected",
  "boolean expected",
  "number expected",
  "array value separator ',' expected",
  "quoted object property name expected",
  "object property name separator ':' expected",
  "object value separator ',' expected",
  "invalid string sequence",
  "expected comment",
};

/* Stuff for decoding unicode sequences */
#define IS_HIGH_SURROGATE(uc) (((uc) & 0xFC00) == 0xD800)
#define IS_LOW_SURROGATE(uc)  (((uc) & 0xFC00) == 0xDC00)
#define DECODE_SURROGATE_PAIR(hi,lo) ((((hi) & 0x3FF) << 10) + ((lo) & 0x3FF) + 0x10000)
static unsigned char utf8_replacement_char[3] = { 0xEF, 0xBF, 0xBD };


struct json_tokener* json_tokener_new(void)
{
  struct json_tokener *tok;

  tok = (struct json_tokener*)calloc(1, sizeof(struct json_tokener));
  if (!tok) return NULL;
  tok->pb = printbuf_new();
  json_tokener_reset(tok);
  return tok;
}

void json_tokener_free(struct json_tokener *tok)
{
  json_tokener_reset(tok);
  if(tok) printbuf_free(tok->pb);
  free(tok);
}

static void json_tokener_reset_level(struct json_tokener *tok, int depth)
{
  tok->stack[depth].state = json_tokener_state_eatws;
  tok->stack[depth].saved_state = json_tokener_state_start;
  json_object_put(tok->stack[depth].current);
  tok->stack[depth].current = NULL;
  free(tok->stack[depth].obj_field_name);
  tok->stack[depth].obj_field_name = NULL;
}

void json_tokener_reset(struct json_tokener *tok)
{
  int i;
  if (!tok)
    return;

  for(i = tok->depth; i >= 0; i--)
    json_tokener_reset_level(tok, i);
  tok->depth = 0;
  tok->err = json_tokener_success;
}

struct json_object* json_tokener_parse(const char *str)
{
  struct json_tokener* tok;
  struct json_object* obj;

  tok = json_tokener_new();
  obj = json_tokener_parse_ex(tok, str, -1);
  if(tok->err != json_tokener_success)
    obj = NULL;
  json_tokener_free(tok);
  return obj;
}

struct json_object* json_tokener_parse_verbose(const char *str, enum json_tokener_error *error)
{
    struct json_tokener* tok;
    struct json_object* obj;

    tok = json_tokener_new();
    obj = json_tokener_parse_ex(tok, str, -1);
    *error = tok->err;
    if(tok->err != json_tokener_success) {
        obj = NULL;
    }

    json_tokener_free(tok);
    return obj;
}


#if !HAVE_STRNDUP
/* CAW: compliant version of strndup() */
char* strndup(const char* str, size_t n)
{
  if(str) {
    size_t len = strlen(str);
    size_t nn = json_min(len,n);
    char* s = (char*)malloc(sizeof(char) * (nn + 1));

    if(s) {
      memcpy(s, str, nn);
      s[nn] = '\0';
    }

    return s;
  }

  return NULL;
}
#endif


#define state  tok->stack[tok->depth].state
#define saved_state  tok->stack[tok->depth].saved_state
#define current tok->stack[tok->depth].current
#define obj_field_name tok->stack[tok->depth].obj_field_name

/* Optimization:
 * json_tokener_parse_ex() consumed a lot of CPU in its main loop,
 * iterating character-by character.  A large performance boost is
 * achieved by using tighter loops to locally handle units such as
 * comments and strings.  Loops that handle an entire token within 
 * their scope also gather entire strings and pass them to 
 * printbuf_memappend() in a single call, rather than calling
 * printbuf_memappend() one char at a time.
 *
 * POP_CHAR() and ADVANCE_CHAR() macros are used for code that is
 * common to both the main loop and the tighter loops.
 */

/* POP_CHAR(dest, tok) macro:
 *   Not really a pop()...peeks at the current char and stores it in dest.
 *   Returns 1 on success, sets tok->err and returns 0 if no more chars.
 *   Implicit inputs:  str, len vars
 */
#define POP_CHAR(dest, tok)                                                  \
  (((tok)->char_offset == len) ?                                          \
   (((tok)->depth == 0 && state == json_tokener_state_eatws && saved_state == json_tokener_state_finish) ? \
    (((tok)->err = json_tokener_success), 0)                              \
    :                                                                   \
    (((tok)->err = json_tokener_continue), 0)                             \
    ) :                                                                 \
   (((dest) = *str), 1)                                                 \
   )
 
/* ADVANCE_CHAR() macro:
 *   Incrementes str & tok->char_offset.
 *   For convenience of existing conditionals, returns the old value of c (0 on eof)
 *   Implicit inputs:  c var
 */
#pragma GCC diagnostic ignored "-Wunused-value"
#define ADVANCE_CHAR(str, tok) \
  ( ++(str), ((tok)->char_offset)++, c)


/* End optimization macro defs */


struct json_object* json_tokener_parse_ex(struct json_tokener *tok,
					  const char *str, int len)
{
  struct json_object *obj = NULL;
  char c = '\1';

  tok->char_offset = 0;
  tok->err = json_tokener_success;

  while (POP_CHAR(c, tok)) {

  redo_char:
    switch(state) {

    case json_tokener_state_eatws:
      /* Advance until we change state */
      while (isspace((int)c)) {
	if ((!ADVANCE_CHAR(str, tok)) || (!POP_CHAR(c, tok)))
	  goto out;
      }
      if(c == '/') {
	printbuf_reset(tok->pb);
	printbuf_memappend_fast(tok->pb, &c, 1);
	state = json_tokener_state_comment_start;
      } else {
	state = saved_state;
	goto redo_char;
      }
      break;

    case json_tokener_state_start:
      switch(c) {
      case '{':
	state = json_tokener_state_eatws;
	saved_state = json_tokener_state_object_field_start;
	current = json_object_new_object();
	break;
      case '[':
	state = json_tokener_state_eatws;
	saved_state = json_tokener_state_array;
	current = json_object_new_array();
	break;
      case 'N':
      case 'n':
	state = json_tokener_state_null;
	printbuf_reset(tok->pb);
	tok->st_pos = 0;
	goto redo_char;
      case '"':
      case '\'':
	state = json_tokener_state_string;
	printbuf_reset(tok->pb);
	tok->quote_char = c;
	break;
      case 'T':
      case 't':
      case 'F':
      case 'f':
	state = json_tokener_state_boolean;
	printbuf_reset(tok->pb);
	tok->st_pos = 0;
	goto redo_char;
#if defined(__GNUC__)
	  case '0' ... '9':
#else
	  case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
#endif
      case '-':
	state = json_tokener_state_number;
	printbuf_reset(tok->pb);
	tok->is_double = 0;
	goto redo_char;
      default:
	tok->err = json_tokener_error_parse_unexpected;
	goto out;
      }
      break;

    case json_tokener_state_finish:
      if(tok->depth == 0) goto out;
      obj = json_object_get(current);
      json_tokener_reset_level(tok, tok->depth);
      tok->depth--;
      goto redo_char;

    case json_tokener_state_null:
      printbuf_memappend_fast(tok->pb, &c, 1);
      if(strncasecmp(json_null_str, tok->pb->buf,
		     json_min(tok->st_pos+1, strlen(json_null_str))) == 0) {
	if(tok->st_pos == strlen(json_null_str)) {
	  current = NULL;
	  saved_state = json_tokener_state_finish;
	  state = json_tokener_state_eatws;
	  goto redo_char;
	}
      } else {
	tok->err = json_tokener_error_parse_null;
	goto out;
      }
      tok->st_pos++;
      break;

    case json_tokener_state_comment_start:
      if(c == '*') {
	state = json_tokener_state_comment;
      } else if(c == '/') {
	state = json_tokener_state_comment_eol;
      } else {
	tok->err = json_tokener_error_parse_comment;
	goto out;
      }
      printbuf_memappend_fast(tok->pb, &c, 1);
      break;

    case json_tokener_state_comment:
              {
          /* Advance until we change state */
          const char *case_start = str;
          while(c != '*') {
            if (!ADVANCE_CHAR(str, tok) || !POP_CHAR(c, tok)) {
              printbuf_memappend_fast(tok->pb, case_start, str-case_start);
              goto out;
            } 
          }
          printbuf_memappend_fast(tok->pb, case_start, 1+str-case_start);
          state = json_tokener_state_comment_end;
        }
            break;

    case json_tokener_state_comment_eol:
      {
	/* Advance until we change state */
	const char *case_start = str;
	while(c != '\n') {
	  if (!ADVANCE_CHAR(str, tok) || !POP_CHAR(c, tok)) {
	    printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	    goto out;
	  }
	}
	printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	MC_DEBUG("json_tokener_comment: %s\n", tok->pb->buf);
	state = json_tokener_state_eatws;
      }
      break;

    case json_tokener_state_comment_end:
      printbuf_memappend_fast(tok->pb, &c, 1);
      if(c == '/') {
	MC_DEBUG("json_tokener_comment: %s\n", tok->pb->buf);
	state = json_tokener_state_eatws;
      } else {
	state = json_tokener_state_comment;
      }
      break;

    case json_tokener_state_string:
      {
	/* Advance until we change state */
	const char *case_start = str;
	while(1) {
	  if(c == tok->quote_char) {
	    printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	    current = json_object_new_string(tok->pb->buf);
	    saved_state = json_tokener_state_finish;
	    state = json_tokener_state_eatws;
	    break;
	  } else if(c == '\\') {
	    printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	    saved_state = json_tokener_state_string;
	    state = json_tokener_state_string_escape;
	    break;
	  }
	  if (!ADVANCE_CHAR(str, tok) || !POP_CHAR(c, tok)) {
	    printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	    goto out;
	  }
	}
      }
      break;

    case json_tokener_state_string_escape:
      switch(c) {
      case '"':
      case '\\':
      case '/':
	printbuf_memappend_fast(tok->pb, &c, 1);
	state = saved_state;
	break;
      case 'b':
      case 'n':
      case 'r':
      case 't':
	if(c == 'b') printbuf_memappend_fast(tok->pb, "\b", 1);
	else if(c == 'n') printbuf_memappend_fast(tok->pb, "\n", 1);
	else if(c == 'r') printbuf_memappend_fast(tok->pb, "\r", 1);
	else if(c == 't') printbuf_memappend_fast(tok->pb, "\t", 1);
	state = saved_state;
	break;
      case 'u':
	tok->ucs_char = 0;
	tok->st_pos = 0;
	state = json_tokener_state_escape_unicode;
	break;
      default:
	tok->err = json_tokener_error_parse_string;
	goto out;
      }
      break;

    case json_tokener_state_escape_unicode:
	{
          unsigned int got_hi_surrogate = 0;

	  /* Handle a 4-byte sequence, or two sequences if a surrogate pair */
	  while(1) {
	    if(strchr(json_hex_chars, c)) {
	      tok->ucs_char += ((unsigned int)hexdigit(c) << ((3-tok->st_pos++)*4));
	      if(tok->st_pos == 4) {
		unsigned char unescaped_utf[4];

                if (got_hi_surrogate) {
		  if (IS_LOW_SURROGATE(tok->ucs_char)) {
                    /* Recalculate the ucs_char, then fall thru to process normally */
                    tok->ucs_char = DECODE_SURROGATE_PAIR(got_hi_surrogate, tok->ucs_char);
                  } else {
                    /* Hi surrogate was not followed by a low surrogate */
                    /* Replace the hi and process the rest normally */
		    printbuf_memappend_fast(tok->pb, (char*)utf8_replacement_char, 3);
                  }
                  got_hi_surrogate = 0;
                }

		if (tok->ucs_char < 0x80) {
		  unescaped_utf[0] = tok->ucs_char;
		  printbuf_memappend_fast(tok->pb, (char*)unescaped_utf, 1);
		} else if (tok->ucs_char < 0x800) {
		  unescaped_utf[0] = 0xc0 | (tok->ucs_char >> 6);
		  unescaped_utf[1] = 0x80 | (tok->ucs_char & 0x3f);
		  printbuf_memappend_fast(tok->pb, (char*)unescaped_utf, 2);
		} else if (IS_HIGH_SURROGATE(tok->ucs_char)) {
                  /* Got a high surrogate.  Remember it and look for the
                   * the beginning of another sequence, which should be the
                   * low surrogate.
                   */
                  got_hi_surrogate = tok->ucs_char;
                  /* Not at end, and the next two chars should be "\u" */
                  if ((tok->char_offset+1 != len) &&
                      (tok->char_offset+2 != len) &&
                      (str[1] == '\\') &&
                      (str[2] == 'u'))
                  {
#pragma GCC diagnostic ignored "-Wunused-value"
	            ADVANCE_CHAR(str, tok);
	            ADVANCE_CHAR(str, tok);

                    /* Advance to the first char of the next sequence and
                     * continue processing with the next sequence.
                     */
	            if (!ADVANCE_CHAR(str, tok) || !POP_CHAR(c, tok)) {
	              printbuf_memappend_fast(tok->pb, (char*)utf8_replacement_char, 3);
	              goto out;
                    }
	            tok->ucs_char = 0;
                    tok->st_pos = 0;
                    continue; /* other json_tokener_state_escape_unicode */
                  } else {
                    /* Got a high surrogate without another sequence following
                     * it.  Put a replacement char in for the hi surrogate
                     * and pretend we finished.
                     */
		    printbuf_memappend_fast(tok->pb, (char*)utf8_replacement_char, 3);
                  }
		} else if (IS_LOW_SURROGATE(tok->ucs_char)) {
                  /* Got a low surrogate not preceded by a high */
		  printbuf_memappend_fast(tok->pb, (char*)utf8_replacement_char, 3);
                } else if (tok->ucs_char < 0x10000) {
		  unescaped_utf[0] = 0xe0 | (tok->ucs_char >> 12);
		  unescaped_utf[1] = 0x80 | ((tok->ucs_char >> 6) & 0x3f);
		  unescaped_utf[2] = 0x80 | (tok->ucs_char & 0x3f);
		  printbuf_memappend_fast(tok->pb, (char*)unescaped_utf, 3);
		} else if (tok->ucs_char < 0x110000) {
		  unescaped_utf[0] = 0xf0 | ((tok->ucs_char >> 18) & 0x07);
		  unescaped_utf[1] = 0x80 | ((tok->ucs_char >> 12) & 0x3f);
		  unescaped_utf[2] = 0x80 | ((tok->ucs_char >> 6) & 0x3f);
		  unescaped_utf[3] = 0x80 | (tok->ucs_char & 0x3f);
		  printbuf_memappend_fast(tok->pb, (char*)unescaped_utf, 4);
		} else {
                  /* Don't know what we got--insert the replacement char */
		  printbuf_memappend_fast(tok->pb, (char*)utf8_replacement_char, 3);
                }
		state = saved_state;
		break;
	      }
	    } else {
	      tok->err = json_tokener_error_parse_string;
	      goto out;
	    }
	  if (!ADVANCE_CHAR(str, tok) || !POP_CHAR(c, tok)) {
            if (got_hi_surrogate) /* Clean up any pending chars */
	      printbuf_memappend_fast(tok->pb, (char*)utf8_replacement_char, 3);
	    goto out;
	  }
	}
      }
      break;

    case json_tokener_state_boolean:
      printbuf_memappend_fast(tok->pb, &c, 1);
      if(strncasecmp(json_true_str, tok->pb->buf,
		     json_min(tok->st_pos+1, strlen(json_true_str))) == 0) {
	if(tok->st_pos == strlen(json_true_str)) {
	  current = json_object_new_boolean(1);
	  saved_state = json_tokener_state_finish;
	  state = json_tokener_state_eatws;
	  goto redo_char;
	}
      } else if(strncasecmp(json_false_str, tok->pb->buf,
			    json_min(tok->st_pos+1, strlen(json_false_str))) == 0) {
	if(tok->st_pos == strlen(json_false_str)) {
	  current = json_object_new_boolean(0);
	  saved_state = json_tokener_state_finish;
	  state = json_tokener_state_eatws;
	  goto redo_char;
	}
      } else {
	tok->err = json_tokener_error_parse_boolean;
	goto out;
      }
      tok->st_pos++;
      break;

    case json_tokener_state_number:
      {
	/* Advance until we change state */
	const char *case_start = str;
	int case_len=0;
	while(c && strchr(json_number_chars, c)) {
	  ++case_len;
	  if(c == '.' || c == 'e') tok->is_double = 1;
	  if (!ADVANCE_CHAR(str, tok) || !POP_CHAR(c, tok)) {
	    printbuf_memappend_fast(tok->pb, case_start, case_len);
	    goto out;
	  }
	}
        if (case_len>0)
          printbuf_memappend_fast(tok->pb, case_start, case_len);
      }
      {
	int64_t num64;
	double  numd;
	if (!tok->is_double && json_parse_int64(tok->pb->buf, &num64) == 0) {
		current = json_object_new_int64(num64);
	} else if(tok->is_double && sscanf(tok->pb->buf, "%lf", &numd) == 1) {
          current = json_object_new_double(numd);
        } else {
          tok->err = json_tokener_error_parse_number;
          goto out;
        }
        saved_state = json_tokener_state_finish;
        state = json_tokener_state_eatws;
        goto redo_char;
      }
      break;

    case json_tokener_state_array:
      if(c == ']') {
	saved_state = json_tokener_state_finish;
	state = json_tokener_state_eatws;
      } else {
	if(tok->depth >= JSON_TOKENER_MAX_DEPTH-1) {
	  tok->err = json_tokener_error_depth;
	  goto out;
	}
	state = json_tokener_state_array_add;
	tok->depth++;
	json_tokener_reset_level(tok, tok->depth);
	goto redo_char;
      }
      break;

    case json_tokener_state_array_add:
      json_object_array_add(current, obj);
      saved_state = json_tokener_state_array_sep;
      state = json_tokener_state_eatws;
      goto redo_char;

    case json_tokener_state_array_sep:
      if(c == ']') {
	saved_state = json_tokener_state_finish;
	state = json_tokener_state_eatws;
      } else if(c == ',') {
	saved_state = json_tokener_state_array;
	state = json_tokener_state_eatws;
      } else {
	tok->err = json_tokener_error_parse_array;
	goto out;
      }
      break;

    case json_tokener_state_object_field_start:
      if(c == '}') {
	saved_state = json_tokener_state_finish;
	state = json_tokener_state_eatws;
      } else if (c == '"' || c == '\'') {
	tok->quote_char = c;
	printbuf_reset(tok->pb);
	state = json_tokener_state_object_field;
      } else {
	tok->err = json_tokener_error_parse_object_key_name;
	goto out;
      }
      break;

    case json_tokener_state_object_field:
      {
	/* Advance until we change state */
	const char *case_start = str;
	while(1) {
	  if(c == tok->quote_char) {
	    printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	    obj_field_name = strdup(tok->pb->buf);
	    saved_state = json_tokener_state_object_field_end;
	    state = json_tokener_state_eatws;
	    break;
	  } else if(c == '\\') {
	    printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	    saved_state = json_tokener_state_object_field;
	    state = json_tokener_state_string_escape;
	    break;
	  }
	  if (!ADVANCE_CHAR(str, tok) || !POP_CHAR(c, tok)) {
	    printbuf_memappend_fast(tok->pb, case_start, str-case_start);
	    goto out;
	  }
	}
      }
      break;

    case json_tokener_state_object_field_end:
      if(c == ':') {
	saved_state = json_tokener_state_object_value;
	state = json_tokener_state_eatws;
      } else {
	tok->err = json_tokener_error_parse_object_key_sep;
	goto out;
      }
      break;

    case json_tokener_state_object_value:
      if(tok->depth >= JSON_TOKENER_MAX_DEPTH-1) {
	tok->err = json_tokener_error_depth;
	goto out;
      }
      state = json_tokener_state_object_value_add;
      tok->depth++;
      json_tokener_reset_level(tok, tok->depth);
      goto redo_char;

    case json_tokener_state_object_value_add:
      json_object_object_add(current, obj_field_name, obj);
      free(obj_field_name);
      obj_field_name = NULL;
      saved_state = json_tokener_state_object_sep;
      state = json_tokener_state_eatws;
      goto redo_char;

    case json_tokener_state_object_sep:
      if(c == '}') {
	saved_state = json_tokener_state_finish;
	state = json_tokener_state_eatws;
      } else if(c == ',') {
	saved_state = json_tokener_state_object_field_start;
	state = json_tokener_state_eatws;
      } else {
	tok->err = json_tokener_error_parse_object_value_sep;
	goto out;
      }
      break;

    }
    if (!ADVANCE_CHAR(str, tok))
      goto out;
  } /* while(POP_CHAR) */

 out:
  if (!c) { /* We hit an eof char (0) */
    if(state != json_tokener_state_finish &&
       saved_state != json_tokener_state_finish)
      tok->err = json_tokener_error_parse_eof;
  }

  if(tok->err == json_tokener_success) return json_object_get(current);
  MC_DEBUG("json_tokener_parse_ex: error %s at offset %d\n",
	   json_tokener_errors[tok->err], tok->char_offset);
  return NULL;
}
