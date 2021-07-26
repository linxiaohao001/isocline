/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "common.h"
#include "stringbuf.h"

// In place growable utf-8 strings
struct stringbuf_s {
  char*     buf;
  ssize_t   buflen;
  ssize_t   count;  
  alloc_t*  mem;
};


//-------------------------------------------------------------
// String column width
//-------------------------------------------------------------

// get `mk_wcwidth` for the column width of unicode characters
#include "wcwidth.c"

// column width of a utf8 single character sequence.
static ssize_t utf8_char_width( const char* s, ssize_t n ) {
  if (n <= 0) return 0;

  uint8_t b = (uint8_t)s[0];
  int32_t c;
  if (b < ' ') {
    return 0;
  }
  else if (b <= 0x7F) {
    return 1;
  }
  else if (b <= 0xC1) { // invalid continuation byte or invalid 0xC0, 0xC1 (check is strictly not necessary as we don't validate..)
    return 1;
  }
  else if (b <= 0xDF && n >= 2) { // b >= 0xC2  // 2 bytes
    c = (((b & 0x1F) << 6) | (s[1] & 0x3F));
    assert(c < 0xD800 || c > 0xDFFF);
    return mk_wcwidth(c);
  }
  else if (b <= 0xEF && n >= 3) { // b >= 0xE0  // 3 bytes 
    c = (((b & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F));
    return mk_wcwidth(c);    
  }
  else if (b <= 0xF4 && n >= 4) { // b >= 0xF0  // 4 bytes 
    c = (((b & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F));
    return mk_wcwidth(c);
  }
  else {
    // failed
    return 1;
  }
}


// The column width of a codepoint (0, 1, or 2)
static ssize_t char_column_width( const char* s, ssize_t n ) {
  if (s == NULL || n <= 0) return 0;
  else if ((uint8_t)(*s) < ' ') return 0;   // also for CSI escape sequences
  else {
    ssize_t w = utf8_char_width(s, n);
    #ifdef _WIN32
    return (w <= 0 ? 1 : w); // windows console seems to use at least one column
    #else
    return w;
    #endif
  }
}

static ssize_t str_column_width_n( const char* s, ssize_t len ) {
  if (s == NULL || len <= 0) return 0;
  ssize_t pos = 0;
  ssize_t cwidth = 0;
  ssize_t cw;
  ssize_t ofs;
  while (s[pos] != 0 && (ofs = str_next_ofs(s, len, pos, &cw)) > 0) {
    cwidth += cw;
    pos += ofs;
  }  
  return cwidth;
}

rp_private ssize_t str_column_width( const char* s ) {
  return str_column_width_n( s, rp_strlen(s) );
}

rp_private const char* str_skip_until_fit( const char* s, ssize_t max_width ) {
  if (s == NULL) return s;
  ssize_t cwidth = str_column_width(s);
  ssize_t len    = rp_strlen(s);
  ssize_t pos = 0;
  ssize_t next;
  ssize_t cw;
  while (cwidth > max_width && (next = str_next_ofs(s, len, pos, &cw)) > 0) {
    cwidth -= cw;
    pos += next;
  }
  return (s + pos);
}


//-------------------------------------------------------------
// String navigation 
//-------------------------------------------------------------

// get offset of the previous codepoint. does not skip back over CSI sequences.
rp_private ssize_t str_prev_ofs( const char* s, ssize_t pos, ssize_t* width ) {
  ssize_t ofs = 0;
  if (s != NULL && pos > 0) {
    ofs = 1;
    while (pos > ofs) {
      uint8_t u = (uint8_t)s[pos - ofs];
      if (u < 0x80 || u > 0xBF) break;  // continue while follower
      ofs++;
    }
  }
  if (width != NULL) *width = char_column_width( s+(pos-ofs), ofs );
  return ofs;
}

// skip a CSI sequence
rp_private bool skip_csi_esc( const char* s, ssize_t len, ssize_t* esclen ) {  
  if (esclen != NULL) *esclen = 0;
  if (s == NULL || len < 2 || s[0] != '\x1B') return false;
  if (s[1] == '[' || s[1] == ']') {
    // <https://en.wikipedia.org/wiki/ANSI_escape_code#CSI_(Control_Sequence_Introducer)_sequences>
    // CSI [ or OSC ]
    ssize_t n = 2;
    bool intermediate = false;
    while (len > n) {
      char c = s[n];
      if (c >= 0x30 && c <= 0x3F) {       // parameter bytes: 0–9:;<=>?
        if (intermediate) break;          // cannot follow intermediate bytes
        n++;
      }
      else if (c >= 0x20 && c <= 0x2F) {  // intermediate bytes: ' ',!"#$%&'()*+,-./
        intermediate = true;
        n++;
      }
      else if (c >= 0x40 && c <= 0x7E) {  // terminating byte: @A–Z[\]^_`a–z{|}~
        n++;
        if (esclen != NULL) *esclen = n;
        return true;
      }
      else {
        break; // illegal character for an escape sequence.
      }
    }
  }
  else {
    // assume single character escape code (like ESC 7)
    if (esclen != NULL) *esclen = 2;
    return true;
  }
  return false;
}

// Offset to the next codepoint, treats CSI escape sequences as a single code point.
rp_private ssize_t str_next_ofs( const char* s, ssize_t len, ssize_t pos, ssize_t* cwidth ) {
  ssize_t ofs = 0;
  if (s != NULL && len > pos) {
    if (skip_csi_esc(s+pos,len-pos,&ofs)) {
      // CSI escape sequence      
    }
    else {
      ofs = 1;
      // utf8 extended character?
      while(len > pos + ofs) {
        uint8_t u = (uint8_t)s[pos + ofs];
        if (u < 0x80 || u > 0xBF) break;  // break if not a follower
        ofs++;
      }      
    } 
  }
  if (cwidth != NULL) *cwidth = char_column_width( s+pos, ofs );
  return ofs;
}

static ssize_t str_limit_to_length( const char* s, ssize_t n ) {
  ssize_t i;
  for(i = 0; i < n && s[i] != 0; i++) { /* nothing */ }
  return i;
}


//-------------------------------------------------------------
// String searching prev/next word, line, ws_word
//-------------------------------------------------------------

typedef bool (match_fun_t)(const char* s, ssize_t len);

static ssize_t str_find_backward( const char* s, ssize_t len, ssize_t pos, match_fun_t* match, bool skip_immediate_matches ) {
  if (pos > len) pos = len;
  if (pos < 0) pos = 0;
  ssize_t i = pos;
  // skip matching first (say, whitespace in case of the previous start-of-word)
  if (skip_immediate_matches) {
    do {
      ssize_t prev = str_prev_ofs(s, i, NULL); 
      if (prev <= 0) break;
      assert(i - prev >= 0);
      if (!match(s + i - prev, prev)) break;
      i -= prev;
    } while (i > 0);  
  }
  // find match
  do {
    ssize_t prev = str_prev_ofs(s, i, NULL); 
    if (prev <= 0) break;
    assert(i - prev >= 0);
    if (match(s + i - prev, prev)) {
      return i;  // found;
    }
    i -= prev;
  } while (i > 0);
  return -1; // not found
}

static ssize_t str_find_forward( const char* s, ssize_t len, ssize_t pos, match_fun_t* match, bool skip_immediate_matches ) {
  if (s == NULL || len < 0) return -1;
  if (pos > len) pos = len;
  if (pos < 0) pos = 0;  
  ssize_t i = pos;
  ssize_t next;
  // skip matching first (say, whitespace in case of the next end-of-word)
  if (skip_immediate_matches) {
    do {
      next = str_next_ofs(s, len, i, NULL); 
      if (next <= 0) break;
      assert( i + next <= len);
      if (!match(s + i, next)) break;
      i += next;
    } while (i < len);  
  }
  // and then look
  do {
    next = str_next_ofs(s, len, i, NULL); 
    if (next <= 0) break;
    assert( i + next <= len);
    if (match(s + i, next)) {
      return i; // found
    }
    i += next;
  } while (i < len);
  return -1;
} 

static bool match_linefeed( const char* s, ssize_t n ) {  
  return (n == 1 && (*s == '\n' || *s == 0));
}

static ssize_t str_find_line_start( const char* s, ssize_t len, ssize_t pos) {
  ssize_t start = str_find_backward(s,len,pos,&match_linefeed,false /* don't skip immediate matches */);
  return (start < 0 ? 0 : start); 
}

static ssize_t str_find_line_end( const char* s, ssize_t len, ssize_t pos) {
  ssize_t end = str_find_forward(s,len,pos, &match_linefeed, false);
  return (end < 0 ? len : end);
}

static bool match_nonletter( const char* s, ssize_t n ) {  
  char c = s[0];
  return !(n > 1 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c > '~');
}

static ssize_t str_find_word_start( const char* s, ssize_t len, ssize_t pos) {
  ssize_t start = str_find_backward(s,len,pos,&match_nonletter,true /* skip immediate matches */);
  return (start < 0 ? 0 : start); 
}

static ssize_t str_find_word_end( const char* s, ssize_t len, ssize_t pos) {
  ssize_t end = str_find_forward(s,len,pos,&match_nonletter,true /* skip immediate matches */);
  return (end < 0 ? len : end); 
}

static bool match_whitespace( const char* s, ssize_t n ) {  
  char c = s[0];
  return (n == 1 && (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
}

static ssize_t str_find_ws_word_start( const char* s, ssize_t len, ssize_t pos) {
  ssize_t start = str_find_backward(s,len,pos,&match_whitespace,true /* skip immediate matches */);
  return (start < 0 ? 0 : start); 
}

static ssize_t str_find_ws_word_end( const char* s, ssize_t len, ssize_t pos) {
  ssize_t end = str_find_forward(s,len,pos,&match_whitespace,true /* skip immediate matches */);
  return (end < 0 ? len : end); 
}


//-------------------------------------------------------------
// String row/column iteration
//-------------------------------------------------------------

// invoke a function for each terminal row; returns total row count.
static ssize_t str_for_each_row( const char* s, ssize_t len, ssize_t termw, ssize_t promptw, ssize_t cpromptw,
                                 row_fun_t* fun, const void* arg, void* res ) 
{
  if (s == NULL) s = "";
  ssize_t i;
  ssize_t rcount = 0;
  ssize_t rcol = 0;
  ssize_t rstart = 0;  
  for(i = 0; i < len; ) {
    ssize_t w;
    ssize_t next = str_next_ofs(s, len, i, &w);    
    if (next <= 0) {
      debug_msg("str: foreach row: next<=0: len %zd, i %zd, w %zd, buf %s\n", len, i, w, s );
      assert(false);
      break;
    }
    ssize_t termcol = rcol + w + (rcount == 0 ? promptw : cpromptw) + 1 /* for the cursor */;
    if (termw != 0 && i != 0 && termcol > termw) {  
      // wrap
      if (fun != NULL) {
        if (fun(s,rcount,rstart,i - rstart,true,arg,res)) return rcount;
      }
      rcount++;
      rstart = i;
      rcol   = 0;
    }
    if (s[i] == '\n') {
      // newline
      if (fun != NULL) {
        if (fun(s,rcount,rstart,i - rstart,false,arg,res)) return rcount;
      }
      rcount++;
      rstart = i+1;
      rcol = 0;
    }
    assert (s[i] != 0);
    i += next;
    rcol += w;
  }
  if (fun != NULL) {
    if (fun(s,rcount,rstart,i - rstart,false,arg,res)) return rcount;
  }
  return rcount+1;
}

//-------------------------------------------------------------
// String: get row/column position
//-------------------------------------------------------------


static bool str_get_current_pos_iter(
    const char* s,
    ssize_t row, ssize_t row_start, ssize_t row_len, 
    bool is_wrap, const void* arg, void* res)
{
  rp_unused(is_wrap);
  rowcol_t* rc = (rowcol_t*)res;
  ssize_t pos = *((ssize_t*)arg);

  if (pos >= row_start && pos <= (row_start + row_len)) {
    // found the cursor row
    rc->row_start = row_start;
    rc->row_len   = row_len;
    rc->row = row;
    rc->col = str_column_width_n( s + row_start, pos - row_start );
    rc->first_on_row = (pos == row_start);
    //ssize_t adjust = (is_wrap  /* wrap has no newline at end */ || 
    //                 (row_len > 0 && s[row_start + row_len - 1] == 0) /* end of user input */ ? 1 : 0);
    rc->last_on_row  = (pos == row_start + row_len);
    // debug_msg("edit: pos iter%s%s, row %zd, pos: %zd, row_start: %zd, rowlen: %zd\n", in_extra ? " inextra" : "", is_wrap ? " wrap" : "", row, eb->pos, row_start, row_len);
  }  
  return false; // always continue to count all rows
}

static ssize_t str_get_rc_at_pos(const char* s, ssize_t len, ssize_t termw, ssize_t promptw, ssize_t cpromptw, ssize_t pos, rowcol_t* rc) {
  ssize_t rows = str_for_each_row(s, len, termw, promptw, cpromptw, &str_get_current_pos_iter, &pos, rc);
  // debug_msg("edit: current pos: (%d, %d) %s %s\n", rc->row, rc->col, rc->first_on_row ? "first" : "", rc->last_on_row ? "last" : "");
  return rows;
}

//-------------------------------------------------------------
// Set position
//-------------------------------------------------------------

static bool str_set_pos_iter(
    const char* s,
    ssize_t row, ssize_t row_start, ssize_t row_len, 
    bool is_wrap, const void* arg, void* res)
{
  rp_unused(arg); rp_unused(is_wrap);
  rowcol_t* rc = (rowcol_t*)arg;
  if (rc->row != row) return false; // keep searching
  // we found our row
  ssize_t col = 0; 
  ssize_t i   = row_start;
  ssize_t end = row_start + row_len;
  while (col < rc->col && i < end) {
    ssize_t cw;
    ssize_t next = str_next_ofs(s, row_start + row_len, i, &cw);
    if (next <= 0) break;
    i   += next;
    col += cw;
  }
  *((ssize_t*)res) = i;
  return true; // stop iteration
}

static ssize_t str_get_pos_at_rc(const char* s, ssize_t len, ssize_t termw, ssize_t promptw, ssize_t cpromptw, ssize_t row, ssize_t col /* without prompt */) {
  rowcol_t rc;
  memset(&rc,0,ssizeof(rc));
  rc.row = row;
  rc.col = col;
  ssize_t pos = -1;
  str_for_each_row(s,len,termw,promptw,cpromptw,&str_set_pos_iter,&rc,&pos);  
  return pos;
}


//-------------------------------------------------------------
// String buffer
//-------------------------------------------------------------

static void sbuf_init( stringbuf_t* sbuf, alloc_t* mem ) {
  sbuf->mem = mem;
  sbuf->buf = NULL;
  sbuf->buflen = 0;
  sbuf->count = 0;
}

static void sbuf_done( stringbuf_t* sbuf ) {
  mem_free( sbuf->mem, sbuf->buf );
  sbuf->buf = NULL;
  sbuf->buflen = 0;
  sbuf->count = 0;
}

rp_private stringbuf_t*  sbuf_new( alloc_t* mem ) {
  stringbuf_t* sbuf = mem_zalloc_tp(mem,stringbuf_t);
  if (sbuf == NULL) return NULL;
  sbuf_init(sbuf,mem);
  return sbuf;
}

rp_private void sbuf_free( stringbuf_t* sbuf ) {
  if (sbuf==NULL) return;
  sbuf_done(sbuf);
  mem_free(sbuf->mem, sbuf);
}

rp_private char* sbuf_free_dup(stringbuf_t* sbuf) {
  if (sbuf == NULL || sbuf->buf == NULL) return NULL;
  char* s = mem_realloc_tp(sbuf->mem, char, sbuf->buf, sbuf_len(sbuf)+1);
  mem_free(sbuf->mem, sbuf);
  return s;
}

rp_private const char* sbuf_string_at( stringbuf_t* sbuf, ssize_t pos ) {
  if (pos < 0 || sbuf->count < pos) return NULL;
  if (sbuf->buf == NULL) return "";
  assert(sbuf->buf[sbuf->count] == 0);
  return sbuf->buf + pos;
}

rp_private const char* sbuf_string( stringbuf_t* sbuf ) {
  return sbuf_string_at( sbuf, 0 );
}

rp_private char sbuf_char_at(stringbuf_t* sbuf, ssize_t pos) {
  if (sbuf->buf == NULL || pos < 0 || sbuf->count < pos) return 0;
  return sbuf->buf[pos];
}

rp_private char* sbuf_strdup_at( stringbuf_t* sbuf, ssize_t pos ) {
  return mem_strdup(sbuf->mem, sbuf_string_at(sbuf,pos));
}

rp_private char* sbuf_strdup( stringbuf_t* sbuf ) {
  return mem_strdup(sbuf->mem, sbuf_string(sbuf));
}

static bool sbuf_ensure_extra(stringbuf_t* s, ssize_t extra) 
{
  if (s->buflen >= s->count + extra) return true;   
  // reallocate
  ssize_t newlen = (s->buflen == 0 ? 124 : 2*s->buflen);
  if (newlen <= s->count + extra) newlen = s->count + extra;
  debug_msg("stringbuf: reallocate: old %zd, new %zd\n", s->buflen, newlen);
  char* newbuf = mem_realloc_tp(s->mem, char, s->buf, newlen+1);
  if (newbuf == NULL) {
    assert(false);
    return false;
  }
  s->buf = newbuf;
  s->buflen = newlen;
  s->buf[s->count] = s->buf[s->buflen] = 0;
  assert(s->buflen >= s->count + extra);
  return true;
}

rp_private ssize_t sbuf_len(const stringbuf_t* s) {
  return s->count;
}

rp_private ssize_t sbuf_append_vprintf(stringbuf_t* sb, ssize_t max_needed, const char* fmt, va_list args) {
  ssize_t extra = max_needed;
  if (!sbuf_ensure_extra(sb, extra)) return sb->count;
  ssize_t avail = sb->buflen - sb->count;
  ssize_t needed = vsnprintf(sb->buf + sb->count, to_size_t(avail), fmt, args);
  sb->count += (needed > avail ? avail : (needed >= 0 ? needed : 0));
  assert(sb->count <= sb->buflen);
  sb->buf[sb->count] = 0;
  return sb->count;
}

rp_private ssize_t sbuf_appendf(stringbuf_t* sb, ssize_t max_needed, const char* fmt, ...) {
  va_list args;
  va_start( args, fmt);
  ssize_t res = sbuf_append_vprintf( sb, max_needed, fmt, args );
  va_end(args);
  return res;
}


rp_private ssize_t sbuf_insert_at_n(stringbuf_t* sbuf, const char* s, ssize_t n, ssize_t pos ) {
  if (pos < 0 || pos > sbuf->count || s == NULL) return pos;
  n = str_limit_to_length(s,n);
  if (n <= 0 || !sbuf_ensure_extra(sbuf,n)) return pos;
  rp_memmove(sbuf->buf + pos + n, sbuf->buf + pos, sbuf->count - pos);
  rp_memcpy(sbuf->buf + pos, s, n);
  sbuf->count += n;
  sbuf->buf[sbuf->count] = 0;
  return (pos + n);
}

rp_private ssize_t sbuf_insert_at(stringbuf_t* sbuf, const char* s, ssize_t pos ) {
  return sbuf_insert_at_n( sbuf, s, rp_strlen(s), pos );
}

rp_private ssize_t sbuf_insert_char_at(stringbuf_t* sbuf, char c, ssize_t pos ) {
  char s[2];
  s[0] = c;
  s[1] = 0;
  return sbuf_insert_at_n( sbuf, s, 1, pos);
}

rp_private ssize_t sbuf_insert_unicode_at(stringbuf_t* sbuf, unicode_t u, ssize_t pos) {
  uint8_t s[5];
  unicode_to_qutf8(u, s);
  return sbuf_insert_at(sbuf, (const char*)s, pos);
}



rp_private void sbuf_delete_at( stringbuf_t* sbuf, ssize_t pos, ssize_t count ) {
  if (pos < 0 || pos >= sbuf->count) return;
  if (pos + count > sbuf->count) count = sbuf->count - pos;
  rp_memmove(sbuf->buf + pos, sbuf->buf + pos + count, sbuf->count - pos - count);
  sbuf->count -= count;
  sbuf->buf[sbuf->count] = 0;
}

rp_private void sbuf_delete_from_to( stringbuf_t* sbuf, ssize_t pos, ssize_t end ) {
  if (end <= pos) return;
  sbuf_delete_at( sbuf, pos, end - pos);
}

rp_private void  sbuf_delete_from(stringbuf_t* sbuf, ssize_t pos ) {
  sbuf_delete_at(sbuf, pos, sbuf_len(sbuf) - pos );
}


rp_private void sbuf_clear( stringbuf_t* sbuf ) {
  sbuf_delete_at(sbuf, 0, sbuf_len(sbuf));
}

rp_private ssize_t sbuf_append_n( stringbuf_t* sbuf, const char* s, ssize_t n ) {
  return sbuf_insert_at_n( sbuf, s, n, sbuf_len(sbuf));
}

rp_private ssize_t sbuf_append( stringbuf_t* sbuf, const char* s ) {
  return sbuf_insert_at( sbuf, s, sbuf_len(sbuf));
}

rp_private ssize_t sbuf_append_char( stringbuf_t* sbuf, char c ) {
  char buf[2];
  buf[0] = c;
  buf[1] = 0;
  return sbuf_append( sbuf, buf );
}

rp_private void sbuf_replace(stringbuf_t* sbuf, const char* s) {
  sbuf_clear(sbuf);
  sbuf_append(sbuf,s);
}

static ssize_t sbuf_next_ofs( stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth ) {
  return str_next_ofs( sbuf->buf, sbuf->count, pos, cwidth);
}

static ssize_t sbuf_prev_ofs( stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth ) {
  return str_prev_ofs( sbuf->buf, pos, cwidth);
}

rp_private ssize_t sbuf_next( stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth) {
  ssize_t ofs = sbuf_next_ofs(sbuf,pos,cwidth);
  if (ofs <= 0) return -1;
  assert(pos + ofs <= sbuf->count);
  return pos + ofs; 
}

rp_private ssize_t sbuf_prev( stringbuf_t* sbuf, ssize_t pos, ssize_t* cwidth) {
  ssize_t ofs = sbuf_prev_ofs(sbuf,pos,cwidth);
  if (ofs <= 0) return -1;
  assert(pos - ofs >= 0);
  return pos - ofs;
}

rp_private ssize_t sbuf_delete_char_before( stringbuf_t* sbuf, ssize_t pos ) {
  ssize_t n = sbuf_prev_ofs(sbuf, pos, NULL);
  if (n <= 0) return 0;  
  assert( pos - n >= 0 );
  sbuf_delete_at(sbuf, pos - n, n);
  return pos - n;
}

rp_private void sbuf_delete_char_at( stringbuf_t* sbuf, ssize_t pos ) {
  ssize_t n = sbuf_next_ofs(sbuf, pos, NULL);
  if (n <= 0) return;  
  assert( pos + n <= sbuf->count );
  sbuf_delete_at(sbuf, pos, n);
  return;
}

rp_private ssize_t sbuf_swap_char( stringbuf_t* sbuf, ssize_t pos ) {
  ssize_t next = sbuf_next_ofs(sbuf, pos, NULL);
  if (next <= 0) return 0;  
  ssize_t prev = sbuf_prev_ofs(sbuf, pos, NULL);
  if (prev <= 0) return 0;  
  char buf[64];
  if (prev >= 63) return 0;
  rp_memcpy(buf, sbuf->buf + pos - prev, prev );
  rp_memmove(sbuf->buf + pos - prev, sbuf->buf + pos, next);
  rp_memmove(sbuf->buf + pos - prev + next, buf, prev);
  return pos - prev;
}

rp_private ssize_t sbuf_find_line_start( stringbuf_t* sbuf, ssize_t pos ) {
  return str_find_line_start( sbuf->buf, sbuf->count, pos);
}

rp_private ssize_t sbuf_find_line_end( stringbuf_t* sbuf, ssize_t pos ) {
  return str_find_line_end( sbuf->buf, sbuf->count, pos);
}

rp_private ssize_t sbuf_find_word_start( stringbuf_t* sbuf, ssize_t pos ) {
  return str_find_word_start( sbuf->buf, sbuf->count, pos);
}

rp_private ssize_t sbuf_find_word_end( stringbuf_t* sbuf, ssize_t pos ) {
  return str_find_word_end( sbuf->buf, sbuf->count, pos);
}

rp_private ssize_t sbuf_find_ws_word_start( stringbuf_t* sbuf, ssize_t pos ) {
  return str_find_ws_word_start( sbuf->buf, sbuf->count, pos);
}

rp_private ssize_t sbuf_find_ws_word_end( stringbuf_t* sbuf, ssize_t pos ) {
  return str_find_ws_word_end( sbuf->buf, sbuf->count, pos);
}

// find row/col position
rp_private ssize_t sbuf_get_pos_at_rc( stringbuf_t* sbuf, ssize_t termw, ssize_t promptw, ssize_t cpromptw, ssize_t row, ssize_t col ) {
  return str_get_pos_at_rc( sbuf->buf, sbuf->count, termw, promptw, cpromptw, row, col);
}

// get row/col for a given position
rp_private ssize_t sbuf_get_rc_at_pos( stringbuf_t* sbuf, ssize_t termw, ssize_t promptw, ssize_t cpromptw, ssize_t pos, rowcol_t* rc ) {
  return str_get_rc_at_pos( sbuf->buf, sbuf->count, termw, promptw, cpromptw, pos, rc);
}

rp_private ssize_t sbuf_for_each_row( stringbuf_t* sbuf, ssize_t termw, ssize_t promptw, ssize_t cpromptw, row_fun_t* fun, void* arg, void* res ) {
  return str_for_each_row( sbuf->buf, sbuf->count, termw, promptw, cpromptw, fun, arg, res);
}


// Duplicate and decode from utf-8 (for non-utf8 terminals)
rp_private char* sbuf_strdup_from_utf8(stringbuf_t* sbuf) {
  ssize_t len = sbuf_len(sbuf);
  if (sbuf == NULL || len <= 0) return NULL;
  char* s = mem_zalloc_tp_n(sbuf->mem, char, len);
  if (s == NULL) return NULL;
  ssize_t dest = 0;
  for (ssize_t i = 0; i < len; ) {
    ssize_t ofs = sbuf_next_ofs(sbuf, i, NULL);
    if (ofs <= 0) {
      // invalid input
      break;
    }
    else if (ofs == 1) {
      // regular character
      s[dest++] = sbuf->buf[i];
    }
    else if (sbuf->buf[i] == '\x1B') {
      // skip escape sequences
    }
    else {
      // decode unicode
      ssize_t nread;
      unicode_t uchr = unicode_from_qutf8(sbuf->buf + i, ofs, &nread);
      uint8_t c;
      if (unicode_is_raw(uchr, &c)) {
        // raw byte, output as is (this will take care of locale specific input)
        s[dest++] = (char)c;
      }
      else if (uchr <= 0x7F) {
        // allow ascii
        s[dest++] = (char)uchr;
      }
      else {
        // skip unknown unicode characters..
        // todo: convert according to locale?
      }
    }
    i += ofs;
  }
  assert(dest <= len);
  s[dest] = 0;
  return s;
}

//-------------------------------------------------------------
// String helpers
//-------------------------------------------------------------

rp_public long rp_prev_char( const char* s, long pos ) {
  ssize_t len = rp_strlen(s);
  if (pos < 0 || pos > len) return -1;
  ssize_t ofs = str_prev_ofs( s, pos, NULL );
  if (ofs <= 0) return -1;
  return (long)(pos - ofs);
}

rp_public long rp_next_char( const char* s, long pos ) {
  ssize_t len = rp_strlen(s);
  if (pos < 0 || pos > len) return -1;
  ssize_t ofs = str_next_ofs( s, len, pos, NULL );
  if (ofs <= 0) return -1;
  return (long)(pos + ofs);
}

rp_public bool rp_starts_with( const char* s, const char* prefix ) {
  if (s==prefix) return true;
  if (prefix==NULL) return true;
  if (s==NULL) return false;

  ssize_t i;
  for( i = 0; s[i] != 0 && prefix[i] != 0; i++) {
    if (s[i] != prefix[i]) return false;
  }
  return (prefix[i] == 0);
}

rp_private char rp_tolower( char c ) {
  return (c >= 'A' && c <= 'Z'  ?  c - 'A' + 'a' : c);
}

rp_public bool rp_istarts_with( const char* s, const char* prefix ) {
  if (s==prefix) return true;
  if (prefix==NULL) return true;
  if (s==NULL) return false;

  ssize_t i;
  for( i = 0; s[i] != 0 && prefix[i] != 0; i++) {
    if (rp_tolower(s[i]) != rp_tolower(prefix[i])) return false;
  }
  return (prefix[i] == 0);
}

static int rp_strnicmp(const char* s1, const char* s2, ssize_t n) {
  if (s1 == NULL && s2 == NULL) return 0;
  if (s1 == NULL) return -1;
  if (s2 == NULL) return 1;
  ssize_t i;
  for (i = 0; s1[i] != 0 && i < n; i++) {  // note: if s2[i] == 0 the loop will stop as c1 != c2
    char c1 = rp_tolower(s1[i]);
    char c2 = rp_tolower(s2[i]);
    if (c1 < c2) return -1;
    if (c1 > c2) return 1;
  }
  return ((i >= n || s2[i] == 0) ? 0 : -1);
}

rp_private int rp_stricmp(const char* s1, const char* s2) {
  ssize_t len1 = rp_strlen(s1);
  ssize_t len2 = rp_strlen(s2);
  return rp_strnicmp(s1, s2, (len1 >= len2 ? len1 : len2));
}

rp_private const char* rp_stristr(const char* s, const char* pat) {
  if (s==NULL) return NULL;
  if (pat==NULL || pat[0] == 0) return s;
  ssize_t patlen = rp_strlen(pat);
  for (ssize_t i = 0; s[i] != 0; i++) {
    if (rp_strnicmp(s + i, pat, patlen) == 0) return (s+i);
  }
  return NULL;
}


// parse a decimal (leave pi unchanged on error)
rp_private bool rp_atoz(const char* s, ssize_t* pi) {
  return (sscanf(s, "%zd", pi) == 1);
}

// parse two decimals separated by a semicolon 
rp_private bool rp_atoz2(const char* s, ssize_t* pi, ssize_t* pj) {
  return (sscanf(s, "%zd;%zd", pi, pj) == 2);
}

// parse unsigned 32-bit (leave pu unchanged on error)
rp_private bool rp_atou32(const char* s, uint32_t* pu) {
  return (sscanf(s, "%" SCNu32, pu) == 1);
}

