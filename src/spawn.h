/*
 *  Process spawn functions
 *  Copyright (C) 2008 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SPAWN_H
#define SPAWN_H

int find_exec ( const char *name, char *out, size_t len );

int spawn_and_store_stdout(const char *prog, char *argv[], char **outp);

int spawnv(const char *prog, char *argv[]);

int spawn_reap(char *stxt, size_t stxtlen);

void spawn_reaper(void);

#endif /* SPAWN_H */
