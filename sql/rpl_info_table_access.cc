/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "rpl_info_table_access.h"
#include "rpl_utility.h"
#include "handler.h"
#include "sql_parse.h"

/**
  Opens and locks a table.

  It's assumed that the caller knows what they are doing:
  - whether it was necessary to reset-and-backup the open tables state
  - whether the requested lock does not lead to a deadlock
  - whether this open mode would work under LOCK TABLES, or inside a
  stored function or trigger.

  Note that if the table can't be locked successfully this operation will
  close it. Therefore it provides guarantee that it either opens and locks
  table or fails without leaving any tables open.

  @param[in]  thd           Thread requesting to open the table
  @param[in]  dbstr         Database where the table resides
  @param[in]  tbstr         Table to be openned
  @param[in]  max_num_field Maximum number of fields
  @param[in]  lock_type     How to lock the table
  @param[out] table         We will store the open table here
  @param[out] backup        Save the lock info. here

  @return
    @retval TRUE open and lock failed - an error message is pushed into the
                                        stack
    @retval FALSE success
*/
bool Rpl_info_table_access::open_table(THD* thd, const LEX_STRING dbstr,
                                       const LEX_STRING tbstr,
                                       uint max_num_field,
                                       enum thr_lock_type lock_type,
                                       TABLE** table,
                                       Open_tables_backup* backup)
{
  TABLE_LIST tables;

  uint flags= (MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
               MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
               MYSQL_OPEN_IGNORE_FLUSH |
               MYSQL_LOCK_IGNORE_TIMEOUT |
               MYSQL_LOCK_RPL_INFO_TABLE);

  DBUG_ENTER("Rpl_info_table_access::open_table");

  /*
    This is equivalent to a new "statement". For that reason, we call both
    lex_start() and mysql_reset_thd_for_next_command.
  */
  if (thd->slave_thread || !current_thd)
  { 
    lex_start(thd);
    mysql_reset_thd_for_next_command(thd);
  }

  thd->reset_n_backup_open_tables_state(backup);

  tables.init_one_table(dbstr.str, dbstr.length, tbstr.str, tbstr.length,
                        tbstr.str, lock_type);

  if (!open_n_lock_single_table(thd, &tables, tables.lock_type, flags))
  {
    close_thread_tables(thd);
    thd->restore_backup_open_tables_state(backup);
    my_error(ER_NO_SUCH_TABLE, MYF(0), dbstr.str, tbstr.str);
    DBUG_RETURN(TRUE);
  }

  DBUG_ASSERT(tables.table->s->table_category == TABLE_CATEGORY_RPL_INFO);

  if (tables.table->s->fields < max_num_field)
  {
    /*
      Safety: this can only happen if someone started the server and then
      altered the table.
    */
    my_error(ER_COL_COUNT_DOESNT_MATCH_CORRUPTED_V2, MYF(0),
             tables.table->s->db.str, tables.table->s->table_name.str,
             max_num_field, tables.table->s->fields);
    close_thread_tables(thd);
    thd->restore_backup_open_tables_state(backup);
    DBUG_RETURN(TRUE);
  }

  *table= tables.table;
  tables.table->use_all_columns();
  DBUG_RETURN(FALSE);
}

/**
  Commits the changes, unlocks the table and closes it. This method
  needs to be called even if the open_table fails, in order to ensure
  the lock info is properly restored.

  @param[in] thd    Thread requesting to close the table
  @param[in] table  Table to be closed
  @param[in] backup Restore the lock info from here
  @param[in] error  If there was an error while updating
                    the table

  If there is an error, rolls back the current statement. Otherwise,
  commits it. However, if a new thread was created and there is an
  error, the transaction must be rolled back. Otherwise, it must be
  committed. In this case, the changes were not done on behalf of
  any user transaction and if not finished, there would be pending
  changes.
  
  @return
    @retval FALSE No error
    @retval TRUE  Failure
*/
bool Rpl_info_table_access::close_table(THD *thd, TABLE* table,
                                        Open_tables_backup *backup,
                                        bool error)
{
  DBUG_ENTER("Rpl_info_table_access::close_table");

  if (table)
  {
    if (error)
      ha_rollback_trans(thd, FALSE);
    else
      ha_commit_trans(thd, FALSE);

    if (saved_current_thd != current_thd)
    {
      if (error)
        ha_rollback_trans(thd, TRUE);
      else
        ha_commit_trans(thd, TRUE);
    }
    close_thread_tables(thd);
    thd->restore_backup_open_tables_state(backup);
  }

  DBUG_RETURN(FALSE);
}

/**
  Positions the internal pointer of `table` to the place where (id)
  is stored.

  In case search succeeded, the table cursor points to the found row.

  @param[in]      server_id    Server id
  @param[in]      idx          Index field
  @param[in,out]  field_values The sequence of values
  @param[in,out]  table        Table

  @return
    @retval FOUND     The row was found.
    @retval NOT_FOUND The row was not found.
    @retval ERROR     There was a failure.
*/
enum enum_return_id Rpl_info_table_access::find_info_for_server_id(ulong server_id,
                                                                   uint idx,
                                                                   Rpl_info_values *field_values,
                                                                   TABLE *table)
{
  uchar key[MAX_KEY_LENGTH];
  DBUG_ENTER("Rpl_info_table_access::find_info_for_server_id");

  field_values->value[idx].set_int(server_id, TRUE, &my_charset_bin);

  if (field_values->value[idx].length() > table->field[idx]->field_length)
    DBUG_RETURN(ERROR_ID);

  table->field[idx]->store(field_values->value[idx].c_ptr_safe(),
                           field_values->value[idx].length(),
                           &my_charset_bin);

  if (!(table->field[idx]->flags & PRI_KEY_FLAG) &&
      table->s->keys_in_use.is_set(0))
    DBUG_RETURN(ERROR_ID);

  key_copy(key, table->record[0], table->key_info, table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0], 0, key, HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    DBUG_RETURN(NOT_FOUND_ID);
  }

  DBUG_RETURN(FOUND_ID);
}

/**
  Reads information from a sequence of fields into a set of LEX_STRING
  structures, where the sequence of values is specified through the object
  Rpl_info_values.

  @param[in] max_num_field Maximum number of fields
  @param[in] fields        The sequence of fields
  @param[in] field_values  The sequence of values

  @return
    @retval FALSE No error
    @retval TRUE  Failure
 */
bool Rpl_info_table_access::load_info_values(uint max_num_field, Field **fields,
                                             Rpl_info_values *field_values)
{
  DBUG_ENTER("Rpl_info_table_access::load_info_values");
  char buff[MAX_FIELD_WIDTH];
  String str(buff, sizeof(buff), &my_charset_bin);

  uint field_idx= 0;
  while (field_idx < max_num_field)
  {
    fields[field_idx]->val_str(&str);
    field_values->value[field_idx].copy(str.c_ptr_safe(), str.length(),
                                             &my_charset_bin);
    field_idx++;
  }

  DBUG_RETURN(FALSE);
}

/**
  Stores information from a sequence of fields into a set of LEX_STRING
  structures, where the sequence of values is specified through the object
  Rpl_info_values.

  @param[in] max_num_field Maximum number of fields
  @param[in] fields        The sequence of fields
  @param[in] field_values  The sequence of values

  @return
    @retval FALSE No error
    @retval TRUE  Failure
 */
bool Rpl_info_table_access::store_info_values(uint max_num_field, Field **fields,
                                              Rpl_info_values *field_values)
{
  DBUG_ENTER("Rpl_info_table_access::store_info_values");
  uint field_idx= 0;

  while (field_idx < max_num_field)
  {
    fields[field_idx]->set_notnull();
    if (fields[field_idx]->store(field_values->value[field_idx].c_ptr_safe(),
                                 field_values->value[field_idx].length(),
                                 &my_charset_bin))
    {
      my_error(ER_RPL_INFO_DATA_TOO_LONG, MYF(0),
               fields[field_idx]->field_name);
      DBUG_RETURN(TRUE);
    }
    field_idx++;
  }

  DBUG_RETURN(FALSE);
}

/**
  Creates a new thread if necessary. In the bootstrap process or in
  the mysqld startup, a thread is created in order to be able to
  access a table. Otherwise, the current thread is used.

  @return
    @retval THD* Pointer to thread structure
*/
THD *Rpl_info_table_access::create_thd()
{
  THD *thd= NULL;
  saved_current_thd= current_thd;

  if (!current_thd)
  {
    thd= new THD;
    thd->thread_stack= (char*) &thd;
    thd->store_globals();
  }
  else
    thd= current_thd;

  return(thd);
}

/**
  Destroys the created thread if necessary and restores the
  system_thread information.

  @param[in] thd Thread requesting to be destroyed

  @return
    @retval FALSE No error
    @retval TRUE  Failure
*/
bool Rpl_info_table_access::drop_thd(THD *thd)
{
  DBUG_ENTER("Rpl_info::drop_thd");

  if (saved_current_thd != current_thd)
  {
    delete thd;
    my_pthread_setspecific_ptr(THR_THD,  NULL);
  }

  DBUG_RETURN(FALSE);
}
