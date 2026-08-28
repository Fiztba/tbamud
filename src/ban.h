/**
* @file boards.h
* Header file for the bulletin board system (boards.c).
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* All rights reserved.  See license for complete information.                                                                
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University 
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.
* 
* @todo Utility functions that could easily be moved elsewhere have been
* marked. Suggest a review of all utility functions (aka. non ACMDs) and
* determine if the utility functions should be placed into a lower level
* shared module.               
*
*/
#ifndef _BAN_H_
#define _BAN_H_

/* don't change these */
#define BAN_NOT   0
#define BAN_NEW   1
#define BAN_SELECT  2
#define BAN_ALL   3

#define BANNED_SITE_LENGTH    50

/** Longest ban-type keyword load_banned() will accept.  Narrowing this one is
 * safe in a way that narrowing a name field is not: the values come from the
 * closed ban_types[] table in ban.c, the longest of which is "select", so no
 * file this MUD writes can approach it.  A token that does exceed it is not a
 * ban type at all, and is refused by name below rather than truncated into
 * something that matches nothing. */
#define BAN_TYPE_FIELD        15

/* The three string fields of a ban record, each bounded by the array that
 * receives it -- see SCANF_WIDTH in utils.h.  load_banned() reads straight out
 * of the FILE before this; with no line buffer in the way there was nothing at
 * all bounding a site or a name. */
#define BAN_TYPE_FMT          "%" SCANF_WIDTH(BAN_TYPE_FIELD) "s"
#define BAN_SITE_FMT          "%" SCANF_WIDTH(BANNED_SITE_LENGTH) "s"
#define BAN_NAME_FMT          "%" SCANF_WIDTH(MAX_NAME_LENGTH) "s"

struct ban_list_element {
   char site[BANNED_SITE_LENGTH+1];
   int  type;
   time_t date;
   char name[MAX_NAME_LENGTH+1];
   struct ban_list_element *next;
};

/* Global functions */
/* Utility Functions */
void load_banned(void);
int isbanned(char *hostname);
int valid_name(char *newname);
void read_invalid_list(void);
void free_invalid_list(void);
/* Command functions without subcommands */
ACMD(do_ban);
ACMD(do_unban);

extern struct ban_list_element *ban_list;
extern int num_invalid;

#endif /* _BAN_H_*/
