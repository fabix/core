/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <fncall.h>

#include <env_context.h>
#include <files_names.h>
#include <expand.h>
#include <vars.h>
#include <evalfunction.h>
#include <policy.h>
#include <string_lib.h>
#include <promises.h>
#include <syntax.h>
#include <audit.h>

/******************************************************************/
/* Argument propagation                                           */
/******************************************************************/

/*

When formal parameters are passed, they should be literal strings, i.e.
values (check for this). But when the values are received the
receiving body should state only variable names without literal quotes.
That way we can feed in the received parameter name directly in as an lvalue

e.g.
       access => myaccess("$(person)"),

       body files myaccess(user)

leads to Hash Association (lval,rval) => (user,"$(person)")

*/

/******************************************************************/

static Rlist *NewExpArgs(EvalContext *ctx, const FnCall *fp, const Promise *pp)
{
    int len;
    Rval rval;
    Rlist *newargs = NULL;
    FnCall *subfp;
    const FnCallType *fn = FnCallTypeGet(fp->name);

    len = RlistLen(fp->args);

    if (!fn->varargs)
    {
        if (len != FnNumArgs(fn))
        {
            Log(LOG_LEVEL_ERR, "Arguments to function %s(.) do not tally. Expect %d not %d",
                  fp->name, FnNumArgs(fn), len);
            PromiseRef(LOG_LEVEL_ERR, pp);
            exit(1);
        }
    }

    for (const Rlist *rp = fp->args; rp != NULL; rp = rp->next)
    {
        switch (rp->val.type)
        {
        case RVAL_TYPE_FNCALL:
            subfp = RlistFnCallValue(rp);
            rval = FnCallEvaluate(ctx, subfp, pp).rval;
            break;
        default:
            rval = ExpandPrivateRval(ctx, NULL, NULL, (Rval) { rp->val.item, rp->val.type});
            break;
        }

        RlistAppend(&newargs, rval.item, rval.type);
        RvalDestroy(rval);
    }

    return newargs;
}

/******************************************************************/

static void DeleteExpArgs(Rlist *args)
{
    RlistDestroy(args);
}

/*******************************************************************/

bool FnCallIsBuiltIn(Rval rval)
{
    FnCall *fp;

    if (rval.type != RVAL_TYPE_FNCALL)
    {
        return false;
    }

    fp = (FnCall *) rval.item;

    if (FnCallTypeGet(fp->name))
    {
        return true;
    }
    else
    {
        return false;
    }
}

/*******************************************************************/

FnCall *FnCallNew(const char *name, Rlist *args)
{
    FnCall *fp;

    fp = xmalloc(sizeof(FnCall));

    fp->name = xstrdup(name);
    fp->args = args;

    return fp;
}

/*******************************************************************/

FnCall *FnCallCopy(const FnCall *f)
{
    return FnCallNew(f->name, RlistCopy(f->args));
}

/*******************************************************************/

void FnCallDestroy(FnCall *fp)
{
    if (fp)
    {
        free(fp->name);
        RlistDestroy(fp->args);
    }
    free(fp);
}

unsigned FnCallHash(const FnCall *fp, unsigned seed, unsigned max)
{
    unsigned hash = StringHash(fp->name, seed, max);

    for (const Rlist *rp = fp->args; rp; rp = rp->next)
    {
        hash = RvalHash(rp->val, hash, max);
    }

    return hash;
}


FnCall *ExpandFnCall(EvalContext *ctx, const char *ns, const char *scope, FnCall *f)
{
    return FnCallNew(f->name, ExpandList(ctx, ns, scope, f->args, false));
}


/*******************************************************************/

void FnCallShow(FILE *fout, const FnCall *fp)
{
    fprintf(fout, "%s(", fp->name);

    for (const Rlist *rp = fp->args; rp != NULL; rp = rp->next)
    {
        switch (rp->val.type)
        {
        case RVAL_TYPE_SCALAR:
            fprintf(fout, "%s,", RlistScalarValue(rp));
            break;

        case RVAL_TYPE_FNCALL:
            FnCallShow(fout, RlistFnCallValue(rp));
            break;

        default:
            fprintf(fout, "(** Unknown argument **)\n");
            break;
        }
    }

    fprintf(fout, ")");
}

/*******************************************************************/

static FnCallResult CallFunction(EvalContext *ctx, FnCall *fp, Rlist *expargs)
{
    Rlist *rp = fp->args;
    const FnCallType *fncall_type = FnCallTypeGet(fp->name);

    int argnum = 0;
    for (argnum = 0; rp != NULL && fncall_type->args[argnum].pattern != NULL; argnum++)
    {
        if (rp->val.type != RVAL_TYPE_FNCALL)
        {
            /* Nested functions will not match to lval so don't bother checking */
            SyntaxTypeMatch err = CheckConstraintTypeMatch(fp->name, rp->val,
                                                           fncall_type->args[argnum].dtype,
                                                           fncall_type->args[argnum].pattern, 1);
            if (err != SYNTAX_TYPE_MATCH_OK && err != SYNTAX_TYPE_MATCH_ERROR_UNEXPANDED)
            {
                FatalError(ctx, "In function '%s', '%s'", fp->name, SyntaxTypeMatchToString(err));
            }
        }

        rp = rp->next;
    }

    char output[CF_BUFSIZE];
    if (argnum != RlistLen(expargs) && !fncall_type->varargs)
    {
        snprintf(output, CF_BUFSIZE, "Argument template mismatch handling function %s(", fp->name);
        RlistShow(stderr, expargs);
        fprintf(stderr, ")\n");

        rp = expargs;
        for (int i = 0; i < argnum; i++)
        {
            printf("  arg[%d] range %s\t", i, fncall_type->args[i].pattern);
            if (rp != NULL)
            {
                RvalShow(stdout, rp->val);
                rp = rp->next;
            }
            else
            {
                printf(" ? ");
            }
            printf("\n");
        }

        FatalError(ctx, "Bad arguments");
    }


    return (*fncall_type->impl) (ctx, fp, expargs);
}

FnCallResult FnCallEvaluate(EvalContext *ctx, FnCall *fp, const Promise *caller)
{
    if (!(ctx->eval_options & EVAL_OPTION_EVAL_FUNCTIONS))
    {
        Log(LOG_LEVEL_VERBOSE, "Skipping function '%s', because evaluation was turned off in the evaluator",
            fp->name);
        return (FnCallResult) { FNCALL_FAILURE, { FnCallCopy(fp), RVAL_TYPE_FNCALL } };
    }
    else if (!EvalContextPromiseIsActive(ctx, caller))
    {
        Log(LOG_LEVEL_VERBOSE, "Skipping function '%s', because it was excluded by classes", fp->name);
        return (FnCallResult) { FNCALL_FAILURE, { FnCallCopy(fp), RVAL_TYPE_FNCALL } };
    }


    Rlist *expargs;
    const FnCallType *fp_type = FnCallTypeGet(fp->name);

    if (!fp_type)
    {
        if (caller)
        {
            Log(LOG_LEVEL_ERR, "No such FnCall \"%s()\" in promise @ %s near line %zd",
                  fp->name, PromiseGetBundle(caller)->source_path, caller->offset.line);
        }
        else
        {
            Log(LOG_LEVEL_ERR, "No such FnCall \"%s()\" - context info unavailable", fp->name);
        }

        return (FnCallResult) { FNCALL_FAILURE, { FnCallCopy(fp), RVAL_TYPE_FNCALL } };
    }

/* If the container classes seem not to be defined at this stage, then don't try to expand the function */

    if ((caller != NULL) && !IsDefinedClass(ctx, caller->classes, PromiseGetNamespace(caller)))
    {
        return (FnCallResult) { FNCALL_FAILURE, { FnCallCopy(fp), RVAL_TYPE_FNCALL } };
    }

    expargs = NewExpArgs(ctx, fp, caller);

    if (UnresolvedArgs(expargs))
    {
        DeleteExpArgs(expargs);
        return (FnCallResult) { FNCALL_FAILURE, { FnCallCopy(fp), RVAL_TYPE_FNCALL } };
    }

    fp->caller = caller;

    FnCallResult result = CallFunction(ctx, fp, expargs);

    if (result.status == FNCALL_FAILURE)
    {
        /* We do not assign variables to failed function calls */
        DeleteExpArgs(expargs);
        return (FnCallResult) { FNCALL_FAILURE, { FnCallCopy(fp), RVAL_TYPE_FNCALL } };
    }

    DeleteExpArgs(expargs);
    return result;
}

/*******************************************************************/

const FnCallType *FnCallTypeGet(const char *name)
{
    int i;

    for (i = 0; CF_FNCALL_TYPES[i].name != NULL; i++)
    {
        if (strcmp(CF_FNCALL_TYPES[i].name, name) == 0)
        {
            return CF_FNCALL_TYPES + i;
        }
    }

    return NULL;
}
