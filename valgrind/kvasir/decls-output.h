/*
   This file is part of Kvasir, a Valgrind skin that implements the
   C language front-end for the Daikon Invariant Detection System

   Copyright (C) 2004 Philip Guo, MIT CSAIL Program Analysis Group

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/* decls-output.h:
   Functions for creating .decls and .dtrace files and outputting
   name and type information into a Daikon-compatible .decls file
*/

#ifndef DECLS_OUTPUT_H
#define DECLS_OUTPUT_H

#include "generate_daikon_data.h"
#include "disambig.h"
#include <stdio.h>

// Un-comment to use experimental code for visiting variables:
// e.g., visitVariable(), visitSingleVar(), visitSequence()
#define USE_EXP_VISIT_CODE

#define MAXIMUM_ARRAY_SIZE_TO_EXPAND 10

int g_daikonVarIndex;

struct genhashtable* g_compNumberMap;
int g_curCompNumber;

// For use by printOneDaikonVariableAndDerivatives
typedef enum VariableOrigin {
  DERIVED_VAR, // Always switches to this after one recursive call
  DERIVED_FLATTENED_ARRAY_VAR, // A derived variable as a result of flattening an array
  GLOBAL_VAR,
  FUNCTION_ENTER_FORMAL_PARAM,
  FUNCTION_EXIT_FORMAL_PARAM,
  FUNCTION_RETURN_VAR // Assume for function exits only
} VariableOrigin;

// For use by printVariablesInVarList() and other functions
typedef enum OutputFileType {
  DECLS_FILE,
  DTRACE_FILE,
  DISAMBIG_FILE,
  DYNCOMP_EXTRA_PROP, // only for DynComp
  FAUX_DECLS_FILE     // only for DynComp
} OutputFileType;

// For use by vars_tree:
typedef struct {
  char* function_daikon_name;
  char* function_variables_tree; // A GNU binary tree of variable names (strings)
} FunctionTree;

const char* ENTRY_DELIMETER;
const char* GLOBAL_STRING;
const char* ENTER_PPT;
const char* EXIT_PPT;

extern UInt MAX_VISIT_STRUCT_DEPTH;
extern UInt MAX_VISIT_NESTING_DEPTH;

// String stack:
#define MAX_STRING_STACK_SIZE 100
char* fullNameStack[MAX_STRING_STACK_SIZE];
int fullNameStackSize;

void stringStackPush(char** stringStack, int* pStringStackSize, char* str);
char* stringStackPop(char** stringStack, int* pStringStackSize);
char* stringStackTop(char** stringStack, int stringStackSize);
void stringStackClear(int* pStringStackSize);
int stringStackStrLen(char** stringStack, int stringStackSize);
char* stringStackStrdup(char** stringStack, int stringStackSize);


char createDeclsAndDtraceFiles(char* appname);
char splitDirectoryAndFilename(const char* input,
                               char** dirnamePtr,
                               char** filenamePtr);

void printDeclsHeader();
void printOneFunctionDecl(DaikonFunctionInfo* funcPtr,
                          char isEnter,
                          char faux_decls);

void printAllFunctionDecls(char faux_decls);
void printAllObjectAndClassDecls();

int compareStrings(const void *a, const void *b);
void initializeProgramPointsTree();

char prog_pts_tree_entry_found(DaikonFunctionInfo* cur_entry);

int compareFunctionTrees(const void *a, const void *b);
void initializeVarsTree();

void outputDeclsFile(char faux_decls);
void DC_outputDeclsAtEnd();
void openTheDtraceFile(void);

void printVariablesInVarList(DaikonFunctionInfo* funcPtr,
                             char isEnter,
			     VariableOrigin varOrigin,
			     char* stackBaseAddr,
			     OutputFileType outputType,
			     char allowVarDumpToFile,
			     char* trace_vars_tree,
                             char printClassProgramPoint,
                             char stopAfterFirstVar);

void visitVariable(DaikonVariable* var,
                   void* pValue,
                   char overrideIsInit,
                   VariableOrigin varOrigin,
                   OutputFileType outputType,
                   char allowVarDumpToFile,
                   char* trace_vars_tree,
                   DaikonFunctionInfo* varFuncInfo,
                   char isEnter);

#endif
