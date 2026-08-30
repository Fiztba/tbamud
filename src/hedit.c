/**************************************************************************
*  File: hedit.c                                           Part of tbaMUD *
*  Usage: Oasis OLC Help Editor.                                          *
* Author: Steve Wolfe, Scott Meisenholder, Rhade                          *
*                                                                         *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
**************************************************************************/

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "db.h"
#include "boards.h"
#include "oasis.h"
#include "genolc.h"
#include "genzon.h"
#include "handler.h"
#include "improved-edit.h"
#include "act.h"
#include "hedit.h"
#include "modify.h"

/* local functions */
static void hedit_disp_menu(struct descriptor_data *);
static void hedit_setup_new(struct descriptor_data *);
static void hedit_setup_existing(struct descriptor_data *, int);
static void hedit_save_to_disk(struct descriptor_data *);
static int hedit_save_internally(struct descriptor_data *);


ACMD(do_oasis_hedit)
{
  char arg[MAX_INPUT_LENGTH];
  struct descriptor_data *d;
  int i;

  /* No building as a mob or while being forced. */
  if (IS_NPC(ch) || !ch->desc || STATE(ch->desc) != CON_PLAYING)
    return;

  if (!can_edit_zone(ch, HEDIT_PERMISSION)) {
    send_to_char(ch, "You don't have access to editing help files.\r\n");
    return;
  }

  for (d = descriptor_list; d; d = d->next) {
    if (STATE(d) == CON_HEDIT) {
      send_to_char(ch, "Sorry, only one can person can edit help files at a time.\r\n");
      return;
    }
  }

  one_argument(argument, arg);

  if (!*arg) {
    send_to_char(ch, "Please specify a help entry to edit.\r\n");
    return;
  }

  d = ch->desc;

  if (!str_cmp("save", arg)) {
    mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(ch)), TRUE, "OLC: %s saves help files.",
           GET_NAME(ch));
    /* The delete path pairs its add_to_save_list with the removal inside
     * hedit_save_to_disk. This one never adds, so the removal finds nothing
     * and logs "remove_from_save_list: Saved item not found." on every
     * `hedit save`. Pairing it here silences that without changing what
     * reaches disk -- it is exactly the noise this commit complains about
     * elsewhere, and fixing it only for the delete was inconsistent. */
    add_to_save_list(HEDIT_PERMISSION, SL_HLP);
    hedit_save_to_disk(d);
    send_to_char(ch, "Saving help files.\r\n");
    return;
  }

  /* Give descriptor an OLC structure. */
  if (d->olc) {
    mudlog(BRF, LVL_IMMORT, TRUE, "SYSERR: do_oasis: Player already had olc structure.");
    free(d->olc);
  }

  CREATE(d->olc, struct oasis_olc_data, 1);
  OLC_NUM(d) = 0;
  OLC_STORAGE(d) = strdup(arg);
  
  OLC_ZNUM(d) = search_help(OLC_STORAGE(d), LVL_IMPL);
  /* Remember which table this index belongs to; the delete refuses if it
   * is rebuilt underneath the editor. */
  OLC_HELP_VERSION(d) = help_table_version;

  /* NOWHERE first. It is 65535 -- an unsigned type -- so reading the row
   * before testing for it indexes about 1.5MB past the array whenever the
   * keyword does not exist at all. A plain build reads garbage and carries
   * on; under a sanitiser it is a SEGV that takes the MUD down. */
  if (OLC_ZNUM(d) != NOWHERE && help_table[OLC_ZNUM(d)].duplicate) {
    for (i = 0; i < top_of_helpt; i++)
      if (help_table[i].duplicate == 0 && help_table[i].entry == help_table[OLC_ZNUM(d)].entry) {
        OLC_ZNUM(d) = i;
        break;
      }
  }

  if (OLC_ZNUM(d) == NOWHERE) {
    send_to_char(ch, "Do you wish to add the '%s' help file? ", OLC_STORAGE(d));
    OLC_MODE(d) = HEDIT_CONFIRM_ADD;
  } else {
    send_to_char(ch, "Do you wish to edit the '%s' help file?", help_table[OLC_ZNUM(d)].keywords);
    OLC_MODE(d) = HEDIT_CONFIRM_EDIT;
  }

  STATE(d) = CON_HEDIT;
  act("$n starts using OLC.", TRUE, d->character, 0, 0, TO_ROOM);
  SET_BIT_AR(PLR_FLAGS(ch), PLR_WRITING);
  mudlog(CMP, MAX(LVL_IMMORT, GET_INVIS_LEV(d->character)), 
    TRUE, "OLC: %s starts editing help files.", GET_NAME(d->character));
}

static void hedit_setup_new(struct descriptor_data *d)
{
  CREATE(OLC_HELP(d), struct help_index_element, 1);

  OLC_HELP(d)->keywords		= strdup(OLC_STORAGE(d));
  OLC_HELP(d)->entry		= strdup("KEYWORDS\r\n\r\nThis help file is unfinished.\r\n");
  OLC_HELP(d)->min_level	= 0;
  OLC_HELP(d)->duplicate	= 0;
  OLC_VAL(d) = 0;

  hedit_disp_menu(d);
}

static void hedit_setup_existing(struct descriptor_data *d, int rnum)
{
  CREATE(OLC_HELP(d), struct help_index_element, 1);

  OLC_HELP(d)->keywords		= str_udup(help_table[rnum].keywords);
  OLC_HELP(d)->entry		= str_udup(help_table[rnum].entry);
  OLC_HELP(d)->duplicate	= help_table[rnum].duplicate;
  OLC_HELP(d)->min_level	= help_table[rnum].min_level;
  OLC_VAL(d) = 0;

  /* How to find this row again if the table is rebuilt underneath the
   * editor. Both are taken now, before the builder can edit either field.
   *
   * The keyword alone is not an identity: a row's keyword is one word, and
   * the first word of a multi-keyword entry can be another entry's only
   * keyword. The shipped help file has eleven such collisions -- `spells`
   * names both its own entry and the first keyword of the magic entry --
   * and matching on the word alone lands on whichever comes first.
   *
   * The text is the entry's identity: load_help strdups it once per entry,
   * so rows of one entry share a pointer and rows of different entries do
   * not. Together they separate the two. */
  OLC_HELP_KEY(d) = str_udup(help_table[rnum].keywords);
  OLC_HELP_TEXT(d) = str_udup(help_table[rnum].entry);

  hedit_disp_menu(d);
}

/* A write has to land on the entry's primary row: hedit_save_to_disk skips
 * duplicates, so one that lands on a duplicate never reaches the file. */
static int hedit_primary_of(int i)
{
  int j;

  if (!help_table[i].duplicate)
    return i;
  for (j = 0; j < top_of_helpt; j++)
    if (!help_table[j].duplicate && help_table[j].entry == help_table[i].entry)
      return j;
  /* A duplicate with no primary. The table is already wrong; writing here
   * at least does not reach into another entry. */
  return i;
}

/* Find the row this editor opened, in a table that has been rebuilt since it
 * opened.
 *
 * Not by what the builder typed: OLC_STORAGE is a word, and answering 'n' at
 * the confirm prompt walks forward to the next row that word abbreviates, so
 * after a walk it names a different entry than the one being edited.
 *
 * And not by the row's keyword alone, which was the first thing tried and is
 * worse than the bug it fixes. A row's keyword is one word, and the first
 * word of a multi-keyword entry can be another entry's only keyword -- the
 * shipped file has eleven such collisions. Relocating on the word alone took
 * whichever came first, so editing the magic entry through `magics` had the
 * save destroy the separate `spells` entry instead.
 *
 * So: the pair, then the keyword alone where it names exactly one row, and
 * otherwise no answer at all rather than a guess. HEDIT_RELOC_AMBIGUOUS is a
 * refusal for the caller to report; HEDIT_RELOC_NOTFOUND means the entry is
 * gone and the save becomes an add. */
#define HEDIT_RELOC_NOTFOUND	(-1)
#define HEDIT_RELOC_AMBIGUOUS	(-2)

static int hedit_relocate(struct descriptor_data *d)
{
  const char *key = OLC_HELP_KEY(d), *text = OLC_HELP_TEXT(d);
  int i, n, found, matched = FALSE;

  /* 1. Both. The reload that changed nothing, and every reload that changed
   *    something else, land here. */
  if (key && text)
    for (i = 0; i < top_of_helpt; i++)
      if (help_table[i].keywords && !strcmp(help_table[i].keywords, key) &&
          help_table[i].entry && !strcmp(help_table[i].entry, text))
        return hedit_primary_of(i);

  /* 2. The keyword, if it names exactly one row: somebody edited the text.
   *
   *    There is deliberately no rule between these two matching on the
   *    text alone. load_help puts the keyword line INTO the entry text, so
   *    text that still matches exactly is text whose keyword line is
   *    unchanged -- which means the captured keyword is still one of that
   *    entry's rows, and step 1 has already answered. A rename changes the
   *    text along with the keywords and lands here or nowhere. */
  if (key) {
    for (i = 0, n = 0, found = -1; i < top_of_helpt; i++)
      if (help_table[i].keywords && !strcmp(help_table[i].keywords, key)) {
        n++;
        found = i;
      }
    if (n == 1)
      return hedit_primary_of(found);
    if (n > 1)
      matched = TRUE;
  }

  return matched ? HEDIT_RELOC_AMBIGUOUS : HEDIT_RELOC_NOTFOUND;
}

/* FALSE means nothing was written and nothing was discarded; the caller says
 * why and leaves the builder in the editor. */
static int hedit_save_internally(struct descriptor_data *d)
{
  struct help_index_element *new_help_table = NULL;

  /* An index into a table that has been rebuilt since the editor opened names
   * whatever now sits in that slot, so writing through it overwrites an entry
   * the builder never asked for. Only a help reload can do that while hedit is
   * open, since hedit refuses a second editor -- `reload xhelp`, `reload all`
   * and `reload *` all reach free_help_table() + index_boot(DB_BOOT_HLP).
   *
   * Take the row again rather than refusing outright: this is the last thing
   * that runs before the editor is torn down, so a flat refusal would throw
   * the builder's work away to protect somebody else's. Treating it as new is
   * not an option either -- a reload that changes nothing still bumps the
   * counter, and appending then puts a second entry in help.hlp under the same
   * keyword, which search_help resolves to the older of the two, so the builder
   * could not reach their own work even by reopening it.
   *
   * Only where the row genuinely cannot be identified is the save refused, and
   * then the version is deliberately left stale: the builder is put back in the
   * editor, and their next attempt has to come through here again rather than
   * sail past a guard that has already been satisfied. */
  if (OLC_HELP_VERSION(d) != help_table_version) {
    int row = hedit_relocate(d);

    if (row == HEDIT_RELOC_AMBIGUOUS)
      return FALSE;

    OLC_ZNUM(d) = (row == HEDIT_RELOC_NOTFOUND) ? NOWHERE : row;
    OLC_HELP_VERSION(d) = help_table_version;
  }

  /* The write always lands on the entry's primary row, stale index or not.
   * hedit_save_to_disk skips duplicates, so a builder who reached one with 'n'
   * at the confirm prompt had their min_level change written nowhere at all --
   * only the entry text survived, and only because the pass below carries it
   * to the primary by hand. */
  if (OLC_ZNUM(d) != NOWHERE && OLC_ZNUM(d) < top_of_helpt)
    OLC_ZNUM(d) = hedit_primary_of(OLC_ZNUM(d));
  OLC_HELP(d)->duplicate = 0;

  if (OLC_ZNUM(d) == NOWHERE) {
    int i;
    CREATE(new_help_table, struct help_index_element, top_of_helpt + 2);

    for (i = 0; i < top_of_helpt; i++)
      new_help_table[i] = help_table[i];
      
    new_help_table[top_of_helpt++] = *OLC_HELP(d);
    free(help_table);
    help_table = new_help_table;
    help_table_version++;
  } else {
    /* The row is overwritten wholesale, so whatever it was holding has to go
     * first -- the caller hands this function ownership of OLC_HELP's strings
     * (CLEANUP_STRUCTS, not CLEANUP_ALL, with a comment saying so) and the
     * old ones are then owned by nobody at all.
     *
     * keywords belongs to this row alone. entry is shared with the row's
     * duplicates -- that is how one entry answers to several keywords -- so
     * it can only be freed once nothing points at it, and the duplicates are
     * pointed at the replacement in the same pass. They have to be: without
     * that they would spend the rest of the function holding a pointer that
     * has just been freed, and `help <the other keyword>` would read it. */
    int i;
    char *oldkey = help_table[OLC_ZNUM(d)].keywords;
    char *oldentry = help_table[OLC_ZNUM(d)].entry;

    help_table[OLC_ZNUM(d)] = *OLC_HELP(d);

    if (oldentry)
      for (i = 0; i < top_of_helpt; i++)
        if (i != OLC_ZNUM(d) && help_table[i].entry == oldentry)
          help_table[i].entry = OLC_HELP(d)->entry;

    if (oldkey)
      free(oldkey);
    if (oldentry)
      free(oldentry);
  }

  add_to_save_list(HEDIT_PERMISSION, SL_HLP);
  hedit_save_to_disk(d);
  return TRUE;
}

static void hedit_save_to_disk(struct descriptor_data *d)
{
  FILE *fp;
  char buf1[MAX_STRING_LENGTH], index_name[READ_SIZE];
  int i;

  snprintf(index_name, sizeof(index_name), "%s%s", HLP_PREFIX, HELP_FILE);
  if (!(fp = fopen(index_name, "w"))) {
    log("SYSERR: Could not write help index file");
    return;
  }

  for (i = 0; i < top_of_helpt; i++) {
    if (help_table[i].duplicate)
      continue;
    strncpy(buf1, help_table[i].entry ? help_table[i].entry : "Empty\r\n", sizeof(buf1) - 1);
    strip_cr(buf1);

    /* Forget making a buffer, lets just write the thing now. */
    fprintf(fp, "%s#%d\n", convert_from_tabs(buf1), help_table[i].min_level);
  }
  /* Write final line and close. */
  fprintf(fp, "$~\n");
  fclose(fp);

  remove_from_save_list(HEDIT_PERMISSION, SL_HLP);

  /* Reboot the help files. */
  free_help_table();     
  index_boot(DB_BOOT_HLP);
}

/* Re-find, by keyword, the row this editor opened.
 *
 * OLC_ZNUM is a help_table index captured when the editor opened, and the
 * table is rebuilt and re-sorted by `reload xhelp` and `reload all` -- which
 * hedit's one-editor lock does not cover, because they are not hedit. Acting
 * on the stale index removes whatever now occupies that slot: a different
 * entry entirely, with nothing in the log to say which one. Every row of a
 * multi-keyword entry carries the same keyword string, so matching on it
 * finds the set again wherever the sort has moved it.
 *
 * Returns -1 if the entry is no longer there, which is the honest answer
 * when the table has been reloaded underneath the editor. */
static int hedit_find_row(struct descriptor_data *d)
{
  /* OLC_ZNUM is the row hedit_setup_existing read to fill the editor, and
   * nothing moves it afterwards -- the CONFIRM_EDIT 'n' walk happens before
   * setup. So once the table is known not to have been rebuilt, that index
   * still names the entry on the builder's screen, and there is nothing
   * left for a re-resolution to add.
   *
   * This used to re-derive the row from the keyword and compare. That is
   * worse than redundant. Both halves came from the CURRENT table, so it
   * could not tell "unchanged" from "something else slid into this slot":
   * a reload that removed the open entry let another entry's canonical row
   * land on the captured index, and the delete took seven rows the builder
   * never saw. It caught the reload that cost nothing and missed the one
   * that cost an entry -- while also refusing the legitimate case of a
   * builder who walked past a twin with 'n' to reach the one they wanted.
   *
   * The version counter asks the question that was actually being asked.
   * Everything now rests on bumping it wherever help_table is rebuilt or
   * replaced; see its declaration in db.c. */
  if (OLC_HELP_VERSION(d) != help_table_version)
    return -1;

  if (OLC_ZNUM(d) == NOWHERE || OLC_ZNUM(d) >= top_of_helpt)
    return -1;

  return OLC_ZNUM(d);
}

/* Remove a help entry, and every row that shares its text.
 *
 * An entry with N keywords is stored as N rows, all sharing one `entry`
 * pointer -- the loader strdups the text once and copies the struct per
 * keyword. help_table is then sorted by keyword (db.c, hsort), so those rows
 * are NOT adjacent and cannot be found by walking the `duplicate` counter.
 * They are found by the shared pointer instead; deleting one row alone would
 * leave the others pointing at freed text. */
static int hedit_delete_entry(int rnum)
{
  char *text;
  int i, w = 0, removed = 0, keep;

  if (rnum < 0 || rnum >= top_of_helpt)
    return FALSE;

  text = help_table[rnum].entry;

  /* Never leave the table empty. hedit_save_to_disk writes help.hlp and
   * index_boot reads it straight back; on a file with no entries at all
   * that is 'boot error - 0 records counted' and exit(1) -- which takes
   * the running server down mid-command AND fails every boot after it,
   * until somebody edits the file by hand. */
  for (i = 0, keep = 0; i < top_of_helpt; i++)
    if (!(text ? (help_table[i].entry == text) : (i == rnum)))
      keep++;
  if (keep == 0)
    return FALSE;

  for (i = 0; i < top_of_helpt; i++) {
    /* A NULL text would match every empty row, so that case takes only the
     * row it was actually asked for. */
    if (text ? (help_table[i].entry == text) : (i == rnum)) {
      if (help_table[i].keywords)
        free(help_table[i].keywords);
      removed++;
      continue;
    }
    if (w != i)
      help_table[w] = help_table[i];
    w++;
  }

  if (text)
    free(text);		/* shared by the whole set; freed once, after the sweep */

  top_of_helpt = w;
  /* The table changed shape, so anyone holding an index into it is
   * holding a stale one. Nothing outlives this command today, but the
   * counter's whole value is that it is bumped without needing to know
   * that. */
  help_table_version++;
  return removed > 0;
}

/* The main menu. */
static void hedit_disp_menu(struct descriptor_data *d)
{
  get_char_colors(d->character);

  write_to_output(d,
      "%s-- Help file editor\r\n"
      "%s1%s) Entry       :\r\n%s%s"
      "%s2%s) Min Level   : %s%d\r\n"
      "%sX%s) Delete this help entry\r\n"
      "%sQ%s) Quit\r\n"
      "Enter choice : ",
       nrm,
       grn, nrm, yel, OLC_HELP(d)->entry,
       grn, nrm, yel, OLC_HELP(d)->min_level,
       grn, nrm,
       grn, nrm
  );
  OLC_MODE(d) = HEDIT_MAIN_MENU;
}

void hedit_parse(struct descriptor_data *d, char *arg)
{
  char buf[MAX_STRING_LENGTH];
  char *oldtext = "";
  int number;

  switch (OLC_MODE(d)) {
  case HEDIT_CONFIRM_SAVESTRING:
    switch (*arg) {
    case 'y':
    case 'Y':
      /* Formatted before the save, emitted after it. Both halves matter.
       *
       * After, because the save can decline: the old order logged the edit
       * and told the builder it had reached disk before the write was even
       * attempted.
       *
       * Before, because by the time the save returns this string is gone.
       * hedit_save_internally hands OLC_HELP's strings to the table and
       * hedit_save_to_disk ends by rebooting the table from the file, and
       * free_help_table() frees every row's keywords on the way -- these
       * among them. Reading them afterwards formats freed heap, with
       * index_boot reallocating in between.
       *
       * The refusal path returns before the mudlog, so nothing is logged
       * for a save that did not happen. */
      snprintf(buf, sizeof(buf), "OLC: %s edits help for %s.", GET_NAME(d->character),
               OLC_HELP(d)->keywords);
      if (!hedit_save_internally(d)) {
        write_to_output(d, "The help files were reloaded while you were editing, and more "
                           "than one entry now answers to what you opened. Writing to the "
                           "wrong one would destroy an entry you never touched, so nothing "
                           "has been saved. Your work is still here.\r\n");
        hedit_disp_menu(d);
        return;
      }
      mudlog(TRUE, MAX(LVL_BUILDER, GET_INVIS_LEV(d->character)), CMP, "%s", buf);
      write_to_output(d, "Help saved to disk.\r\n");

      /* Do not free strings, just the help structure. */
      cleanup_olc(d, CLEANUP_STRUCTS);
      break;
    case 'n':
    case 'N':
      /* Free everything up, including strings, etc. */
      cleanup_olc(d, CLEANUP_ALL);
      break;
    default:
      write_to_output(d, "Invalid choice!\r\nDo you wish to save your changes? : \r\n");
      break;
    }
    return;

  case HEDIT_CONFIRM_EDIT:
    /* Above the switch, not inside one arm of it. All three arms read
     * help_table[OLC_ZNUM(d)] -- 'y' to fill the editor, 'n' to walk to the
     * next match, and the reprompt to name the entry -- and the index they
     * share was taken before a reload could move it. The reprompt is the one
     * a builder is most likely to reach, since a bare RETURN lands there.
     *
     * Refusing costs nothing here: nothing has been typed yet. That is why
     * this says so and stops, where the save -- which runs after the work is
     * done -- goes looking for the row instead. */
    if (OLC_HELP_VERSION(d) != help_table_version) {
      write_to_output(d, "The help files were reloaded while you were deciding, so "
                         "that is not necessarily the entry you asked for any more. "
                         "Nothing has been changed; run hedit again.\r\n");
      cleanup_olc(d, CLEANUP_ALL);
      return;
    }
    switch (*arg)  {
    case 'y': case 'Y':
      hedit_setup_existing(d, OLC_ZNUM(d));
      break;
    case 'q': case 'Q': 
      cleanup_olc(d, CLEANUP_ALL);
      break;       
    case 'n': case 'N':
      OLC_ZNUM(d)++;
      for (; OLC_ZNUM(d) < top_of_helpt; OLC_ZNUM(d)++)
        if (is_abbrev(OLC_STORAGE(d), help_table[OLC_ZNUM(d)].keywords))
          break;
        else
          OLC_ZNUM(d) = top_of_helpt + 1;

      if (OLC_ZNUM(d) > top_of_helpt) {
        write_to_output(d, "Do you wish to add the '%s' help file? ",
            OLC_STORAGE(d));
        OLC_MODE(d) = HEDIT_CONFIRM_ADD;
      } else {
        write_to_output(d, "Do you wish to edit the '%s' help file? ",
            help_table[OLC_ZNUM(d)].keywords);
        OLC_MODE(d) = HEDIT_CONFIRM_EDIT;
      }     
      break;
    default:
      write_to_output(d, "Invalid choice!\r\n"
                         "Do you wish to edit the '%s' help file? ",
                         help_table[OLC_ZNUM(d)].keywords);
      break;
    }
    return;

  case HEDIT_CONFIRM_ADD:
    switch (*arg)  {
      case 'y': case 'Y':
      hedit_setup_new(d);
      break;
    case 'n': case 'N': case 'q': case 'Q':
      cleanup_olc(d, CLEANUP_ALL);
      break;
    default:
      write_to_output(d, "Invalid choice!\r\n"
                         "Do you wish to add the '%s' help file? ",
                         OLC_STORAGE(d));
      break;
    }
    return;

  case HEDIT_CONFIRM_DELETE: {
    int row;
    switch (*arg) {
    case 'y':
    case 'Y':
      row = hedit_find_row(d);
      if (row >= 0 && hedit_delete_entry(row)) {
        /* hedit_save_to_disk ends by removing this from the save list, so
         * it has to be on it -- hedit_save_internally adds it immediately
         * before saving for exactly this reason. Without the add, every
         * deletion logs "remove_from_save_list: Saved item not found." */
        add_to_save_list(HEDIT_PERMISSION, SL_HLP);
        mudlog(CMP, MAX(LVL_BUILDER, GET_INVIS_LEV(d->character)), TRUE,
               "OLC: %s deletes help entry '%s'", GET_NAME(d->character),
               OLC_HELP(d)->keywords ? OLC_HELP(d)->keywords : "unnamed");
        write_to_output(d, "Help entry deleted.\r\n");
        /* Rewrites help.hlp from the table and reboots it, which is how
         * every other hedit change reaches disk. */
        hedit_save_to_disk(d);
        cleanup_olc(d, CLEANUP_ALL);
        return;
      }
      /* Nothing was removed, so nothing is thrown away either --
       * cleanup_olc here would discard the builder's unsaved work on top
       * of refusing the delete. */
      if (hedit_find_row(d) >= 0)
        /* Found, so the refusal came from the last-entry guard. Saying it
         * was reloaded would be false twice over, with the entry still on
         * screen underneath. */
        write_to_output(d, "That is the last help entry left. The MUD cannot boot from a help file with none, so it will not be deleted.\r\n");
      else
        write_to_output(d, "That entry is no longer in the help table. It may have been reloaded while you were editing it. Nothing was deleted.\r\n");
      hedit_disp_menu(d);
      return;
    case 'n':
    case 'N':
      hedit_disp_menu(d);
      return;
    default:
      write_to_output(d, "Invalid choice!\r\n");
      write_to_output(d, "Delete this help entry, and every keyword that reaches it? : ");
      return;
    }
  }

  case HEDIT_MAIN_MENU:
    switch (*arg) {
    case 'q':
    case 'Q':
      if (OLC_VAL(d)) {
        /* Something has been modified. */
        write_to_output(d, "Do you wish to save your changes? : ");
        OLC_MODE(d) = HEDIT_CONFIRM_SAVESTRING;
      } else {
        write_to_output(d, "No changes made.\r\n");
        cleanup_olc(d, CLEANUP_ALL);
      }
      break;
    case 'x':
    case 'X':
      if (hedit_find_row(d) < 0) {
        write_to_output(d, "That entry is not in the help table -- either it was never saved, or the table was reloaded while you were editing. Quit without saving.\r\n");
        hedit_disp_menu(d);
        return;
      }
      write_to_output(d, "Delete this help entry, and every keyword that reaches it? : ");
      OLC_MODE(d) = HEDIT_CONFIRM_DELETE;
      return;
    case '1':
      OLC_MODE(d) = HEDIT_ENTRY;
      clear_screen(d);
      send_editor_help(d);
      write_to_output(d, "Enter help entry: (/s saves /h for help)\r\n");
      if (OLC_HELP(d)->entry) {
        write_to_output(d, "%s", OLC_HELP(d)->entry);
        oldtext = strdup(OLC_HELP(d)->entry);
      }
      string_write(d, &OLC_HELP(d)->entry, MAX_MESSAGE_LENGTH, 0, oldtext);
      OLC_VAL(d) = 1;
      break;
    case '2':
      write_to_output(d, "Enter min level : ");
      OLC_MODE(d) = HEDIT_MIN_LEVEL;
      break;
    default:
      write_to_output(d, "Invalid choice!\r\n");
      hedit_disp_menu(d);
      break;
    }
    return;

  case HEDIT_KEYWORDS:
    if (OLC_HELP(d)->keywords)
      free(OLC_HELP(d)->keywords);
    if (strlen(arg) > MAX_HELP_KEYWORDS)
      arg[MAX_HELP_KEYWORDS - 1] = '\0';
    strip_cr(arg);
    OLC_HELP(d)->keywords = str_udup(arg);
    break;

  case HEDIT_ENTRY:
    /* We will NEVER get here, we hope. */
    mudlog(TRUE, LVL_BUILDER, BRF, "SYSERR: Reached HEDIT_ENTRY case in parse_hedit");
    break;

  case HEDIT_MIN_LEVEL:
    number = atoi(arg);
    if ((number < 0) || (number > LVL_IMPL))
      write_to_output(d, "That is not a valid choice!\r\nEnter min level:-\r\n] ");
    else {
      OLC_HELP(d)->min_level = number;
      break;
    }
    return;

  default:
    /* We should never get here. */
    mudlog(TRUE, LVL_BUILDER, BRF, "SYSERR: Reached default case in parse_hedit");
    break;
  }

  /* If we get this far, something has been changed. */
  OLC_VAL(d) = 1;
  hedit_disp_menu(d);
}

void hedit_string_cleanup(struct descriptor_data *d, int terminator)
{
  switch (OLC_MODE(d)) {
  case HEDIT_ENTRY:
    hedit_disp_menu(d);
    break;
  }
}

ACMD(do_helpcheck)
{

  char buf[MAX_STRING_LENGTH];
  int i, count = 0;
  size_t len = 0, nlen;

  for (i = 1; *(complete_cmd_info[i].command) != '\n'; i++) {
    if (complete_cmd_info[i].command_pointer != do_action && complete_cmd_info[i].minimum_level >= 0) {
      if (search_help(complete_cmd_info[i].command, LVL_IMPL) == NOWHERE) {
        nlen = snprintf(buf + len, sizeof(buf) - len, "%-20.20s%s", complete_cmd_info[i].command,
                        (++count % 3 ? "" : "\r\n"));
        if (len + nlen >= sizeof(buf))
          break;
        len += nlen;
      }
    }
  }
  if (count % 3 && len < sizeof(buf))
    snprintf(buf + len, sizeof(buf) - len, "\r\n");

  if (ch->desc) {
	if (len == 0)
	 send_to_char(ch, "All commands have help entries.\r\n");
	else {
	 send_to_char(ch, "Commands without help entries:\r\n");
	 page_string(ch->desc, buf, TRUE);
	}
  }
}

ACMD(do_hindex)
{
  int len, len2, count = 0, count2=0, i;
  char buf[MAX_STRING_LENGTH], buf2[MAX_STRING_LENGTH];

  skip_spaces(&argument);

  if (!*argument) {
    send_to_char(ch, "Usage: hindex <string>\r\n");
    return;
  }

  len = sprintf(buf, "\t1Help index entries beginning with '%s':\t2\r\n", argument);
  len2 = sprintf(buf2, "\t1Help index entries containing '%s':\t2\r\n", argument);
  for (i = 0; i < top_of_helpt; i++) {
    if (is_abbrev(argument, help_table[i].keywords)
        && (GET_LEVEL(ch) >= help_table[i].min_level))
      len +=
          snprintf(buf + len, sizeof(buf) - len, "%-20.20s%s", help_table[i].keywords,
                   (++count % 3 ? "" : "\r\n"));
    else if (strstr(help_table[i].keywords, argument)
        && (GET_LEVEL(ch) >= help_table[i].min_level))
      len2 +=
          snprintf(buf2 + len2, sizeof(buf2) - len2, "%-20.20s%s", help_table[i].keywords,
                   (++count2 % 3 ? "" : "\r\n"));
  }
  if (count % 3)
    len += snprintf(buf + len, sizeof(buf) - len, "\r\n");
  if (count2 % 3)
    len2 += snprintf(buf2 + len2, sizeof(buf2) - len2, "\r\n");

  if (!count)
    len += snprintf(buf + len, sizeof(buf) - len, "  None.\r\n");
  if (!count2)
    snprintf(buf2 + len2, sizeof(buf2) - len2, "  None.\r\n");

  // Join the two strings
  len += snprintf(buf + len, sizeof(buf) - len, "%s", buf2);

  snprintf(buf + len, sizeof(buf) - len, "\t1Applicable Index Entries: \t3%d\r\n"
                                                 "\t1Total Index Entries: \t3%d\tn\r\n", count + count2, top_of_helpt);

  page_string(ch->desc, buf, TRUE);
}
