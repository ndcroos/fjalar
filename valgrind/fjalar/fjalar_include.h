/*
   This file is part of Fjalar, a dynamic analysis framework for
   C and C++ programs.

   Copyright (C) 2004-2006 Philip Guo <pgbovine (@) alum (.) mit (.) edu>,
   MIT CSAIL Program Analysis Group

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
*/

/*********************************************************************
fjalar_include.h

This file is the public API of Fjalar and contains the functions and
data structures that tools can use.

**********************************************************************/

#ifndef FJALAR_INCLUDE_H
#define FJALAR_INCLUDE_H

// Interfaces with Valgrind core:
#include "pub_tool_basics.h"
#include "pub_tool_options.h"

#include "GenericHashtable.h"

/*********************************************************************
Supporting data structures and enums
**********************************************************************/

typedef enum _DeclaredType {
  D_NO_TYPE, // Create padding

  D_UNSIGNED_CHAR,
  D_CHAR,
  D_UNSIGNED_SHORT,
  D_SHORT,
  D_UNSIGNED_INT,
  D_INT,
  D_UNSIGNED_LONG_LONG_INT,
  D_LONG_LONG_INT,

  D_FLOAT,
  D_DOUBLE,
  D_LONG_DOUBLE,

  D_ENUMERATION,
  D_STRUCT_CLASS,
  D_UNION,

  D_FUNCTION,
  D_VOID,
  D_CHAR_AS_STRING,   // when .disambig 'C' option is used with chars
  D_BOOL              // C++ only
} DeclaredType;


typedef enum _VisibilityType {
  PUBLIC_VISIBILITY,    // purposely made this the default
  PROTECTED_VISIBILITY,
  PRIVATE_VISIBILITY
} VisibilityType;



// Simple generic singly-linked list with a void* element
// (only meant to support forward traversal)
//    Yep, re-inventing the wheel once again :)
typedef struct _SimpleList SimpleList;
typedef struct _SimpleNode SimpleNode;

struct _SimpleNode {
  void* elt;
  SimpleNode* next;
};

struct _SimpleList {
  SimpleNode* first;
  SimpleNode* last;
  UInt numElts;
};

// These list operations don't perform any dynamic memory allocation
// or de-allocation, so the client needs to explicitly allocate and
// free 'lst' and 'elt'

// Initializes the list 'lst' with 0 elements, assuming 'lst' has
// already been allocated by the client:
void SimpleListInit(SimpleList* lst);

// Insert 'elt' at the end of the list 'lst':
void SimpleListInsert(SimpleList* lst, void* elt);

// Pops element from head of the list and returns it
// (Returns 0 if list is empty):
void* SimpleListPop(SimpleList* lst);

// Clears all elements in the list by freeing the SimpleNode nodes,
// but does NOT call free on 'elts':
void SimpleListClear(SimpleList* lst);


/*********************************************************************

These three main types represent the compile-time information in the
target program: FunctionEntry, VariableEntry, TypeEntry

FunctionEntry - functions
VariableEntry - variables: globals, function params, member variables
TypeEntry     - types: base types, structs, unions, C++ classes, enums

All of these 'classes' can be 'subclassed' by tools, so tools should
only create and destroy instances using the 'constructor' and
'destructor' functions listed in fjalar_tool.h and not directly use
VG_(malloc) and VG_(free).

Each tool can make at most one subclass of each of these classes by
creating a struct with an instance of one of these classes inlined as
the first field (structural inheritance) and then specifying for the
constructor and destructor to allocate and deallocate memory regions
of the size of the subclass instance (not the base class instance):

Example of subclassing by structural inheritance:

typedef struct _CustomTypeEntry {
  TypeEntry tE;  // Must be the FIRST member
  // All additional fields of the subclass
  int foo;
  char* bar;
  double baz;

} CustomTypeEntry

TypeEntry* constructTypeEntry() {
  return (TypeEntry*)(VG_(calloc)(1, sizeof(CustomTypeEntry)));
}

void destroyTypeEntry(TypeEntry* t) {
  VG_(free)((CustomTypeEntry*)t);
}

**********************************************************************/

/******** TypeEntry ********/

typedef struct _AggregateType AggregateType;

// TypeEntry instances only exist for structs, classes, unions, enums,
// and base types.  There is no distinction between a type and a
// pointer to that type (or a pointer to a pointer to that type,
// etc.).  Pointers are represented using the ptrLevels field of the
// VariableEntry object that contains a TypeEntry instance.  Thus,
// arbitrary levels of pointers to a particular type can be
// represented by one TypeEntry instance.  Instances of this type
// should definitely be IMMUTABLE because they are often aliased and
// shared.
typedef struct _TypeEntry {
  DeclaredType decType;

  // The name of the enumeration or struct/union/class type
  // Only non-null if decType == {D_ENUMERATION, D_STRUCT_CLASS, D_UNION}
  char* typeName;

  // Number of bytes for each variable of this type
  int byteSize;

  // Only non-null if this type is a struct/union/class type:
  // (decType == {D_STRUCT_CLASS, D_UNION})
  AggregateType* aggType;

} TypeEntry;

// Macros for identifying properties of TypeEntry instances:
#define IS_AGGREGATE_TYPE(t) (t->aggType)


// Boring forward references:
typedef struct _VarList VarList;
typedef struct _VarNode VarNode;

// TypeEntry information for an aggregate type (struct, class, union)
struct _AggregateType {
  // Non-static (instance) member variables
  // (only non-null if at least 1 exists)
  VarList* memberVarList;

  // Static (class) member variables
  // (only non-null if at least 1 exists)
  // Remember that static member variables are actually allocated
  // at statically-fixed locations just like global variables
  // (All VariableEntry instances in this list are also aliased in the
  // globalVars list because static member variables are really
  // globals albeit with limited scoping)
  VarList* staticMemberVarList;

  // For C++: A list of pointers to member functions of this class:
  // Every elt is a FunctionEntry* so this is like List<FunctionEntry>
  // (Only non-null if there is at least 1 member function)
  SimpleList* /* <FunctionEntry> */ memberFunctionList;

  // Special member functions that are constructors/destructors:
  // (Only non-null if there is at least 1 element in the list)
  SimpleList* /* <FunctionEntry> */ constructorList;
  SimpleList* /* <FunctionEntry> */ destructorList;

  // A list of classes that are the superclasses of this class
  // Every elt is a Superclass* so this is like List<Superclass>
  // (Only non-null if there is at least 1 superclass)
  // TODO: We never free the dynamically-allocated Superclass entries,
  //       but that shouldn't really matter in practice - oh well...
  SimpleList* /* <Superclass> */ superclassList;

};


// Holds information about class inheritance for C++
typedef struct _Superclass {
  char* className;
  TypeEntry* class;           // (class->typeName == className)
  VisibilityType inheritance; // The visibility of inheritance
  // All the member vars of this superclass are located within the
  // subclass starting at location member_var_offset.  This means that
  // we must add member_var_offset to the data_member_location of
  // member variables in 'class' in order to find them in the
  // subclass (this is 0 except when there is multiple inheritance):
  unsigned long member_var_offset;

} Superclass;


// Global singleton entries for basic types.  To see whether a
// particular TypeEntry instances is a basic type, simply do a pointer
// comparison to the address of one of the following entries:
TypeEntry UnsignedCharType;
TypeEntry CharType;
TypeEntry UnsignedShortType;
TypeEntry ShortType;
TypeEntry UnsignedIntType;
TypeEntry IntType;
TypeEntry UnsignedLongLongIntType;
TypeEntry LongLongIntType;
TypeEntry FloatType;
TypeEntry DoubleType;
TypeEntry LongDoubleType;
TypeEntry FunctionType;
TypeEntry VoidType;
TypeEntry BoolType;


// Return a type entry, given the name of the type (only for
// struct/union/class types): [Fast hashtable lookup]
TypeEntry* getTypeEntry(char* typeName);


// Iterator for TypeEntry entries:
/*
Programming idiom for iterating over all struct/union/class types used
in the target program:

  TypeIterator* typeIt = newTypeIterator();

  while (hasNextType(typeIt)) {
    TypeEntry* cur_type = nextType(typeIt);
    ... work with cur_type ...
  }

  deleteTypeIterator(typeIt);
*/
typedef struct _TypeIterator{
  struct geniterator* it;
} TypeIterator;

TypeIterator* newTypeIterator(void);
Bool hasNextType(TypeIterator* typeIt);
TypeEntry* nextType(TypeIterator* typeIt);
void deleteTypeIterator(TypeIterator* typeIt);


/******** VariableEntry ********/

typedef struct _GlobalVarInfo GlobalVarInfo;
typedef struct _StaticArrayInfo StaticArrayInfo;
typedef struct _MemberVarInfo MemberVarInfo;

// VariableEntry contains information about a variable in the program:
// Instances should be mostly IMMUTABLE after initialization (with the
// exception of the disambigMultipleElts and
// pointerHasEverBeenObserved fields).  Do not modify its fields
// unless you are in the process of initializing it.
typedef struct _VariableEntry {
  // For non-global variables, this is the variable's name as it
  // appears in the program, but for globals and file-static
  // variables, this is a UNIQUE name that's the result of prepending
  // a '/' (for true globals), a filename (for file-static), and a
  // function name (for file-statics declared within a function) to
  // the front of the variable's original name.
  char* name;

  // Byte offset of this variable from head of stack frame (%ebp) for
  // function parameters and local variables
  int byteOffset;

  // Global variable information:
  // (if null, then this variable is not a global variable)
  GlobalVarInfo* globalVar;

  // (If null, then this variable is not a static array)
  StaticArrayInfo* staticArr;

  // Struct/class/union member variable information
  // (If null, then this is not a member variable)
  MemberVarInfo* memberVar;

  // varType refers to the type of the variable after all pointer
  // dereferences are completed, so don't directly use
  // varType->byteSize to get the size of the variable that a
  // VariableEntry instance is referring to; use getBytesBetweenElts()
  TypeEntry* varType;

  // Levels of pointer indirection until we reach the type indicated
  // by varType.  This allows a single varType TypeEntry instance to
  // be able to represent variables that are arbitrary levels of
  // pointers to that type.  If something is an array, ptrLevels is
  // incremented by 1.  (For C++, this does NOT take reference (&)
  // modifiers into account - see the referenceLevels field.)  For
  // example, a variable of type 'unsigned int**' would have:
  // (varType == &UnsignedIntType) && (ptrLevels == 2).
  UInt ptrLevels;

  // For C++ only, this is 1 if this variable is a reference to the
  // type denoted by varType (this shouldn't ever increase above 1
  // because you can't have multiple levels of references, I don't
  // think)
  // For example, a variable of type 'unsigned int*&' would have
  // (varType == &UnsignedIntType) && (referenceLevels == 1) &&
  // (ptrLevels == 1)
  UInt referenceLevels;

  Bool isString; // Does this variable look like a C-style string (or
		 // a pointer to a string, or a pointer to a pointer
		 // to a string, etc)?  True iff (varType == &CharType)
		 // and (ptrLevels > 0)

  // For .disambig option: 0 for no disambig info, 'A' for array, 'P'
  // for pointer, 'C' for char, 'I' for integer, 'S' for string
  // (Automatically set a 'P' disambig for the C++ 'this' parameter
  // since that will always point to one element)
  char disambig;

  // Only relevant for pointer variables (ptrLevels > 0): True if this
  // particular variable has ever pointed to more than 1 element, 0
  // otherwise.  These are the only two fields of this struct that
  // could possibly be modified after initialization.  They are used
  // to generate a .disambig file using the --smart-disambig option.
  Bool disambigMultipleElts;       // mutable
  Bool pointerHasEverBeenObserved; // mutable

} VariableEntry;


// VariableEntry information for struct/class/union member variables:
struct _MemberVarInfo {
  // The offset of this member variable from the beginning of the
  // struct/union/class (always 0 for unions)
  unsigned long data_member_location;

  // This is non-null (along with globalVar) for C++ class static
  // member variables, or it's also non-null (globalVar null) for all
  // member variables.  It indicates the struct/union/class to which
  // this variable belongs:
  TypeEntry* structParentType;

  // Only relevant for C++ member variables
  VisibilityType visibility;

  // For bit-fields (full support not yet implemented)
  int internalByteSize;
  int internalBitOffset;  // Bit offset from the start of byteOffset
  int internalBitSize;    // Bit size for bitfields

};


// VariableEntry information for global variables:
struct _GlobalVarInfo {
  char* fileName;  // The file where this variable was declared -
                   // useful for disambiguating two or more file-static
                   // variables in different files with the same name
                   // (in that case, the name field will contain the
                   // filename prepended in front of the variable name)

  Bool isExternal; // True if this variable is visible outside of
                   // fileName (i.e., it is truly a global variable).
                   // False if it is a file-static variable (only
                   // visible inside of one file) or a static variable
                   // declared within a function (only visible inside
                   // of one function).

  Addr globalLocation;  // The address of this global variable

  Addr functionStartPC; // The start PC address of the function which
                        // this variable belongs to - This is only
                        // valid (non-null) for file-static variables
                        // that are declared within functions
};


// VariableEntry information for static arrays:
struct _StaticArrayInfo {
  UInt  numDimensions; // The number of dimensions of this array

  // This is an array of size numDimensions:
  UInt* upperBounds;   // The upper bound in each dimension,
                       // which is 1 less than the size
                       // e.g. myArray[30][40][50] would have
                       // numDimensions=3 and upperBounds={29, 39, 49}
};

// Macros for identifying properties of VariableEntry instances:
#define IS_GLOBAL_VAR(v) (v->globalVar)
#define IS_STATIC_ARRAY_VAR(v) (v->staticArr)
#define IS_MEMBER_VAR(v) (v->memberVar)


// A more sophisticated linked list for VariableEntry objects.  It is
// doubly-linked and each node contains a pointer to a VariableEntry
// instance (in order to support 'subclassing')

struct _VarNode {
  // dynamically-allocated with constructVariableEntry() - must be
  // destroyed with destroyVariableEntry() (see fjalar_tool.h)
  VariableEntry* var;
  VarNode* prev;
  VarNode* next;
};

struct _VarList {
  VarNode* first;
  VarNode* last;
  unsigned int numVars;
};

// Clears the VarList referred-to by varListPtr, and if
// destroyVariableEntries is True, also calls
// destroyVariableEntry() on each var in the list (see fjalar_tool.h).
void clearVarList(VarList* varListPtr, Bool destroyVariableEntries);

// Inserts a new node at the tail of the list and allocates a new
// VariableEntry using constructVariableEntry()
void insertNewNode(VarList* varListPtr);

// Deletes the last node of the list and destroys the VariableEntry
// within that node using destroyVariableEntry()
void deleteTailNode(VarList* varListPtr);


// Iterator for VariableEntry entries:
/*
Programming idiom for iterating over all variables in a given VarList

  VarList* vlist = ... set to point to a VarList ...
  VarIterator* varIt = newVarIterator(vlist);

  while (hasNextVar(varIt)) {
    VariableEntry* cur_var = nextVar(varIt);
    ... work with cur_var ...
  }

  deleteVarIterator(varIt);
*/
typedef struct {
  VarNode* curNode;
} VarIterator;

VarIterator* newVarIterator(VarList* vlist);
Bool hasNextVar(VarIterator* varIt);
VariableEntry* nextVar(VarIterator* varIt);
void deleteVarIterator(VarIterator* varIt);


// List of all global variables
// (including C++ static member variables)
VarList globalVars;


/******** FunctionEntry ********/

// Contains information about a particular function -
// Should be IMMUTABLE after initialization
typedef struct _FunctionEntry {
  // The standard C name for a function, with no parens or formal
  // param types (i.e. "sum")
  // Even C++ functions have regular C names.
  // i.e., for a C++ function "sum(int, int)", its C name is "sum"
  char* name;

  // The mangled name (C++ only) - based on some standard mangling scheme
  char* mangled_name;

  // The de-mangled name (C++ only)
  // This will have parens and formal param types. e.g., "sum(int, int)"
  char* demangled_name;

  // The file where this function is defined
  char* filename;

  // fjalar_name is a version of name that is guaranteed (hopefully)
  // to be unique.

  // Global functions have a '..' prepended to the front: eg ..main()
  // File-static functions have the filename appended:
  //    dirname/filename.c.staticFunction()
  // C++ member functions have class name appended:
  //    className.memberFunction()
  char *fjalar_name; // (This is initialized only once when the
                     //  FunctionEntry entry is created from the
                     //  corresponding dwarf_entry in
                     //  initializeFunctionTable())

  // All instructions within the function are between startPC and
  // endPC, inclusive
  Addr startPC;
  Addr endPC;

  // True if globally visible, False if file-static scope
  Bool isExternal;

  VarList formalParameters;        // List of formal parameter variables

  VarList localArrayAndStructVars; // Local struct and static array
                                   // variables (useful for finding
                                   // out how many elements a pointer
                                   // to the stack refers to)

  VarList returnValue;    // Variable holding the return value (should
                          // contain at most 1 element)

  TypeEntry* parentClass; // only non-null if this is a C++ member
                          // function; points to the class to which
                          // this function belongs

  VisibilityType visibility; // Only relevant for C++ member functions

  // GNU Binary tree of variables (accessed using tsearch(), tfind(),
  // etc...) to trace within this function.  This is only non-null
  // when Fjalar is run with the --var-list-file command-line option.
  // This is initialized in initializeFunctionTable().
  char* trace_vars_tree;
  // Has trace_vars_tree been initialized?
  Bool trace_vars_tree_already_initialized;

} FunctionEntry;


// Returns a FunctionEntry* given its starting address (startPC)
// [Fast hashtable lookup]
__inline__ FunctionEntry* getFunctionEntryFromStartAddr(Addr startPC);

// Returns a FunctionEntry* given its fjalar_name
// [Slow linear search lookup]
FunctionEntry* getFunctionEntryFromFjalarName(char* fjalar_name);

// Returns a FunctionEntry* given an address within the range of
// [startPC, endPC], inclusive
// [Slow linear search lookup]
FunctionEntry* getFunctionEntryFromAddr(Addr addr);


// Iterator for FunctionEntry entries:
/*
Programming idiom for iterating over all functions used in the target
program (including C++ member functions):

  FuncIterator* funcIt = newFuncIterator();

  while (hasNextFunc(funcIt)) {
    FunctionEntry* cur_func = nextFunc(funcIt);
    ... work with cur_func ...
  }

  deleteFuncIterator(funcIt);
*/
typedef struct {
  struct geniterator* it;
} FuncIterator;

FuncIterator* newFuncIterator(void);
Bool hasNextFunc(FuncIterator* funcIt);
FunctionEntry* nextFunc(FuncIterator* funcIt);
void deleteFuncIterator(FuncIterator* funcIt);


/*********************************************************************
These data structures and functions provide mechanisms for runtime
traversals within data structures and arrays
**********************************************************************/

// Entries for tracking the runtime state of functions at entrances
// and exits (used mainly by FunctionExecutionStateStack in
// fjalar_main.c).  This class CANNOT BE SUBCLASSED because it is
// placed inline into FunctionExecutionStateStack.
typedef struct {
  // The function whose runtime state we are currently tracking
  FunctionEntry* func;

  Addr EBP;       // %ebp as calculated from %esp at function entrance

  Addr lowestESP; // The LOWEST value of %esp that has ever been
                  // encountered while we are in this function.  The
                  // problem is that Fjalar's function exit handling
                  // code runs AFTER the function increments ESP so
                  // that everything in the current function's stack
                  // frame is marked invalid by Memcheck.  Thus, we
                  // need this value to see how deep a function has
                  // penetrated into the stack, so that we can know
                  // what values are valid when this function exits.

  // Return values at function exit

  int EAX;     // %EAX
  int EDX;     // %EDX
  double FPU;  // FPU %st(0)

  // This is a copy of the portion of the function's stack frame that
  // is above EBP.  It holds function formal parameter values that
  // were passed into this function at entrance time.  We reference
  // this virtualStack at function exit in order to visit the SAME
  // formal parameter values upon exit as upon entrance.  We need this
  // because the compiler is free to modify the formal parameter
  // values that are on the real stack when executing the function.
  // Using the virtualStack means that local changes to formal
  // parameters are not visible at function exit (but changes made via
  // params passed by pointers are visible).  This is okay and
  // justified because local changes should be invisible to the
  // calling function anyways.
  char* virtualStack;
  int virtualStackByteSize; // Number of 1-byte entries in virtualStack

} FunctionExecutionState;


// Enum for storing the state of a traversal process:
typedef enum _VariableOrigin{
  // A variable that was derived either from dereferencing a pointer
  // or traversing inside of a data structure
  DERIVED_VAR,
  // A derived variable as a result of flattening an array
  DERIVED_FLATTENED_ARRAY_VAR,

  GLOBAL_VAR,
  FUNCTION_FORMAL_PARAM,

  // Only relevant for function exits
  FUNCTION_RETURN_VAR
} VariableOrigin;


// These result values control the actions of the data structure
// traversal mechanism.  It is the type of the value that the tool's
// traversal callback function must return.
typedef enum _TraversalResult{
  INVALID_RESULT = 0, // Should never happen
  // Return this when you don't really care about pointer dereferences
  // at all; e.g., you might just be interested in the names of the
  // variables and not their actual values (this is not the same as
  // DO_NOT_DEREF_MORE_POINTERS!)
  DISREGARD_PTR_DEREFS,

  // Return this when you don't want to derive further values by
  // dereferencing pointers.  All values of variables derived from the
  // current variable will simply be null.  However, we will still
  // continue to derive variables by traversing inside of data
  // structures and arrays:
  DO_NOT_DEREF_MORE_POINTERS,
  // Return this when you want to attempt to derive more values by
  // dereferencing pointers after visiting the current variable:
  DEREF_MORE_POINTERS,

  // Stop the entire traversal process after the current variable and
  // do not derive anything further:
  STOP_TRAVERSAL

} TraversalResult;


// Enum for determining whether a pointer-type disambiguation
// (.disambig) option is in effect:
// See the Fjalar Programmer's Manual
// (documentation/fjalar-www/fjalar_manual.htm) for detailed
// instructions on pointer-type disambiguation.
typedef enum _DisambigOverride {
  OVERRIDE_NONE,
  OVERRIDE_CHAR_AS_STRING, // 'C' for base "char" and "unsigned char" types
  OVERRIDE_STRING_AS_ONE_CHAR_STRING, // 'C' for pointer to "char" and "unsigned char"
  OVERRIDE_STRING_AS_INT_ARRAY, // 'A' for pointer to "char" and "unsigned char"
  OVERRIDE_STRING_AS_ONE_INT,   // 'P' for pointer to "char" and "unsigned char"
  OVERRIDE_ARRAY_AS_POINTER     // 'P' for pointer to anything
} DisambigOverride;


// This variable keeps a running count of how many actual & derived
// variables have been visited.  It increments every time a call to
// visitSingleVar() or visitSequence() is made.  It is up to the
// caller to reset this properly at the beginning of every sequence of
// traversals!  (This interface is kind of ugly at the moment, so
// please see code samples from tools for usage examples.  I will
// attempt to improve it in the future.)
int g_variableIndex;


/*
The traversal functions below all take a performAction function
pointer to indicate a callback function that should be called for each
variable visited.  Here is the prototype of such a function:

TraversalResult performAction(VariableEntry* var,
                              char* varName,
                              VariableOrigin varOrigin,
                              UInt numDereferences,
                              UInt layersBeforeBase,
                              Bool overrideIsInit,
                              DisambigOverride disambigOverride,
                              Bool isSequence,
                              void* pValue,
                              void** pValueArray,
                              UInt numElts,
                              FunctionEntry* varFuncInfo,
                              Bool isEnter);

Each time that the traversal visits a variable, it calls the given
function and populates its parameters with the following information:

VariableEntry* var - The current variable
char* varName - The variable name with all prefixed appended
                e.g., "foo->bar[].baz"
VariableOrigin varOrigin - The origin of this variable
UInt numDereferences - The number of pointer dereferences of 'var'
                       at this point of the traversal
UInt layersBeforeBase - The number of pointer levels left before reaching
                        the base variable indicated by 'var'
Bool overrideIsInit - 1 if the pointer referring to this variable's value
                      should automatically be deemed as valid, and 0
                      otherwise (most of the time, it's 0)
DisambigOverride disambigOverride - See .disambig option
Bool isSequence - Are we traversing a single value or a sequence of values?
void* pValue - A pointer to the value of the current variable
               (Only valid if isSequence is 0)
void** pValueArray - An array of pointers to the values of the current variable
                     sequence (Only valid if isSequence is 1)
UInt numElts - The number of elements in pValueArray
               (Only valid if isSequence is 1)
FunctionEntry* varFuncInfo - The function that is active during the current
                             traversal (may be null sometimes)
Bool isEnter - 1 if this is a function entrance, 0 if exit

*/


// Visits an entire group of variables, depending on the value of varOrigin:
// If varOrigin == GLOBAL_VAR, then visit all global variables
// If varOrigin == FUNCTION_FORMAL_PARAM, then visit all formal parameters
// of the function denoted by funcPtr
// If varOrigin == FUNCTION_RETURN_VAR, then visit the return value variable
// of the function denoted by funcPtr (but this does not work for trying to
// actually grab the return value; it only grabs the names.  Instead, use
// visitReturnValue() when you need the names and values)
void visitVariableGroup(VariableOrigin varOrigin,
                        FunctionEntry* funcPtr, // 0 for unspecified function
                        Bool isEnter,           // 1 for function entrance,
                                                // 0 for exit
                        // Address of the base of the currently
                        // executing function's stack frame: (Only used for
                        // varOrigin == FUNCTION_FORMAL_PARAM)
                        char* stackBaseAddr,
                        // This function performs an action for each
                        // variable visited:
                        TraversalResult (*performAction)(VariableEntry*,
                                                         char*,
                                                         VariableOrigin,
                                                         UInt,
                                                         UInt,
                                                         Bool,
                                                         DisambigOverride,
                                                         Bool,
                                                         void*,
                                                         void**,
                                                         UInt,
                                                         FunctionEntry*,
                                                         Bool));

// Grabs the appropriate return value of the function denoted by the
// execution state 'e' from Valgrind simulated registers and visits
// the derived variables to perform some action.  This differs from
// calling visitVariableGroup() with the FUNCTION_RETURN_VAR parameter
// because it actually grabs the appropriate value from the simulated
// registers.  This should only be called at function exit time.
void visitReturnValue(FunctionExecutionState* e,
                      // This function performs an action for each
                      // variable visited:
                      TraversalResult (*performAction)(VariableEntry*,
                                                       char*,
                                                       VariableOrigin,
                                                       UInt,
                                                       UInt,
                                                       Bool,
                                                       DisambigOverride,
                                                       Bool,
                                                       void*,
                                                       void**,
                                                       UInt,
                                                       FunctionEntry*,
                                                       Bool));

// Visits one variable (denoted by 'var') and all variables that are
// derived from it by traversing inside of data structures and arrays.
void visitVariable(VariableEntry* var,
                   // Pointer to the location of the variable's
                   // current value in memory:
                   void* pValue,
                   // We only use overrideIsInit when we pass in
                   // things (e.g. return values) that cannot be
                   // checked by the Memcheck A and V bits. Never have
                   // overrideIsInit when you derive variables (make
                   // recursive calls) because their addresses are
                   // different from the original's
                   Bool overrideIsInit,
                   // This should almost always be 0, but whenever you
                   // want finer control over struct dereferences, you
                   // can override this with a number representing the
                   // number of structs you have dereferenced so far
                   // to get here (Useful for the 'this' parameter of
                   // member functions):
                   UInt numStructsDereferenced,
                   // This function performs an action for each
                   // variable visited:
                   TraversalResult (*performAction)(VariableEntry*,
                                                    char*,
                                                    VariableOrigin,
                                                    UInt,
                                                    UInt,
                                                    Bool,
                                                    DisambigOverride,
                                                    Bool,
                                                    void*,
                                                    void**,
                                                    UInt,
                                                    FunctionEntry*,
                                                    Bool),
                   VariableOrigin varOrigin,
                   FunctionEntry* varFuncInfo,
                   Bool isEnter);

// Visits all member variables of the class and all of its
// superclasses (and variables derived from those members) without
// regard to actually grabbing their values.  This is useful for
// printing out names and performing other non-value-dependent
// operations.
void visitClassMembersNoValues(TypeEntry* class,
                               TraversalResult (*performAction)(VariableEntry*,
                                                                char*,
                                                                VariableOrigin,
                                                                UInt,
                                                                UInt,
                                                                Bool,
                                                                DisambigOverride,
                                                                Bool,
                                                                void*,
                                                                void**,
                                                                UInt,
                                                                FunctionEntry*,
                                                                Bool));

// Takes a TypeEntry* and (optionally, a pointer to its current value
// in memory), and traverses through all of the members of the
// specified class.  This should also traverse inside of the class's
// superclasses and visit variables within them (as well as derived
// variables):
void visitClassMemberVariables(TypeEntry* class,
                               // Pointer to the current value of an
                               // instance of 'class' (only valid if
                               // isSequence is 0)
                               void* pValue,
                               Bool isSequence,
                               // An array of pointers to instances of
                               // 'class' (only valid if isSequence is
                               // non-zero):
                               void** pValueArray,
                               UInt numElts, // Size of pValueArray
                               // This function performs an action for
                               // each variable visited:
                               TraversalResult (*performAction)(VariableEntry*,
                                                                char*,
                                                                VariableOrigin,
                                                                UInt,
                                                                UInt,
                                                                Bool,
                                                                DisambigOverride,
                                                                Bool,
                                                                void*,
                                                                void**,
                                                                UInt,
                                                                FunctionEntry*,
                                                                Bool),
                               VariableOrigin varOrigin,
                               // A (possibly null) GNU binary tree of
                               // variables to trace:
                               char* trace_vars_tree,
                               // The number of structs we have
                               // dereferenced for a particular call
                               // of visitVariable(); Starts at 0 and
                               // increments every time we hit a
                               // variable which is a base struct type
                               // Range: [0, MAX_VISIT_NESTING_DEPTH]
                               UInt numStructsDereferenced,
                               FunctionEntry* varFuncInfo,
                               Bool isEnter,
                               TraversalResult tResult);


// Misc. symbols that are useful for printing variable names during
// the traversal process:
char* DEREFERENCE; // "[]"
char* ZEROTH_ELT;  // "[0]"
char* DOT;         // "."
char* ARROW;       // "->"
char* STAR;        // "*"


/*********************************************************************
These functions provide information extracted from an executable's
symbol table:
**********************************************************************/

// Returns True iff the address is within a global area as specified
// by the executable's symbol table (it lies within the .data, .bss,
// or .rodata sections):
Bool addressIsGlobal(Addr addr);

// Returns the start address of a function with a particular name.
// Accepts regular name for C and mangled name for C++.
// [Fast hashtable lookup in FunctionSymbolTable]

Addr getFunctionStartAddr(char* name);
// Returns the regular name for C and mangled name for C++, given a
// function's start address.
// [Fast hashtable lookup in ReverseFunctionSymbolTable]

char* getFunctionName(Addr startAddr);
// Returns the address of a global variable, given its regular name
// for C and its mangled name for C++.
// [Fast hashtable lookup in VariableSymbolTable]
Addr getGlobalVarAddr(char* name);


/*********************************************************************
These functions provide Fjalar's run-time memory initialization and
array size calculation functionality:
**********************************************************************/

// Takes a variable (var) and its current address in memory
// (varLocation) and returns the upper bound of the array starting at
// varLocation (returns 0 if it only points to 1 element).  This
// function tries to look for arrays in the global region, on the
// stack, and on the heap.
int returnArrayUpperBoundFromPtr(VariableEntry* var, Addr varLocation);

// Returns the number of bytes between successive elements of the
// variable var.  This is useful for figuring how how much separation
// there is between elements in an array.  It also indicates how much
// room this particular variable takes up in memory.  It returns
// var->varType->byteSize if ptrLevels == 0, sizeof(void*) otherwise
int getBytesBetweenElts(VariableEntry* var);

// Returns True if all numBytes bytes starting at addressInQuestion
// have been allocated, False otherwise
Bool addressIsAllocated(Addr addressInQuestion, UInt numBytes);

// Returns True if all numBytes bytes starting at addressInQuestion
// have been initialized by the program, False otherwise (indicates a
// possible garbage value)
Bool addressIsInitialized(Addr addressInQuestion, UInt numBytes);


/*********************************************************************
These global variables are set by command-line options.  Please see
the 'Fjalar command-line options section in the Fjalar Programmer's
Manual (documentation/fjalar-www/fjalar_manual.htm) for details.

**********************************************************************/

// Boolean flags
Bool fjalar_debug;                         // --fjalar-debug
Bool fjalar_with_gdb;                      // --with-gdb
Bool fjalar_ignore_globals;                // --ignore-globals
Bool fjalar_ignore_static_vars;            // --ignore-static-vars
Bool fjalar_all_static_vars;               // --all-static-vars
Bool fjalar_default_disambig;              // --disambig
Bool fjalar_smart_disambig;                // --smart-disambig
Bool fjalar_output_struct_vars;            // --output-struct-vars
Bool fjalar_flatten_arrays;                // --flatten-arrays
Bool fjalar_func_disambig_ptrs;            // --func-disambig-ptrs
Bool fjalar_disambig_ptrs;                 // --disambig-ptrs
int  fjalar_array_length_limit;            // --array-length-limit

UInt MAX_VISIT_STRUCT_DEPTH;               // --struct-depth
UInt MAX_VISIT_NESTING_DEPTH;              // --nesting-depth

// The following are used both as strings and as boolean flags - They
// are initialized to 0 so if they are never filled with values by the
// respective command-line options, then they can be treated as False
char* fjalar_dump_prog_pt_names_filename;  // --dump-ppt-file
char* fjalar_dump_var_names_filename;      // --dump-var-file
char* fjalar_trace_prog_pts_filename;      // --ppt-list-file
char* fjalar_trace_vars_filename;          // --var-list-file
char* fjalar_disambig_filename;            // --disambig-file
char* fjalar_xml_output_filename;          // --xml-output-file


/*********************************************************************
Misc.
**********************************************************************/

// The filename of the target executable:
char* executable_filename;

// returns ID1 == ID2 - needed for GenericHashtable
int equivalentIDs(int ID1, int ID2);

// returns whether 2 strings are equal - needed for GenericHashtable
int equivalentStrings(char* str1, char* str2);
// hashes a string (in very primitive way ... could improve if needed)
unsigned int hashString(char* str);

// Returns True if the FunctionEntry denoted by cur_entry has been
// specified by the user in a ppt-list-file
// Only relevant if the --ppt-list-file option is used (and
// fjalar_trace_prog_pts_filename is non-null)
Bool prog_pts_tree_entry_found(FunctionEntry* cur_entry);


#endif
