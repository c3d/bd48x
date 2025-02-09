/*
 * Copyright (c) 2014, Claudio Lapilli and the newRPL Team
 * All rights reserved.
 * This file is released under the 3-clause BSD license.
 * See the file LICENSE.txt that shipped with this distribution.
 */

#include "cmdcodes.h"
#include "hal_api.h"
#include "libraries.h"
#include "newrpl.h"
#include "sysvars.h"

#define EXIT_LOOP -10000

void rplCompileAppend(WORD word)
{
    *CompileEnd = word;
    CompileEnd++;
    // ADJUST MEMORY AS NEEDED
    if(CompileEnd >= TempObSize) {
        // ENLARGE TEMPOB AS NEEDED
        growTempOb(((WORD) (CompileEnd - TempOb)) + TEMPOBSLACK);
    }

}

// INSERT A WORD IN THE MIDDLE OF THE COMPILED STREAM
void rplCompileInsert(WORDPTR position, WORD word)
{
    memmovew(position + 1, position, CompileEnd - position);
    *position = word;
    CompileEnd++;
    // ADJUST MEMORY AS NEEDED
    if(CompileEnd >= TempObSize) {
        // ENLARGE TEMPOB AS NEEDED
        growTempOb(((WORD) (CompileEnd - TempOb)) + TEMPOBSLACK);
    }

}

// REMOVE WORDS THAT WERE ALLOCATED DURING COMPILATION
void rplCompileRemoveWords(BINT nwords)
{
    CompileEnd -= nwords;
}

// ALLOCATE NWORDS FOR FUTURE COMPILATION BUT DOESN'T
// ACTUALLY APPEND ANYTHING.
// RETURNS A POINTER TO THE AREA OF MEMORY WHERE THE
// CALLER WILL HAVE TO STORE THE WORDS

WORDPTR rplCompileAppendWords(BINT nwords)
{
    CompileEnd += nwords;
    // ADJUST MEMORY AS NEEDED
    if(CompileEnd >= TempObSize) {
        // ENLARGE TEMPOB AS NEEDED
        growTempOb(((WORD) (CompileEnd - TempOb)) + TEMPOBSLACK);
    }

    return CompileEnd - nwords;
}

// REVERSE-SKIP AN OBJECT, FROM A POINTER TO AFTER THE OBJECT TO SKIP
// NO ARGUMENT CHECKS, DO NOT CALL UNLESS THERE'S A VALID OBJECT LIST
WORDPTR rplReverseSkipOb(WORDPTR list_start, WORDPTR after_object)
{
    WORDPTR next;
    while((next = rplSkipOb(list_start)) < after_object)
        list_start = next;
    if(next > after_object)
        return NULL;
    return list_start;
}

// ROTATES A FUNCTION ARGUMENT LIST SO THE FIRST ARGUMENT BECOMES THE LAST,
// SECOND ARGUMENT BECOMES FIRST, ETC.
// ONLY USED BY THE SPECIAL CASE FUNCEVAL TO MOVE THE NAME OF THE FUNCTION LAST
BINT rplRotArgs(BINT nargs)
{

    WORDPTR ptr = CompileEnd, symbstart = *(ValidateTop - 1) + 1;

    //FIND THE START OF THE 'N' ARGUMENTS
    for(; (nargs > 0) && ptr; --nargs) {
        ptr = rplReverseSkipOb(symbstart, ptr);
    }

    if(nargs || (!ptr)) {
        rplError(ERR_BADARGCOUNT);
        return 0;       // TOO FEW ARGUMENTS!
    }

    BINT firstsize = rplObjSize(ptr);
    // ADJUST MEMORY AS NEEDED
    if(CompileEnd + firstsize >= TempObSize) {
        // ENLARGE TEMPOB AS NEEDED
        growTempOb(((WORD) (CompileEnd + firstsize - TempOb)) + TEMPOBSLACK);
        if(Exceptions)
            return 0;   // NOT ENOUGH MEMORY
    }

    // COPY THE FIRST ARGUMENT LAST
    memmovew(CompileEnd, ptr, firstsize);
    // MOVE THE ENTIRE LIST DOWN
    memmovew(ptr, ptr + firstsize, CompileEnd - ptr);

    return 1;

}

// APPLIES THE SYMBOLIC OPERATOR TO THE OUTPUT QUEUE
// ONLY CALLED BY THE COMPILER
// ON ENTRY: CompileEnd = top of the output stream (pointing after the last object)
//           *(ValidateTop-1) = START OF THE SYMBOLIC OBJECT
static BINT rplInfixApply(WORD opcode, BINT nargs)
{
    // FORMAT OF SYMBOLIC OBJECT:
    // DOSYMB PROLOG
    // OPCODE
    // ARG1 OBJECT (ARGUMENT LIST)
    // ARG2 OBJECT
    // ...
    // ARGn OBJECT
    // END OF SYMBOLIC OBJECT

    WORDPTR ptr = CompileEnd, symbstart = *(ValidateTop - 1) + 1;

    //FIND THE START OF THE 'N' ARGUMENTS
    for(; (nargs > 0) && ptr; --nargs) {
        ptr = rplReverseSkipOb(symbstart, ptr);
    }

    if(nargs || (!ptr)) {
        rplError(ERR_BADARGCOUNT);
        return 0;       // TOO FEW ARGUMENTS!
    }

    CompileEnd += 2;
    // ADJUST MEMORY AS NEEDED
    if(CompileEnd >= TempObSize) {
        // ENLARGE TEMPOB AS NEEDED
        growTempOb(((WORD) (CompileEnd - TempOb)) + TEMPOBSLACK);
        if(Exceptions)
            return 0;   // NOT ENOUGH MEMORY
    }

    // MOVE THE ENTIRE LIST TO MAKE ROOM FOR THE HEADER
    memmovew(ptr + 2, ptr, CompileEnd - ptr - 2);

    ptr[0] = MKPROLOG(DOSYMB, CompileEnd - ptr - 1);
    ptr[1] = opcode;

    return 1;
}

// COMPILE A STRING AND RETURN A POINTER TO THE FIRST COMMAND/OBJECT
// IF addwrapper IS NON-ZERO, IT WILL WRAP THE CODE WITH :: ... ; EXITRPL
// (USED BY THE COMMAND LINE FOR IMMEDIATE COMMANDS)

WORDPTR rplCompile(BYTEPTR string, BINT length, BINT addwrapper)
{
    // COMPILATION USES TEMPOB
    CompileEnd = TempObEnd;

    // START COMPILATION LOOP
    BINT force_libnum, splittoken, validate = 0, infixmode;
    BINT probe_libnum = 0, probe_tokeninfo = 0, previous_tokeninfo;
    LIBHANDLER handler, ValidateHandler;
    BINT libcnt, libnum;
    WORDPTR InfixOpTop = 0;

    LAMTopSaved = LAMTop;       // SAVE LAM ENVIRONMENT

    ValidateTop = ValidateBottom = RSTop;
    ValidateHandler = NULL;

    force_libnum = -1;
    splittoken = 0;
    infixmode = 0;
    previous_tokeninfo = 0;

    NextTokenStart = (WORDPTR) string;
    CompileStringEnd = (WORDPTR) (string + length);

    if(addwrapper) {
        rplCompileAppend(MKPROLOG(DOCOL, 0));
        if(RStkSize <= (ValidateTop - RStk))
            growRStk(ValidateTop - RStk + RSTKSLACK);
        if(Exceptions) {
            LAMTop = LAMTopSaved;
            return 0;
        }
        *ValidateTop++ = CompileEnd - 1;        // POINTER TO THE WORD OF THE COMPOSITE, NEEDED TO STORE THE SIZE
    }

    // FIND THE START OF NEXT TOKEN
    while((NextTokenStart < CompileStringEnd)
            && ((*((BYTEPTR) NextTokenStart) == ' ')
                || (*((BYTEPTR) NextTokenStart) == '\t')
                || (*((BYTEPTR) NextTokenStart) == '\n')
                || (*((BYTEPTR) NextTokenStart) == '\r')))
        NextTokenStart = (WORDPTR) (((BYTEPTR) NextTokenStart) + 1);

    do {
        if(!splittoken) {
            TokenStart = NextTokenStart;
            BlankStart = TokenStart;
            while((BlankStart < CompileStringEnd)
                    && (*((char *)BlankStart) != ' ')
                    && (*((char *)BlankStart) != '\t')
                    && (*((char *)BlankStart) != '\n')
                    && (*((char *)BlankStart) != '\r'))
                BlankStart = (WORDPTR) (((char *)BlankStart) + 1);
            NextTokenStart = BlankStart;
            while((NextTokenStart < CompileStringEnd)
                    && ((*((char *)NextTokenStart) == ' ')
                        || (*((char *)NextTokenStart) == '\t')
                        || (*((char *)NextTokenStart) == '\n')
                        || (*((char *)NextTokenStart) == '\r')))
                NextTokenStart = (WORDPTR) (((char *)NextTokenStart) + 1);
        }
        else
            splittoken = 0;

        TokenLen = (BINT) utf8nlen((char *)TokenStart, (char *)BlankStart);
        BlankLen = (BINT) ((BYTEPTR) NextTokenStart - (BYTEPTR) BlankStart);
        CurrentConstruct = (BINT) ((ValidateTop > ValidateBottom) ? **(ValidateTop - 1) : 0);   // CARRIES THE WORD OF THE CURRENT CONSTRUCT/COMPOSITE
        ValidateHandler = rplGetLibHandler(LIBNUM(CurrentConstruct));
        LastCompiledObject = CompileEnd;
        if(force_libnum < 0) {
            // SCAN THROUGH ALL THE LIBRARIES, FROM HIGH TO LOW, TO SEE WHICH ONE WANTS THE TOKEN
            libcnt = MAXLIBNUMBER;
        }
        else
            libcnt = 0; // EXECUTE THE LOOP ONLY ONCE

        if(TokenStart >= CompileStringEnd)
            break;

        if(infixmode) {
            probe_libnum = -1;
            probe_tokeninfo = 0;
        }

        while(libcnt >= 0) {
            if(force_libnum < 0) {
                libnum = libcnt;
                handler = rplGetLibHandler(libcnt);
            }
            else {
                libnum = force_libnum;
                handler = rplGetLibHandler(force_libnum);
            }
            libcnt = rplGetNextLib(libcnt);

            if(!handler)
                continue;

            if(infixmode == 1) {
                CurOpcode = MKOPCODE(libnum, OPCODE_PROBETOKEN);
            }
            else {
                if(force_libnum != -1)
                    CurOpcode = MKOPCODE(libnum, OPCODE_COMPILECONT);
                else
                    CurOpcode = MKOPCODE(libnum, OPCODE_COMPILE);
            }
            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
            WORDPTR *tmpRSTop = RSTop;
            if(infixmode)
                RSTop = (WORDPTR *) InfixOpTop;
            else
                RSTop = (WORDPTR *) ValidateTop;
            (*handler) ();
            RSTop = tmpRSTop;

            if(RetNum >= OK_TOKENINFO) {
                // PROCESS THE INFORMATION ABOUT THE TOKEN
                if(TI_LENGTH(RetNum) > TI_LENGTH(probe_tokeninfo)) {
                    probe_libnum = libnum;
                    probe_tokeninfo = RetNum;
                }
            }
            else
                switch (RetNum) {
                case OK_CONTINUE:
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    validate = 1;
                    break;
                case OK_CONTINUE_NOVALIDATE:
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    break;

                case OK_STARTCONSTRUCT:
                    if(RStkSize <= (ValidateTop - RStk))
                        growRStk(ValidateTop - RStk + RSTKSLACK);
                    if(Exceptions) {
                        LAMTop = LAMTopSaved;
                        return 0;
                    }
                    *ValidateTop++ = CompileEnd - 1;    // POINTER TO THE WORD OF THE COMPOSITE, NEEDED TO STORE THE SIZE
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    if(ISPROLOG((BINT) ** (ValidateTop - 1)))
                        validate = 0;
                    else
                        validate = 1;
                    break;
                case OK_CHANGECONSTRUCT:
                    *(ValidateTop - 1) = CompileEnd - 1;        // POINTER TO THE WORD OF THE COMPOSITE, NEEDED TO STORE THE SIZE
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    break;
                case OK_INCARGCOUNT:
                    **(ValidateTop - 1) = **(ValidateTop - 1) + 1;      // POINTER TO THE WORD OF THE COMPOSITE, TEMPORARILY STORE THE NUMBER OF ARGUMENTS AS THE SIZE
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    break;

                case OK_ENDCONSTRUCT:
                    --ValidateTop;
                    if(ValidateTop < ValidateBottom) {
                        rplError(ERR_ENDWITHOUTSTART);
                        LAMTop = LAMTopSaved;
                        return 0;
                    }
                    if(ISPROLOG((BINT) ** ValidateTop)) {
                        // STORE THE SIZE OF THE COMPOSITE IN THE WORD
                        **ValidateTop =
                                (**ValidateTop ^ OBJSIZE(**ValidateTop)) |
                                (((WORD) ((PTR2NUMBER) CompileEnd -
                                        (PTR2NUMBER) * ValidateTop) >> 2) - 1);
                        // PREPARE THE NEWLY CREATED OBJECT FOR VALIDATION BY ITS PARENT
                        CurrentConstruct = (BINT) ((ValidateTop > ValidateBottom) ? **(ValidateTop - 1) : 0);   // CARRIES THE WORD OF THE CURRENT CONSTRUCT/COMPOSITE
                        ValidateHandler =
                                rplGetLibHandler(LIBNUM(CurrentConstruct));
                        LastCompiledObject = *ValidateTop;
                        validate = 1;
                    }

                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    break;

                case OK_NEEDMORE:
                    force_libnum = libnum;
                    libcnt = EXIT_LOOP;
                    break;
                case OK_NEEDMORESTARTCONST:
                    if(RStkSize <= (ValidateTop - RStk))
                        growRStk(ValidateTop - RStk + RSTKSLACK);
                    if(Exceptions) {
                        LAMTop = LAMTopSaved;
                        return 0;
                    }
                    *ValidateTop++ = CompileEnd - 1;    // POINTER TO THE WORD OF THE COMPOSITE, NEEDED TO STORE THE SIZE
                    force_libnum = libnum;
                    libcnt = EXIT_LOOP;
                    validate = 0;
                    break;
                case OK_SPLITTOKEN:
                    splittoken = 1;
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    validate = 1;
                    break;
                case OK_STARTCONSTRUCT_SPLITTOKEN:
                    if(RStkSize <= (ValidateTop - RStk))
                        growRStk(ValidateTop - RStk + RSTKSLACK);
                    if(Exceptions) {
                        LAMTop = LAMTopSaved;
                        return 0;
                    }
                    *ValidateTop++ = CompileEnd - 1;    // POINTER TO THE WORD OF THE COMPOSITE, NEEDED TO STORE THE SIZE
                    splittoken = 1;
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    validate = 0;
                    break;
                case OK_STARTCONSTRUCT_INFIX:
                    if(RStkSize <= (ValidateTop - RStk))
                        growRStk(ValidateTop - RStk + RSTKSLACK);
                    if(Exceptions) {
                        LAMTop = LAMTopSaved;
                        return 0;
                    }
                    *ValidateTop++ = CompileEnd - 1;    // POINTER TO THE WORD OF THE COMPOSITE, NEEDED TO STORE THE SIZE
                    infixmode = 1;
                    previous_tokeninfo = 0;
                    InfixOpTop = (WORDPTR) ValidateTop;
                    probe_libnum = -1;
                    probe_tokeninfo = 0;
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    validate = 1;
                    break;
                case OK_ENDCONSTRUCT_INFIX_SPLITTOKEN:
                    splittoken = 1;
                    // LET IT FALL THROUGH THE REGULAR INFIX
                case OK_ENDCONSTRUCT_INFIX:

                    if(infixmode) {
                        // FLUSH OUT ANY OPERATORS IN THE STACK
                        while(InfixOpTop > (WORDPTR) ValidateTop) {
                            InfixOpTop -= 2;
                            if(TI_TYPE(InfixOpTop[1]) == TITYPE_OPENBRACKET) {
                                // MISSING BRACKET SOMEWHERE!
                                rplError(ERR_MISSINGBRACKET);
                                LAMTop = LAMTopSaved;
                                return 0;
                            }
                            if(!rplInfixApply(InfixOpTop[0],
                                        TI_NARGS(InfixOpTop[1]))) {
                                // ERROR IS SET BY rplInfixApply
                                LAMTop = LAMTopSaved;
                                return 0;
                            }
                        }
                        // ALL PENDING OPERATORS WERE APPLIED, NOW CHECK THAT THERE IS ONE AND ONLY ONE RESULT

                        if(rplSkipOb(*(ValidateTop - 1) + 1) != CompileEnd) {
                            rplError(ERR_SYNTAXERROR);
                            LAMTop = LAMTopSaved;
                            return 0;
                        }

                        infixmode = 0;
                    }
                    --ValidateTop;
                    if(ValidateTop < ValidateBottom) {
                        rplError(ERR_ENDWITHOUTSTART);
                        LAMTop = LAMTopSaved;
                        return 0;
                    }
                    if(ISPROLOG((BINT) ** ValidateTop)) {
                        **ValidateTop = (**ValidateTop ^ OBJSIZE(**ValidateTop)) | (((WORD) ((PTR2NUMBER) CompileEnd - (PTR2NUMBER) * ValidateTop) >> 2) - 1);  // STORE THE SIZE OF THE COMPOSITE IN THE WORD
                        // PREPARE THE NEWLY CREATED OBJECT FOR VALIDATION BY ITS PARENT
                        CurrentConstruct = (BINT) ((ValidateTop > ValidateBottom) ? **(ValidateTop - 1) : 0);   // CARRIES THE WORD OF THE CURRENT CONSTRUCT/COMPOSITE
                        ValidateHandler =
                                rplGetLibHandler(LIBNUM(CurrentConstruct));
                        LastCompiledObject = *ValidateTop;
                        validate = 1;
                    }

                    // FUTURE OPTIMIZATION:
                    // THE COMPILER GENERATES A DOSYMB WRAPPER FOR ALL ITEMS
                    // THAT COULD BE REMOVED. DURING COMPILE IT IS REQUIRED IN ORDER TO HAVE
                    // A CURRENT CONSTRUCT POINTING TO A SYMBOLIC, SO THAT LIBRARIES CAN DETERMINE
                    // THAT A COMMAND IS BEING COMPILED WITHIN A SYMBOLIC.
                    // BUT REMOVING IT IMPLIES MOVING THE ENTIRE OBJECT 1 WORD DOWN IN MEMORY
                    // SO IT'S NOT DONE FOR SPEED REASONS

                    libcnt = EXIT_LOOP;
                    force_libnum = -1;

                    break;

                case ERR_NOTMINE:
                    break;
                case ERR_NOTMINE_SPLITTOKEN:
                    splittoken = 1;
                    libcnt = EXIT_LOOP;
                    force_libnum = -1;
                    break;

                case ERR_INVALID:
                case ERR_SYNTAX:
                    // RAISE THE ERROR ONLY IF THE LIBRARY DIDN'T DO IT
                    if(!Exceptions)
                        rplError(ERR_SYNTAXERROR);
                    LAMTop = LAMTopSaved;
                    return 0;

                }
        }

        if(libcnt > EXIT_LOOP) {
            if(infixmode) {
                // FINISHED PROBING FOR TOKENS

                if(probe_libnum < 0) {
                    rplError(ERR_INVALIDTOKEN);
                }
                else {

                    if(TI_TYPE(probe_tokeninfo) == TITYPE_NOTALLOWED) {
                        // THIS TOKEN IS NOT ALLOWED IN SYMBOLICS
                        rplError(ERR_NOTALLOWEDINSYMBOLICS);
                        LAMTop = LAMTopSaved;
                        return 0;
                    }

                    // GOT THE NEXT TOKEN IN THE STREAM
                    // COMPILE THE TOKEN

                    handler = rplGetLibHandler(probe_libnum);
                    CurOpcode = MKOPCODE(probe_libnum, OPCODE_COMPILE);

                    NextTokenStart = BlankStart =
                            (WORDPTR) utf8nskip((char *)TokenStart,
                            (char *)BlankStart, TI_LENGTH(probe_tokeninfo));
                    while((NextTokenStart < CompileStringEnd)
                            && ((*((char *)NextTokenStart) == ' ')
                                || (*((char *)NextTokenStart) == '\t')
                                || (*((char *)NextTokenStart) == '\n')
                                || (*((char *)NextTokenStart) == '\r')))
                        NextTokenStart =
                                (WORDPTR) (((char *)NextTokenStart) + 1);
                    TokenLen =
                            (BINT) utf8nlen((char *)TokenStart,
                            (char *)BlankStart);
                    BlankLen =
                            (BINT) ((BYTEPTR) NextTokenStart -
                            (BYTEPTR) BlankStart);
                    CurrentConstruct = (BINT) ((ValidateTop > ValidateBottom) ? **(ValidateTop - 1) : 0);       // CARRIES THE WORD OF THE CURRENT CONSTRUCT/COMPOSITE
                    LastCompiledObject = CompileEnd;

                    RetNum = -1;
                    if(handler) {
                        // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                        WORDPTR *tmpRSTop = RSTop;
                        RSTop = (WORDPTR *) InfixOpTop;
                        (*handler) ();
                        RSTop = tmpRSTop;

                    }

                    if(RetNum != OK_CONTINUE) {
                        // THE LIBRARY ACCEPTED THE TOKEN DURING PROBE, SO WHAT COULD POSSIBLY GO WRONG?
                        if(!Exceptions)
                            rplError(ERR_INVALIDTOKEN); // GIVE A CHANCE FOR THE LIBRARY TO SET ITS OWN ERROR CODE
                        LAMTop = LAMTopSaved;
                        return 0;
                    }

                    // HERE LastCompiledObject HAS THE NEW OBJECT/COMMAND

                    // THE LIBRARY MAY HAVE COMPILED SOME ARGUMENTS AND THEN
                    // LEAVE THE OPERATOR LAST. FOR EXAMPLE THE SUPERSCRIPT NUMBER
                    // 2 (X²) COULD BE COMPILED AS THE OBJECT 2 AND THE OPERATOR POW
                    // UNITS ARE COMPILED AS A UNIT OBJECT AND THEN THE OPERATOR
                    // UNITAPPLY (_[)

                    {
                        WORDPTR NextObject = LastCompiledObject;
                        while(rplSkipOb(NextObject) < CompileEnd)
                            NextObject = rplSkipOb(NextObject);

                        // HERE NextObject POINTS TO THE LAST OBJECT COMPILED BY THE LIBRARY
                        // LEAVE ALL PREVIOUS OBJECTS IN THE OUTPUT STREAM AS ARGUMENTS
                        LastCompiledObject = NextObject;
                    }

                    // IF IT'S AN ATOMIC OBJECT, JUST LEAVE IT THERE IN THE OUTPUT STREAM

                    // IF IT'S AN OPERATOR
                    if(TI_TYPE(probe_tokeninfo) > TITYPE_OPERATORS) {

                        // ALL OPERATORS AND COMMANDS ARE SINGLE-WORD, ONLY OBJECTS TAKE MORE SPACE
                        WORD Opcode = *LastCompiledObject;
                        // REMOVE THE OPERATOR FROM THE OUTPUT STREAM
                        CompileEnd = LastCompiledObject;

                        // SPECIAL CASE THAT CAN ONLY BE HANDLED BY THE COMPILER:
                        // AMBIGUITY BETWEEN UNARY MINUS AND SUBSTRACTION
                        if(Opcode == (CMD_OVR_SUB)) {
                            switch (TI_TYPE(previous_tokeninfo)) {
                            case 0:
                            case TITYPE_BINARYOP_LEFT:
                            case TITYPE_BINARYOP_RIGHT:
                            case TITYPE_CASBINARYOP_LEFT:
                            case TITYPE_CASBINARYOP_RIGHT:
                            case TITYPE_OPENBRACKET:
                            case TITYPE_PREFIXOP:
                            case TITYPE_COMMA:

                                // IT'S A UNARY MINUS
                                Opcode = (CMD_OVR_UMINUS);
                                probe_tokeninfo =
                                        MKTOKENINFO(1, TITYPE_PREFIXOP, 1, 4);
                                break;
                            default:
                                break;
                            }

                        }

                        // AMBIGUITY BETWEEN UNARY PLUS AND ADDITION
                        if(Opcode == (CMD_OVR_ADD)) {
                            switch (TI_TYPE(previous_tokeninfo)) {
                            case 0:
                            case TITYPE_BINARYOP_LEFT:
                            case TITYPE_BINARYOP_RIGHT:
                            case TITYPE_CASBINARYOP_LEFT:
                            case TITYPE_CASBINARYOP_RIGHT:

                            case TITYPE_OPENBRACKET:
                            case TITYPE_PREFIXOP:
                            case TITYPE_COMMA:
                                // IT'S A UNARY PLUS
                                Opcode = (CMD_OVR_UPLUS);
                                probe_tokeninfo =
                                        MKTOKENINFO(1, TITYPE_PREFIXOP, 1, 4);
                                break;
                            default:
                                break;
                            }

                        }

                        if(TI_TYPE(probe_tokeninfo) == TITYPE_OPENBRACKET) {

                            if(!previous_tokeninfo
                                    || (TI_TYPE(previous_tokeninfo) >
                                        TITYPE_OPERATORS)) {
                                // THIS IS A PARENTHESIS FOLLOWING AN OPERATOR
                                // PUSH THE NEW OPERATOR
                                if(RStkSize <=
                                        (InfixOpTop + 3 - (WORDPTR) RStk))
                                    growRStk(InfixOpTop - (WORDPTR) RStk +
                                            RSTKSLACK);
                                if(Exceptions) {
                                    LAMTop = LAMTopSaved;
                                    return 0;
                                }
                                InfixOpTop[0] = CompileEnd - TempObEnd; // SAVE POSITION TO START COUNTING ARGUMENTS
                                InfixOpTop[1] = probe_tokeninfo;
                                InfixOpTop[2] = Opcode; // SAVE OPCODE TO DISTINGUISH BRACKET TYPES
                                InfixOpTop[3] = probe_tokeninfo;
                                InfixOpTop += 4;
                            }
                            else {
                                // THIS IS EITHER MATRIX/VECTOR INDEXING OR A USER FUNCTION CALL

                                // PUSH OPERATOR FUNCEVAL FIRST
                                if(RStkSize <=
                                        (InfixOpTop + 5 - (WORDPTR) RStk))
                                    growRStk(InfixOpTop - (WORDPTR) RStk +
                                            RSTKSLACK);
                                if(Exceptions) {
                                    LAMTop = LAMTopSaved;
                                    return 0;
                                }
                                InfixOpTop[0] = (CMD_OVR_FUNCEVAL);     // SAVE POSITION TO START COUNTING ARGUMENTS
                                InfixOpTop[1] =
                                        MKTOKENINFO(TI_LENGTH
                                        (previous_tokeninfo), TITYPE_FUNCTION,
                                        0xf, 2);
                                InfixOpTop += 2;

                                // THEN THE OPENING BRACKET
                                InfixOpTop[0] = CompileEnd - TempObEnd; // SAVE POSITION TO START COUNTING ARGUMENTS
                                InfixOpTop[1] = probe_tokeninfo;
                                InfixOpTop[2] = Opcode; // SAVE OPCODE TO DISTINGUISH BRACKET TYPES
                                InfixOpTop[3] = probe_tokeninfo;
                                InfixOpTop += 4;

                            }
                        }
                        else {

                            if((TI_TYPE(probe_tokeninfo) == TITYPE_CLOSEBRACKET)
                                    || (TI_TYPE(probe_tokeninfo) ==
                                        TITYPE_COMMA)) {
                                // POP ALL OPERATORS OFF THE STACK UNTIL THE OPENING BRACKET IS FOUND

                                while(InfixOpTop > (WORDPTR) ValidateTop) {
                                    if((TI_TYPE(*(InfixOpTop - 1)) ==
                                                TITYPE_OPENBRACKET)) {
                                        // CHECK IF THE BRACKET IS THE RIGHT TYPE OF BRACKET
                                        if((TI_TYPE(probe_tokeninfo) ==
                                                    TITYPE_CLOSEBRACKET)
                                                && (*(InfixOpTop - 2) !=
                                                    (Opcode - 1))) {
                                            // MISMATCHED BRACKET TYPE
                                            // SPECIAL CASE: ALLOW LISTBRACKET TO CLOSE CLISTBRACKETS
                                            if(!((Opcode == CMD_LISTCLOSEBRACKET) && (*(InfixOpTop - 2) == CMD_CLISTOPENBRACKET))) {
                                                rplError(ERR_MISSINGBRACKET);
                                                LAMTop = LAMTopSaved;
                                                return 0;
                                            }
                                        }
                                        break;
                                    }
                                    // POP OPERATORS OFF THE STACK AND APPLY TO OBJECTS
                                    InfixOpTop -= 2;
                                    if(!rplInfixApply(InfixOpTop[0],
                                                TI_NARGS(InfixOpTop[1]))) {
                                        LAMTop = LAMTopSaved;
                                        return 0;
                                    }
                                }

                                if(InfixOpTop <= (WORDPTR) ValidateTop) {
                                    // OPENING BRACKET NOT FOUND, SYNTAX ERROR
                                    rplError(ERR_MISSINGBRACKET);
                                    LAMTop = LAMTopSaved;
                                    return 0;
                                }

                                if(TI_TYPE(probe_tokeninfo) ==
                                        TITYPE_CLOSEBRACKET) {
                                    // COUNT THE NUMBER OF ARGUMENTS WE HAVE
                                    // REMOVE THE OPENING BRACKET
                                    InfixOpTop -= 4;

                                    BINT nargs = 0;
                                    WORD brackettype = InfixOpTop[2];   // OPCODE WITH BRACKET TYPE TO DISTINGUISH BRACKETS
                                    WORDPTR list = TempObEnd + InfixOpTop[0];
                                    WORDPTR ptr = CompileEnd;

                                    while((ptr = rplReverseSkipOb(list,
                                                    ptr)) != NULL)
                                        ++nargs;

                                    // CHECK IF THE TOP OF STACK IS A FUNCTION
                                    if((InfixOpTop > (WORDPTR) ValidateTop)
                                            && ((TI_TYPE(*(InfixOpTop - 1)) ==
                                                    TITYPE_FUNCTION)
                                                || (TI_TYPE(*(InfixOpTop -
                                                            1)) ==
                                                    TITYPE_CASFUNCTION))) {
                                        BINT needargs =
                                                (BINT) TI_NARGS(*(InfixOpTop -
                                                    1));
                                        if((needargs != 0xf)
                                                && (nargs != needargs)) {
                                            rplError(ERR_BADARGCOUNT);
                                            LAMTop = LAMTopSaved;
                                            return 0;
                                        }
                                        // POP FUNCTION OFF THE STACK AND APPLY
                                        InfixOpTop -= 2;
                                        // SPECIAL CASE: IF THE OPERATOR IS FUNCEVAL, ADD ONE MORE PARAMETER: THE NAME OF THE FUNCTION
                                        if(InfixOpTop[0] == (CMD_OVR_FUNCEVAL)) {
                                            ++nargs;
                                            if(!rplRotArgs(nargs)) {
                                                LAMTop = LAMTopSaved;
                                                return 0;
                                            }
                                        }

                                        if(!rplInfixApply(InfixOpTop[0], nargs)) {
                                            LAMTop = LAMTopSaved;
                                            return 0;
                                        }

                                    }
                                    else {
                                        // THERE'S NO FUNCTION ON THE STACK, USE THE BRACKET THE MAIN OPERATOR
                                        if((brackettype != CMD_OPENBRACKET)
                                                || (nargs > 1)) {

                                            if(!rplInfixApply(brackettype,
                                                        nargs)) {
                                                LAMTop = LAMTopSaved;
                                                return 0;
                                            }

                                        }

                                    }

                                }
                                else {
                                    // THE PARAMETER IS A COMMA
                                    // DO NOTHING FOR NOW
                                }

                            }
                            else {

                                // FLUSH OTHER OPERATORS ACCORDING TO PRECEDENCE
                                // BUT NOT FOR PREFIX OPERATORS, SINCE THE ARGUMENT WASN'T INPUT YET TO THE STREAM
                                if(TI_TYPE(probe_tokeninfo) != TITYPE_PREFIXOP) {

                                    // IN INFIX MODE, USE RStk AS THE OPERATOR STACK, STARTING AT ValidateTop

                                    while(InfixOpTop > (WORDPTR) ValidateTop) {
                                        if((BINT) TI_PRECEDENCE(*(InfixOpTop -
                                                        1)) <
                                                TI_PRECEDENCE(probe_tokeninfo))
                                        {
                                            // POP OPERATORS OFF THE STACK AND APPLY TO OBJECTS
                                            InfixOpTop -= 2;

                                            if(!rplInfixApply(InfixOpTop[0],
                                                        TI_NARGS(InfixOpTop
                                                            [1]))) {
                                                LAMTop = LAMTopSaved;
                                                return 0;
                                            }

                                        }
                                        else {
                                            if(((TI_TYPE(probe_tokeninfo) ==
                                                            TITYPE_BINARYOP_LEFT)
                                                        ||
                                                        (TI_TYPE
                                                            (probe_tokeninfo) ==
                                                            TITYPE_CASBINARYOP_LEFT))
                                                    && ((BINT)
                                                        TI_PRECEDENCE(*
                                                            (InfixOpTop - 1)) <=
                                                        TI_PRECEDENCE
                                                        (probe_tokeninfo))) {
                                                InfixOpTop -= 2;

                                                if(!rplInfixApply(InfixOpTop[0],
                                                            TI_NARGS(InfixOpTop
                                                                [1]))) {
                                                    LAMTop = LAMTopSaved;
                                                    return 0;
                                                }

                                            }
                                            else
                                                break;
                                        }
                                    }

                                }
                                // PUSH THE NEW OPERATOR
                                if(RStkSize <=
                                        (InfixOpTop + 1 - (WORDPTR) RStk))
                                    growRStk(InfixOpTop - (WORDPTR) RStk +
                                            RSTKSLACK);
                                if(Exceptions) {
                                    LAMTop = LAMTopSaved;
                                    return 0;
                                }
                                InfixOpTop[0] = Opcode;
                                InfixOpTop[1] = probe_tokeninfo;
                                InfixOpTop += 2;
                            }
                        }

                    }

                    previous_tokeninfo = probe_tokeninfo;
                }

            }
            else {
                rplError(ERR_INVALIDTOKEN);
                LAMTop = LAMTopSaved;
                return 0;
            }

        }

        // HERE WE HAVE A COMPILED OPCODE

        // SUBMIT THE LAST COMPILED OBJECT FOR VALIDATION WITH THE CURRENT CONSTRUCT
        if(validate && !infixmode) {
            if(ValidateHandler) {
                // CALL THE LIBRARY TO SEE IF IT'S OK TO HAVE THIS OBJECT
                CurOpcode = MKOPCODE(LIBNUM(CurrentConstruct), OPCODE_VALIDATE);

                (*ValidateHandler) ();

                switch (RetNum) {
                case OK_INCARGCOUNT:
                    **(ValidateTop - 1) = **(ValidateTop - 1) + 1;      // POINTER TO THE WORD OF THE COMPOSITE, TEMPORARILY STORE THE NUMBER OF ARGUMENTS AS THE SIZE
                    break;

                case ERR_INVALID:
                    // GIVE A CHANCE TO THE LIBRARY TO SET ITS OWN ERROR CODE
                    if(!Exceptions)
                        rplError(ERR_SYNTAXERROR);
                    LAMTop = LAMTopSaved;
                    return 0;
                case OK_ENDCONSTRUCT:
                    --ValidateTop;
                    if(ValidateTop < ValidateBottom) {
                        rplError(ERR_ENDWITHOUTSTART);
                        LAMTop = LAMTopSaved;
                        return 0;
                    }
                    if(ISPROLOG((BINT) ** ValidateTop)) {
                        // STORE THE SIZE OF THE COMPOSITE IN THE WORD
                        **ValidateTop =
                                (**ValidateTop ^ OBJSIZE(**ValidateTop)) |
                                (((WORD) ((PTR2NUMBER) CompileEnd -
                                        (PTR2NUMBER) * ValidateTop) >> 2) - 1);
                        // PREPARE THE NEWLY CREATED OBJECT FOR VALIDATION BY ITS PARENT
                        CurrentConstruct = (BINT) ((ValidateTop > ValidateBottom) ? **(ValidateTop - 1) : 0);   // CARRIES THE WORD OF THE CURRENT CONSTRUCT/COMPOSITE
                        ValidateHandler =
                                rplGetLibHandler(LIBNUM(CurrentConstruct));
                        LastCompiledObject = *ValidateTop;
                        validate = 1;
                    }

                    break;

                }

            }
            validate = 0;
        }

    }
    while((splittoken || (NextTokenStart < CompileStringEnd)) && !Exceptions);

    if(force_libnum >= 0) {
        rplError(ERR_STARTWITHOUTEND);
        LAMTop = LAMTopSaved;
        return 0;

    }

    if(!Exceptions && addwrapper) {
        // JUST FINISHED THE STRING, NOW ADD THE END OF THE WRAPPER
        rplCompileAppend(CMD_SEMI);
        --ValidateTop;
        if(ValidateTop < ValidateBottom) {
            rplError(ERR_ENDWITHOUTSTART);
            LAMTop = LAMTopSaved;
            return 0;
        }
        if(ISPROLOG((BINT) ** ValidateTop))
            **ValidateTop |= ((WORD) ((PTR2NUMBER) CompileEnd - (PTR2NUMBER) * ValidateTop) >> 2) - 1;  // STORE THE SIZE OF THE COMPOSITE IN THE WORD
        rplCompileAppend(CMD_ENDOFCODE);
    }

// END OF STRING OBJECT WAS REACHED
    if(!Exceptions) {
        if(ValidateTop < ValidateBottom) {
            rplError(ERR_ENDWITHOUTSTART);
        }
        else {
            if(ValidateTop > ValidateBottom) {
                rplError(ERR_STARTWITHOUTEND);
            }
        }

    }

    LAMTop = LAMTopSaved;       // RESTORE LAM ENVIRONMENT BEFORE RETURN

    if((CompileEnd != TempObEnd) && !Exceptions) {

        if(CompileEnd + TEMPOBSLACK > TempObSize) {
            // ENLARGE TEMPOB AS NEEDED
            growTempOb((BINT) (CompileEnd - TempOb) + TEMPOBSLACK);
            if(Exceptions)
                return 0;
        }

        // STORE BLOCK SIZE
        rplAddTempBlock(TempObEnd);
        WORDPTR newobject = TempObEnd;
        TempObEnd = CompileEnd;
        return newobject;
    }

    return 0;

}

enum
{
    INFIX_OFF = 0,
    INFIX_STARTSYMBOLIC,
    INFIX_STARTEXPRESSION,
    INFIX_CUSTOMFUNCARG,
    INFIX_FUNCARGUMENT,
    INFIX_PREFIXOP,
    INFIX_PREFIXARG,
    INFIX_POSTFIXOP,
    INFIX_POSTFIXARG,
    INFIX_BINARYLEFT,
    INFIX_BINARYMID,
    INFIX_BINARYOP,
    INFIX_BINARYRIGHT,
    INFIX_ATOMIC
};

void rplDecompAppendChar(BYTE c)
{

    *((BYTEPTR) DecompStringEnd) = c;
    DecompStringEnd = (WORDPTR) (((BYTEPTR) DecompStringEnd) + 1);

    if(!(((PTR2NUMBER) DecompStringEnd) & 3)) {
        if(((WORDPTR) ((((PTR2NUMBER) DecompStringEnd) +
                            3) & ~((PTR2NUMBER) 3))) + TEMPOBSLACK >=
                TempObSize) {
            // ENLARGE TEMPOB AS NEEDED
            growTempOb((((((BYTEPTR) DecompStringEnd) + 3 -
                                (BYTEPTR) TempOb)) >> 2) + TEMPOBSLACK);
        }
    }

}

void rplDecompAppendUTF8(WORD utf8bytes)
{
    while(utf8bytes) {
        *((BYTEPTR) DecompStringEnd) = utf8bytes & 0xff;
        DecompStringEnd = (WORDPTR) (((BYTEPTR) DecompStringEnd) + 1);
        utf8bytes >>= 8;
    }

    if(!(((PTR2NUMBER) DecompStringEnd) & 3)) {
        if(((WORDPTR) ((((PTR2NUMBER) DecompStringEnd) +
                            3) & ~((PTR2NUMBER) 3))) + TEMPOBSLACK >=
                TempObSize) {
            // ENLARGE TEMPOB AS NEEDED
            growTempOb((((((BYTEPTR) DecompStringEnd) + 3 -
                                (BYTEPTR) TempOb)) >> 2) + TEMPOBSLACK);
        }
    }

}

void rplDecompAppendString(BYTEPTR str)
{
    BINT len = stringlen((char *)str);

    if(((WORDPTR) ((((PTR2NUMBER) DecompStringEnd) + len +
                        3) & ~((PTR2NUMBER) 3))) + TEMPOBSLACK >= TempObSize) {

        rplPushDataNoGrow((WORDPTR) str);
        // ENLARGE TEMPOB AS NEEDED
        growTempOb((((((BYTEPTR) DecompStringEnd) + len + 3 -
                            (BYTEPTR) TempOb)) >> 2) + TEMPOBSLACK);
        str = (BYTEPTR) rplPopData();
        // IF THERE'S NOT ENOUGH MEMORY, RETURN IMMEDIATELY
        if(Exceptions & EX_OUTOFMEM)
            return;
    }

    BYTEPTR ptr = (BYTEPTR) DecompStringEnd;

    while(*str != 0) {
        *ptr = *str;
        ++ptr;
        ++str;
    }
    DecompStringEnd = (WORDPTR) ptr;
}

// APPEND A STRING OF A GIVEN LENGTH
// IF PASSED POINTER IS NULL, MEMORY IS RESERVED BUT NOTHING IS COPIED

void rplDecompAppendString2(BYTEPTR str, BINT len)
{
    if(((WORDPTR) ((((PTR2NUMBER) DecompStringEnd) + len +
                        3) & ~((PTR2NUMBER) 3))) + TEMPOBSLACK >= TempObSize) {
        if(str)
            rplPushDataNoGrow((WORDPTR) str);
        // ENLARGE TEMPOB AS NEEDED
        growTempOb((((((BYTEPTR) DecompStringEnd) + len + 3 -
                            (BYTEPTR) TempOb)) >> 2) + TEMPOBSLACK);
        if(str)
            str = (BYTEPTR) rplPopData();

        // IF THERE'S NOT ENOUGH MEMORY, RETURN IMMEDIATELY
        if(Exceptions & EX_OUTOFMEM)
            return;

    }

    if(str) {

        BYTEPTR ptr = (BYTEPTR) DecompStringEnd;

        while(len) {
            *ptr = *str;
            ++ptr;
            ++str;
            --len;
        }

        DecompStringEnd = (WORDPTR) ptr;

    }
    else {
        BYTEPTR ptr = (BYTEPTR) DecompStringEnd;
        ptr += len;
        DecompStringEnd = (WORDPTR) ptr;
    }
}

// BASIC DECOMPILE ONE OBJECT
// RETURNS A NEW STRING OBJECT IN TEMPOB
#define SAVED_POINTERS  4

WORDPTR rplDecompile(WORDPTR object, BINT flags)
{
    LIBHANDLER han;
    BINT infixmode = 0, indent = 0, lastnewline = 0, lastnloffset = 0, maxwidth;
    UBINT savecstruct = 0, savedecompmode = 0, dhints, savedhints = 0;
    BINT validtop = 0, validbottom = 0;
    WORDPTR *SavedRSTop = 0;
    if(flags & DECOMP_EMBEDDED) {
        SavedRSTop = RSTop;
        savecstruct = CurrentConstruct;
        savedecompmode = DecompMode;
        savedhints = DecompHints;
        validtop = ValidateTop - RSTop;
        validbottom = ValidateBottom - RSTop;
        if(ValidateTop > RSTop)
            RSTop = ValidateTop;
        // SAVE ALL DECOMPILER POINTERS
        *RSTop++ = DecompileObject;
        *RSTop++ = EndOfObject;
        *RSTop++ = (WORDPTR) LAMTopSaved;
        *RSTop++ = SavedDecompObject;

        // STORE POINTER BEFORE POSSIBLY TRIGGERING A GC
        DecompileObject = object;

        // RESERVE MEMORY
        if(RStkSize <= (RSTop + RSTKSLACK - RStk))
            growRStk(RSTop - RStk + RSTKSLACK);
        if(Exceptions)
            return 0;

        flags |= DECOMP_NOHINTS;        // FORCE NO HINTS ON EMBEDDED DECOMPILATION

    }
    else
        DecompileObject = object;

    maxwidth = DECOMP_GETMAXWIDTH(flags);
    if(!maxwidth) {
        maxwidth = DEFAULT_DECOMP_WIDTH;
        flags |= DECOMP_MAXWIDTH(DEFAULT_DECOMP_WIDTH);
    }

    WORDPTR InfixOpTop = (WORDPTR) RSTop;

    // START DECOMPILE LOOP
    // CREATE A STRING AT THE END OF TEMPOB
    if(!(flags & DECOMP_EMBEDDED))
        CompileEnd = TempObEnd;
    // SKIPOB TO DETERMINE END OF COMPILATION
    EndOfObject = rplSkipOb(object);

    LAMTopSaved = LAMTop;       //SAVE LAM ENVIRONMENTS
    ValidateTop = ValidateBottom = RSTop;
    *ValidateTop++ = DecompileObject;   // STORE START OF OBJECT FOR QUICK SKIPPING
    // HERE ALL POINTERS ARE STORED IN GC-UPDATEABLE AREA

    if(!(flags & DECOMP_EMBEDDED)) {
        // CREATE EMPTY STRING AT END OF TEMPOB
        rplCompileAppend(MKPROLOG(DOSTRING, 0));
        DecompStringEnd = CompileEnd;
    }

    while(DecompileObject < EndOfObject) {
        // GET FIRST WORD
        // GET LIBRARY NUMBER
        // CALL LIBRARY HANDLER TO DECOMPILE
        han = rplGetLibHandler(LIBNUM(*DecompileObject));

        CurOpcode = MKOPCODE(0, OPCODE_GETINFO);
        DecompMode = infixmode | (flags << 16);

        if(!han) {
            RetNum = ERR_INVALID;
        }
        else {
            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
            WORDPTR *tmpRSTop = RSTop;
            if(infixmode)
                RSTop = (WORDPTR *) InfixOpTop;
            else
                RSTop = (WORDPTR *) ValidateTop;
            (*han) ();
            RSTop = tmpRSTop;
        }

        // HERE WE HAVE INFORMATION ABOUT THE TOKEN WITH DECOMPILER HINTS
        // IN DecompHints AND TypeInfo
        dhints = DecompHints;

        if(Exceptions)
            break;

        // CHECK FOR HINTS BEFORE
        /*
           if(!infixmode && !(flags&DECOMP_NOHINTS) && (dhints&HINT_REBASEINDENT)) {
           // START THE IDENT FROM THE CURRENT POSITION IN THE LINE

           // BACKTRACK THE TEXT TO FIND THE START OF LINE AND COUNT THE NUMBER OF CHARACTERS

           BYTEPTR dstring=(BYTEPTR)CompileEnd;
           BYTEPTR end=(BYTEPTR)DecompStringEnd;
           BYTEPTR ptr=end-1;
           while( (ptr>dstring)&&(*ptr!='\n')) --ptr;

           if(*ptr=='\n') ++ptr;

           BINT nchars=utf8nlenst(ptr,end);

           indent=nchars;

           }
         */

        if(!infixmode && !(flags & DECOMP_NOHINTS) && (dhints & HINT_ALLBEFORE)) {
            if(lastnewline) {
                //  WE ALREADY ADDED A NEWLINE PER HINT
                // CHECK IF WE NEED MORE OR LESS INDENT
                if(dhints & HINT_ADDINDENTBEFORE) {
                    indent += 2;
                    rplDecompAppendChar(' ');   // APPLY INDENT
                    rplDecompAppendChar(' ');
                }
                if(dhints & HINT_SUBINDENTBEFORE) {
                    // WE NEED TO REDUCE THE INDENT
                    if(indent >= 2)
                        DecompStringEnd =
                                (WORDPTR) (((BYTEPTR) DecompStringEnd) - 2);
                    indent -= 2;

                }

            }
            else {
                if(dhints & HINT_ADDINDENTBEFORE)
                    indent += 2;
                if(dhints & HINT_SUBINDENTBEFORE)
                    indent -= 2;
                if(dhints & HINT_NLBEFORE) {
                    rplDecompAppendChar('\n');
                    int k;
                    for(k = 0; k < indent; ++k)
                        rplDecompAppendChar(' ');       // APPLY INDENT
                }
            }
        }

        lastnewline = 0;

        // NOW ACTUALLY DECOMPILE THE OBJECT

        CurOpcode =
                MKOPCODE(0,
                (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT : OPCODE_DECOMPILE);
        if(ValidateTop > ValidateBottom)
            CurrentConstruct = **(ValidateTop - 1);
        else
            CurrentConstruct = 0;
        if(!han) {
            RetNum = ERR_INVALID;
        }
        else {
            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
            WORDPTR *tmpRSTop = RSTop;
            if(infixmode)
                RSTop = (WORDPTR *) InfixOpTop;
            else
                RSTop = (WORDPTR *) ValidateTop;
            DecompHints = SET_INDENT(dhints, indent);
            (*han) ();
            indent = GET_INDENT(DecompHints);
            dhints = GET_HINTS(DecompHints);
            RSTop = tmpRSTop;
        }
        if(Exceptions)
            break;
        switch (RetNum) {
        case OK_CONTINUE:
            DecompileObject = rplSkipOb(DecompileObject);
            break;
        case OK_STARTCONSTRUCT:
            if(RStkSize <= (ValidateTop - RStk))
                growRStk(ValidateTop - RStk + RSTKSLACK);
            if(Exceptions) {
                LAMTop = LAMTopSaved;
                if(flags & DECOMP_EMBEDDED) {
                    // RESTORE ALL POINTERS BEFORE RETURNING
                    SavedDecompObject = *--RSTop;
                    LAMTopSaved = (WORDPTR *) * --RSTop;
                    EndOfObject = *--RSTop;
                    DecompileObject = *--RSTop;
                    CurrentConstruct = savecstruct;
                    DecompMode = savedecompmode;
                    DecompHints = savedhints;
                    RSTop = SavedRSTop;
                    ValidateTop = RSTop + validtop;
                    ValidateBottom = RSTop + validbottom;

                }
                return 0;
            }
            *ValidateTop++ = DecompileObject;   // POINTER TO THE WORD THAT CREATES THE CONSTRUCT
            ++DecompileObject;
            break;
        case OK_CHANGECONSTRUCT:
            --ValidateTop;
            if(ValidateTop < ValidateBottom) {
                rplError(ERR_MALFORMEDOBJECT);
                LAMTop = LAMTopSaved;
                if(flags & DECOMP_EMBEDDED) {
                    // RESTORE ALL POINTERS BEFORE RETURNING
                    SavedDecompObject = *--RSTop;
                    LAMTopSaved = (WORDPTR *) * --RSTop;
                    EndOfObject = *--RSTop;
                    DecompileObject = *--RSTop;
                    CurrentConstruct = savecstruct;
                    DecompMode = savedecompmode;
                    DecompHints = savedhints;
                    RSTop = SavedRSTop;
                    ValidateTop = RSTop + validtop;
                    ValidateBottom = RSTop + validbottom;

                }
                return 0;
            }
            *ValidateTop++ = DecompileObject;   // POINTER TO THE WORD THAT CREATES THE CONSTRUCT
            ++DecompileObject;
            break;
        case OK_ENDCONSTRUCT:
            --ValidateTop;
            if(ValidateTop < ValidateBottom) {
                rplError(ERR_MALFORMEDOBJECT);
                LAMTop = LAMTopSaved;
                if(flags & DECOMP_EMBEDDED) {
                    // RESTORE ALL POINTERS BEFORE RETURNING
                    SavedDecompObject = *--RSTop;
                    LAMTopSaved = (WORDPTR *) * --RSTop;
                    EndOfObject = *--RSTop;
                    DecompileObject = *--RSTop;
                    CurrentConstruct = savecstruct;
                    DecompMode = savedecompmode;
                    DecompHints = savedhints;
                    RSTop = SavedRSTop;
                    ValidateTop = RSTop + validtop;
                    ValidateBottom = RSTop + validbottom;

                }
                return 0;
            }
            DecompileObject = rplSkipOb(DecompileObject);
            break;

        case OK_STARTCONSTRUCT_INFIX:
            // PUSH THE SYMBOLIC ON A STACK AND SAVE THE COMPILER STATE
            if(!infixmode)
                InfixOpTop = (WORDPTR) ValidateTop;
            if(RStkSize <= (InfixOpTop + 1 - (WORDPTR) RStk))
                growRStk(InfixOpTop - (WORDPTR) RStk + RSTKSLACK);
            if(Exceptions) {
                LAMTop = LAMTopSaved;
                if(flags & DECOMP_EMBEDDED) {
                    // RESTORE ALL POINTERS BEFORE RETURNING
                    SavedDecompObject = *--RSTop;
                    LAMTopSaved = (WORDPTR *) * --RSTop;
                    EndOfObject = *--RSTop;
                    DecompileObject = *--RSTop;
                    CurrentConstruct = savecstruct;
                    DecompMode = savedecompmode;
                    DecompHints = savedhints;
                    RSTop = SavedRSTop;
                    ValidateTop = RSTop + validtop;
                    ValidateBottom = RSTop + validbottom;
                }
                return 0;
            }
            InfixOpTop[1] = infixmode;
            InfixOpTop[0] = DecompileObject - EndOfObject;
            InfixOpTop += 2;
            ++DecompileObject;
            if(infixmode) {
                // SAVE PREVIOUS MODE AND START A SUBEXPRESSION
                infixmode = INFIX_STARTEXPRESSION;
            }
            else
                infixmode = INFIX_STARTSYMBOLIC;
            break;
        default:
            rplDecompAppendString((BYTEPTR) "INVALID_COMMAND");
            ++DecompileObject;
            break;
        }

      end_of_expression:

        // LOOP UNTIL END OF DECOMPILATION ADDRESS IS REACHED
        if(infixmode) {
            // IN INFIX MODE, OBJECTS ARE LISTS, BUT FIRST ELEMENT
            // MIGHT BE AN OPERATOR, AND ARGUMENTS MIGHT NEED TO BE PROCESSED
            // IN DIFFERENT ORDER

            switch (infixmode) {
            case INFIX_STARTSYMBOLIC:
                rplDecompAppendChar('\'');
                if(Exceptions)
                    break;
                // DELIBERATE FALL THROUGH
            case INFIX_STARTEXPRESSION:
            {

                // EVALUATE THE TYPE OF SYMBOLIC OPERATOR
                // GET INFORMATION ABOUT THE TOKEN
                LIBHANDLER handler = rplGetLibHandler(LIBNUM(*DecompileObject));
                RetNum = 0;
                if(handler) {

                    CurOpcode =
                            MKOPCODE(LIBNUM(*DecompileObject), OPCODE_GETINFO);
                    DecompMode = infixmode | (flags << 16);

                    // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                    WORDPTR *tmpRSTop = RSTop;
                    RSTop = (WORDPTR *) InfixOpTop;
                    (*handler) ();
                    RSTop = tmpRSTop;
                }

                if(RetNum < OK_TOKENINFO)
                    RetNum = MKTOKENINFO(0, TITYPE_FUNCTION, 0, 20);    //    TREAT LIKE A NORMAL FUNCTION, THAT WILL BE CALLED [INVALID] LATER

                if(TI_TYPE(RetNum) >= TITYPE_OPERATORS) {
                    // PUSH THE OPERATOR ON THE STACK
                    if(RStkSize <= (InfixOpTop + 1 - (WORDPTR) RStk))
                        growRStk(InfixOpTop - (WORDPTR) RStk + RSTKSLACK);
                    if(Exceptions) {
                        LAMTop = LAMTopSaved;
                        if(flags & DECOMP_EMBEDDED) {
                            // RESTORE ALL POINTERS BEFORE RETURNING
                            SavedDecompObject = *--RSTop;
                            LAMTopSaved = (WORDPTR *) * --RSTop;
                            EndOfObject = *--RSTop;
                            DecompileObject = *--RSTop;
                            CurrentConstruct = savecstruct;
                            DecompMode = savedecompmode;
                            DecompHints = savedhints;
                            RSTop = SavedRSTop;
                            ValidateTop = RSTop + validtop;
                            ValidateBottom = RSTop + validbottom;
                        }
                        return 0;
                    }
                    InfixOpTop[0] = *DecompileObject;
                    InfixOpTop[1] = RetNum;
                    InfixOpTop += 2;

                    // CHECK PRECEDENCE TO SEE IF WE NEED PARENTHESIS
                    if((InfixOpTop - 6) >= (WORDPTR) ValidateTop) {
                        // THERE'S AN OPERATOR IN THE STACK
                        if(ISPROLOG(*(InfixOpTop - 6))) {
                            // THIS IS AN EXPRESSION START WITHOUT ANY OPERATORS
                            // NO NEED FOR PARENTHESIS
                        }
                        else {
                            // HANDLE SPECIAL CASE: ADD PARENTHESIS TO SEPARATE OPERATORS
                            if(TI_TYPE(RetNum) == TITYPE_PREFIXOP) {
                                if((*(InfixOpTop - 3) == INFIX_BINARYRIGHT) ||
                                        (*(InfixOpTop - 3) == INFIX_BINARYMID)
                                        || (*(InfixOpTop - 3) ==
                                            INFIX_POSTFIXARG)
                                        || (*(InfixOpTop - 3) ==
                                            INFIX_PREFIXARG))
                                    rplDecompAppendChar('(');
                                else if(*(InfixOpTop - 6) == (CMD_OVR_POW)) {
                                    // ALWAYS PARENTHESIZE THE ARGUMENTS OF POWER
                                    if((TI_TYPE(*(InfixOpTop - 1)) !=
                                                TITYPE_FUNCTION)
                                            && (TI_TYPE(*(InfixOpTop - 1)) !=
                                                TITYPE_CASFUNCTION)
                                            && (TI_TYPE(*(InfixOpTop - 1)) !=
                                                TITYPE_CUSTOMFUNC)
                                            && (TI_TYPE(RetNum) !=
                                                TITYPE_OPENBRACKET))
                                        rplDecompAppendChar('(');

                                }

                            }
                            else {

                                if(TI_PRECEDENCE(*(InfixOpTop - 5)) ==
                                        TI_PRECEDENCE(RetNum)) {
                                    // ALWAYS ADD PARENTHESIS, EXCEPT FOR MUL AND ADD
                                    if((*DecompileObject != (CMD_OVR_MUL))
                                            && (*DecompileObject !=
                                                (CMD_OVR_ADD))) {
                                        if((TI_TYPE(*(InfixOpTop - 5)) != TITYPE_FUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CASFUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CUSTOMFUNC) && (TI_TYPE(RetNum) != TITYPE_OPENBRACKET))       // DO NOT ADD PARENTHESIS TO FUNCTION ARGUMENTS! OR BRACKET-TYPE DELIMITERS
                                            rplDecompAppendChar('(');

                                    }
                                }
                                else if(TI_PRECEDENCE(*(InfixOpTop - 5)) <
                                        TI_PRECEDENCE(RetNum)) {
                                    if((TI_TYPE(*(InfixOpTop - 5)) != TITYPE_FUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CASFUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CUSTOMFUNC) && (TI_TYPE(RetNum) != TITYPE_OPENBRACKET))   // DO NOT ADD PARENTHESIS TO FUNCTION ARGUMENTS! OR BRACKET-TYPE DELIMITERS
                                        rplDecompAppendChar('(');
                                }
                            }
                        }
                    }

                    switch (TI_TYPE(RetNum)) {
                    case TITYPE_BINARYOP_LEFT:
                    case TITYPE_BINARYOP_RIGHT:
                    case TITYPE_CASBINARYOP_LEFT:
                    case TITYPE_CASBINARYOP_RIGHT:

                        ++DecompileObject;
                        infixmode = INFIX_BINARYLEFT;
                        break;
                    case TITYPE_POSTFIXOP:
                        ++DecompileObject;
                        infixmode = INFIX_POSTFIXARG;
                        break;
                    case TITYPE_PREFIXOP:
                        // DECOMPILE THE OPERATOR NOW!
                        CurOpcode =
                                MKOPCODE(LIBNUM(*DecompileObject),
                                (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                                OPCODE_DECOMPILE);
                        {
                            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                            WORDPTR *tmpRSTop = RSTop;
                            RSTop = (WORDPTR *) InfixOpTop;
                            DecompMode = infixmode | (flags << 16);

                            (*handler) ();
                            RSTop = tmpRSTop;
                        }
                        // IGNORE THE RESULT OF DECOMPILATION
                        if(RetNum != OK_CONTINUE) {
                            rplDecompAppendString((BYTEPTR) "##INVALID##");

                            /*
                               rplError(ERR_INVALIDOPERATORINSYMBOLIC);
                               LAMTop=LAMTopSaved;     // RESTORE ENVIRONMENTS
                               if(flags&DECOMP_EMBEDDED) {
                               // RESTORE ALL POINTERS BEFORE RETURNING
                               SavedDecompObject=*--RSTop;
                               LAMTopSaved=(WORDPTR *)*--RSTop;
                               EndOfObject=*--RSTop;
                               DecompileObject=*--RSTop;
                               RSTop=SavedRSTop;
                               }

                               return 0;
                             */
                        }
                        ++DecompileObject;
                        infixmode = INFIX_PREFIXARG;
                        break;

                    case TITYPE_CUSTOMFUNC:
                    {
                        // DECOMPILE THE FUNCTION NAME NOW, THEN ADD PARENTHESIS FOR THE LIST
                        WORDPTR argList = DecompileObject + 1;
                        WORDPTR EndofExpression =
                                rplSkipOb(*(signed int *)(InfixOpTop - 4) +
                                EndOfObject);
                        WORDPTR firstobj = argList;
                        // FIND THE LAST ARGUMENT
                        while(rplSkipOb(argList) < EndofExpression)
                            argList = rplSkipOb(argList);

                        // THE NEXT ELEMENT IS THE LAST, WHICH CONTAINS THE NAME
                        rplPushRet(DecompileObject);
                        DecompileObject = argList;
                        CurOpcode =
                                MKOPCODE(LIBNUM(*argList),
                                (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                                OPCODE_DECOMPILE);
                        DecompMode = infixmode | (flags << 16);
                        handler = rplGetLibHandler(LIBNUM(*argList));

                        RetNum = -1;
                        if(handler) {
                            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                            WORDPTR *tmpRSTop = RSTop;
                            RSTop = (WORDPTR *) InfixOpTop;

                            (*handler) ();
                            RSTop = tmpRSTop;
                        }
                        DecompileObject = rplPopRet();  // RESTORE THE NEXT OBJECT
                        // IGNORE THE RESULT OF DECOMPILATION
                        if(RetNum != OK_CONTINUE) {
                            rplDecompAppendString((BYTEPTR) "##INVALID##");
                        }
                        rplDecompAppendChar('(');
                        ++DecompileObject;

                        if(argList == firstobj) {
                            // SPECIAL CASE OF FUNCTION WITHOUT ANY ARGUMENTS
                            rplDecompAppendChar(')');
                            // END OF THIS EXPRESSION
                            // POP EXPRESSION FROM THE STACK
                            InfixOpTop -= 4;
                            // RESTORE PREVIOUS EXPRESSION STATE
                            infixmode = InfixOpTop[1];
                            DecompileObject =
                                    rplSkipOb(*(signed int *)InfixOpTop +
                                    EndOfObject);
                            if(!infixmode)
                                rplDecompAppendChar('\'');

                            goto end_of_expression;

                        }

                        infixmode = INFIX_CUSTOMFUNCARG;
                        break;

                    }

                    case TITYPE_OPENBRACKET:
                    {
                        // DECOMPILE THE OPERATOR NOW, THEN ADD PARENTHESIS FOR THE LIST
                        CurOpcode =
                                MKOPCODE(LIBNUM(*DecompileObject),
                                (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                                OPCODE_DECOMPILE);
                        DecompMode = infixmode | (flags << 16);

                        RetNum = -1;
                        if(handler) {
                            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                            WORDPTR *tmpRSTop = RSTop;
                            RSTop = (WORDPTR *) InfixOpTop;

                            (*handler) ();
                            RSTop = tmpRSTop;
                        }
                        // IGNORE THE RESULT OF DECOMPILATION
                        if(RetNum != OK_CONTINUE) {
                            rplDecompAppendString((BYTEPTR) "##INVALID##");
                        }

                        ++DecompileObject;
                        infixmode = INFIX_FUNCARGUMENT;
                        break;
                    }

                    case TITYPE_FUNCTION:
                    case TITYPE_CASFUNCTION:
                    default:
                    {
                        // DECOMPILE THE OPERATOR NOW, THEN ADD PARENTHESIS FOR THE LIST
                        CurOpcode =
                                MKOPCODE(LIBNUM(*DecompileObject),
                                (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                                OPCODE_DECOMPILE);
                        DecompMode = infixmode | (flags << 16);

                        RetNum = -1;
                        if(handler) {
                            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                            WORDPTR *tmpRSTop = RSTop;
                            RSTop = (WORDPTR *) InfixOpTop;

                            (*handler) ();
                            RSTop = tmpRSTop;
                        }
                        // IGNORE THE RESULT OF DECOMPILATION
                        if(RetNum != OK_CONTINUE) {
                            rplDecompAppendString((BYTEPTR) "##INVALID##");
                        }
                        rplDecompAppendChar('(');
                        ++DecompileObject;

                        // CHECK IF THIS IS THE LAST ARGUMENT
                        WORDPTR EndofExpression =
                                rplSkipOb(*(signed int *)(InfixOpTop - 4) +
                                EndOfObject);

                        if(DecompileObject == EndofExpression) {

                            rplDecompAppendChar(')');

                            // END OF THIS EXPRESSION
                            // POP EXPRESSION FROM THE STACK
                            InfixOpTop -= 4;
                            // RESTORE PREVIOUS EXPRESSION STATE

                            infixmode = InfixOpTop[1];
                            DecompileObject =
                                    rplSkipOb(*(signed int *)InfixOpTop +
                                    EndOfObject);
                            if(!infixmode)
                                rplDecompAppendChar('\'');

                            goto end_of_expression;
                        }

                        infixmode = INFIX_FUNCARGUMENT;
                        break;
                    }
                    }
                }
                else
                    infixmode = INFIX_ATOMIC;

                break;
            }
            case INFIX_BINARYLEFT:
            {
                LIBHANDLER handler;
                // ADD THE OPERATOR AFTER THE LEFT OPERAND
                WORD Operator = *(InfixOpTop - 2);
                BINT no_output = 0;

                SavedDecompObject = DecompileObject;

                // HANDLE SPECIAL CASES: A+(-B) --> A-B
                //                       A*INV(B) --> A/B
                //                       A+(-B)/2 --> A-B/2
                //                       A-(-B)/2 --> A-(-B)/2

                if(Operator == (CMD_OVR_ADD)) {
                    WORD newop = rplSymbMainOperator(DecompileObject);
                    if(newop == (CMD_OVR_UMINUS)) {
                        Operator = (CMD_OVR_SUB);
                        // MAKE NEXT OBJECT SKIP THE UMINUS OPERATOR
                        SavedDecompObject = rplSymbUnwrap(DecompileObject) + 2;
                    }
                    if((newop == (CMD_OVR_MUL)) || (newop == (CMD_OVR_DIV))) {
                        // IF THE FIRST ARGUMENT IN THE MUL OR DIV EXPRESSION IS NEGATIVE, THEN ADD PARENTHESIS
                        newop = rplSymbMainOperator(rplSymbUnwrap
                                (DecompileObject) + 2);
                        if(newop == (CMD_OVR_UMINUS)) {
                            // WE DON'T NEED TO PUT THE '+' SIGN, SINCE A MINUS WILL FOLLOW
                            no_output = 1;
                        }
                    }

                }

                if(Operator == (CMD_OVR_MUL)) {
                    if(rplSymbMainOperator(DecompileObject) == (CMD_OVR_INV)) {
                        Operator = (CMD_OVR_DIV);
                        // MAKE NEXT OBJECT SKIP THE INV OPERATOR
                        SavedDecompObject = rplSymbUnwrap(DecompileObject) + 2;
                        if(ISIDENT(*SavedDecompObject)) {
                            // TODO: CHECK IF THE IDENT IS A MATRIX, KEEP THE INV() OPERATOR IF THAT'S THE CASE

                        }

                    }

                }

                if(no_output == 0) {
                    BINT libnum = LIBNUM(Operator);
                    DecompileObject = &Operator;
                    CurOpcode =
                            MKOPCODE(libnum,
                            (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                            OPCODE_DECOMPILE);
                    DecompMode = infixmode | (flags << 16);

                    handler = rplGetLibHandler(libnum);
                    RetNum = -1;

                    if(handler) {
                        // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                        WORDPTR *tmpRSTop = RSTop;
                        RSTop = (WORDPTR *) InfixOpTop;
                        (*handler) ();
                        RSTop = tmpRSTop;

                    }

                    DecompileObject = SavedDecompObject;
                    // IGNORE THE RESULT OF DECOMPILATION
                    if(RetNum != OK_CONTINUE) {
                        rplDecompAppendString((BYTEPTR) "##INVALID##");
                    }
                }

                // NOW CHECK IF THE RIGHT ARGUMENT IS INDEED THE LAST ONE
                WORDPTR afternext = rplSkipOb(DecompileObject);
                WORDPTR EndofExpression =
                        rplSkipOb(*(signed int *)(InfixOpTop - 4) +
                        EndOfObject);

                if(afternext == EndofExpression) {
                    // THE NEXT ELEMENT IS THE LAST (IT SHOULD ALWAYS BE IF THE BINARY OPERATOR HAS ONLY 2 ARGUMENTS
                    // BUT WE CAN PACK MORE TERMS ON THE SAME '+' OR '*' THIS WAY
                    infixmode = INFIX_BINARYRIGHT;
                }
                else
                    infixmode = INFIX_BINARYMID;        // IF IT'S NOT, THEN KEEP IT AS THE MID OPERATOR FOR THE NEXT ARGUMENT
                break;

            }
            case INFIX_BINARYMID:
            {
                LIBHANDLER handler;
                WORD Operator = *(InfixOpTop - 2);
                BINT no_output = 0;

                SavedDecompObject = DecompileObject;

                if(Operator == (CMD_OVR_ADD)) {
                    WORD newop = rplSymbMainOperator(DecompileObject);
                    if(newop == (CMD_OVR_UMINUS)) {
                        Operator = (CMD_OVR_SUB);
                        // MAKE NEXT OBJECT SKIP THE UMINUS OPERATOR
                        SavedDecompObject = rplSymbUnwrap(DecompileObject) + 2;
                    }
                    if((newop == (CMD_OVR_MUL)) || (newop == (CMD_OVR_DIV))) {
                        // IF THE FIRST ARGUMENT IN THE MUL OR DIV EXPRESSION IS NEGATIVE, THEN ADD PARENTHESIS
                        newop = rplSymbMainOperator(rplSymbUnwrap
                                (DecompileObject) + 2);
                        if(newop == (CMD_OVR_UMINUS)) {
                            // WE DON'T NEED TO PUT THE '+' SIGN, SINCE A MINUS WILL FOLLOW
                            no_output = 1;
                        }
                    }

                }

                if(Operator == (CMD_OVR_MUL)) {
                    if(rplSymbMainOperator(DecompileObject) == (CMD_OVR_INV)) {
                        Operator = (CMD_OVR_DIV);
                        // MAKE NEXT OBJECT SKIP THE INV OPERATOR
                        SavedDecompObject = rplSymbUnwrap(DecompileObject) + 2;

                        // TODO: CHECK IF THE EXPRESSION IS A MATRIX, KEEP THE INV() OPERATOR IF THAT'S THE CASE

                    }

                }

                if(no_output == 0) {

                    BINT libnum = LIBNUM(Operator);
                    DecompileObject = &Operator;
                    CurOpcode =
                            MKOPCODE(libnum,
                            (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                            OPCODE_DECOMPILE);
                    DecompMode = infixmode | (flags << 16);

                    handler = rplGetLibHandler(libnum);
                    RetNum = -1;

                    if(handler) {
                        // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                        WORDPTR *tmpRSTop = RSTop;
                        RSTop = (WORDPTR *) InfixOpTop;
                        (*handler) ();
                        RSTop = tmpRSTop;
                    }

                    DecompileObject = SavedDecompObject;
                    // IGNORE THE RESULT OF DECOMPILATION
                    if(RetNum != OK_CONTINUE) {
                        rplDecompAppendString((BYTEPTR) "##INVALID##");
                    }
                }

                // NEVER CLOSE PARENTHESIS IN THE MIDDLE ARGUMENT
                /*
                   if( (InfixOpTop-6)>=RSTop) {
                   // THERE'S AN OPERATOR IN THE STACK
                   if(ISPROLOG(*(InfixOpTop-6))) {
                   // THIS IS AN EXPRESSION START WITHOUT ANY OPERATORS
                   // NO NEED FOR PARENTHESIS
                   }
                   else {
                   // HANDLE SPECIAL CASE: ADD PARENTHESIS TO SEPARATE OPERATORS
                   if(infixmode==INFIX_PREFIXARG) {
                   if( (*(InfixOpTop-3)==INFIX_BINARYRIGHT) ||
                   (*(InfixOpTop-3)==INFIX_BINARYMID) ||
                   (*(InfixOpTop-3)==INFIX_POSTFIXARG) ||
                   (*(InfixOpTop-3)==INFIX_PREFIXARG))
                   rplDecompAppendChar(')');
                   }
                   else

                   if(TI_PRECEDENCE(*(InfixOpTop-5))<TI_PRECEDENCE(*(InfixOpTop-1))) {
                   if(TI_TYPE(*(InfixOpTop-5))!=TITYPE_FUNCTION) // DON'T ADD PARENTHESIS TO FUNCTION ARGUMENTS
                   rplDecompAppendChar(')');
                   }
                   }
                   }
                 */

                // NOW CHECK IF THE RIGHT ARGUMENT IS INDEED THE LAST ONE
                WORDPTR afternext = rplSkipOb(DecompileObject);
                WORDPTR EndofExpression =
                        rplSkipOb(*(signed int *)(InfixOpTop - 4) +
                        EndOfObject);

                if(afternext == EndofExpression) {
                    // THE NEXT ELEMENT IS THE LAST (IT SHOULD ALWAYS BE IF THE BINARY OPERATOR HAS ONLY 2 ARGUMENTS
                    // BUT WE CAN PACK MORE TERMS ON THE SAME '+' OR '*' THIS WAY
                    infixmode = INFIX_BINARYRIGHT;
                }
                else
                    infixmode = INFIX_BINARYMID;        // IF IT'S NOT, THEN KEEP IT AS THE MID OPERATOR FOR THE NEXT ARGUMENT
                break;

            }

            case INFIX_BINARYRIGHT:
            case INFIX_PREFIXARG:
            {
                // WE KNOW THIS IS THE LAST ARGUMENT
                // POP EXPRESSION FROM THE STACK
                // CHECK PRECEDENCE TO SEE IF WE NEED PARENTHESIS
                if((InfixOpTop - 6) >= (WORDPTR) ValidateTop) {
                    // THERE'S AN OPERATOR IN THE STACK
                    if(ISPROLOG(*(InfixOpTop - 6))) {
                        // THIS IS AN EXPRESSION START WITHOUT ANY OPERATORS
                        // NO NEED FOR PARENTHESIS
                    }
                    else {
                        // HANDLE SPECIAL CASE: ADD PARENTHESIS TO SEPARATE OPERATORS
                        if(infixmode == INFIX_PREFIXARG) {
                            if((*(InfixOpTop - 3) == INFIX_BINARYRIGHT) ||
                                    (*(InfixOpTop - 3) == INFIX_BINARYMID) ||
                                    (*(InfixOpTop - 3) == INFIX_POSTFIXARG) ||
                                    (*(InfixOpTop - 3) == INFIX_PREFIXARG))
                                rplDecompAppendChar(')');
                            else if(*(InfixOpTop - 6) == (CMD_OVR_POW)) {
                                // ALWAYS PARENTHESIZE THE ARGUMENTS OF POWER
                                if((TI_TYPE(*(InfixOpTop - 1)) !=
                                            TITYPE_FUNCTION)
                                        && (TI_TYPE(*(InfixOpTop - 1)) !=
                                            TITYPE_CASFUNCTION)
                                        && (TI_TYPE(*(InfixOpTop - 1)) !=
                                            TITYPE_CUSTOMFUNC))
                                    rplDecompAppendChar(')');

                            }

                        }
                        else {
                            if(TI_PRECEDENCE(*(InfixOpTop - 5)) ==
                                    TI_PRECEDENCE(*(InfixOpTop - 1))) {
                                // ALWAYS ADD PARENTHESIS, EXCEPT FOR MUL AND ADD
                                if((*(InfixOpTop - 2) != (CMD_OVR_MUL))
                                        && (*(InfixOpTop - 2) !=
                                            (CMD_OVR_ADD))) {
                                    rplDecompAppendChar(')');

                                }
                            }
                            if(TI_PRECEDENCE(*(InfixOpTop - 5)) <
                                    TI_PRECEDENCE(*(InfixOpTop - 1))) {
                                if((TI_TYPE(*(InfixOpTop - 5)) != TITYPE_FUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CASFUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CUSTOMFUNC))  // DON'T ADD PARENTHESIS TO FUNCTION ARGUMENTS
                                    rplDecompAppendChar(')');
                            }
                        }
                    }
                }
                InfixOpTop -= 4;
                // RESTORE PREVIOUS EXPRESSION STATE
                infixmode = InfixOpTop[1];
                DecompileObject =
                        rplSkipOb(*(signed int *)InfixOpTop + EndOfObject);
                if(!infixmode)
                    rplDecompAppendChar('\'');

                goto end_of_expression;
            }
                break;
            case INFIX_POSTFIXARG:
            {
                LIBHANDLER handler;
                // ADD THE OPERATOR AFTER THE OPERAND
                BINT libnum = LIBNUM(*(InfixOpTop - 2));
                SavedDecompObject = DecompileObject;
                DecompileObject = InfixOpTop - 2;
                CurOpcode =
                        MKOPCODE(libnum,
                        (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                        OPCODE_DECOMPILE);
                DecompMode = infixmode | (flags << 16);

                handler = rplGetLibHandler(libnum);
                RetNum = -1;

                if(handler) {
                    // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                    WORDPTR *tmpRSTop = RSTop;
                    RSTop = (WORDPTR *) InfixOpTop;
                    (*handler) ();
                    RSTop = tmpRSTop;
                }

                DecompileObject = SavedDecompObject;
                // IGNORE THE RESULT OF DECOMPILATION
                if(RetNum != OK_CONTINUE) {
                    rplDecompAppendString((BYTEPTR) "##INVALID##");
                }

                // CHECK PRECEDENCE TO SEE IF WE NEED PARENTHESIS
                if((InfixOpTop - 6) >= (WORDPTR) ValidateTop) {
                    // THERE'S AN OPERATOR IN THE STACK
                    if(ISPROLOG(*(InfixOpTop - 6))) {
                        // THIS IS AN EXPRESSION START WITHOUT ANY OPERATORS
                        // NO NEED FOR PARENTHESIS
                    }
                    else {
                        if(TI_PRECEDENCE(*(InfixOpTop - 5)) ==
                                TI_PRECEDENCE(*(InfixOpTop - 1))) {
                            // ALWAYS ADD PARENTHESIS, EXCEPT FOR MUL AND ADD
                            if((*(InfixOpTop - 2) != (CMD_OVR_MUL))
                                    && (*(InfixOpTop - 2) != (CMD_OVR_ADD))) {
                                rplDecompAppendChar(')');

                            }
                        }

                        if(TI_PRECEDENCE(*(InfixOpTop - 5)) <
                                TI_PRECEDENCE(*(InfixOpTop - 1))) {
                            if((TI_TYPE(*(InfixOpTop - 5)) != TITYPE_FUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CASFUNCTION) && (TI_TYPE(*(InfixOpTop - 5)) != TITYPE_CUSTOMFUNC))      // DON'T ADD PARENTHESIS TO FUNCTION ARGUMENTS
                                rplDecompAppendChar(')');
                        }
                    }
                }
                // POP EXPRESSION FROM THE STACK
                InfixOpTop -= 4;
                // RESTORE PREVIOUS EXPRESSION STATE
                infixmode = InfixOpTop[1];
                DecompileObject =
                        rplSkipOb(*(signed int *)InfixOpTop + EndOfObject);
                if(!infixmode)
                    rplDecompAppendChar('\'');

                goto end_of_expression;

            }
                break;

            case INFIX_CUSTOMFUNCARG:
            {
                // CHECK IF THIS IS THE LAST ARGUMENT
                WORDPTR EndofExpression =
                        rplSkipOb(*(signed int *)(InfixOpTop - 4) +
                        EndOfObject);

                if((DecompileObject >= EndofExpression)
                        || (rplSkipOb(DecompileObject) == EndofExpression)) {
                    rplDecompAppendChar(')');
                    // END OF THIS EXPRESSION
                    // POP EXPRESSION FROM THE STACK
                    InfixOpTop -= 4;
                    // RESTORE PREVIOUS EXPRESSION STATE
                    infixmode = InfixOpTop[1];
                    DecompileObject =
                            rplSkipOb(*(signed int *)InfixOpTop + EndOfObject);
                    if(!infixmode)
                        rplDecompAppendChar('\'');

                    goto end_of_expression;
                }
                else {
                    // IF NOT, KEEP PROCESSING ARGUMENTS
                    UBINT64 Locale = rplGetSystemLocale();

                    rplDecompAppendUTF8(cp2utf8(ARG_SEP(Locale)));
                }

                break;
            }

            case INFIX_FUNCARGUMENT:
            {
                // CHECK IF THIS IS THE LAST ARGUMENT
                WORDPTR EndofExpression =
                        rplSkipOb(*(signed int *)(InfixOpTop - 4) +
                        EndOfObject);

                if(DecompileObject == EndofExpression) {
                    LIBHANDLER handler;
                    // ADD THE OPERATOR AFTER THE OPERAND
                    WORD functype = *(InfixOpTop - 1);

                    if(TI_TYPE(functype) == TITYPE_OPENBRACKET) {
                        BINT libnum = LIBNUM(*(InfixOpTop - 2));
                        WORD closebracket;
                        closebracket = *(InfixOpTop - 2) + 1;
                        DecompileObject = &closebracket;
                        CurOpcode =
                                MKOPCODE(libnum,
                                (flags & DECOMP_EDIT) ? OPCODE_DECOMPEDIT :
                                OPCODE_DECOMPILE);
                        DecompMode = infixmode | (flags << 16);
                        handler = rplGetLibHandler(libnum);
                        RetNum = -1;

                        if(handler) {
                            // PROTECT OPERATOR'S STACK FROM BEING OVERWRITTEN
                            WORDPTR *tmpRSTop = RSTop;
                            RSTop = (WORDPTR *) InfixOpTop;
                            (*handler) ();
                            RSTop = tmpRSTop;
                        }

                        // IGNORE THE RESULT OF DECOMPILATION
                        if(RetNum != OK_CONTINUE) {
                            rplDecompAppendString((BYTEPTR) "##INVALID##");
                        }
                    }
                    else
                        rplDecompAppendChar(')');

                    // END OF THIS EXPRESSION
                    // POP EXPRESSION FROM THE STACK
                    InfixOpTop -= 4;
                    // RESTORE PREVIOUS EXPRESSION STATE

                    infixmode = InfixOpTop[1];
                    DecompileObject =
                            rplSkipOb(*(signed int *)InfixOpTop + EndOfObject);
                    if(!infixmode)
                        rplDecompAppendChar('\'');

                    goto end_of_expression;
                }
                else {
                    // IF NOT, KEEP PROCESSING ARGUMENTS
                    UBINT64 Locale = rplGetSystemLocale();

                    rplDecompAppendUTF8(cp2utf8(ARG_SEP(Locale)));
                }

                break;
            }
            case INFIX_ATOMIC:
            {
                // CHECK IF THIS IS THE LAST ARGUMENT
                WORDPTR EndofExpression =
                        rplSkipOb(*(signed int *)(InfixOpTop - 2) +
                        EndOfObject);

                if(DecompileObject == EndofExpression) {
                    // END OF THIS EXPRESSION
                    // POP EXPRESSION FROM THE STACK
                    InfixOpTop -= 2;
                    // RESTORE PREVIOUS EXPRESSION STATE
                    infixmode = InfixOpTop[1];
                    DecompileObject =
                            rplSkipOb(*(signed int *)InfixOpTop + EndOfObject);
                    if(!infixmode)
                        rplDecompAppendChar('\'');
                    goto end_of_expression;
                }
                else {
                    // IF NOT, KEEP PROCESSING A LIST OF EXPRESSIONS (???)
                    UBINT64 Locale = rplGetSystemLocale();

                    rplDecompAppendUTF8(cp2utf8(ARG_SEP(Locale)));
                }
                break;
            }

            default:
                break;

            }

        }
        else {

            // UPDATE LAST NEWLINE

            BYTEPTR start = (((BYTEPTR) CompileEnd) + lastnloffset), ptr =
                    (BYTEPTR) DecompStringEnd;

            if(!(flags & DECOMP_NOHINTS)) {
                do {
                    --ptr;
                    if(*ptr == '\n')
                        break;
                }
                while(ptr > start);

                lastnloffset = ptr - ((BYTEPTR) CompileEnd);
                if(*ptr == '\n')
                    ++lastnloffset;
            }
            else
                lastnloffset = 0;

            // CHECK IF MAXIMUM WIDTH EXCEEDED, THEN ADD A NEW LINE
            if(((BYTEPTR) DecompStringEnd) - (((BYTEPTR) CompileEnd) +
                        lastnloffset) > maxwidth)
                dhints |= HINT_NLAFTER;

            if(!(flags & DECOMP_NOHINTS) && (dhints & HINT_ALLAFTER)) {
                // TODO: APPLY FORMATTING AFTER THE OBJECT
                if(dhints & HINT_ADDINDENTAFTER)
                    indent += 2;
                if(dhints & HINT_SUBINDENTAFTER)
                    indent -= 2;
                if(dhints & HINT_NLAFTER) {
                    lastnewline = 1;
                    rplDecompAppendChar('\n');
                    int k;
                    for(k = 0; k < indent; ++k)
                        rplDecompAppendChar(' ');       // APPLY INDENT
                }
                else if(DecompileObject < EndOfObject)
                    rplDecompAppendChar(' ');

            }
            else if(DecompileObject < EndOfObject)
                rplDecompAppendChar(' ');

            if(Exceptions)
                break;
        }
    }

    // DONE, HERE WE HAVE THE STRING FINISHED

    // REMOVE END NEWLINE AND INDENT IF PRESENT
    if(lastnewline && !Exceptions) {
        BYTEPTR start = (((BYTEPTR) CompileEnd) + lastnloffset), ptr =
                (BYTEPTR) DecompStringEnd;
        do {
            --ptr;
            if(*ptr == '\n')
                break;
            if(*ptr != ' ')
                break;
        }
        while(ptr > start);

        if(*ptr == '\n')
            DecompStringEnd = (WORDPTR) ptr;
    }

    if(!(flags & DECOMP_EMBEDDED)) {
        // STORE THE SIZE OF THE STRING IN WORDS IN THE PROLOG
        *(CompileEnd - 1) =
                MKPROLOG(DOSTRING + ((-(PTR2NUMBER) DecompStringEnd) & 3),
                ((WORD) ((PTR2NUMBER) DecompStringEnd -
                        (PTR2NUMBER) CompileEnd) + 3) >> 2);
        CompileEnd = rplSkipOb(CompileEnd - 1);
    }

    LAMTop = LAMTopSaved;       // RESTORE ENVIRONMENTS
    if(!Exceptions) {

        // GUARANTEE MINIMUM SLACK SPACE
        if(CompileEnd + TEMPOBSLACK > TempObSize) {
            // ENLARGE TEMPOB AS NEEDED
            growTempOb((BINT) (CompileEnd - TempOb) + TEMPOBSLACK);
            if(Exceptions) {
                if(flags & DECOMP_EMBEDDED) {
                    // RESTORE ALL POINTERS BEFORE RETURNING
                    SavedDecompObject = *--RSTop;
                    LAMTopSaved = (WORDPTR *) * --RSTop;
                    EndOfObject = *--RSTop;
                    DecompileObject = *--RSTop;
                    CurrentConstruct = savecstruct;
                    DecompMode = savedecompmode;
                    DecompHints = savedhints;
                    RSTop = SavedRSTop;
                    ValidateTop = RSTop + validtop;
                    ValidateBottom = RSTop + validbottom;

                }
                return 0;
            }
        }

        if(flags & DECOMP_EMBEDDED) {
            // RESTORE ALL POINTERS BEFORE RETURNING
            SavedDecompObject = *--RSTop;
            LAMTopSaved = (WORDPTR *) * --RSTop;
            EndOfObject = *--RSTop;
            DecompileObject = *--RSTop;
            CurrentConstruct = savecstruct;
            DecompMode = savedecompmode;
            DecompHints = savedhints;
            RSTop = SavedRSTop;
            ValidateTop = RSTop + validtop;
            ValidateBottom = RSTop + validbottom;

        }
        else {
            // STORE BLOCK SIZE
            rplAddTempBlock(TempObEnd);
            WORDPTR newobject = TempObEnd;
            TempObEnd = CompileEnd;

            return newobject;
        }
    }

    return 0;

}

// APPLY ANY HINTS DURING DECOMPILATION
// THIS IS DONE AUTOMATICALLY FOR ATOMIC OBJECTS
// THIS FUNCTION IS ONLY TO BE CALLED FROM COMPOSITE OBJECTS
// THAT DECOMPILE INNER OBJECTS USING EMBEDDED SESSIONS
// AFTER EACH EMBEDDED SESSION OBJECT
// ALSO CHECKS FOR MAXIMUM WIDTH AND INSERTS A NEWLINE AND INDENTATION AS NEEDED
// EXPECTS DecompMode AND DecompHints TO BE SET AND VALID
// CALL THIS FUNCTION ONLY FROM A OPCODE_DECOMP OR OPCODE_DECOMPEDIT HANDLER.

// RETURNS 1 IF A NEWLINE WAS ADDED TO THE STREAM (NO SEPARATOR NEEDED), 0 IF NOTHING WAS DONE
BINT rplDecompDoHintsWidth(BINT dhints)
{
    BINT flags = DecompMode >> 16;
    BINT infixmode = DecompMode & 0xffff;

    if(!infixmode && !(flags & DECOMP_NOHINTS)) {
        BINT indent = GET_INDENT(DecompHints);

        BYTEPTR start = (BYTEPTR) CompileEnd, ptr = (BYTEPTR) DecompStringEnd;

        do {
            --ptr;
            if(*ptr == '\n')
                break;
        }
        while(ptr > start);

        if(*ptr == '\n')
            ++ptr;

        // CHECK IF MAXIMUM WIDTH EXCEEDED, THEN ADD A NEW LINE
        if(((BYTEPTR) DecompStringEnd) - ptr > DECOMP_GETMAXWIDTH(flags))
            dhints |= HINT_NLAFTER;

        if(dhints & HINT_SUBINDENTBEFORE) {
            BINT currentindent = 0;

            // SET THE INDENTATION OF THE CURRENT LINE
            while((ptr < (BYTEPTR) DecompStringEnd) && (*ptr == ' ')) {
                ++currentindent;
                ++ptr;
            }

            if(ptr == (BYTEPTR) DecompStringEnd) {
                // ONLY CHANGE INDENTING IF THE CURRENT LINE HASN'T STARTED YET
                indent -= 2;
                if(indent < 0)
                    indent = 0;
                DecompHints = SET_INDENT(DecompHints, indent);

                if(currentindent > indent)
                    DecompStringEnd =
                            (WORDPTR) (((BYTEPTR) DecompStringEnd) -
                            (currentindent - indent));
                else {
                    while(currentindent < indent) {
                        rplDecompAppendChar(' ');
                        ++currentindent;
                    }
                }
            }

        }

        if(dhints & HINT_ALLAFTER) {
            // TODO: APPLY FORMATTING AFTER THE OBJECT
            if(dhints & HINT_ADDINDENTAFTER) {
                indent += 2;
                DecompHints = SET_INDENT(DecompHints, indent);
            }
            if(dhints & HINT_SUBINDENTAFTER) {
                indent -= 2;
                if(indent < 0)
                    indent = 0;
                DecompHints = SET_INDENT(DecompHints, indent);
            }
            if(dhints & HINT_NLAFTER) {

                // DETERMINE THE INDENT OF THE CURRENT LINE

                //while( (ptr<(BYTEPTR)DecompStringEnd)&&(*ptr==' ')) { ++indent; ++ptr; }

                rplDecompAppendChar('\n');
                int k;
                for(k = 0; k < indent; ++k)
                    rplDecompAppendChar(' ');   // APPLY INDENT

                return 1;
            }
        }

    }
    return 0;
}
