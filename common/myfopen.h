/*  -*- mode: C; c-file-style: "gnu"; fill-column: 78 -*-
 *
 * Copyright (c) 2010, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */
#ifndef SATSOLVER_MYFOPEN_H
# define SATSOLVER_MYFOPEN_H
# include <stdio.h>
# include <zlib.h>

FILE *mygzfopen(gzFile* gzf);
FILE* myfopen(const char* fn);

#endif // SATSOLVER_MYFOPEN_H
