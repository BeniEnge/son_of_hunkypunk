#ifdef RCSID
static char RCSid[] =
"$Header: d:/cvsroot/tads/TADS2/MCS.C,v 1.2 1999/05/17 02:52:12 MJRoberts Exp $";
#endif

/* 
 *   Copyright (c) 1991, 2002 Michael J. Roberts.  All Rights Reserved.
 *   
 *   Please see the accompanying license file, LICENSE.TXT, for information
 *   on using and copying this software.  
 */
/*
Name
  mcs.c - memory cache swapping manager
Function
  Implements swapping functions for cache manager
Notes
  None
Modified
  08/04/91 MJRoberts     - creation
*/

#include <assert.h>
#include <stdio.h>
#include "os.h"
#include "std.h"
#include "err.h"
#include "mcs.h"
#include "mch.h"
#include "mcm.h"

/* initialize swapper:  allocate memory for swap page table */
void mcsini(mcscxdef *ctx, mcmcx1def *gmemctx, ulong maxsiz,
            osfildef *fp, char *swapfilename, errcxdef *errctx)
{
    uchar  *p;

    ctx->mcscxtab = (mcsdsdef **)0;                   /* anticipate failure */
    
    /* allocate space from the low-level heap for page table and one page */
    p = mchalo(errctx, (ushort)((MCSPAGETAB * sizeof(mcsdsdef *))
                                + (MCSPAGECNT * sizeof(mcsdsdef))), "mcsini");
    
    /* set up the context with pointers to this chunk */
    ctx->mcscxtab = (mcsdsdef **)p;
    memset(p, 0, (size_t)(MCSPAGETAB * sizeof(mcsdsdef *)));
    p += MCSPAGETAB * sizeof(mcsdsdef *);
    ctx->mcscxtab[0] = (mcsdsdef *)p;
    
    /* set up the rest of the context */
    ctx->mcscxtop = (ulong)0;
    ctx->mcscxmax = maxsiz;
    ctx->mcscxmsg = 0;
    ctx->mcscxfp  = fp;
    ctx->mcscxerr = errctx;
    ctx->mcscxmem = gmemctx;

    /* 
     *   store the swap filename - make a copy so that the caller doesn't
     *   have to retain the original copy (in case it's on the stack) 
     */
    if (swapfilename != 0)
    {
        ctx->mcscxfname = (char *)mchalo(errctx,
                                         (ushort)(strlen(swapfilename)+1),
                                         "mcsini");
        strcpy(ctx->mcscxfname, swapfilename);
    }
    else
        ctx->mcscxfname = 0;
}

/* close the swapper */
void mcsclose(mcscxdef *ctx)
{
    if (ctx->mcscxtab) mchfre(ctx->mcscxtab);
}

/*
 *   Attempt to compact the swap file when it grows too big.  The segment
 *   descriptors are always allocated in increasing seek location within
 *   the swap file.  To compress the file, make each descriptor's
 *   allocated size equal its used size for each in-use segment, and leave
 *   free segments at their allocated sizes.
 */
static void mcscompact(mcscxdef *ctx)
{
    char      buf[512];
    ulong     max;
    mcsseg    cur_in;
    mcsseg    cur_out;
    mcsdsdef *desc_in;
    mcsdsdef *desc_out;
    uint      siz;
    uint      rdsiz;
    ulong     ptr_in;
    ulong     ptr_out;
    
    max = 0;                            /* start at offset zero within file */
    for (cur_in = cur_out = 0 ; cur_in < ctx->mcscxmsg ; ++cur_in)
    {
        desc_in = mcsdsc(ctx, cur_in);
        
        /*
         *   If the present descriptor's address is wrong, and the swap
         *   segment is in use, move the swap segment.  If it's not in
         *   use, we don't need to move it, because we're going to throw
         *   away the segment entirely.  
         */
        if (desc_in->mcsdsptr != max
            && (desc_in->mcsdsflg & MCSDSFINUSE))
        {
            /* ptr_in is the old location, ptr_out is the new location */
            ptr_in = desc_in->mcsdsptr;
            ptr_out = max;
            
            /* copy through our buffer */
            for (siz = desc_in->mcsdsosz ; siz ; siz -= rdsiz)
            {
                /* size is whole buffer, or last piece if smaller */
                rdsiz = (siz > sizeof(buf) ? sizeof(buf) : siz);

                /* seek to old location and get the piece */
                osfseek(ctx->mcscxfp, ptr_in, OSFSK_SET);
                osfrb(ctx->mcscxfp, buf, (size_t)rdsiz);
                
                /* seek to new location and write the piece */
                osfseek(ctx->mcscxfp, ptr_out, OSFSK_SET);
                osfwb(ctx->mcscxfp, buf, (size_t)rdsiz);
                
                /* adjust the pointers by the size copied */
                ptr_in += rdsiz;
                ptr_out += rdsiz;
            }
        }
        
        /* adjust object descriptor to reflect new location */
        desc_in->mcsdsptr = max;
        
        /*
         *   Make current object's size exact if it's in use.  If it's
         *   not in use, delete the segment altogether.
         */
        if (desc_in->mcsdsflg & MCSDSFINUSE)
        {
            desc_in->mcsdssiz = desc_in->mcsdsosz;
            max += desc_in->mcsdssiz;
            
            /* copy descriptor to correct position to close any holes */
            if (cur_out != cur_in)
            {
                desc_out = mcsdsc(ctx, cur_out);
                OSCPYSTRUCT(*desc_out, *desc_in);
                
                /* we need to renumber the corresponding object as well */
                mcmcsw(ctx->mcscxmem, (mcmon)desc_in->mcsdsobj,
                       cur_out, cur_in);
            }
            
            /* we actually wrote this one, so move output pointer */
            ++cur_out;
        }
        else
        {
            /*
             *   We need to renumber the corresponding object so that it
             *   knows there is no swap segment for it any more.  
             */
            mcmcsw(ctx->mcscxmem, (mcmon)desc_in->mcsdsobj,
                   MCSSEGINV, cur_in);
        }
    }
    
    /*
     *   Adjust the top of the file for our new size, and add the savings
     *   into the available space counter.  Also, adjust the total handle
     *   count to reflect any descriptors that we've deleted.
     */
    ctx->mcscxmax += (ctx->mcscxtop - max);
    ctx->mcscxtop = max;
    ctx->mcscxmsg = cur_out;
}

/* swap an object out to the swap file */
mcsseg mcsout(mcscxdef *ctx, uint objid, uchar *ptr, ushort siz,
              mcsseg oldseg, int dirty)
{
    mcsdsdef  *desc;
    mcsdsdef **pagep;
    uint       i;
    uint       j;
    mcsseg     min;
    mcsseg     cur;
    ushort     minsiz;
    
    IF_DEBUG(printf("<< mcsout: objid=%d, ptr=%lx, siz=%u, oldseg=%u >>\n",
                    objid, (unsigned long)ptr, siz, oldseg));
    
    /* see if old segment can be reused */
    if (oldseg != MCSSEGINV)
    {
        desc = mcsdsc(ctx, oldseg);
        if (!(desc->mcsdsflg & MCSDSFINUSE)     /* if old seg is not in use */
            && desc->mcsdsobj == objid            /* and it has same object */
            && desc->mcsdssiz >= siz           /* and it's still big enough */
            && !dirty)      /* and the object in memory hasn't been changed */
        {
            /* we can reuse the old segment without rewriting it */
            desc->mcsdsflg |= MCSDSFINUSE;        /* mark segment as in use */
            return(oldseg);
        }
    }
    
    /* look for the smallest unused segment big enough for this object */
    for (cur = 0, min = MCSSEGINV, i = 0, pagep = ctx->mcscxtab
         ; cur < ctx->mcscxmsg && i < MCSPAGETAB && *pagep ; ++pagep, ++i)
    {
        for (j = 0, desc = *pagep ; cur < ctx->mcscxmsg && j < MCSPAGECNT
             ; ++desc, ++j, ++cur)
        {
            if (!(desc->mcsdsflg & MCSDSFINUSE)
                && desc->mcsdssiz >= siz
                && (min == MCSSEGINV || desc->mcsdssiz < minsiz))
            {
                min = cur;
                minsiz = desc->mcsdssiz;
                if (minsiz == siz) break;       /* exact match - we're done */
            }
        }
        /* quit if we found an exact match */
        if (min != MCSSEGINV && minsiz == siz) break;
    }
    
    /* if we found nothing, allocate a new segment if possible */
    if (min == MCSSEGINV)
    {
        if (siz > ctx->mcscxmax)
        {
            /* swap file is too big; compact it and try again */
            mcscompact(ctx);
            if (siz > ctx->mcscxmax)
                errsig(ctx->mcscxerr, ERR_SWAPBIG);
        }
            
        min = ctx->mcscxmsg;
        if ((min >> 8) >= MCSPAGETAB)      /* exceeded pages in page table? */
            errsig(ctx->mcscxerr, ERR_SWAPPG);
        
        if (!ctx->mcscxtab[min >> 8])         /* haven't allocate page yet? */
        {
            ctx->mcscxtab[min >> 8] = 
                (mcsdsdef *)mchalo(ctx->mcscxerr,
                                   (ushort)(MCSPAGECNT * sizeof(mcsdsdef)),
                                   "mcsout");
        }

        /* set up new descriptor */
        desc = mcsdsc(ctx, min);
        desc->mcsdsptr = ctx->mcscxtop;
        desc->mcsdssiz = siz;
        desc->mcsdsobj = objid;

        /* write out the segment */
        mcswrt(ctx, desc, ptr, siz);
        desc->mcsdsflg = MCSDSFINUSE;

        /* update context information to account for new segment */
        ctx->mcscxtop += siz;             /* add to top seek offset in file */
        ctx->mcscxmax -= siz;                     /* take size out of quota */
        ctx->mcscxmsg++;                /* increment last segment allocated */
        
        return(min);
    }
    else
    {
        desc = mcsdsc(ctx, min);
        desc->mcsdsobj = objid;
        mcswrt(ctx, desc, ptr, siz);
        desc->mcsdsflg |= MCSDSFINUSE;

        return(min);
    }
}

void mcsin(mcscxdef *ctx, mcsseg seg, uchar *ptr, ushort siz)
{
    mcsdsdef *desc = mcsdsc(ctx, seg);

    IF_DEBUG(printf("<< mcsin: seg=%u, ptr=%lx, siz=%d, objid=%u >>\n",
                    seg, (unsigned long)ptr, siz, desc->mcsdsobj));

    assert(seg < ctx->mcscxmsg);

    /* can only swap in as much as we wrote */
    if (desc->mcsdsosz < siz) siz = desc->mcsdsosz;

    /* seek to and read the segment */
    if (osfseek(ctx->mcscxfp, desc->mcsdsptr, OSFSK_SET))
        errsig(ctx->mcscxerr, ERR_FSEEK);
    if (osfrb(ctx->mcscxfp, ptr, (size_t)siz))
        errsig(ctx->mcscxerr, ERR_FREAD);
    
    desc->mcsdsflg &= ~MCSDSFINUSE;             /* segment no longer in use */
}

void mcswrt(mcscxdef *ctx, mcsdsdef *desc, uchar *buf, ushort bufl)
{
    int tries;
    
    desc->mcsdsosz = bufl;
    
    for (tries = 0 ; tries < 2 ; ++tries)
    {
        /* attempt to write the object to the swap file */
        if (osfseek(ctx->mcscxfp, desc->mcsdsptr, OSFSK_SET))
            errsig(ctx->mcscxerr, ERR_FSEEK);
        if (!osfwb(ctx->mcscxfp, buf, (size_t)bufl))
            return;
        
        /* couldn't write it; compact the swap file */
        mcscompact(ctx);
    }
    
    /* couldn't write to swap file, even after compacting it */
    errsig(ctx->mcscxerr, ERR_FWRITE);
}

