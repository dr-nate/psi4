# PSI4: an ab initio quantum chemistry software package
#
# Copyright (c) 2007-2013 The PSI4 Developers.
#
# The copyrights for code used from other parties are included in
# the corresponding files.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# - Find libdl
# Find the native libdl includes and library
#
#  LIBDL_INCLUDE_DIR - where to find dlfcn.h
#  LIBDL_LIBRARIES   - List of libraries when using libdl.
#  LIBDL_FOUND       - True if libdl found.
find_lib_x(dl dlfcn.h dlfcn.h ltdl)
