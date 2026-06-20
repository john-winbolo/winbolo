/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */


#ifndef _DEBUG_FILE_OUTPUT_H
#define _DEBUG_FILE_OUTPUT_H

#include <stdio.h>
#include <string.h>

char streamText[2048];

void getFileName(char *file);

void setFileName(char *newFileName);

int openDebugFile();

int writeDebugFile(char *text);

int closeDebugFile();

int getWriteToDebugFileStream();

void setWriteToDebugFileStream(int write);

#endif