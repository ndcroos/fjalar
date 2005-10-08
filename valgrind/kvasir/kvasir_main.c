/*
   This file is part of Kvasir, a Valgrind skin that implements the
   C language front-end for the Daikon Invariant Detection System

   Copyright (C) 2004-2005 Philip Guo, MIT CSAIL Program Analysis Group

   2005-04-28:
   Ported over to Valgrind 3 and integrated with the DynComp dynamic
   comparability analysis tool for C/C++.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/* kvasir_main.c:

   This file contains most of the code to interact with the Valgrind
   core.  It is called from mc_main.c since mc_main.c is the
   launching-point for Kvasir.
*/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <search.h>

#include "tool.h"
#include "generate_daikon_data.h"
#include "kvasir_main.h"
#include "decls-output.h"
#include "dtrace-output.h"
#include "mc_include.h"
#include "disambig.h"
#include "dyncomp_main.h"
#include "dyncomp_runtime.h"

// Global variables that are set by command-line options
char* kvasir_decls_filename = 0;
char* kvasir_dtrace_filename = 0;
Bool kvasir_with_dyncomp = False;
Bool kvasir_dyncomp_no_gc = False;
Bool kvasir_dyncomp_fast_mode = False;
Bool kvasir_print_debug_info = False;
Bool kvasir_ignore_globals = False;
Bool kvasir_ignore_static_vars = False;
Bool kvasir_dtrace_append = False;
Bool kvasir_dtrace_no_decs = False;
Bool kvasir_dtrace_gzip = False;
Bool kvasir_output_fifo = False;
#ifdef KVASIR_DEVEL_BUILD
Bool kvasir_asserts_aborts_on = True;
#else
Bool kvasir_asserts_aborts_on = False;
#endif
Bool kvasir_decls_only = False;
Bool kvasir_limit_static_vars = False;
Bool kvasir_default_disambig = False;
Bool kvasir_smart_disambig = False;
Bool kvasir_use_bit_level_precision = False;
Bool kvasir_output_struct_vars = False;
Bool kvasir_repair_format = False;
Bool kvasir_flatten_arrays = False;
Bool kvasir_disambig_ptrs = False;
Bool dyncomp_print_debug_info = False;
Bool dyncomp_separate_entry_exit_comp = False;
int kvasir_array_length_limit = -1;
char* kvasir_dump_prog_pt_names_filename = 0;
char* kvasir_dump_var_names_filename = 0;
char* kvasir_trace_prog_pts_filename = 0;
char* kvasir_trace_vars_filename = 0;
char* kvasir_disambig_filename = 0;
char *kvasir_program_stdout_filename = 0;
char *kvasir_program_stderr_filename = 0;

int dyncomp_gc_after_n_tags = 10000000;

Bool actually_output_separate_decls_dtrace = 0;
Bool print_declarations = 1;

Bool dyncomp_without_dtrace = False;

/*------------------------------------------------------------*/
/*--- Function stack                                       ---*/
/*------------------------------------------------------------*/
// TODO: Should I MAKE THIS HUGE??? or dynamically-allocate??? (original = 10000)
#define FN_STACK_SIZE 10000

FunctionEntry fn_stack[FN_STACK_SIZE];
Int fn_stack_top;       // actually just above top -- next free slot

void printFunctionEntryStack()
{
   int i;
   for (i = fn_stack_top - 1; i >= 0; i--)
      {
         FunctionEntry* cur_fn = &fn_stack[i];
         VG_(printf)("fn_stack[%d] %s - EBP: 0x%x, lowestESP: 0x%x, localArrayVarPtr: %p\n",
                i,
                cur_fn->daikon_name,
                cur_fn->EBP,
                cur_fn->lowestESP,
                cur_fn->localArrayVariablesPtr);
      }
}

/*------------------------------------------------------------*/
/*--- Function entry/exit                                  ---*/
/*------------------------------------------------------------*/

/*
Requires:
Modifies: fn_stack, fn_stack_top
Returns:
Effects: pushes a FunctionEntry onto the top of fn_stack and initializes
         it with function name (f), and EBP values.
         This is called during function entrance.  Initializes
         "virtual stack" and then calls handleFunctionEntrance() to
         generate .dtrace file output at function entrance time
*/
static void push_fn(Char* daikon_name, Addr EBP, Addr startPC)
{
  DaikonFunctionInfo* daikonFuncPtr =
    findFunctionInfoByStartAddr(startPC);

  int formalParamStackByteSize =
     determineFormalParametersStackByteSize(daikonFuncPtr);

  FunctionEntry* top;

  DPRINTF("formalParamStackByteSize is %d\n", formalParamStackByteSize);

  //  VG_(printf)(" @@@ push_fn: %s\n", daikon_name);

  if (fn_stack_top >= FN_STACK_SIZE) VG_(tool_panic)("overflowed fn_stack");

  top = &fn_stack[ fn_stack_top ];

  top->daikon_name = daikon_name;
  top->EBP = EBP;
  top->startPC = startPC;
  top->lowestESP = EBP + 4;
  top->EAX = 0;
  top->EDX = 0;
  top->FPU = 0;

  // Initialize virtual stack and copy parts of the Valgrind stack
  // into that virtual stack
  if (formalParamStackByteSize > 0) {
     // For some reason, VG_(calloc) doesn't work here:
      // This is the error msg. that it gives:
      //   kvasir: the `impossible' happened:
      //   add_MAC_Chunk: shadow area is accessible
     top->virtualStack = calloc(formalParamStackByteSize, sizeof(char));
     top->virtualStackByteSize = formalParamStackByteSize;

     VG_(memcpy)(top->virtualStack, (void*)EBP, (formalParamStackByteSize * sizeof(char)));
     // VERY IMPORTANT!!! Copy all the A & V bits over from EBP to virtualStack!!!
     // (As a consequence, this copies over the tags as well - look in mc_main.c)
     mc_copy_address_range_state(EBP, (Addr)(top->virtualStack), formalParamStackByteSize);
  }
  else {
     // Watch out for null pointer segfaults here:
     top->virtualStack = 0;
     top->virtualStackByteSize = 0;
  }

  // Initialize the FunctionEntry.localArrayVariablesPtr field:
  top->localArrayVariablesPtr = &(daikonFuncPtr->localArrayVariables);

  fn_stack_top++;

  // We used to do this BEFORE the push - does it make a difference???
  //  VG_(printf)("-- PUSH_FN: fn_stack_top: %d, f: %s\n", fn_stack_top, daikon_name);

  // Do this AFTER initializing virtual stack and lowestESP
  handleFunctionEntrance(top);
}

/*
Requires:
Modifies: fn_stack, fn_stack_top
Returns:
Effects: pops a FunctionEntry off of the top of fn_stack and initializes
         it with EAX, EDX, and FPU values. Then calls handleFunctionExit()
         to generate .dtrace file output at function exit time
*/
static void pop_fn(Char* daikon_name,
                   int EAX, int EDX, double FPU_top,
                   UInt EAXshadow, UInt EDXshadow, ULong FPUshadow,
                   UInt EAXtag, UInt EDXtag, UInt FPUtag)
{
   FunctionEntry* top;
   int i;

   //   VG_(printf)(" *** pop_fn: %s\n", daikon_name);

   // s is null if an "unwind" is popped off the stack
   // Only do something if this function name matches what's on the top of the stack
   if (!daikon_name || (!VG_STREQ(fn_stack[fn_stack_top - 1].daikon_name, daikon_name))) {
      VG_(printf)("MISMATCHED on pop_fn! top name: %s, daikon_name: %s\n",
                  fn_stack[fn_stack_top - 1].daikon_name,
                  daikon_name);
      return;
   }

  if (fn_stack_top < 1) VG_(tool_panic)("underflowed fn_stack");

  top =  &fn_stack[ fn_stack_top - 1 ];

  top->EAX = EAX;
  top->EDX = EDX;
  top->FPU = FPU_top;

  // Very important!  Set the A and V bits of the appropriate
  // FunctionEntry object and the tags from the (x86) guest state
  // as well:

  for (i = 0; i < 4; i++) {
     set_abit((Addr)(&(top->EAX)) + (Addr)i, VGM_BIT_VALID);
     set_abit((Addr)(&(top->EDX)) + (Addr)i, VGM_BIT_VALID);
     set_abit((Addr)(&(top->FPU)) + (Addr)i, VGM_BIT_VALID);

     set_vbyte((Addr)(&(top->EAX)) + (Addr)i, (UChar)((EAXshadow & 0xff) << (i * 8)));
     set_vbyte((Addr)(&(top->EDX)) + (Addr)i, (UChar)((EDXshadow & 0xff) << (i * 8)));
     set_vbyte((Addr)(&(top->FPU)) + (Addr)i, (UChar)((FPUshadow & 0xff) << (i * 8)));

     if (kvasir_with_dyncomp) {
        set_tag((Addr)(&(top->EAX)) + (Addr)i, EAXtag);
        set_tag((Addr)(&(top->EDX)) + (Addr)i, EDXtag);
        set_tag((Addr)(&(top->FPU)) + (Addr)i, FPUtag);
     }
  }

  for (i = 4; i < 8; i++) {
     set_abit((Addr)(&(top->FPU)) + (Addr)i, VGM_BIT_VALID);

     set_vbyte((Addr)(&(top->FPU)) + (Addr)i, (UChar)((FPUshadow & 0xff) << (i * 8)));

     if (kvasir_with_dyncomp) {
        set_tag((Addr)(&(top->FPU)) + (Addr)i, FPUtag);
     }
  }

  //  VG_(printf)("-- POP_FN: fn_stack_top: %d, s: %s\n", fn_stack_top, daikon_name);

  handleFunctionExit(top);

   // Destroy the memory allocated by virtualStack
   if (top->virtualStack) {
      // For some reason, VG_(calloc) still doesn't work!!!
      // This is the error msg. that it gives:
      //   kvasir: the `impossible' happened:
      //   add_MAC_Chunk: shadow area is accessible
      free(top->virtualStack);
   }

   fn_stack_top--; // Now pop it off by decrementing fn_stack_top
}


// This is called whenever we encounter an IMark statement.  From the
// IR documentation (Copyright (c) 2004-2005 OpenWorks LLP):
//
// IMark(literal guest address, length)
//
// Semantically a no-op.  However, indicates that the IR statements
// which follow it originally came from a guest instruction of the
// stated length at the stated guest address.  This information is
// needed by some kinds of profiling tools.

// This gets updated whenever we encounter a Ist_IMark instruction.
// It is required to track function exits because the address does not
// come with the Ist_Exit IR instruction:
static Addr currentAddr = 0;

extern char* prog_pts_tree; // from decls-output.c

static char atLeastOneFunctionHandled = 0;

// We will utilize this information to pause the target program at
// function entrances
void handle_possible_entry(MCEnv* mce, Addr64 addr) {
   IRDirty  *di;
   DaikonFunctionInfo* curFuncPtr = 0;

   // Right now, for x86, we only care about 32-bit instructions

   // REMEMBER TO ALWAYS UPDATE THIS regardless of whether this is
   // truly a function entry:
   currentAddr = (Addr)(addr);

   // If this is truly a function entry and we are interested in
   // tracking this particular function ...  This ensures that we only
   // track functions which we have in DaikonFunctionInfoTable!!!
   curFuncPtr = findFunctionInfoByStartAddr(currentAddr);


  // If it's the first time you've ever handled a possible function entrance,
  // then you better run outputDeclsAndCloseFile so that Kvasir
  // can take advantage of all of Valgrind's name demangling functionality
  // while still producing a complete .decls file before the .dtrace file
  // in order to allow streaming feeds into Daikon:
  if (curFuncPtr && !atLeastOneFunctionHandled)
    {
      // Remember to not actually output the .decls right now when
      // we're running DynComp.  We need to wait until the end to
      // actually output .decls, but we need to make a fake run in
      // order to set up the proper data structures
      outputDeclsFile(kvasir_with_dyncomp);

      if (actually_output_separate_decls_dtrace && !dyncomp_without_dtrace) {
	openTheDtraceFile();
      }

      atLeastOneFunctionHandled = 1;
    }

   if (curFuncPtr &&
       // Also, if kvasir_trace_prog_pts_filename is on (we are
       // reading in a ppt list file), then DO NOT generate IR code
       // to call helper functions for functions whose name is
       // NOT located in prog_pts_tree.  This
       // will greatly speed up processing because these functions
       // are filtered out at translation-time, not at run-time
       (!kvasir_trace_prog_pts_filename ||
        prog_pts_tree_entry_found(curFuncPtr))) {

      //         VG_(printf)("entry (Addr: 0x%u) | Daikon name: %s\n",
      //                      currentAddr, curFuncPtr->daikon_name);

      di = unsafeIRDirty_0_N(2/*regparms*/,
                             "enter_function",
                             &enter_function,
                             mkIRExprVec_2(IRExpr_Const(IRConst_U32((Addr)curFuncPtr->daikon_name)),
                                           IRExpr_Const(IRConst_U32(currentAddr))));

      // For function entry, we are interested in observing the ESP so make
      // sure that it's updated by setting the proper annotations:
      di->nFxState = 1;
      di->fxState[0].fx     = Ifx_Read;
      di->fxState[0].offset = mce->layout->offset_SP;
      di->fxState[0].size   = mce->layout->sizeof_SP;

      stmt( mce->bb, IRStmt_Dirty(di) );
   }
}

// Handle a function exit statement, which contains a jump kind of
// 'Ret'.  It seems pretty accurate to cue off of currentAddr, which
// is updated every time an Ist_IMark statement is translated, which
// is quite often
void handle_possible_exit(MCEnv* mce, IRJumpKind jk) {
   if (Ijk_Ret == jk) {
      IRDirty  *di;

      DaikonFunctionInfo* curFuncPtr = findFunctionInfoByAddrSlow(currentAddr);

      if (curFuncPtr &&
          // Also, if kvasir_trace_prog_pts_filename is on (we are
          // reading in a ppt list file), then DO NOT generate IR
          // code to call helper functions for functions whose
          // names are NOT located in prog_pts_tree.  This will
          // greatly speed up processing because these functions
          // are filtered out at translation-time, not at run-time
          (!kvasir_trace_prog_pts_filename ||
           prog_pts_tree_entry_found(curFuncPtr))) {

         //            VG_(printf)("exit Daikon name: %s\n",
         //                        curFuncPtr->daikon_name);

         di = unsafeIRDirty_0_N(1/*regparms*/,
                                "exit_function",
                                &exit_function,
                                mkIRExprVec_1(IRExpr_Const(IRConst_U32((Addr)curFuncPtr->daikon_name))));

         // For function exit, we are interested in observing the
         // ESP, EAX, EDX, FPTOP, and FPREG[], so make sure that
         // they are updated by setting the proper annotations:
         di->nFxState = 4;
         di->fxState[0].fx     = Ifx_Read;
         di->fxState[0].offset = mce->layout->offset_SP;
         di->fxState[0].size   = mce->layout->sizeof_SP;

         // Now I'm totally hacking based upon the definition of
         // VexGuestX86State in vex/pub/libvex_guest_x86.h:
         // (This is TOTALLY x86 dependent right now, but oh well)
         di->fxState[1].fx     = Ifx_Read;
         di->fxState[1].offset = 0; // offset of EAX
         di->fxState[1].size   = sizeof(UInt); // 4 bytes

         di->fxState[2].fx     = Ifx_Read;
         di->fxState[2].offset = 8; // offset of EDX
         di->fxState[2].size   = sizeof(UInt); // 4 bytes

         di->fxState[3].fx     = Ifx_Read;
         di->fxState[3].offset = 60; // offset of FPTOP
         // Size of FPTOP + all 8 elements of FPREG
         di->fxState[3].size   = sizeof(UInt) + (8 * sizeof(ULong));

         stmt( mce->bb, IRStmt_Dirty(di) );
      }
   }
}


/*
Requires:
Modifies: fn_stack, fn_stack_top
Returns:
Effects: This is the hook into Valgrind that is called whenever the target
         program enters a function.  Calls push_fn() if all goes well.
*/
// Rudimentary function entrance/exit tracking
VGA_REGPARM(2)
void enter_function(Char* daikon_name, Addr StartPC)
{
   Addr  ESP = VG_(get_SP)(VG_(get_running_tid)());
   // Assign %esp - 4 to %ebp - empirically tested to be
   // correct for calling conventions
   Addr  EBP = ESP - 4;

   DPRINTF("Enter function: %s - StartPC: %p\n",
	   daikon_name, (void*)StartPC);

   DPRINTF("Calling push_fn for %s\n", daikon_name);

   push_fn(daikon_name, EBP, StartPC);
}

/*
Requires:
Modifies: fn_stack, fn_stack_top, topEntry
Returns:
Effects: This is the hook into Valgrind that is called whenever the target
         program exits a function.  Initializes topEntry of fn_stack with
         return values from EAX, EDX, and FPU.  Calls pop_fn() if all goes well.
*/
//
VGA_REGPARM(1)
void exit_function(Char* daikon_name)
{
   ThreadId currentTID = VG_(get_running_tid)();

   // Get the value at the simulated %EAX (integer and pointer return
   // values are stored here upon function exit)
   Addr EAX = VG_(get_EAX)(currentTID);

   // Get the value of the simulated %EDX (the high 32-bits of the
   // long long int return value is stored here upon function exit)
   Addr EDX = VG_(get_EDX)(currentTID);

   // Ok, in Valgrind 2.X, we needed to directly code some assembly to grab
   // the top of the floating-point stack, but Valgrind 3.0 provides a virtual
   // FPU stack, so we can just grab that.  Plus, we now have shadow V-bits
   // for the FPU stack.
   double fpuReturnVal = VG_(get_FPU_stack_top)(currentTID);

   // 64 bits
   // Use SHADOW values of Valgrind simulated registers to get V-bits
   UInt EAXshadow = VG_(get_shadow_EAX)(currentTID);
   UInt EDXshadow = VG_(get_shadow_EDX)(currentTID);
   ULong FPUshadow = VG_(get_shadow_FPU_stack_top)(currentTID);

   UInt EAXtag = 0;
   UInt EDXtag = 0;
   UInt FPUtag = 0;

   if (kvasir_with_dyncomp) {
      EAXtag = VG_(get_EAX_tag)(currentTID);
      EDXtag = VG_(get_EDX_tag)(currentTID);
      FPUtag = VG_(get_FPU_stack_top_tag)(currentTID);
   }

   DPRINTF("Exit function: %s - EAX: 0x%x, EAXshadow: 0x%x, EDXshadow: 0x%x FPUshadow: 0x%x %x\n",
               daikon_name, EAX,
               EAXshadow, EDXshadow,
               (UInt)(FPUshadow & 0xffffffff), (UInt)(FPUshadow >> 32));

   pop_fn(daikon_name,
          EAX, EDX, fpuReturnVal,
          EAXshadow, EDXshadow, FPUshadow,
          EAXtag, EDXtag, FPUtag);
}


/*
Requires:
Modifies: lots of global stuff
Returns:
Effects: All of the Kvasir initialization is performed right here;
         Memcheck mc_main.c calls this at the end of its own
         initialization and this must extract DWARF2 debugging
         information from the ELF executable, process the
         dwarf_entry_array, and create .decls and .dtrace files
*/

// Before processing command-line options
void kvasir_pre_clo_init()
{
   // Clear fn_stack
   VG_(memset)(fn_stack, 0, FN_STACK_SIZE * sizeof(*fn_stack));

   // Clear all global variables before processing command-line options:
   kvasir_decls_filename = 0;
   kvasir_dtrace_filename = 0;
   kvasir_print_debug_info = False;
   kvasir_ignore_globals = False;
   kvasir_ignore_static_vars = False;
   kvasir_dtrace_append = False;
   kvasir_dtrace_no_decs = False;
   kvasir_dtrace_gzip = False;
   kvasir_output_fifo = False;

#ifdef KVASIR_DEVEL_BUILD
   kvasir_asserts_aborts_on = True;
#else
   kvasir_asserts_aborts_on = False;
#endif

   kvasir_decls_only = False;
   kvasir_limit_static_vars = False;
   kvasir_default_disambig = False;
   kvasir_dump_prog_pt_names_filename = 0;
   kvasir_dump_var_names_filename = 0;
   kvasir_trace_prog_pts_filename = 0;
   kvasir_trace_vars_filename = 0;
   kvasir_disambig_filename = 0;
   kvasir_program_stdout_filename = 0;
   kvasir_program_stderr_filename = 0;
}

// Initialize kvasir after processing command-line options
void kvasir_post_clo_init()
{
  // Assume that the filename is the FIRST string in
  // VG(client_argv) since that is the client argv array
  // after being parsed by the Valgrind core:
  char* filename = VG_(client_argv)[0];

  // Handle variables set by command-line options:
  char* DISAMBIG = ".disambig";
  int DISAMBIG_LEN = VG_(strlen)(DISAMBIG);

  DPRINTF("\nReading binary file \"%s\" [0x%x] (Assumes that filename is first argument in client_argv)\n\n",
	  filename, filename);
  DPRINTF("handleFunctionEntrance is at %p\n", handleFunctionEntrance);

  // --disambig results in the disambig filename being ${filename}.disambig
  // (overrides --disambig-file option)
  if (kvasir_default_disambig) {
    char* disambig_filename =
      VG_(calloc)(VG_(strlen)(filename) + DISAMBIG_LEN + 1,
	     sizeof(*disambig_filename));

    VG_(strcpy)(disambig_filename, filename);
    VG_(strcat)(disambig_filename, DISAMBIG);
    kvasir_disambig_filename = disambig_filename;
  }

  DPRINTF("\n%s\n\n", kvasir_disambig_filename);

  // Special-case .dtrace handling if kvasir_dtrace_filename ends in ".gz"
  if (kvasir_dtrace_filename) {
    int filename_len = VG_(strlen)(kvasir_dtrace_filename);
    if VG_STREQ(kvasir_dtrace_filename + filename_len - 3, ".gz") {
      DPRINTF("\nFilename ends in .gz\n");
      // Chop off '.gz' from the end of the filename
      kvasir_dtrace_filename[filename_len - 3] = '\0';
      // Activate kvasir_dtrace_gzip
      kvasir_dtrace_gzip = True;
    }
  }

  // Output separate .decls and .dtrace files if:
  // --decls-only is on OR --decls-file=<filename> is on
  // OR kvasir_with_dyncomp is ON (since DynComp needs to create .decls
  // at the END of the target program's execution so that it can include
  // the comparability info)
  if (kvasir_decls_only || kvasir_decls_filename || kvasir_with_dyncomp) {
    DPRINTF("\nSeparate .decls\n\n");
    actually_output_separate_decls_dtrace = True;
  }

  // Special handling for BOTH kvasir_with_dyncomp and kvasir_decls_only
  // We need to actually do a full .dtrace run but just not output anything
  // to the .dtrace file
  if (kvasir_decls_only && kvasir_with_dyncomp) {
     kvasir_decls_only = False;
     dyncomp_without_dtrace = True;
  }

  // If we are only printing .dtrace and have --dtrace-no-decs,
  // then do not print out declarations
  if (!actually_output_separate_decls_dtrace && kvasir_dtrace_no_decs) {
     print_declarations = 0;
  }

  // If we are using DynComp with the garbage collector, initialize
  // g_oldToNewMap:
  extern UInt* g_oldToNewMap;
  if (kvasir_with_dyncomp && !kvasir_dyncomp_no_gc) {
     g_oldToNewMap = VG_(shadow_alloc)((dyncomp_gc_after_n_tags + 1) * sizeof(*g_oldToNewMap));
  }

  process_elf_binary_data(filename);
  daikon_preprocess_entry_array();
  createDeclsAndDtraceFiles(filename);
}

void kvasir_print_usage()
{
   VG_(printf)(
"\n  Output file format:\n"
"    --decls-file=<string>    The output .decls file location\n"
"                             (forces generation of separate .decls file)\n"
"    --decls-only             Exit after creating .decls file [--no-decls-only]\n"
"    --dtrace-file=<string>   The output .dtrace file location\n"
"                             [daikon-output/PROGRAM_NAME.dtrace]\n"
"    --dtrace-no-decs         Do not include declarations in .dtrace file\n"
"                             [--no-dtrace-no-decs]\n"
"    --dtrace-append          Appends .dtrace data to the end of an existing .dtrace file\n"
"                             [--no-dtrace-append]\n"
"    --dtrace-gzip            Compresses .dtrace data [--no-dtrace-gzip]\n"
"                             (Automatically ON if --dtrace-file string ends in '.gz')\n"
"    --output-fifo            Create output files as named pipes [--no-output-fifo]\n"
"    --program-stdout=<file>  Redirect instrumented program stdout to file\n"
"                             [Kvasir's stdout, or /dev/tty if --dtrace-file=-]\n"
"    --program-stderr=<file>  Redirect instrumented program stderr to file\n"

"\n  Selective program tracing:\n"
"    --ppt-list-file=<string> Trace only the program points listed in this file\n"
"    --var-list-file=<string> Trace only the variables listed in this file\n"
"    --dump-ppt-file=<string> Outputs all program point names to a file\n"
"    --dump-var-file=<string> Outputs all variable names to a file\n"
"    --ignore-globals         Ignores all global variables [--no-ignore-globals]\n"
"    --ignore-static-vars     Ignores all static variables [--no-ignore-static-vars]\n"
"    --limit-static-vars      Limits the output of static vars [--no-limit-static-vars]\n"

"\n  Pointer type disambiguation:\n"
"    --disambig-file=<string> Reads in disambig file if exists; otherwise creates one\n"
"    --disambig               Uses <program name>.disambig as the disambig file\n"
"    --smart-disambig         Infers sensible values for each entry in .disambig file\n"
"                             generated using the --disambig or --disambig-file options\n"
"    --disambig-ptrs          Forces all pointer vars. to point to a single element\n"

"\n  DynComp dynamic comparability analysis\n"
"    --with-dyncomp           Enables DynComp comparability analysis\n"
"    --gc-num-tags            The number of tags that get assigned between successive runs\n"
"                             of the garbage collector (between 1 and INT_MAX)\n"
"                             (The default is to garbage collect every 10,000,000 tags created)\n"
"    --no-dyncomp-gc          Do NOT use the tag garbage collector for DynComp.  (Faster\n"
"                             but may run out of memory for long-running programs)\n"
"    --dyncomp-fast-mode      Approximates the handling of literals for comparability.\n"
"                             (Loses some precision but faster and takes less memory)\n"
"    --separate-entry-exit-comp  Allows variables to have distinct comparability\n"
"                                numbers at function entrance/exit when run with\n"
"                                DynComp.  This provides more accuracy, but may\n"
"                                sometimes lead to output that Daikon cannot accept.\n"

"\n  Misc. options:\n"
"    --flatten-arrays         Force flattening of all statically-sized arrays\n"
"    --output-struct-vars     Outputs struct variables along with their contents\n"
"    --bit-level-precision    Uses bit-level precision to produce more accurate\n"
"                             output at the expense of speed [--no-bit-level-precision]\n"
"    --nesting-depth=N        Limits the maximum number of dereferences of any\n"
"                             structure to N (default is 2)\n"
"    --struct-depth=N         Limits the maximum number of dereferences of recursively\n"
"                             defined structures (i.e. linked lists) to N (default is 2)\n"
"                             (N must be an integer between 0 and 100)\n"
"    --repair-format          Output format for data structure repair tool (internal use)\n"

"\n  Debugging:\n"
#ifdef KVASIR_DEVEL_BUILD
"    --asserts-aborts         Turn on safety asserts and aborts (ON BY DEFAULT)\n"
"                             [--asserts-aborts]\n"
#else
"    --asserts-aborts         Turn on safety asserts and aborts (OFF BY DEFAULT)\n"
"                             [--no-asserts-aborts]\n"
#endif
"    --debug                  Print Kvasir-internal debug messages [--no-debug]\n"
"    --dyncomp-debug          Print DynComp debug messages (--with-dyncomp must also be on)\n"
"                             [--no-dyncomp-debug]\n"
   );
}

/* Like VG_BOOL_CLO, but of the form "--foo", "--no-foo" rather than
   "--foo=yes", "--foo=no". Note that qq_option should not have a
   leading "--". */

#define VG_YESNO_CLO(qq_option, qq_var) \
   if (VG_CLO_STREQ(arg, "--"qq_option)) { (qq_var) = True; } \
   else if (VG_CLO_STREQ(arg, "--no-"qq_option))  { (qq_var) = False; }

// Processes command-line options
Bool kvasir_process_cmd_line_option(Char* arg)
{
   VG_STR_CLO(arg, "--decls-file", kvasir_decls_filename)
   else VG_STR_CLO(arg, "--dtrace-file",    kvasir_dtrace_filename)
   else VG_YESNO_CLO("with-dyncomp",   kvasir_with_dyncomp)
   else VG_BNUM_CLO(arg, "--gc-num-tags", dyncomp_gc_after_n_tags,
                    1, 0x7fffffff)
   else VG_YESNO_CLO("no-dyncomp-gc",     kvasir_dyncomp_no_gc)
   else VG_YESNO_CLO("dyncomp-fast-mode", kvasir_dyncomp_fast_mode)
   else VG_YESNO_CLO("debug",          kvasir_print_debug_info)
   else VG_YESNO_CLO("dyncomp-debug",  dyncomp_print_debug_info)
   else VG_YESNO_CLO("separate-entry-exit-comp",  dyncomp_separate_entry_exit_comp)
   else VG_YESNO_CLO("ignore-globals", kvasir_ignore_globals)
   else VG_YESNO_CLO("ignore-static-vars", kvasir_ignore_static_vars)
   else VG_YESNO_CLO("dtrace-append",  kvasir_dtrace_append)
   else VG_YESNO_CLO("dtrace-no-decs",  kvasir_dtrace_no_decs)
   else VG_YESNO_CLO("dtrace-gzip",    kvasir_dtrace_gzip)
   else VG_YESNO_CLO("output-fifo",    kvasir_output_fifo)
   else VG_YESNO_CLO("asserts-aborts", kvasir_asserts_aborts_on)
   else VG_YESNO_CLO("decls-only",     kvasir_decls_only)
   else VG_YESNO_CLO("limit-static-vars", kvasir_limit_static_vars)
   else VG_YESNO_CLO("bit-level-precision", kvasir_use_bit_level_precision)
   else VG_YESNO_CLO("output-struct-vars", kvasir_output_struct_vars)
   else VG_YESNO_CLO("repair-format", kvasir_repair_format)
   else VG_YESNO_CLO("flatten-arrays", kvasir_flatten_arrays)
   else VG_YESNO_CLO("disambig-ptrs", kvasir_disambig_ptrs)
   else VG_YESNO_CLO("smart-disambig", kvasir_smart_disambig)
   else VG_BNUM_CLO(arg, "--struct-depth",  MAX_STRUCT_INSTANCES, 0, 100) // [0 to 100]
   else VG_BNUM_CLO(arg, "--nesting-depth", MAX_NUM_STRUCTS_TO_DEREFERENCE, 0, 100) // [0 to 100]
   else VG_BNUM_CLO(arg, "--array-length-limit", kvasir_array_length_limit,
                    -1, 0x7fffffff)
   else VG_YESNO_CLO("disambig", kvasir_default_disambig)
   else VG_STR_CLO(arg, "--dump-ppt-file",  kvasir_dump_prog_pt_names_filename)
   else VG_STR_CLO(arg, "--dump-var-file",  kvasir_dump_var_names_filename)
   else VG_STR_CLO(arg, "--ppt-list-file",  kvasir_trace_prog_pts_filename)
   else VG_STR_CLO(arg, "--var-list-file",  kvasir_trace_vars_filename)
   else VG_STR_CLO(arg, "--disambig-file",  kvasir_disambig_filename)
   else VG_STR_CLO(arg, "--program-stdout", kvasir_program_stdout_filename)
   else VG_STR_CLO(arg, "--program-stderr", kvasir_program_stderr_filename)
   else
      return MAC_(process_common_cmd_line_option)(arg);

  return True;
}


// This runs after Kvasir finishes
void kvasir_finish() {
  extern UInt nextTag;

  // If kvasir_smart_disambig is on, then
  // we must create the .disambig file at the very end after
  // Kvasir has run though the entire program so that it can
  // determine whether each pointer variable has only referenced
  // one element or multiple elements throughout this particular execution
  if (disambig_writing && kvasir_smart_disambig) {
    generateDisambigFile();
  }

  if (kvasir_with_dyncomp) {
     // Do one extra propagation of variable comparability at the end
     // of execution once all of the value comparability sets have
     // been properly updated:
     DC_extra_propagate_val_to_var_sets();

     // Now print out the .decls file at the very end of execution:
     DC_outputDeclsAtEnd();
  }

  DYNCOMP_DPRINTF("\n*** nextTag: %u ***\n\n", nextTag);

  //  VG_(printf)("\n*** nextTag: %u ***\n\n", nextTag);

  if (!dyncomp_without_dtrace) {
     finishDtraceFile();
  }
}
