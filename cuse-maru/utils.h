/*  cuse-maru - CUSE implementation of Open Sound System using libmaru.
 *  Copyright (C) 2012 - Hans-Kristian Arntzen
 *  Copyright (C) 2012 - Agnes Heyer
 *
 *  cuse-maru is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  cuse-maru is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with cuse-maru.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MARU_UTILS_H__
#define MARU_UTILS_H__

static inline unsigned next_pot(unsigned v)
{
   v--;
   v |= v >> 1;
   v |= v >> 2;
   v |= v >> 4;
   v |= v >> 8;
   v |= v >> 16;
   v++;
   return v;
}

#endif

