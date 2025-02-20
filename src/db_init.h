/*
 * Copyright (C) 2015 Christian Meffert <christian.meffert@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef SRC_DB_INIT_H_
#define SRC_DB_INIT_H_

#include <sqlite3.h>

/* Rule of thumb: Will the current version of the server work with the new
 * version of the database? If yes, then it is a minor upgrade, if no, then it
 * is a major upgrade. In other words minor version upgrades permit downgrading
 * the server after the database was upgraded. */
#define SCHEMA_VERSION_MAJOR 22
#define SCHEMA_VERSION_MINOR 3

int
db_init_indices(sqlite3 *hdl);

int
db_init_triggers(sqlite3 *hdl);

int
db_init_tables(sqlite3 *hdl);

#endif /* SRC_DB_INIT_H_ */
