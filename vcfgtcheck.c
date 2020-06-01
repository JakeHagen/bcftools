/*  vcfgtcheck.c -- Check sample identity.

    Copyright (C) 2013-2020 Genome Research Ltd.

    Author: Petr Danecek <pd3@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.  */

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <htslib/vcf.h>
#include <htslib/synced_bcf_reader.h>
#include <htslib/vcfutils.h>
#include <htslib/kbitset.h>
#include <inttypes.h>
#include <sys/time.h>
#include "bcftools.h"
#include "extsort.h"
//#include "hclust.h"

typedef struct
{
    int iqry, igt;
}
pair_t;

typedef struct
{
    bcf_srs_t *files;           // first reader is the query VCF - single sample normally or multi-sample for cross-check
    bcf_hdr_t *gt_hdr, *qry_hdr; // VCF with genotypes to compare against and the query VCF
    char *cwd, **argv, *gt_samples, *qry_samples, *regions, *targets, *qry_fname, *gt_fname, *pair_samples;
    int argc, gt_samples_is_file, qry_samples_is_file, regions_is_file, targets_is_file, pair_samples_is_file;
    int qry_use_GT,gt_use_GT, nqry_smpl,ngt_smpl, *qry_smpl,*gt_smpl;
    uint32_t *ndiff,*ncnt,ncmp, npairs;
    int32_t *qry_arr,*gt_arr, nqry_arr,ngt_arr;
    pair_t *pairs;
    double *hwe_prob;
    double min_inter_err, max_intra_err;
    int all_sites, hom_only, ntop, cross_check, calc_hwe_prob, sort_by_hwe, dry_run;
    FILE *fp;

    // for --distinctive-sites
    double distinctive_sites;
    kbitset_t *kbs_diff;
    size_t diff_sites_size;
    extsort_t *es;
    char *es_tmp_dir, *es_max_mem;
}
args_t;

static void throw_and_clean(args_t *args, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    if ( args->es )
    {
        fprintf(stderr,"Cleaning\n");
        extsort_destroy(args->es);
    }
    exit(-1);
}

static void set_cwd(args_t *args)
{
    int i;
    char *buf;
    size_t nbuf = 500;
    args->cwd = (char*) malloc(sizeof(char)*nbuf);
    for (i=0; i<5; i++)
    {
        if ( (buf = getcwd(args->cwd, nbuf)) ) break;
        nbuf *= 2;
        args->cwd = (char*) realloc(args->cwd, sizeof(char)*nbuf);
    }
    assert(buf);
}
static void print_header(args_t *args, FILE *fp)
{
    fprintf(fp, "# This file was produced by bcftools (%s+htslib-%s), the command line was:\n", bcftools_version(), hts_version());
    fprintf(fp, "# \t bcftools %s ", args->argv[0]);
    int i;
    for (i=1; i<args->argc; i++)
        fprintf(fp, " %s",args->argv[i]);
    fprintf(fp, "\n# and the working directory was:\n");
    fprintf(fp, "# \t %s\n#\n", args->cwd);
}

static int cmp_int(const void *_a, const void *_b)
{
    int a = *((int*)_a);
    int b = *((int*)_b);
    if ( a < b ) return -1;
    if ( a > b ) return 1;
    return 0;
}
static int cmp_pair(const void *_a, const void *_b)
{
    pair_t *a = (pair_t*)_a;
    pair_t *b = (pair_t*)_b;
    if ( a->iqry < b->iqry ) return -1;
    if ( a->iqry > b->iqry ) return 1;
    if ( a->igt < b->igt ) return -1;
    if ( a->igt > b->igt ) return 1;
    return 0;
}


typedef struct
{
    uint32_t ndiff,rid,pos,rand; // rand is to shuffle sites with the same ndiff from across all chromosoms
    unsigned long kbs_dat[1];
}
diff_sites_t;
static int diff_sites_cmp(const void *aptr, const void *bptr)
{
    diff_sites_t *a = (diff_sites_t*)aptr;
    diff_sites_t *b = (diff_sites_t*)bptr;
    if ( a->ndiff < b->ndiff ) return 1;        // descending order
    if ( a->ndiff > b->ndiff ) return -1;
    if ( a->rand < b->rand ) return -1;
    if ( a->rand > b->rand ) return 1;
    return 0;
}
static void diff_sites_init(args_t *args)
{
    int nsites = args->distinctive_sites<=1 ? args->npairs*args->distinctive_sites : args->distinctive_sites;
    if ( nsites<=0 ) throw_and_clean(args,"The value for --distinctive-sites was set too low: %d\n",nsites);
    if ( nsites>args->npairs )
    {
        fprintf(stderr,"The value for --distinctive-sites was set too high, setting to all pairs (%d) instead\n",args->npairs);
        nsites = args->npairs;
    }
    args->distinctive_sites = nsites;
    args->kbs_diff = kbs_init(args->npairs);
    size_t n = (args->npairs + KBS_ELTBITS-1) / KBS_ELTBITS;
    assert( n==args->kbs_diff->n );
    args->diff_sites_size = sizeof(diff_sites_t) + (n-1)*sizeof(unsigned long);
    args->es = extsort_alloc();
    extsort_set_opt(args->es,size_t,DAT_SIZE,args->diff_sites_size);
    extsort_set_opt(args->es,const char*,TMP_DIR,args->es_tmp_dir);
    extsort_set_opt(args->es,const char*,MAX_MEM,args->es_max_mem);
    extsort_set_opt(args->es,extsort_cmp_f,FUNC_CMP,diff_sites_cmp);
    extsort_init(args->es);
}
static void diff_sites_destroy(args_t *args)
{
    kbs_destroy(args->kbs_diff);
    extsort_destroy(args->es);
}
static inline void diff_sites_reset(args_t *args)
{
    kbs_clear(args->kbs_diff);
}
static inline void diff_sites_push(args_t *args, int ndiff, int rid, int pos)
{
    diff_sites_t *dat = (diff_sites_t*) malloc(args->diff_sites_size);
    memset(dat,0,sizeof(*dat)); // for debugging: prevent warnings about uninitialized memory coming from struct padding (not needed after rand added)
    dat->ndiff = ndiff;
    dat->rid  = rid;
    dat->pos  = pos;
    dat->rand = (uint32_t)rand();
    memcpy(dat->kbs_dat,args->kbs_diff->b,args->kbs_diff->n*sizeof(unsigned long));
    extsort_push(args->es,dat);
}
static inline int diff_sites_shift(args_t *args, int *ndiff, int *rid, int *pos)
{
    diff_sites_t *dat = (diff_sites_t*) extsort_shift(args->es);
    if ( !dat ) return 0;
    *ndiff = dat->ndiff;
    *rid   = dat->rid;
    *pos   = dat->pos;
    memcpy(args->kbs_diff->b,dat->kbs_dat,args->kbs_diff->n*sizeof(unsigned long));
    return 1;
}

static void init_data(args_t *args)
{
    args->files = bcf_sr_init();
    if ( args->regions && bcf_sr_set_regions(args->files, args->regions, args->regions_is_file)<0 ) throw_and_clean(args,"Failed to read the regions: %s\n", args->regions);
    if ( args->targets && bcf_sr_set_targets(args->files, args->targets, args->targets_is_file, 0)<0 ) throw_and_clean(args,"Failed to read the targets: %s\n", args->targets);

    if ( args->gt_fname ) bcf_sr_set_opt(args->files, BCF_SR_REQUIRE_IDX);
    if ( !bcf_sr_add_reader(args->files,args->qry_fname) ) throw_and_clean(args,"Failed to open %s: %s\n", args->qry_fname,bcf_sr_strerror(args->files->errnum));
    if ( args->gt_fname && !bcf_sr_add_reader(args->files, args->gt_fname) )
        throw_and_clean(args,"Failed to read from %s: %s\n", !strcmp("-",args->gt_fname)?"standard input":args->gt_fname,bcf_sr_strerror(args->files->errnum));

    args->qry_hdr = bcf_sr_get_header(args->files,0);
    if ( !bcf_hdr_nsamples(args->qry_hdr) ) throw_and_clean(args,"No samples in %s?\n", args->qry_fname);
    if ( args->gt_fname )
    {
        args->gt_hdr = bcf_sr_get_header(args->files,1);
        if ( !bcf_hdr_nsamples(args->gt_hdr) ) throw_and_clean(args,"No samples in %s?\n", args->gt_fname);
    }

    // Determine whether GT or PL will be used
    if ( args->qry_use_GT==-1 ) // not set by -u, qry uses PL by default
    {
        if ( bcf_hdr_id2int(args->qry_hdr,BCF_DT_ID,"PL")>=0 )
            args->qry_use_GT = 0;
        else if ( bcf_hdr_id2int(args->qry_hdr,BCF_DT_ID,"GT")>=0 )
            args->qry_use_GT = 1;
        else
            throw_and_clean(args,"[E::%s] Neither PL nor GT tag is present in the header of %s\n", __func__, args->qry_fname);
    }
    else if ( args->qry_use_GT==1 )
    {
        if ( bcf_hdr_id2int(args->qry_hdr,BCF_DT_ID,"GT")<0 )
            throw_and_clean(args,"[E::%s] The GT tag is not present in the header of %s\n", __func__, args->qry_fname);
    }
    else if ( bcf_hdr_id2int(args->qry_hdr,BCF_DT_ID,"PL")<0 )
        throw_and_clean(args,"[E::%s] The PL tag is not present in the header of %s\n", __func__, args->qry_fname);

    if ( args->gt_hdr )
    {
        if ( args->gt_use_GT==-1 ) // not set by -u, gt uses GT by default
        {
            if ( bcf_hdr_id2int(args->gt_hdr,BCF_DT_ID,"GT")>=0 )
                args->gt_use_GT = 1;
            else if ( bcf_hdr_id2int(args->gt_hdr,BCF_DT_ID,"PL")>=0 )
                args->gt_use_GT = 0;
            else
                throw_and_clean(args,"[E::%s] Neither PL nor GT tag is present in the header of %s\n", __func__, args->gt_fname);
        }
        else if ( args->gt_use_GT==1 )
        {
            if ( bcf_hdr_id2int(args->gt_hdr,BCF_DT_ID,"GT")<0 )
                throw_and_clean(args,"[E::%s] The GT tag is not present in the header of %s\n", __func__, args->gt_fname);
        }
        else if ( bcf_hdr_id2int(args->gt_hdr,BCF_DT_ID,"PL")<0 )
            throw_and_clean(args,"[E::%s] The PL tag is not present in the header of %s\n", __func__, args->gt_fname);
    }
    else
        args->gt_use_GT = args->qry_use_GT;

    // Prepare samples
    int i;
    args->nqry_smpl = bcf_hdr_nsamples(args->qry_hdr);
    if ( args->qry_samples )
    {
        char **tmp = hts_readlist(args->qry_samples, args->qry_samples_is_file, &args->nqry_smpl);
        if ( !tmp || !args->nqry_smpl ) throw_and_clean(args,"Failed to parse %s\n", args->qry_samples);
        args->qry_smpl = (int*) malloc(sizeof(*args->qry_smpl)*args->nqry_smpl);
        for (i=0; i<args->nqry_smpl; i++)
        {
            int idx = bcf_hdr_id2int(args->qry_hdr, BCF_DT_SAMPLE, tmp[i]);
            if ( idx<0 ) throw_and_clean(args,"No such sample in %s: [%s]\n",args->qry_fname,tmp[i]);
            // todo: add a check to prevent duplicates
            args->qry_smpl[i] = idx;
            free(tmp[i]);
        }
        free(tmp);
        qsort(args->qry_smpl,args->nqry_smpl,sizeof(*args->qry_smpl),cmp_int);
    }
    if ( args->gt_hdr )
    {
        args->ngt_smpl = bcf_hdr_nsamples(args->gt_hdr);
        if ( args->gt_samples )
        {
            char **tmp = hts_readlist(args->gt_samples, args->gt_samples_is_file, &args->ngt_smpl);
            if ( !tmp || !args->ngt_smpl ) throw_and_clean(args,"Failed to parse %s\n", args->gt_samples);
            args->gt_smpl = (int*) malloc(sizeof(*args->gt_smpl)*args->ngt_smpl);
            for (i=0; i<args->ngt_smpl; i++)
            {
                int idx = bcf_hdr_id2int(args->gt_hdr, BCF_DT_SAMPLE, tmp[i]);
                if ( idx<0 ) throw_and_clean(args,"No such sample in %s: [%s]\n",args->gt_fname,tmp[i]);
                // todo: add a check to prevent duplicates
                args->gt_smpl[i] = idx;
                free(tmp[i]);
            }
            free(tmp);
            qsort(args->gt_smpl,args->ngt_smpl,sizeof(*args->gt_smpl),cmp_int);
        }
    }
    else if ( args->gt_samples )
    {
        char **tmp = hts_readlist(args->gt_samples, args->gt_samples_is_file, &args->ngt_smpl);
        if ( !tmp || !args->ngt_smpl ) throw_and_clean(args,"Failed to parse %s\n", args->gt_samples);
        args->gt_smpl = (int*) malloc(sizeof(*args->gt_smpl)*args->ngt_smpl);
        for (i=0; i<args->ngt_smpl; i++)
        {
            int idx = bcf_hdr_id2int(args->qry_hdr, BCF_DT_SAMPLE, tmp[i]);
            if ( idx<0 ) throw_and_clean(args,"No such sample in %s: [%s]\n",args->gt_fname,tmp[i]);
            // todo: add a check to prevent duplicates
            args->gt_smpl[i] = idx;
            free(tmp[i]);
        }
        free(tmp);
        qsort(args->gt_smpl,args->ngt_smpl,sizeof(*args->gt_smpl),cmp_int);
    }
    else if ( args->pair_samples )
    {
        int npairs;
        char **tmp = hts_readlist(args->pair_samples, args->pair_samples_is_file, &npairs);
        if ( !tmp || !npairs ) throw_and_clean(args,"Failed to parse %s\n", args->pair_samples);
        if ( !args->pair_samples_is_file && npairs%2 ) throw_and_clean(args,"Expected even number of comma-delimited samples with -p\n");
        args->npairs = args->pair_samples_is_file ? npairs : npairs/2;
        args->pairs  = (pair_t*) calloc(args->npairs,sizeof(*args->pairs));
        if ( !args->pair_samples_is_file )
        {
            for (i=0; i<args->npairs; i++)
            {
                args->pairs[i].iqry = bcf_hdr_id2int(args->qry_hdr, BCF_DT_SAMPLE, tmp[2*i]);
                args->pairs[i].igt  = bcf_hdr_id2int(args->gt_hdr?args->gt_hdr:args->qry_hdr, BCF_DT_SAMPLE, tmp[2*i+1]);
                if ( args->pairs[i].iqry < 0 ) throw_and_clean(args,"No such sample in %s: [%s]\n",args->qry_fname,tmp[2*i]);
                if ( args->pairs[i].igt  < 0 ) throw_and_clean(args,"No such sample in %s: [%s]\n",args->gt_fname?args->gt_fname:args->qry_fname,tmp[2*i+1]);
                free(tmp[2*i]);
                free(tmp[2*i+1]);
            }
        }
        else
        {
            for (i=0; i<args->npairs; i++)
            {
                char *ptr = tmp[i];
                while ( *ptr && !isspace(*ptr) ) ptr++;
                if ( !*ptr ) throw_and_clean(args,"Could not parse %s: %s\n",args->pair_samples,tmp[i]);
                *ptr = 0;
                args->pairs[i].iqry = bcf_hdr_id2int(args->qry_hdr, BCF_DT_SAMPLE, tmp[i]);
                if ( args->pairs[i].iqry < 0 ) throw_and_clean(args,"No such sample in %s: [%s]\n",args->qry_fname,tmp[i]);
                ptr++;
                while ( *ptr && isspace(*ptr) ) ptr++;
                args->pairs[i].igt = bcf_hdr_id2int(args->gt_hdr?args->gt_hdr:args->qry_hdr, BCF_DT_SAMPLE, ptr);
                if ( args->pairs[i].igt < 0 ) throw_and_clean(args,"No such sample in %s: [%s]\n",args->gt_fname?args->gt_fname:args->qry_fname,ptr);
                free(tmp[i]);
            }
        }
        free(tmp);
        qsort(args->pairs,args->npairs,sizeof(*args->pairs),cmp_pair);
    }
    else
    {
        args->ngt_smpl = args->nqry_smpl;
        args->gt_smpl  = args->qry_smpl;
        args->cross_check = 1;
    }

    // The data arrays
    if ( !args->npairs ) args->npairs = args->cross_check ? args->nqry_smpl*(args->nqry_smpl+1)/2 : args->ngt_smpl*args->nqry_smpl;
    args->ndiff = (uint32_t*) calloc(args->npairs,sizeof(*args->ndiff));    // number of differing genotypes for each pair of samples
    args->ncnt  = (uint32_t*) calloc(args->npairs,sizeof(*args->ncnt));     // number of comparisons performed (non-missing data)
    if ( !args->ncnt ) throw_and_clean(args,"Error: failed to allocate %.1f Mb\n", args->npairs*sizeof(*args->ncnt)/1e6);
    if ( args->calc_hwe_prob )
    {
        // prob of the observed sequence of matches given site AFs and HWE
        args->hwe_prob = (double*) calloc(args->npairs,sizeof(*args->hwe_prob));
        if ( !args->hwe_prob ) throw_and_clean(args,"Error: failed to allocate %.1f Mb. Run with --no-HWE-prob to save some memory.\n", args->npairs*sizeof(*args->hwe_prob)/1e6);
    }

    if ( args->distinctive_sites ) diff_sites_init(args);

    args->fp = stdout;
    print_header(args, args->fp);
}

static void destroy_data(args_t *args)
{
    fclose(args->fp);
    if ( args->distinctive_sites ) diff_sites_destroy(args);
    free(args->hwe_prob);
    free(args->cwd);
    free(args->qry_arr);
    if ( args->gt_hdr ) free(args->gt_arr);
    free(args->ndiff);
    free(args->ncnt);
    free(args->qry_smpl);
    if ( args->gt_smpl!=args->qry_smpl ) free(args->gt_smpl);
    free(args->pairs);
    bcf_sr_destroy(args->files);
}

/*
   Return -1 on missing data, 0 on mismatch, 1 on match.
   Note that:
        - currently only diploid, non-missing values are considered
        - with PLs we only compare whether the most likely PL=P(D|G) values match
*/
#define _SLOWER_BRANCH 0
#if _SLOWER_BRANCH
static inline int match_GT_GT(int32_t *aptr, int32_t *bptr, int *adsg, int *bdsg)
{
    if ( bcf_gt_is_missing(aptr[0]) || bcf_gt_is_missing(aptr[1]) || aptr[1]==bcf_int32_vector_end ) return -1;
    if ( bcf_gt_is_missing(bptr[0]) || bcf_gt_is_missing(bptr[1]) || bptr[1]==bcf_int32_vector_end ) return -1;
    *adsg = (bcf_gt_allele(aptr[0])?1:0) + (bcf_gt_allele(aptr[1])?1:0);
    *bdsg = (bcf_gt_allele(bptr[0])?1:0) + (bcf_gt_allele(bptr[1])?1:0);
    return *adsg==*bdsg ? 1 : 0;
}
static inline int match_GT_PL(int32_t *aptr, int32_t *bptr, int *adsg, int *bdsg)
{
    if ( bcf_gt_is_missing(aptr[0]) || bcf_gt_is_missing(aptr[1]) || aptr[1]==bcf_int32_vector_end ) return -1;
    if ( bptr[0]==bcf_int32_missing || bptr[1]==bcf_int32_missing || bptr[2]==bcf_int32_missing || bptr[1]==bcf_int32_vector_end || bptr[2]==bcf_int32_vector_end ) return -1;
    *adsg = (bcf_gt_allele(aptr[0])?1:0) + (bcf_gt_allele(aptr[1])?1:0);
    *bdsg = 0;
    int i, min = bptr[0];
    for (i=1; i<3; i++)
        if ( min > bptr[i] ) { min = bptr[i]; *bdsg = i; }
    return min==bptr[*adsg] ? 1 : 0;
}
static inline int match_PL_PL(int32_t *aptr, int32_t *bptr, int *adsg, int *bdsg)
{
    if ( aptr[0]==bcf_int32_missing || aptr[1]==bcf_int32_missing || aptr[2]==bcf_int32_missing || aptr[1]==bcf_int32_vector_end || aptr[2]==bcf_int32_vector_end ) return -1;
    if ( bptr[0]==bcf_int32_missing || bptr[1]==bcf_int32_missing || bptr[2]==bcf_int32_missing || bptr[1]==bcf_int32_vector_end || bptr[2]==bcf_int32_vector_end ) return -1;
    int i, amin = aptr[0], bmin = bptr[0];
    *adsg = 0; *bdsg = 0;
    for (i=1; i<3; i++)
        if ( amin > aptr[i] ) { amin = aptr[i]; *adsg = i; }
    for (i=1; i<3; i++)
        if ( bmin > bptr[i] ) { bmin = bptr[i]; *bdsg = i; }
    for (i=0; i<3; i++)
        if ( aptr[i]==amin && bptr[i]==bmin ) { *adsg = *bdsg = i; return 1; }
    return 0;
}
#else   /* faster branch for missing data */
#define HAS_GT(ptr) (!bcf_gt_is_missing(ptr[0]) && !bcf_gt_is_missing(ptr[1]) && ptr[1]!=bcf_int32_vector_end)
#define HAS_PL(ptr) (ptr[0]!=bcf_int32_missing && ptr[1]!=bcf_int32_missing && ptr[2]!=bcf_int32_missing && ptr[1]!=bcf_int32_vector_end && ptr[2]!=bcf_int32_vector_end)
#define MIN_PL(ptr) ptr[0]<ptr[1]?(ptr[0]<ptr[2]?ptr[0]:ptr[2]):(ptr[1]<ptr[2]?ptr[1]:ptr[2])
#define DSG_PL(ptr) ptr[0]<ptr[1]?(ptr[0]<ptr[2]?0:2):(ptr[1]<ptr[2]?1:2)
#define DSG_GT(ptr) (bcf_gt_allele(ptr[0])?1:0) + (bcf_gt_allele(ptr[1])?1:0)
#endif

static void process_line(args_t *args)
{
    int nqry1, ngt1;

    bcf1_t *gt_rec = NULL, *qry_rec = bcf_sr_get_line(args->files,0);   // the query file
    if ( args->qry_use_GT )
    {
        if ( (nqry1=bcf_get_genotypes(args->qry_hdr,qry_rec,&args->qry_arr,&args->nqry_arr)) <= 0 ) return;
        if ( nqry1 != 2*bcf_hdr_nsamples(args->qry_hdr) ) return;    // only diploid data for now
        nqry1 = 2;
    }
    else
    {
        if ( (nqry1=bcf_get_format_int32(args->qry_hdr,qry_rec,"PL",&args->qry_arr,&args->nqry_arr)) <= 0 ) return;
        if ( nqry1 != 3*bcf_hdr_nsamples(args->qry_hdr) ) return;    // not diploid
        nqry1 = 3;
    }

    if ( args->gt_hdr )
    {
        gt_rec = bcf_sr_get_line(args->files,1);
        if ( args->gt_use_GT )
        {
            if ( (ngt1=bcf_get_genotypes(args->gt_hdr,gt_rec,&args->gt_arr,&args->ngt_arr)) <= 0 ) return;
            if ( ngt1 != 2*bcf_hdr_nsamples(args->gt_hdr) ) return;    // not diploid
            ngt1 = 2;
        }
        else
        {
            if ( (ngt1=bcf_get_format_int32(args->gt_hdr,gt_rec,"PL",&args->gt_arr,&args->ngt_arr)) <= 0 ) return;
            if ( ngt1 != 3*bcf_hdr_nsamples(args->gt_hdr) ) return;    // not diploid
            ngt1 = 3;
        }
    }
    else
    {
        ngt1 = nqry1;
        args->gt_arr = args->qry_arr;
    }

    args->ncmp++;

    double af,hwe[3];
    if ( args->calc_hwe_prob )
    {
        int ac[2];
        if ( args->gt_hdr )
        {
            if ( bcf_calc_ac(args->gt_hdr, gt_rec, ac, BCF_UN_INFO|BCF_UN_FMT)!=1 ) throw_and_clean(args,"todo: bcf_calc_ac() failed\n");
        }
        else if ( bcf_calc_ac(args->qry_hdr, qry_rec, ac, BCF_UN_INFO|BCF_UN_FMT)!=1 ) throw_and_clean(args,"todo: bcf_calc_ac() failed\n");

        const double min_af = 1e-3;
        af = (double)ac[1]/(ac[0]+ac[1]);
        hwe[0] = af>min_af ? -log(af*af) : -log(min_af*min_af);
        hwe[1] = af>min_af && af<1-min_af ? -log(2*af*(1-af)) : -log(2*min_af*(1-min_af));
        hwe[2] = af<1-min_af ? -log((1-af)*(1-af)) : -log(min_af*min_af);
    }

    // The sample pairs were given explicitly via -p/-P options
    if ( args->pairs )
    {
        if ( args->kbs_diff ) diff_sites_reset(args);

        int32_t *aptr, aval, *bptr, bval;
        int i,k, ndiff=0;
        if ( args->qry_use_GT && args->gt_use_GT )
        {
            for (i=0; i<args->npairs; i++)
            {
                aptr = args->qry_arr + args->pairs[i].iqry*nqry1;
                if ( !HAS_GT(aptr) ) continue;
                bptr = args->gt_arr  + args->pairs[i].igt*ngt1;
                if ( !HAS_GT(bptr) ) continue;
                aval = DSG_GT(aptr);
                bval = DSG_GT(bptr);
                if ( args->hom_only && bval==1 ) continue;
                if ( aval!=bval )
                {
                    args->ndiff[i]++;
                    if ( args->kbs_diff ) { ndiff++; kbs_insert(args->kbs_diff, i); }
                }
                else if ( args->calc_hwe_prob ) args->hwe_prob[i] += hwe[aval];
                args->ncnt[i]++;
            }
        }
        else if ( !args->qry_use_GT && !args->gt_use_GT )
        {
            for (i=0; i<args->npairs; i++)
            {
                aptr = args->qry_arr + args->pairs[i].iqry*nqry1;
                if ( !HAS_PL(aptr) ) continue;
                bptr = args->gt_arr  + args->pairs[i].igt*ngt1;
                if ( !HAS_PL(bptr) ) continue;
                aval = MIN_PL(aptr);
                bval = MIN_PL(bptr);
                if ( args->hom_only && bptr[1]==bval ) continue;
                int match = 0;
                for (k=0; k<3; k++)
                    if ( aptr[k]==aval && bptr[k]==bval ) { match = 1; break; }
                if ( !match )
                {
                    args->ndiff[i]++;
                    if ( args->kbs_diff ) { ndiff++; kbs_insert(args->kbs_diff, i); }
                }
                else if ( args->calc_hwe_prob ) args->hwe_prob[i] += hwe[DSG_PL(aptr)];
                args->ncnt[i]++;
            }
        }
        else if ( args->qry_use_GT )
        {
            for (i=0; i<args->npairs; i++)
            {
                aptr = args->qry_arr + args->pairs[i].iqry*nqry1;
                if ( !HAS_GT(aptr) ) continue;
                bptr = args->gt_arr  + args->pairs[i].igt*ngt1;
                if ( !HAS_PL(bptr) ) continue;
                aval = DSG_GT(aptr);
                bval = MIN_PL(bptr);
                if ( args->hom_only && bptr[1]==bval ) continue;
                if ( bptr[aval]!=bval )
                {
                    args->ndiff[i]++;
                    if ( args->kbs_diff ) { ndiff++; kbs_insert(args->kbs_diff, i); }
                }
                else if ( args->calc_hwe_prob ) args->hwe_prob[i] += hwe[aval];
                args->ncnt[i]++;
            }
        }
        else    // !args->qry_use_GT
        {
            for (i=0; i<args->npairs; i++)
            {
                aptr = args->qry_arr + args->pairs[i].iqry*nqry1;
                if ( !HAS_PL(aptr) ) continue;
                bptr = args->gt_arr  + args->pairs[i].igt*ngt1;
                if ( !HAS_GT(bptr) ) continue;
                aval = MIN_PL(aptr);
                bval = DSG_GT(bptr);
                if ( args->hom_only && bval==1 ) continue;
                if ( aval!=aptr[bval] )
                {
                    args->ndiff[i]++;
                    if ( args->kbs_diff ) { ndiff++; kbs_insert(args->kbs_diff, i); }
                }
                else if ( args->calc_hwe_prob ) args->hwe_prob[i] += hwe[DSG_PL(aptr)];
                args->ncnt[i]++;
            }
        }
        if ( ndiff ) diff_sites_push(args, ndiff, qry_rec->rid, qry_rec->pos);
        return;
    }

#if _SLOWER_BRANCH
    int i,j,idx=0;
    for (i=0; i<args->nqry_smpl; i++)
    {
        int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
        int ngt  = args->cross_check ? i : args->ngt_smpl;     // two files or a sub-diagnoal cross-check mode?
        for (j=0; j<ngt; j++)
        {
            int igt = args->gt_smpl ? args->gt_smpl[j] : j;
            int32_t *aptr = args->qry_arr + iqry*nqry1;
            int32_t *bptr = args->gt_arr + igt*ngt1;
            int match, qry_dsg, ign;
            if ( args->qry_use_GT && args->gt_use_GT )
                match = match_GT_GT(aptr,bptr,&qry_dsg,&ign);
            else if ( !args->qry_use_GT && !args->gt_use_GT )
                match = match_PL_PL(aptr,bptr,&qry_dsg,&ign);
            else if ( args->qry_use_GT )
                match = match_GT_PL(aptr,bptr,&qry_dsg,&ign);
            else
                match = match_GT_PL(bptr,aptr,&ign,&qry_dsg);
            if ( match>=0 ) 
            {
                if ( !match ) args->ndiff[idx]++;
                else if ( args->calc_hwe_prob ) args->hwe_prob[idx] += hwe[qry_dsg];
                args->ncnt[idx]++;
            }
            idx++;
        }
    }
#elif _FASTER_BRANCH
    int i,j,idx=0;
    for (i=0; i<args->nqry_smpl; i++)
    {
        int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
        int ngt  = args->cross_check ? i : args->ngt_smpl;     // two files or a sub-diagnoal cross-check mode?
        int32_t *aptr = args->qry_arr + iqry*nqry1;
        int32_t aval, qry_dsg;
        if ( args->qry_use_GT )
        {
            if ( !HAS_GT(aptr) ) { idx += ngt; continue; }
            aval = qry_dsg = DSG_GT(aptr);
        }
        else
        {
            if ( !HAS_PL(aptr) ) { idx += ngt; continue; }
            aval = MIN_PL(aptr);
            qry_dsg = DSG_PL(aptr);
        }

        for (j=0; j<ngt; j++)
        {
            int igt = args->gt_smpl ? args->gt_smpl[j] : j;
            int32_t *bptr = args->gt_arr + igt*ngt1;
            int32_t bval;
            if ( args->gt_use_GT )
            {
                if ( !HAS_GT(bptr) ) { idx++; bptr[0] = bcf_gt_missing; continue; }
                bval = DSG_GT(bptr);
            }
            else
            {
                if ( !HAS_PL(bptr) ) { idx++; bptr[0] = bcf_int32_missing; continue; }
                bval = MIN_PL(bptr);
            }

            int match;
            if ( args->qry_use_GT )
            {
                if ( args->gt_use_GT )
                    match = aval==bval ? 1 : 0;
                else
                    match = bptr[aval]==bval ? 1 : 0;
            }
            else
            {
                if ( args->gt_use_GT )
                    match = aptr[bval]==aval ? 1 : 0;
                else
                {
                    int k;
                    match = 0;
                    for (k=0; k<3; k++)
                        if ( aptr[k]==aval && bptr[k]==bval ) { match = 1; break; }
                }
            }
            if ( !match ) args->ndiff[idx]++;
            else if ( args->calc_hwe_prob ) args->hwe_prob[idx] += hwe[qry_dsg];
            args->ncnt[idx]++;
            idx++;
        }
    }
#else   // the default
    int i,j,k,idx=0;
    if ( args->qry_use_GT && args->gt_use_GT )
    {
        for (i=0; i<args->nqry_smpl; i++)
        {
            int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
            int ngt  = args->cross_check ? i : args->ngt_smpl;     // two files or a sub-diagnoal cross-check mode?
            int32_t *aptr = args->qry_arr + iqry*nqry1;
            int32_t aval, qry_dsg;
            if ( !HAS_GT(aptr) ) { idx += ngt; continue; }
            aval = qry_dsg = DSG_GT(aptr);
            for (j=0; j<ngt; j++)
            {
                int igt = args->gt_smpl ? args->gt_smpl[j] : j;
                int32_t *bptr = args->gt_arr + igt*ngt1;
                int32_t bval;
                if ( !HAS_GT(bptr) ) { idx++; bptr[0] = bcf_gt_missing; continue; }
                bval = DSG_GT(bptr);
                if ( args->hom_only && bval==1 ) { idx++; continue; }
                if ( aval!=bval ) args->ndiff[idx]++;
                else if ( args->calc_hwe_prob ) args->hwe_prob[idx] += hwe[qry_dsg];
                args->ncnt[idx]++;
                idx++;
            }
        }
    }
    else if ( !args->qry_use_GT && !args->gt_use_GT )
    {
        for (i=0; i<args->nqry_smpl; i++)
        {
            int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
            int ngt  = args->cross_check ? i : args->ngt_smpl;     // two files or a sub-diagnoal cross-check mode?
            int32_t *aptr = args->qry_arr + iqry*nqry1;
            int32_t aval, qry_dsg;
            if ( !HAS_PL(aptr) ) { idx += ngt; continue; }
            aval = MIN_PL(aptr);
            qry_dsg = DSG_PL(aptr);
            for (j=0; j<ngt; j++)
            {
                int igt = args->gt_smpl ? args->gt_smpl[j] : j;
                int32_t *bptr = args->gt_arr + igt*ngt1;
                int32_t bval;
                if ( !HAS_PL(bptr) ) { idx++; bptr[0] = bcf_int32_missing; continue; }
                bval = MIN_PL(bptr);
                if ( args->hom_only && bval==bptr[1] ) { idx++; continue; }
                int match = 0;
                for (k=0; k<3; k++)
                    if ( aptr[k]==aval && bptr[k]==bval ) { match = 1; break; }
                if ( !match ) args->ndiff[idx]++;
                else if ( args->calc_hwe_prob ) args->hwe_prob[idx] += hwe[qry_dsg];
                args->ncnt[idx]++;
                idx++;
            }
        }
    }
    else if ( args->qry_use_GT )
    {
        for (i=0; i<args->nqry_smpl; i++)
        {
            int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
            int ngt  = args->cross_check ? i : args->ngt_smpl;     // two files or a sub-diagnoal cross-check mode?
            int32_t *aptr = args->qry_arr + iqry*nqry1;
            int32_t aval, qry_dsg;
            if ( !HAS_GT(aptr) ) { idx += ngt; continue; }
            aval = qry_dsg = DSG_GT(aptr);
            for (j=0; j<ngt; j++)
            {
                int igt = args->gt_smpl ? args->gt_smpl[j] : j;
                int32_t *bptr = args->gt_arr + igt*ngt1;
                int32_t bval;
                if ( !HAS_PL(bptr) ) { idx++; bptr[0] = bcf_int32_missing; continue; }
                bval = MIN_PL(bptr);
                if ( args->hom_only && bval==bptr[1] ) { idx++; continue; }
                if ( bptr[aval]!=bval ) args->ndiff[idx]++;
                else if ( args->calc_hwe_prob ) args->hwe_prob[idx] += hwe[qry_dsg];
                args->ncnt[idx]++;
                idx++;
            }
        }
    }
    else    // !args->qry_use_GT
    {
        for (i=0; i<args->nqry_smpl; i++)
        {
            int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
            int ngt  = args->cross_check ? i : args->ngt_smpl;     // two files or a sub-diagnoal cross-check mode?
            int32_t *aptr = args->qry_arr + iqry*nqry1;
            int32_t aval, qry_dsg;
            if ( !HAS_PL(aptr) ) { idx += ngt; continue; }
            aval = MIN_PL(aptr);
            qry_dsg = DSG_PL(aptr);
            for (j=0; j<ngt; j++)
            {
                int igt = args->gt_smpl ? args->gt_smpl[j] : j;
                int32_t *bptr = args->gt_arr + igt*ngt1;
                int32_t bval;
                if ( !HAS_GT(bptr) ) { idx++; bptr[0] = bcf_gt_missing; continue; }
                bval = DSG_GT(bptr);
                if ( args->hom_only && bval==1 ) { idx++; continue; }
                if ( aptr[bval]!=aval ) args->ndiff[idx]++;
                else if ( args->calc_hwe_prob ) args->hwe_prob[idx] += hwe[qry_dsg];
                args->ncnt[idx]++;
                idx++;
            }
        }
    }
#endif
}


typedef struct
{
    int ism, idx;
    double val;
}
idbl_t;
static int cmp_idbl(const void *_a, const void *_b)
{
    idbl_t *a = (idbl_t*)_a;
    idbl_t *b = (idbl_t*)_b;
    if ( a->val < b->val ) return -1;
    if ( a->val > b->val ) return 1;
    return 0;
}
static void report_distinctive_sites(args_t *args)
{
    extsort_sort(args->es);

    fprintf(args->fp,"# DS, distinctive sites:\n");
    fprintf(args->fp,"#     - chromosome\n");
    fprintf(args->fp,"#     - position\n");
    fprintf(args->fp,"#     - cumulative number of pairs distinguished by this block\n");
    fprintf(args->fp,"#     - block id\n");
    fprintf(args->fp,"#DS\t[2]Chromosome\t[3]Position\t[4]Cumulative number of distinct pairs\t[5]Block id\n");

    kbitset_t *kbs_blk = kbs_init(args->npairs);
    kbitset_iter_t itr;
    int i,ndiff,rid,pos,ndiff_tot = 0, ndiff_min = args->distinctive_sites, iblock = 0;
    while ( diff_sites_shift(args,&ndiff,&rid,&pos) )
    {
        int ndiff_new = 0, ndiff_dbg = 0;
        kbs_start(&itr);
        while ( (i=kbs_next(args->kbs_diff, &itr))>=0 )
        {
            ndiff_dbg++;
            if ( kbs_exists(kbs_blk,i) ) continue;   // already set
            kbs_insert(kbs_blk,i);
            ndiff_new++;
        }
        if ( ndiff_dbg!=ndiff ) throw_and_clean(args,"Corrupted data, fixme: %d vs %d\n",ndiff_dbg,ndiff);
        if ( !ndiff_new ) continue;     // no new pair distinguished by this site
        ndiff_tot += ndiff_new;
        fprintf(args->fp,"DS\t%s\t%d\t%d\t%d\n",bcf_hdr_id2name(args->qry_hdr,rid),pos+1,ndiff_tot,iblock);
        if ( ndiff_tot < ndiff_min ) continue;   // fewer than the requested number of pairs can be distinguished at this point
        iblock++;
        ndiff_tot = 0;
        kbs_clear(kbs_blk);
    }
    kbs_destroy(kbs_blk);
}
static void report(args_t *args)
{
    fprintf(args->fp,"# DC, discordance:\n");
    fprintf(args->fp,"#     - query sample\n");
    fprintf(args->fp,"#     - genotyped sample\n");
    fprintf(args->fp,"#     - discordance (number of mismatches; smaller is better)\n");
    fprintf(args->fp,"#     - negative log of HWE probability at matching sites (bigger is better)\n");
    fprintf(args->fp,"#     - number of sites compared (bigger is better)\n");
    fprintf(args->fp,"#DC\t[2]Query Sample\t[3]Genotyped Sample\t[4]Discordance\t[5]-log P(HWE)\t[6]Number of sites compared\n");

    int trim = args->ntop;
    if ( !args->pairs )
    {
        if ( !args->ngt_smpl && args->nqry_smpl <= args->ntop ) trim = 0;
        if ( args->ngt_smpl && args->ngt_smpl <= args->ntop  ) trim = 0;
    }

    if ( args->pairs )
    {
        int i;
        for (i=0; i<args->npairs; i++)
        {
            int iqry = args->pairs[i].iqry;
            int igt  = args->pairs[i].igt;
            fprintf(args->fp,"DC\t%s\t%s\t%u\t%e\t%u\n",
                    args->qry_hdr->samples[iqry],
                    args->gt_hdr?args->gt_hdr->samples[igt]:args->qry_hdr->samples[igt],
                    args->ndiff[i],
                    args->calc_hwe_prob ? args->hwe_prob[i] : 0,
                    args->ncnt[i]);
        }
    }
    else if ( !trim )
    {
        int i,j,idx=0;
        for (i=0; i<args->nqry_smpl; i++)
        {
            int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
            int ngt  = args->cross_check ? i : args->ngt_smpl;
            for (j=0; j<ngt; j++)
            {
                int igt = args->gt_smpl ? args->gt_smpl[j] : j;
                fprintf(args->fp,"DC\t%s\t%s\t%u\t%e\t%u\n",
                        args->qry_hdr->samples[iqry],
                        args->gt_hdr?args->gt_hdr->samples[igt]:args->qry_hdr->samples[igt],
                        args->ndiff[idx],
                        args->calc_hwe_prob ? args->hwe_prob[idx] : 0,
                        args->ncnt[idx]);
                idx++;
            }
        }
    }
    else if ( !args->cross_check )
    {
        idbl_t *arr = (idbl_t*)malloc(sizeof(*arr)*args->ngt_smpl);
        int i,j;
        for (i=0; i<args->nqry_smpl; i++)
        {
            int idx  = i*args->ngt_smpl;
            for (j=0; j<args->ngt_smpl; j++)
            {
                if ( args->sort_by_hwe )
                    arr[j].val = -args->hwe_prob[idx];
                else
                    arr[j].val = args->ncnt[idx] ? (double)args->ndiff[idx]/args->ncnt[idx] : 0;
                arr[j].ism = j;
                arr[j].idx = idx;
                idx++;
            }
            qsort(arr, args->ngt_smpl, sizeof(*arr), cmp_idbl);
            int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
            for (j=0; j<args->ntop; j++)
            {
                int idx = arr[j].idx;
                int igt = args->gt_smpl ? args->gt_smpl[arr[j].ism] : arr[j].ism;
                fprintf(args->fp,"DC\t%s\t%s\t%u\t%e\t%u\n",
                        args->qry_hdr->samples[iqry],
                        args->gt_hdr?args->gt_hdr->samples[igt]:args->qry_hdr->samples[igt],
                        args->ndiff[idx],
                        args->calc_hwe_prob ? args->hwe_prob[idx] : 0,
                        args->ncnt[idx]);
            }
        }
        free(arr);
    }
    else
    {
        int narr = args->nqry_smpl-1;
        idbl_t *arr = (idbl_t*)malloc(sizeof(*arr)*narr);
        int i,j,k,idx;
        for (i=0; i<args->nqry_smpl; i++)
        {
            k = 0, idx = i*(i-1)/2;
            for (j=0; j<i; j++)
            {
                if ( args->sort_by_hwe )
                    arr[k].val = -args->hwe_prob[idx];
                else
                    arr[k].val = args->ncnt[idx] ? (double)args->ndiff[idx]/args->ncnt[idx] : 0;
                arr[k].ism = j;
                arr[k].idx = idx;
                idx++;
                k++;
            }
            for (; j<narr; j++)
            {
                idx = j*(j+1)/2 + i;
                if ( args->sort_by_hwe )
                    arr[k].val = -args->hwe_prob[idx];
                else
                    arr[k].val = args->ncnt[idx] ? (double)args->ndiff[idx]/args->ncnt[idx] : 0;
                arr[k].ism = j + 1;
                arr[k].idx = idx;
                k++;
            }
            qsort(arr, narr, sizeof(*arr), cmp_idbl);
            int iqry = args->qry_smpl ? args->qry_smpl[i] : i;
            for (j=0; j<args->ntop; j++)
            {
                if ( i <= arr[j].ism ) continue;
                int idx = arr[j].idx;
                int igt = args->qry_smpl ? args->qry_smpl[arr[j].ism] : arr[j].ism;
                fprintf(args->fp,"DC\t%s\t%s\t%u\t%e\t%u\n",
                        args->qry_hdr->samples[iqry],
                        args->qry_hdr->samples[igt],
                        args->ndiff[idx],
                        args->calc_hwe_prob ? args->hwe_prob[idx] : 0,
                        args->ncnt[idx]);
            }
        }
        free(arr);
    }
}

static void usage(void)
{
    fprintf(stderr, "\n");
    fprintf(stderr, "About:   Check sample identity. With no -g BCF given, multi-sample cross-check is performed.\n");
    fprintf(stderr, "Usage:   bcftools gtcheck [options] [-g <genotypes.vcf.gz>] <query.vcf.gz>\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "    -a, --all-sites                    output comparison for all sites\n");
    fprintf(stderr, "    -c, --cluster MIN,MAX              min inter- and max intra-sample error [0.23,-0.3]\n");
    fprintf(stderr, "        --distinctive-sites NUM        find sites that can distinguish between NUM sample pairs. If NUM is smaller or equal\n");
    fprintf(stderr, "                                           to 1, it is interpreted as the fraction of samples, otherwise as count\n");
    fprintf(stderr, "        --dry-run                      stop after first record to estimate required time\n");
    fprintf(stderr, "    -g, --genotypes FILE               genotypes to compare against\n");
    fprintf(stderr, "    -H, --homs-only                    homozygous genotypes only, useful with low coverage data (requires -g)\n");
    fprintf(stderr, "        --n-matches INT                print only top INT matches for each sample, 0 for unlimited. Use negative value\n");
    fprintf(stderr, "                                            to sort by HWE probability rather than the number of discordant sites [0]\n");
    fprintf(stderr, "        --no-HWE-prob                  disable calculation of HWE probability\n");
    fprintf(stderr, "    -p, --pairs LIST                   comma-separated sample pairs to compare (qry,gt[,qry,gt..] with -g or qry,qry[,qry,qry..] w/o)\n");
    fprintf(stderr, "    -P, --pairs-file FILE              file with tab-delimited sample pairs to compare (qry,gt with -g or qry,qry w/o)\n");
    fprintf(stderr, "    -r, --regions REGION               restrict to comma-separated list of regions\n");
    fprintf(stderr, "    -R, --regions-file FILE            restrict to regions listed in a file\n");
    fprintf(stderr, "    -s, --samples [qry|gt]:LIST        list of query or -g samples (by default all samples are compared)\n");
    fprintf(stderr, "    -S, --samples-file [qry|gt]:FILE   file with the query or -g samples to compare\n");
    fprintf(stderr, "    -t, --targets REGION               similar to -r but streams rather than index-jumps\n");
    fprintf(stderr, "    -T, --targets-file FILE            similar to -R but streams rather than index-jumps\n");
    fprintf(stderr, "    -u, --use TAG1[,TAG2]              which tag to use in the query file (TAG1) and the -g (TAG2) files [PL,GT]\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "   # Are there any matching samples in file A and B?\n");
    fprintf(stderr, "   bcftools gtcheck -g A.bcf B.bcf > out.txt\n");
    fprintf(stderr, "\n");
    exit(1);
}

int main_vcfgtcheck(int argc, char *argv[])
{
    int c;
    args_t *args = (args_t*) calloc(1,sizeof(args_t));
    args->argc   = argc; args->argv = argv; set_cwd(args);
    args->qry_use_GT = -1;
    args->gt_use_GT  = -1;
    args->calc_hwe_prob = 1;

    // external sort for --distinctive-sites
    args->es_tmp_dir = "/tmp/bcftools-gtcheck.XXXXXX";
    args->es_max_mem = "500M";

    // In simulated sample swaps the minimum error was 0.3 and maximum intra-sample error was 0.23
    //    - min_inter: pairs with smaller err value will be considered identical 
    //    - max_intra: pairs with err value bigger than abs(max_intra_err) will be considered
    //                  different. If negative, the cutoff may be heuristically lowered
    args->min_inter_err =  0.23;
    args->max_intra_err = -0.3;

    static struct option loptions[] =
    {
        {"use",1,0,'u'},
        {"cluster",1,0,'c'},
        {"GTs-only",1,0,'G'},
        {"all-sites",0,0,'a'},
        {"homs-only",0,0,'H'},
        {"help",0,0,'h'},
        {"genotypes",1,0,'g'},
        {"plot",1,0,'p'},
        {"samples",1,0,'s'},
        {"samples-file",1,0,'S'},
        {"n-matches",1,0,2},
        {"no-HWE-prob",0,0,3},
        {"target-sample",1,0,4},
        {"dry-run",0,0,5},
        {"distinctive-sites",1,0,6},
        {"regions",1,0,'r'},
        {"regions-file",1,0,'R'},
        {"targets",1,0,'t'},
        {"targets-file",1,0,'T'},
        {"pairs",1,0,'p'},
        {"pairs-file",1,0,'P'},
        {0,0,0,0}
    };
    char *tmp;
    while ((c = getopt_long(argc, argv, "hg:p:s:S:p:P:Hr:R:at:T:G:c:u:",loptions,NULL)) >= 0) {
        switch (c) {
            case 'u':
                {
                    int i,nlist;
                    char **list = hts_readlist(optarg, 0, &nlist);
                    if ( !list || nlist<=0 || nlist>2 ) throw_and_clean(args,"Failed to parse --use %s\n", optarg);
                    if ( !strcasecmp("GT",list[0]) ) args->qry_use_GT = 1;
                    else if ( !strcasecmp("PL",list[0]) ) args->qry_use_GT = 0;
                    else throw_and_clean(args,"Failed to parse --use %s; only GT and PL are supported\n", optarg);
                    if ( nlist==2 )
                    {
                        if ( !strcasecmp("GT",list[1]) ) args->gt_use_GT = 1;
                        else if ( !strcasecmp("PL",list[1]) ) args->gt_use_GT = 0;
                        else throw_and_clean(args,"Failed to parse --use %s; only GT and PL are supported\n", optarg);
                    }
                    else args->gt_use_GT = args->qry_use_GT;
                    for (i=0; i<nlist; i++) free(list[i]);
                    free(list);
                }
                break;
            case 2 :
                args->ntop = strtol(optarg,&tmp,10);
                if ( !tmp || *tmp ) throw_and_clean(args,"Could not parse: --n-matches %s\n", optarg);
                if ( args->ntop < 0 )
                {
                    args->sort_by_hwe = 1;
                    args->ntop *= -1;
                }
                break;
            case 3 : args->calc_hwe_prob = 0; break;
            case 4 : throw_and_clean(args,"The option -S, --target-sample has been deprecated\n"); break;
            case 5 : args->dry_run = 1; break;
            case 6 : 
                args->distinctive_sites = strtod(optarg,&tmp);
                if ( *tmp )  throw_and_clean(args,"Could not parse: --distinctive-sites %s\n", optarg);
                break;
            case 'c':
                args->min_inter_err = strtod(optarg,&tmp);
                if ( *tmp )
                {
                    if ( *tmp!=',') throw_and_clean(args,"Could not parse: -c %s\n", optarg);
                    args->max_intra_err = strtod(tmp+1,&tmp);
                    if ( *tmp ) throw_and_clean(args,"Could not parse: -c %s\n", optarg);
                }
                break;
            case 'G': throw_and_clean(args,"The option -G, --GTs-only has been deprecated\n"); break;
            case 'a': args->all_sites = 1; break;
            case 'H': args->hom_only = 1; break;
            case 'g': args->gt_fname = optarg; break;
//            case 'p': args->plot = optarg; break;
            case 's':
                if ( !strncasecmp("gt:",optarg,3) ) args->gt_samples = optarg+3;
                else if ( !strncasecmp("qry:",optarg,4) ) args->qry_samples = optarg+4;
                else throw_and_clean(args,"Which one? Query samples (qry:%s) or genotype samples (gt:%s)?\n",optarg,optarg);
                break;
            case 'S': 
                if ( !strncasecmp("gt:",optarg,3) ) args->gt_samples = optarg+3, args->gt_samples_is_file = 1;
                else if ( !strncasecmp("qry:",optarg,4) ) args->qry_samples = optarg+4, args->qry_samples_is_file = 1;
                else throw_and_clean(args,"Which one? Query samples (qry:%s) or genotype samples (gt:%s)?\n",optarg,optarg);
                break;
            case 'p': args->pair_samples = optarg; break;
            case 'P': args->pair_samples = optarg; args->pair_samples_is_file = 1; break;
            case 'r': args->regions = optarg; break;
            case 'R': args->regions = optarg; args->regions_is_file = 1; break;
            case 't': args->targets = optarg; break;
            case 'T': args->targets = optarg; args->targets_is_file = 1; break;
            case 'h':
            case '?': usage(); break;
            default: throw_and_clean(args,"Unknown argument: %s\n", optarg);
        }
    }
    if ( optind==argc )
    {
        if ( !isatty(fileno((FILE *)stdin)) ) args->qry_fname = "-";  // reading from stdin
        else usage();   // no files given
    }
    else args->qry_fname = argv[optind];
    if ( argc>optind+1 ) throw_and_clean(args,"Error: too many files given, run with -h for help\n");  // too many files given
    if ( args->pair_samples )
    {
        if ( args->gt_samples || args->qry_samples ) throw_and_clean(args,"The -p/-P option cannot be combined with -s/-S\n");
        if ( args->ntop ) throw_and_clean(args,"The --n-matches option cannot be combined with -p/-P\n");
    }
    if ( args->distinctive_sites && !args->pair_samples ) throw_and_clean(args,"The experimental option --distinctive-sites requires -p/-P");
    if ( args->hom_only && !args->gt_fname ) throw_and_clean(args,"The option --homs-only requires --genotypes\n");

    init_data(args);

    int ret;
    while ( (ret=bcf_sr_next_line(args->files)) )
    {
        if ( args->gt_hdr && ret!=2 ) continue;     // not a cross-check mode and lines don't match

        // time one record to give the user an estimate with very big files
        struct timeval t0, t1;
        if ( !args->ncmp )  gettimeofday(&t0, NULL);

        process_line(args);

        if ( args->ncmp==1 )
        {
            gettimeofday(&t1, NULL);
            double delta = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_usec - t0.tv_usec);
            fprintf(stderr,"INFO:\tTime required to process one record .. %f seconds\n",delta/1e6);
            fprintf(args->fp,"INFO\tTime required to process one record .. %f seconds\n",delta/1e6);
            if ( args->dry_run ) break;
        }
    }
    if ( !args->dry_run )
    {
        report(args);
        if ( args->distinctive_sites ) report_distinctive_sites(args);
    }

    destroy_data(args);
    free(args);
    return 0;
}

