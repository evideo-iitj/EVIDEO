/*****************************************************************************
 * slicetype.c: h264 encoder library
 *****************************************************************************
 * Copyright (C) 2005-2008 x264 project
 *
 * Authors: Jason Garrett-Glaser <darkshikari@gmail.com>
 *          Loren Merritt <lorenm@u.washington.edu>
 *          Dylan Yudaken <dyudaken@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include <math.h>

#include "common/common.h"
#include "macroblock.h"
#include "me.h"

// Indexed by pic_struct values
static const uint8_t delta_tfi_divisor[10] = { 0, 2, 1, 1, 2, 2, 3, 3, 4, 6 };

static int x264_slicetype_frame_cost( x264_t *h, x264_mb_analysis_t *a,
                                      x264_frame_t **frames, int p0, int p1, int b,
                                      int b_intra_penalty );

static void x264_lowres_context_init( x264_t *h, x264_mb_analysis_t *a )
{
    a->i_qp = X264_LOOKAHEAD_QP;
    a->i_lambda = x264_lambda_tab[ a->i_qp ];
    x264_mb_analyse_load_costs( h, a );
    if( h->param.analyse.i_subpel_refine > 1 )
    {
        h->mb.i_me_method = X264_MIN( X264_ME_HEX, h->param.analyse.i_me_method );
        h->mb.i_subpel_refine = 4;
    }
    else
    {
        h->mb.i_me_method = X264_ME_DIA;
        h->mb.i_subpel_refine = 2;
    }
    h->mb.b_chroma_me = 0;
}

/* makes a non-h264 weight (i.e. fix7), into an h264 weight */
static void x264_weight_get_h264( unsigned int weight_nonh264, int offset, x264_weight_t *w )
{
    w->i_offset = offset;
    w->i_denom = 7;
    w->i_scale = weight_nonh264;
    while( w->i_denom > 0 && (w->i_scale > 127 || !(w->i_scale & 1)) )
    {
        w->i_denom--;
        w->i_scale >>= 1;
    }
    w->i_scale = X264_MIN( w->i_scale, 127 );
}

static NOINLINE pixel *x264_weight_cost_init_luma( x264_t *h, x264_frame_t *fenc, x264_frame_t *ref, pixel *dest )
{
    int ref0_distance = fenc->i_frame - ref->i_frame - 1;
    /* Note: this will never run during lookahead as weights_analyse is only called if no
     * motion search has been done. */
    if( fenc->lowres_mvs[0][ref0_distance][0][0] != 0x7FFF )
    {
        int i_stride = fenc->i_stride_lowres;
        int i_lines = fenc->i_lines_lowres;
        int i_width = fenc->i_width_lowres;
        int i_mb_xy = 0;
        pixel *p = dest;

        for( int y = 0; y < i_lines; y += 8, p += i_stride*8 )
            for( int x = 0; x < i_width; x += 8, i_mb_xy++ )
            {
                int mvx = fenc->lowres_mvs[0][ref0_distance][i_mb_xy][0];
                int mvy = fenc->lowres_mvs[0][ref0_distance][i_mb_xy][1];
                h->mc.mc_luma( p+x, i_stride, ref->lowres, i_stride,
                               mvx+(x<<2), mvy+(y<<2), 8, 8, weight_none );
            }
        x264_emms();
        return dest;
    }
    x264_emms();
    return ref->lowres[0];
}

static NOINLINE unsigned int x264_weight_cost( x264_t *h, x264_frame_t *fenc, pixel *src, x264_weight_t *w )
{
    unsigned int cost = 0;
    int i_stride = fenc->i_stride_lowres;
    int i_lines = fenc->i_lines_lowres;
    int i_width = fenc->i_width_lowres;
    pixel *fenc_plane = fenc->lowres[0];
    ALIGNED_ARRAY_8( pixel, buf,[8*8] );
    int pixoff = 0;
    int i_mb = 0;

    if( w )
    {
        for( int y = 0; y < i_lines; y += 8, pixoff = y*i_stride )
            for( int x = 0; x < i_width; x += 8, i_mb++, pixoff += 8)
            {
                w->weightfn[8>>2]( buf, 8, &src[pixoff], i_stride, w, 8 );
                cost += X264_MIN( h->pixf.mbcmp[PIXEL_8x8]( buf, 8, &fenc_plane[pixoff], i_stride ), fenc->i_intra_cost[i_mb] );
            }
        /* Add cost of weights in the slice header. */
        int numslices;
        if( h->param.i_slice_count )
            numslices = h->param.i_slice_count;
        else if( h->param.i_slice_max_mbs )
            numslices = (h->mb.i_mb_width * h->mb.i_mb_height + h->param.i_slice_max_mbs-1) / h->param.i_slice_max_mbs;
        else
            numslices = 1;
        /* FIXME: find a way to account for --slice-max-size?
         * Multiply by 2 as there will be a duplicate. 10 bits added as if there is a weighted frame, then an additional duplicate is used.
         * Since using lowres frames, assume lambda = 1. */
        cost += numslices * ( 10 + 2 * ( bs_size_ue( w[0].i_denom ) + bs_size_se( w[0].i_scale ) + bs_size_se( w[0].i_offset ) ) );
    }
    else
        for( int y = 0; y < i_lines; y += 8, pixoff = y*i_stride )
            for( int x = 0; x < i_width; x += 8, i_mb++, pixoff += 8 )
                cost += X264_MIN( h->pixf.mbcmp[PIXEL_8x8]( &src[pixoff], i_stride, &fenc_plane[pixoff], i_stride ), fenc->i_intra_cost[i_mb] );
    x264_emms();
    return cost;
}

void x264_weights_analyse( x264_t *h, x264_frame_t *fenc, x264_frame_t *ref, int b_lookahead )
{
    float fenc_mean, ref_mean, fenc_var, ref_var;
    int offset_search;
    int minoff, minscale, mindenom;
    unsigned int minscore, origscore;
    int i_delta_index = fenc->i_frame - ref->i_frame - 1;
    /* epsilon is chosen to require at least a numerator of 127 (with denominator = 128) */
    const float epsilon = 1.0/128.0;
    float guess_scale;
    int found;
    x264_weight_t *weights = fenc->weight[0];

    fenc_var = round( sqrt( fenc->i_pixel_ssd[0] ) );
    ref_var  = round( sqrt(  ref->i_pixel_ssd[0] ) );
    fenc_mean = (float)fenc->i_pixel_sum[0] / (fenc->i_lines[0] * fenc->i_width[0]);
    ref_mean  = (float) ref->i_pixel_sum[0] / (fenc->i_lines[0] * fenc->i_width[0]);

    //early termination
    if( fabs( ref_mean - fenc_mean ) < 0.5 && fabs( 1 - fenc_var / ref_var ) < epsilon )
    {
        SET_WEIGHT( weights[0], 0, 1, 0, 0 );
        return;
    }

    guess_scale = ref_var ? fenc_var/ref_var : 0;
    x264_weight_get_h264( round( guess_scale * 128 ), 0, &weights[0] );

    found = 0;
    mindenom = weights[0].i_denom;
    minscale = weights[0].i_scale;
    minoff = 0;
    offset_search = x264_clip3( floor( fenc_mean - ref_mean * minscale / (1 << mindenom) + 0.5f*b_lookahead ), -128, 126 );

    if( !fenc->b_intra_calculated )
    {
        x264_mb_analysis_t a;
        x264_lowres_context_init( h, &a );
        x264_slicetype_frame_cost( h, &a, &fenc, 0, 0, 0, 0 );
    }
    pixel *mcbuf = x264_weight_cost_init_luma( h, fenc, ref, h->mb.p_weight_buf[0] );
    origscore = minscore = x264_weight_cost( h, fenc, mcbuf, 0 );

    if( !minscore )
    {
        SET_WEIGHT( weights[0], 0, 1, 0, 0 );
        return;
    }

    // This gives a slight improvement due to rounding errors but only tests
    // one offset on lookahead.
    // TODO: currently searches only offset +1. try other offsets/multipliers/combinations thereof?
    for( int i_off = offset_search; i_off <= offset_search+!b_lookahead; i_off++ )
    {
        SET_WEIGHT( weights[0], 1, minscale, mindenom, i_off );
        unsigned int s = x264_weight_cost( h, fenc, mcbuf, &weights[0] );
        COPY3_IF_LT( minscore, s, minoff, i_off, found, 1 );
    }
    x264_emms();

    /* FIXME: More analysis can be done here on SAD vs. SATD termination. */
    /* 0.2% termination derived experimentally to avoid weird weights in frames that are mostly intra. */
    if( !found || (minscale == 1<<mindenom && minoff == 0) || (float)minscore / origscore > 0.998 )
    {
        SET_WEIGHT( weights[0], 0, 1, 0, 0 );
        return;
    }
    else
        SET_WEIGHT( weights[0], 1, minscale, mindenom, minoff );

    if( h->param.analyse.i_weighted_pred == X264_WEIGHTP_FAKE && weights[0].weightfn )
        fenc->f_weighted_cost_delta[i_delta_index] = (float)minscore / origscore;

    if( weights[0].weightfn && b_lookahead )
    {
        //scale lowres in lookahead for slicetype_frame_cost
        pixel *src = ref->buffer_lowres[0];
        pixel *dst = h->mb.p_weight_buf[0];
        int width = ref->i_width_lowres + PADH*2;
        int height = ref->i_lines_lowres + PADV*2;
        x264_weight_scale_plane( h, dst, ref->i_stride_lowres, src, ref->i_stride_lowres,
                                 width, height, &weights[0] );
        fenc->weighted[0] = h->mb.p_weight_buf[0] + PADH + ref->i_stride_lowres * PADV;
    }
}

static void x264_slicetype_mb_cost( x264_t *h, x264_mb_analysis_t *a,
                                    x264_frame_t **frames, int p0, int p1, int b,
                                    int dist_scale_factor, int do_search[2], const x264_weight_t *w )
{
    x264_frame_t *fref0 = frames[p0];
    x264_frame_t *fref1 = frames[p1];
    x264_frame_t *fenc  = frames[b];
    const int b_bidir = (b < p1);
    const int i_mb_x = h->mb.i_mb_x;
    const int i_mb_y = h->mb.i_mb_y;
    const int i_mb_stride = h->mb.i_mb_width;
    const int i_mb_xy = i_mb_x + i_mb_y * i_mb_stride;
    const int i_stride = fenc->i_stride_lowres;
    const int i_pel_offset = 8 * (i_mb_x + i_mb_y * i_stride);
    const int i_bipred_weight = h->param.analyse.b_weighted_bipred ? 64 - (dist_scale_factor>>2) : 32;
    int16_t (*fenc_mvs[2])[2] = { &frames[b]->lowres_mvs[0][b-p0-1][i_mb_xy], &frames[b]->lowres_mvs[1][p1-b-1][i_mb_xy] };
    int (*fenc_costs[2]) = { &frames[b]->lowres_mv_costs[0][b-p0-1][i_mb_xy], &frames[b]->lowres_mv_costs[1][p1-b-1][i_mb_xy] };
    int b_frame_score_mb = (i_mb_x > 0 && i_mb_x < h->mb.i_mb_width - 1 &&
                            i_mb_y > 0 && i_mb_y < h->mb.i_mb_height - 1) ||
                            h->mb.i_mb_width <= 2 || h->mb.i_mb_height <= 2;

    ALIGNED_ARRAY_8( pixel, pix1,[9*FDEC_STRIDE] );
    pixel *pix2 = pix1+8;
    x264_me_t m[2];
    int i_bcost = COST_MAX;
    int list_used = 0;

    h->mb.pic.p_fenc[0] = h->mb.pic.fenc_buf;
    h->mc.copy[PIXEL_8x8]( h->mb.pic.p_fenc[0], FENC_STRIDE, &fenc->lowres[0][i_pel_offset], i_stride, 8 );

    if( p0 == p1 )
        goto lowres_intra_mb;

    // no need for h->mb.mv_min[]
    h->mb.mv_min_fpel[0] = -8*h->mb.i_mb_x - 4;
    h->mb.mv_max_fpel[0] = 8*( h->mb.i_mb_width - h->mb.i_mb_x - 1 ) + 4;
    h->mb.mv_min_spel[0] = 4*( h->mb.mv_min_fpel[0] - 8 );
    h->mb.mv_max_spel[0] = 4*( h->mb.mv_max_fpel[0] + 8 );
    if( h->mb.i_mb_x >= h->mb.i_mb_width - 2 )
    {
        h->mb.mv_min_fpel[1] = -8*h->mb.i_mb_y - 4;
        h->mb.mv_max_fpel[1] = 8*( h->mb.i_mb_height - h->mb.i_mb_y - 1 ) + 4;
        h->mb.mv_min_spel[1] = 4*( h->mb.mv_min_fpel[1] - 8 );
        h->mb.mv_max_spel[1] = 4*( h->mb.mv_max_fpel[1] + 8 );
    }

#define LOAD_HPELS_LUMA(dst, src) \
    { \
        (dst)[0] = &(src)[0][i_pel_offset]; \
        (dst)[1] = &(src)[1][i_pel_offset]; \
        (dst)[2] = &(src)[2][i_pel_offset]; \
        (dst)[3] = &(src)[3][i_pel_offset]; \
    }
#define LOAD_WPELS_LUMA(dst,src) \
    (dst) = &(src)[i_pel_offset];

#define CLIP_MV( mv ) \
    { \
        mv[0] = x264_clip3( mv[0], h->mb.mv_min_spel[0], h->mb.mv_max_spel[0] ); \
        mv[1] = x264_clip3( mv[1], h->mb.mv_min_spel[1], h->mb.mv_max_spel[1] ); \
    }
#define TRY_BIDIR( mv0, mv1, penalty ) \
    { \
        int i_cost; \
        if( h->param.analyse.i_subpel_refine <= 1 ) \
        { \
            int hpel_idx1 = (((mv0)[0]&2)>>1) + ((mv0)[1]&2); \
            int hpel_idx2 = (((mv1)[0]&2)>>1) + ((mv1)[1]&2); \
            pixel *src1 = m[0].p_fref[hpel_idx1] + ((mv0)[0]>>2) + ((mv0)[1]>>2) * m[0].i_stride[0]; \
            pixel *src2 = m[1].p_fref[hpel_idx2] + ((mv1)[0]>>2) + ((mv1)[1]>>2) * m[1].i_stride[0]; \
            h->mc.avg[PIXEL_8x8]( pix1, 16, src1, m[0].i_stride[0], src2, m[1].i_stride[0], i_bipred_weight ); \
        } \
        else \
        { \
            int stride1 = 16, stride2 = 16; \
            pixel *src1, *src2; \
            src1 = h->mc.get_ref( pix1, &stride1, m[0].p_fref, m[0].i_stride[0], \
                                  (mv0)[0], (mv0)[1], 8, 8, w ); \
            src2 = h->mc.get_ref( pix2, &stride2, m[1].p_fref, m[1].i_stride[0], \
                                  (mv1)[0], (mv1)[1], 8, 8, w ); \
            h->mc.avg[PIXEL_8x8]( pix1, 16, src1, stride1, src2, stride2, i_bipred_weight ); \
        } \
        i_cost = penalty * a->i_lambda + h->pixf.mbcmp[PIXEL_8x8]( \
                           m[0].p_fenc[0], FENC_STRIDE, pix1, 16 ); \
        COPY2_IF_LT( i_bcost, i_cost, list_used, 3 ); \
    }

    m[0].i_pixel = PIXEL_8x8;
    m[0].p_cost_mv = a->p_cost_mv;
    m[0].i_stride[0] = i_stride;
    m[0].p_fenc[0] = h->mb.pic.p_fenc[0];
    m[0].weight = w;
    m[0].i_ref = 0;
    LOAD_HPELS_LUMA( m[0].p_fref, fref0->lowres );
    m[0].p_fref_w = m[0].p_fref[0];
    if( w[0].weightfn )
        LOAD_WPELS_LUMA( m[0].p_fref_w, fenc->weighted[0] );

    if( b_bidir )
    {
        int16_t *mvr = fref1->lowres_mvs[0][p1-p0-1][i_mb_xy];
        ALIGNED_ARRAY_8( int16_t, dmv,[2],[2] );

        m[1].i_pixel = PIXEL_8x8;
        m[1].p_cost_mv = a->p_cost_mv;
        m[1].i_stride[0] = i_stride;
        m[1].p_fenc[0] = h->mb.pic.p_fenc[0];
        m[1].i_ref = 0;
        m[1].weight = weight_none;
        LOAD_HPELS_LUMA( m[1].p_fref, fref1->lowres );
        m[1].p_fref_w = m[1].p_fref[0];

        dmv[0][0] = ( mvr[0] * dist_scale_factor + 128 ) >> 8;
        dmv[0][1] = ( mvr[1] * dist_scale_factor + 128 ) >> 8;
        dmv[1][0] = dmv[0][0] - mvr[0];
        dmv[1][1] = dmv[0][1] - mvr[1];
        CLIP_MV( dmv[0] );
        CLIP_MV( dmv[1] );
        if( h->param.analyse.i_subpel_refine <= 1 )
            M64( dmv ) &= ~0x0001000100010001ULL; /* mv & ~1 */

        TRY_BIDIR( dmv[0], dmv[1], 0 );
        if( M64( dmv ) )
        {
            int i_cost;
            h->mc.avg[PIXEL_8x8]( pix1, 16, m[0].p_fref[0], m[0].i_stride[0], m[1].p_fref[0], m[1].i_stride[0], i_bipred_weight );
            i_cost = h->pixf.mbcmp[PIXEL_8x8]( m[0].p_fenc[0], FENC_STRIDE, pix1, 16 );
            COPY2_IF_LT( i_bcost, i_cost, list_used, 3 );
        }
    }

    for( int l = 0; l < 1 + b_bidir; l++ )
    {
        if( do_search[l] )
        {
            int i_mvc = 0;
            int16_t (*fenc_mv)[2] = fenc_mvs[l];
            ALIGNED_4( int16_t mvc[4][2] );

            /* Reverse-order MV prediction. */
            M32( mvc[0] ) = 0;
            M32( mvc[2] ) = 0;
#define MVC(mv) { CP32( mvc[i_mvc], mv ); i_mvc++; }
            if( i_mb_x < h->mb.i_mb_width - 1 )
                MVC( fenc_mv[1] );
            if( i_mb_y < h->mb.i_mb_height - 1 )
            {
                MVC( fenc_mv[i_mb_stride] );
                if( i_mb_x > 0 )
                    MVC( fenc_mv[i_mb_stride-1] );
                if( i_mb_x < h->mb.i_mb_width - 1 )
                    MVC( fenc_mv[i_mb_stride+1] );
            }
#undef MVC
            if( i_mvc <= 1 )
                CP32( m[l].mvp, mvc[0] );
            else
                x264_median_mv( m[l].mvp, mvc[0], mvc[1], mvc[2] );

            /* Fast skip for cases of near-zero residual.  Shortcut: don't bother except in the mv0 case,
             * since anything else is likely to have enough residual to not trigger the skip. */
            if( !M32( m[l].mvp ) )
            {
                m[l].cost = h->pixf.mbcmp[PIXEL_8x8]( m[l].p_fenc[0], FENC_STRIDE, m[l].p_fref[0], m[l].i_stride[0] );
                if( m[l].cost < 64 )
                {
                    M32( m[l].mv ) = 0;
                    goto skip_motionest;
                }
            }

            x264_me_search( h, &m[l], mvc, i_mvc );
            m[l].cost -= 2 * a->i_lambda; // remove mvcost from skip mbs
            if( M32( m[l].mv ) )
                m[l].cost += 5 * a->i_lambda;

skip_motionest:
            CP32( fenc_mvs[l], m[l].mv );
            *fenc_costs[l] = m[l].cost;
        }
        else
        {
            CP32( m[l].mv, fenc_mvs[l] );
            m[l].cost = *fenc_costs[l];
        }
        COPY2_IF_LT( i_bcost, m[l].cost, list_used, l+1 );
    }

    if( b_bidir && ( M32( m[0].mv ) || M32( m[1].mv ) ) )
        TRY_BIDIR( m[0].mv, m[1].mv, 5 );

lowres_intra_mb:
    if( !fenc->b_intra_calculated )
    {
        ALIGNED_ARRAY_16( pixel, edge,[33] );
        pixel *pix = &pix1[8+FDEC_STRIDE - 1];
        pixel *src = &fenc->lowres[0][i_pel_offset - 1];
        const int intra_penalty = 5 * a->i_lambda;
        int satds[3];

        memcpy( pix-FDEC_STRIDE, src-i_stride, 17 * sizeof(pixel) );
        for( int i = 0; i < 8; i++ )
            pix[i*FDEC_STRIDE] = src[i*i_stride];
        pix++;

        h->pixf.intra_mbcmp_x3_8x8c( h->mb.pic.p_fenc[0], pix, satds );
        int i_icost = X264_MIN3( satds[0], satds[1], satds[2] );

        if( h->param.analyse.i_subpel_refine > 1 )
        {
            h->predict_8x8c[I_PRED_CHROMA_P]( pix );
            int satd = h->pixf.mbcmp[PIXEL_8x8]( pix, FDEC_STRIDE, h->mb.pic.p_fenc[0], FENC_STRIDE );
            i_icost = X264_MIN( i_icost, satd );
            h->predict_8x8_filter( pix, edge, ALL_NEIGHBORS, ALL_NEIGHBORS );
            for( int i = 3; i < 9; i++ )
            {
                h->predict_8x8[i]( pix, edge );
                satd = h->pixf.mbcmp[PIXEL_8x8]( pix, FDEC_STRIDE, h->mb.pic.p_fenc[0], FENC_STRIDE );
                i_icost = X264_MIN( i_icost, satd );
            }
        }

        i_icost += intra_penalty;
        fenc->i_intra_cost[i_mb_xy] = i_icost;
        if( b_frame_score_mb )
        {
            int *row_satd_intra = frames[b]->i_row_satds[0][0];
            int i_icost_aq = i_icost;
            if( h->param.rc.i_aq_mode )
                i_icost_aq = (i_icost_aq * frames[b]->i_inv_qscale_factor[i_mb_xy] + 128) >> 8;
            fenc->i_cost_est[0][0] += i_icost;
            fenc->i_cost_est_aq[0][0] += i_icost_aq;
            row_satd_intra[h->mb.i_mb_y] += i_icost_aq;
        }
    }

    /* forbid intra-mbs in B-frames, because it's rare and not worth checking */
    /* FIXME: Should we still forbid them now that we cache intra scores? */
    if( !b_bidir )
    {
        int i_icost = fenc->i_intra_cost[i_mb_xy];
        int b_intra = i_icost < i_bcost;
        if( b_intra )
        {
            i_bcost = i_icost;
            list_used = 0;
        }
        if( b_frame_score_mb )
            fenc->i_intra_mbs[b-p0] += b_intra;
    }

    /* In an I-frame, we've already added the results above in the intra section. */
    if( p0 != p1 )
    {
        int i_bcost_aq = i_bcost;
        if( h->param.rc.i_aq_mode )
            i_bcost_aq = (i_bcost_aq * frames[b]->i_inv_qscale_factor[i_mb_xy] + 128) >> 8;
        fenc->i_row_satds[b-p0][p1-b][h->mb.i_mb_y] += i_bcost_aq;
        if( b_frame_score_mb )
        {
            /* Don't use AQ-weighted costs for slicetype decision, only for ratecontrol. */
            frames[b]->i_cost_est[b-p0][p1-b] += i_bcost;
            frames[b]->i_cost_est_aq[b-p0][p1-b] += i_bcost_aq;
        }
    }

    fenc->lowres_costs[b-p0][p1-b][i_mb_xy] = X264_MIN( i_bcost, LOWRES_COST_MASK ) + (list_used << LOWRES_COST_SHIFT);
}
#undef TRY_BIDIR

#define NUM_MBS\
   (h->mb.i_mb_width > 2 && h->mb.i_mb_height > 2 ?\
   (h->mb.i_mb_width - 2) * (h->mb.i_mb_height - 2) :\
    h->mb.i_mb_width * h->mb.i_mb_height)

static int x264_slicetype_frame_cost( x264_t *h, x264_mb_analysis_t *a,
                                      x264_frame_t **frames, int p0, int p1, int b,
                                      int b_intra_penalty )
{
    int i_score = 0;
    int do_search[2];
    const x264_weight_t *w = weight_none;
    /* Check whether we already evaluated this frame
     * If we have tried this frame as P, then we have also tried
     * the preceding frames as B. (is this still true?) */
    /* Also check that we already calculated the row SATDs for the current frame. */
    if( frames[b]->i_cost_est[b-p0][p1-b] >= 0 && (!h->param.rc.i_vbv_buffer_size || frames[b]->i_row_satds[b-p0][p1-b][0] != -1) )
        i_score = frames[b]->i_cost_est[b-p0][p1-b];
    else
    {
        int dist_scale_factor = 128;
        int *row_satd = frames[b]->i_row_satds[b-p0][p1-b];
        int *row_satd_intra = frames[b]->i_row_satds[0][0];

        /* For each list, check to see whether we have lowres motion-searched this reference frame before. */
        do_search[0] = b != p0 && frames[b]->lowres_mvs[0][b-p0-1][0][0] == 0x7FFF;
        do_search[1] = b != p1 && frames[b]->lowres_mvs[1][p1-b-1][0][0] == 0x7FFF;
        if( do_search[0] )
        {
            if( ( h->param.analyse.i_weighted_pred == X264_WEIGHTP_SMART ||
                  h->param.analyse.i_weighted_pred == X264_WEIGHTP_FAKE ) && b == p1 )
            {
                x264_emms();
                x264_weights_analyse( h, frames[b], frames[p0], 1 );
                w = frames[b]->weight[0];
            }
            frames[b]->lowres_mvs[0][b-p0-1][0][0] = 0;
        }
        if( do_search[1] ) frames[b]->lowres_mvs[1][p1-b-1][0][0] = 0;

        if( b == p1 )
            frames[b]->i_intra_mbs[b-p0] = 0;
        if( !frames[b]->b_intra_calculated )
        {
            frames[b]->i_cost_est[0][0] = 0;
            frames[b]->i_cost_est_aq[0][0] = 0;
        }
        if( p1 != p0 )
            dist_scale_factor = ( ((b-p0) << 8) + ((p1-p0) >> 1) ) / (p1-p0);

        frames[b]->i_cost_est[b-p0][p1-b] = 0;
        frames[b]->i_cost_est_aq[b-p0][p1-b] = 0;

        /* Lowres lookahead goes backwards because the MVs are used as predictors in the main encode.
         * This considerably improves MV prediction overall. */

        /* The edge mbs seem to reduce the predictive quality of the
         * whole frame's score, but are needed for a spatial distribution. */
        if( h->param.rc.b_mb_tree || h->param.rc.i_vbv_buffer_size ||
            h->mb.i_mb_width <= 2 || h->mb.i_mb_height <= 2 )
        {
            for( h->mb.i_mb_y = h->mb.i_mb_height - 1; h->mb.i_mb_y >= 0; h->mb.i_mb_y-- )
            {
                row_satd[h->mb.i_mb_y] = 0;
                if( !frames[b]->b_intra_calculated )
                    row_satd_intra[h->mb.i_mb_y] = 0;
                for( h->mb.i_mb_x = h->mb.i_mb_width - 1; h->mb.i_mb_x >= 0; h->mb.i_mb_x-- )
                    x264_slicetype_mb_cost( h, a, frames, p0, p1, b, dist_scale_factor, do_search, w );
            }
        }
        else
        {
            for( h->mb.i_mb_y = h->mb.i_mb_height - 2; h->mb.i_mb_y >= 1; h->mb.i_mb_y-- )
                for( h->mb.i_mb_x = h->mb.i_mb_width - 2; h->mb.i_mb_x >= 1; h->mb.i_mb_x-- )
                    x264_slicetype_mb_cost( h, a, frames, p0, p1, b, dist_scale_factor, do_search, w );
        }

        i_score = frames[b]->i_cost_est[b-p0][p1-b];
        if( b != p1 )
            i_score = (uint64_t)i_score * 100 / (120 + h->param.i_bframe_bias);
        else
            frames[b]->b_intra_calculated = 1;

        frames[b]->i_cost_est[b-p0][p1-b] = i_score;
        x264_emms();
    }

    if( b_intra_penalty )
    {
        // arbitrary penalty for I-blocks after B-frames
        int nmb = NUM_MBS;
        i_score += i_score * frames[b]->i_intra_mbs[b-p0] / (nmb * 8);
    }
    return i_score;
}

/* If MB-tree changes the quantizers, we need to recalculate the frame cost without
 * re-running lookahead. */
static int x264_slicetype_frame_cost_recalculate( x264_t *h, x264_frame_t **frames, int p0, int p1, int b )
{
    int i_score = 0;
    int *row_satd = frames[b]->i_row_satds[b-p0][p1-b];
    float *qp_offset = IS_X264_TYPE_B(frames[b]->i_type) ? frames[b]->f_qp_offset_aq : frames[b]->f_qp_offset;
    x264_emms();
    for( h->mb.i_mb_y = h->mb.i_mb_height - 1; h->mb.i_mb_y >= 0; h->mb.i_mb_y-- )
    {
        row_satd[ h->mb.i_mb_y ] = 0;
        for( h->mb.i_mb_x = h->mb.i_mb_width - 1; h->mb.i_mb_x >= 0; h->mb.i_mb_x-- )
        {
            int i_mb_xy = h->mb.i_mb_x + h->mb.i_mb_y*h->mb.i_mb_stride;
            int i_mb_cost = frames[b]->lowres_costs[b-p0][p1-b][i_mb_xy] & LOWRES_COST_MASK;
            float qp_adj = qp_offset[i_mb_xy];
            i_mb_cost = (i_mb_cost * x264_exp2fix8(qp_adj) + 128) >> 8;
            row_satd[ h->mb.i_mb_y ] += i_mb_cost;
            if( (h->mb.i_mb_y > 0 && h->mb.i_mb_y < h->mb.i_mb_height - 1 &&
                 h->mb.i_mb_x > 0 && h->mb.i_mb_x < h->mb.i_mb_width - 1) ||
                 h->mb.i_mb_width <= 2 || h->mb.i_mb_height <= 2 )
            {
                i_score += i_mb_cost;
            }
        }
    }
    return i_score;
}

static void x264_macroblock_tree_finish( x264_t *h, x264_frame_t *frame, int ref0_distance )
{
    x264_emms();
    float weightdelta = 0.0;
    if( ref0_distance && frame->f_weighted_cost_delta[ref0_distance-1] > 0 )
        weightdelta = (1.0 - frame->f_weighted_cost_delta[ref0_distance-1]);

    /* Allow the strength to be adjusted via qcompress, since the two
     * concepts are very similar. */
    float strength = 5.0f * (1.0f - h->param.rc.f_qcompress);
    for( int mb_index = 0; mb_index < h->mb.i_mb_count; mb_index++ )
    {
        int intra_cost = (frame->i_intra_cost[mb_index] * frame->i_inv_qscale_factor[mb_index]+128)>>8;
        if( intra_cost )
        {
            int propagate_cost = frame->i_propagate_cost[mb_index];
            float log2_ratio = x264_log2(intra_cost + propagate_cost) - x264_log2(intra_cost) + weightdelta;
            frame->f_qp_offset[mb_index] = frame->f_qp_offset_aq[mb_index] - strength * log2_ratio;
        }
    }
}

static void x264_macroblock_tree_propagate( x264_t *h, x264_frame_t **frames, int p0, int p1, int b, int referenced )
{
    uint16_t *ref_costs[2] = {frames[p0]->i_propagate_cost,frames[p1]->i_propagate_cost};
    int dist_scale_factor = ( ((b-p0) << 8) + ((p1-p0) >> 1) ) / (p1-p0);
    int i_bipred_weight = h->param.analyse.b_weighted_bipred ? 64 - (dist_scale_factor>>2) : 32;
    int16_t (*mvs[2])[2] = { frames[b]->lowres_mvs[0][b-p0-1], frames[b]->lowres_mvs[1][p1-b-1] };
    int bipred_weights[2] = {i_bipred_weight, 64 - i_bipred_weight};
    int *buf = h->scratch_buffer;
    uint16_t *propagate_cost = frames[b]->i_propagate_cost;

    /* For non-reffed frames the source costs are always zero, so just memset one row and re-use it. */
    if( !referenced )
        memset( frames[b]->i_propagate_cost, 0, h->mb.i_mb_width * sizeof(uint16_t) );

    for( h->mb.i_mb_y = 0; h->mb.i_mb_y < h->mb.i_mb_height; h->mb.i_mb_y++ )
    {
        int mb_index = h->mb.i_mb_y*h->mb.i_mb_stride;
        h->mc.mbtree_propagate_cost( buf, propagate_cost,
            frames[b]->i_intra_cost+mb_index, frames[b]->lowres_costs[b-p0][p1-b]+mb_index,
            frames[b]->i_inv_qscale_factor+mb_index, h->mb.i_mb_width );
        if( referenced )
            propagate_cost += h->mb.i_mb_width;
        for( h->mb.i_mb_x = 0; h->mb.i_mb_x < h->mb.i_mb_width; h->mb.i_mb_x++, mb_index++ )
        {
            int propagate_amount = buf[h->mb.i_mb_x];
            /* Don't propagate for an intra block. */
            if( propagate_amount > 0 )
            {
                /* Access width-2 bitfield. */
                int lists_used = frames[b]->lowres_costs[b-p0][p1-b][mb_index] >> LOWRES_COST_SHIFT;
                /* Follow the MVs to the previous frame(s). */
                for( int list = 0; list < 2; list++ )
                    if( (lists_used >> list)&1 )
                    {
#define CLIP_ADD(s,x) (s) = X264_MIN((s)+(x),(1<<16)-1)
                        int listamount = propagate_amount;
                        /* Apply bipred weighting. */
                        if( lists_used == 3 )
                            listamount = (listamount * bipred_weights[list] + 32) >> 6;

                        /* Early termination for simple case of mv0. */
                        if( !M32( mvs[list][mb_index] ) )
                        {
                            CLIP_ADD( ref_costs[list][mb_index], listamount );
                            continue;
                        }

                        int x = mvs[list][mb_index][0];
                        int y = mvs[list][mb_index][1];
                        int mbx = (x>>5)+h->mb.i_mb_x;
                        int mby = (y>>5)+h->mb.i_mb_y;
                        int idx0 = mbx + mby * h->mb.i_mb_stride;
                        int idx1 = idx0 + 1;
                        int idx2 = idx0 + h->mb.i_mb_stride;
                        int idx3 = idx0 + h->mb.i_mb_stride + 1;
                        x &= 31;
                        y &= 31;
                        int idx0weight = (32-y)*(32-x);
                        int idx1weight = (32-y)*x;
                        int idx2weight = y*(32-x);
                        int idx3weight = y*x;

                        /* We could just clip the MVs, but pixels that lie outside the frame probably shouldn't
                         * be counted. */
                        if( mbx < h->mb.i_mb_width-1 && mby < h->mb.i_mb_height-1 && mbx >= 0 && mby >= 0 )
                        {
                            CLIP_ADD( ref_costs[list][idx0], (listamount*idx0weight+512)>>10 );
                            CLIP_ADD( ref_costs[list][idx1], (listamount*idx1weight+512)>>10 );
                            CLIP_ADD( ref_costs[list][idx2], (listamount*idx2weight+512)>>10 );
                            CLIP_ADD( ref_costs[list][idx3], (listamount*idx3weight+512)>>10 );
                        }
                        else /* Check offsets individually */
                        {
                            if( mbx < h->mb.i_mb_width && mby < h->mb.i_mb_height && mbx >= 0 && mby >= 0 )
                                CLIP_ADD( ref_costs[list][idx0], (listamount*idx0weight+512)>>10 );
                            if( mbx+1 < h->mb.i_mb_width && mby < h->mb.i_mb_height && mbx+1 >= 0 && mby >= 0 )
                                CLIP_ADD( ref_costs[list][idx1], (listamount*idx1weight+512)>>10 );
                            if( mbx < h->mb.i_mb_width && mby+1 < h->mb.i_mb_height && mbx >= 0 && mby+1 >= 0 )
                                CLIP_ADD( ref_costs[list][idx2], (listamount*idx2weight+512)>>10 );
                            if( mbx+1 < h->mb.i_mb_width && mby+1 < h->mb.i_mb_height && mbx+1 >= 0 && mby+1 >= 0 )
                                CLIP_ADD( ref_costs[list][idx3], (listamount*idx3weight+512)>>10 );
                        }
                    }
            }
        }
    }

    if( h->param.rc.i_vbv_buffer_size && h->param.rc.i_lookahead && referenced )
        x264_macroblock_tree_finish( h, frames[b], b == p1 ? b - p0 : 0 );
}

static void x264_macroblock_tree( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int num_frames, int b_intra )
{
    int idx = !b_intra;
    int last_nonb, cur_nonb = 1;
    int bframes = 0;
    int i = num_frames;

    if( b_intra )
        x264_slicetype_frame_cost( h, a, frames, 0, 0, 0, 0 );

    while( i > 0 && frames[i]->i_type == X264_TYPE_B )
        i--;
    last_nonb = i;

    /* Lookaheadless MB-tree is not a theoretically distinct case; the same extrapolation could
     * be applied to the end of a lookahead buffer of any size.  However, it's most needed when
     * lookahead=0, so that's what's currently implemented. */
    if( !h->param.rc.i_lookahead )
    {
        if( b_intra )
        {
            memset( frames[0]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
            memcpy( frames[0]->f_qp_offset, frames[0]->f_qp_offset_aq, h->mb.i_mb_count * sizeof(float) );
            return;
        }
        XCHG( uint16_t*, frames[last_nonb]->i_propagate_cost, frames[0]->i_propagate_cost );
        memset( frames[0]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
    }
    else
    {
        if( last_nonb < idx )
            return;
        memset( frames[last_nonb]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
    }

    while( i-- > idx )
    {
        cur_nonb = i;
        while( frames[cur_nonb]->i_type == X264_TYPE_B && cur_nonb > 0 )
            cur_nonb--;
        if( cur_nonb < idx )
            break;
        x264_slicetype_frame_cost( h, a, frames, cur_nonb, last_nonb, last_nonb, 0 );
        memset( frames[cur_nonb]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
        bframes = last_nonb - cur_nonb - 1;
        if( h->param.i_bframe_pyramid && bframes > 1 )
        {
            int middle = (bframes + 1)/2 + cur_nonb;
            x264_slicetype_frame_cost( h, a, frames, cur_nonb, last_nonb, middle, 0 );
            memset( frames[middle]->i_propagate_cost, 0, h->mb.i_mb_count * sizeof(uint16_t) );
            while( i > cur_nonb )
            {
                int p0 = i > middle ? middle : cur_nonb;
                int p1 = i < middle ? middle : last_nonb;
                if( i != middle )
                {
                    x264_slicetype_frame_cost( h, a, frames, p0, p1, i, 0 );
                    x264_macroblock_tree_propagate( h, frames, p0, p1, i, 0 );
                }
                i--;
            }
            x264_macroblock_tree_propagate( h, frames, cur_nonb, last_nonb, middle, 1 );
        }
        else
        {
            while( i > cur_nonb )
            {
                x264_slicetype_frame_cost( h, a, frames, cur_nonb, last_nonb, i, 0 );
                x264_macroblock_tree_propagate( h, frames, cur_nonb, last_nonb, i, 0 );
                i--;
            }
        }
        x264_macroblock_tree_propagate( h, frames, cur_nonb, last_nonb, last_nonb, 1 );
        last_nonb = cur_nonb;
    }

    if( !h->param.rc.i_lookahead )
    {
        x264_macroblock_tree_propagate( h, frames, 0, last_nonb, last_nonb, 1 );
        XCHG( uint16_t*, frames[last_nonb]->i_propagate_cost, frames[0]->i_propagate_cost );
    }

    x264_macroblock_tree_finish( h, frames[last_nonb], last_nonb );
    if( h->param.i_bframe_pyramid && bframes > 1 && !h->param.rc.i_vbv_buffer_size )
        x264_macroblock_tree_finish( h, frames[last_nonb+(bframes+1)/2], 0 );
}

static int x264_vbv_frame_cost( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int p0, int p1, int b )
{
    int cost = x264_slicetype_frame_cost( h, a, frames, p0, p1, b, 0 );
    if( h->param.rc.i_aq_mode )
    {
        if( h->param.rc.b_mb_tree )
            return x264_slicetype_frame_cost_recalculate( h, frames, p0, p1, b );
        else
            return frames[b]->i_cost_est_aq[b-p0][p1-b];
    }
    return cost;
}

static void x264_calculate_durations( x264_t *h, x264_frame_t *cur_frame, x264_frame_t *prev_frame, int *i_cpb_delay, int *i_coded_fields )
{
    cur_frame->i_cpb_delay = *i_cpb_delay;
    cur_frame->i_dpb_output_delay = cur_frame->i_field_cnt - *i_coded_fields;

    // add a correction term for frame reordering
    cur_frame->i_dpb_output_delay += h->sps->vui.i_num_reorder_frames*2;

    // fix possible negative dpb_output_delay because of pulldown changes and reordering
    if( cur_frame->i_dpb_output_delay < 0 )
    {
        cur_frame->i_cpb_delay += cur_frame->i_dpb_output_delay;
        cur_frame->i_dpb_output_delay = 0;
        if( prev_frame )
            prev_frame->i_cpb_duration += cur_frame->i_dpb_output_delay;
    }

    if( cur_frame->b_keyframe )
        *i_cpb_delay = 0;

    *i_cpb_delay += cur_frame->i_duration;
    *i_coded_fields += cur_frame->i_duration;
    cur_frame->i_cpb_duration = cur_frame->i_duration;
}

static void x264_vbv_lookahead( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int num_frames, int keyframe )
{
    int last_nonb = 0, cur_nonb = 1, idx = 0;
    x264_frame_t *prev_frame = NULL;
    int prev_frame_idx = 0;
    while( cur_nonb < num_frames && frames[cur_nonb]->i_type == X264_TYPE_B )
        cur_nonb++;
    int next_nonb = keyframe ? last_nonb : cur_nonb;

    if( frames[cur_nonb]->i_coded_fields_lookahead >= 0 )
    {
        h->i_coded_fields_lookahead = frames[cur_nonb]->i_coded_fields_lookahead;
        h->i_cpb_delay_lookahead = frames[cur_nonb]->i_cpb_delay_lookahead;
    }

    while( cur_nonb < num_frames )
    {
        /* P/I cost: This shouldn't include the cost of next_nonb */
        if( next_nonb != cur_nonb )
        {
            int p0 = IS_X264_TYPE_I( frames[cur_nonb]->i_type ) ? cur_nonb : last_nonb;
            frames[next_nonb]->i_planned_satd[idx] = x264_vbv_frame_cost( h, a, frames, p0, cur_nonb, cur_nonb );
            frames[next_nonb]->i_planned_type[idx] = frames[cur_nonb]->i_type;
            frames[cur_nonb]->i_coded_fields_lookahead = h->i_coded_fields_lookahead;
            frames[cur_nonb]->i_cpb_delay_lookahead = h->i_cpb_delay_lookahead;
            x264_calculate_durations( h, frames[cur_nonb], prev_frame, &h->i_cpb_delay_lookahead, &h->i_coded_fields_lookahead );
            if( prev_frame )
            {
                frames[next_nonb]->f_planned_cpb_duration[prev_frame_idx] = (double)prev_frame->i_cpb_duration *
                                                                            h->sps->vui.i_num_units_in_tick / h->sps->vui.i_time_scale;
            }
            frames[next_nonb]->f_planned_cpb_duration[idx] = (double)frames[cur_nonb]->i_cpb_duration *
                                                             h->sps->vui.i_num_units_in_tick / h->sps->vui.i_time_scale;
            prev_frame = frames[cur_nonb];
            prev_frame_idx = idx;
            idx++;
        }
        /* Handle the B-frames: coded order */
        for( int i = last_nonb+1; i < cur_nonb; i++, idx++ )
        {
            frames[next_nonb]->i_planned_satd[idx] = x264_vbv_frame_cost( h, a, frames, last_nonb, cur_nonb, i );
            frames[next_nonb]->i_planned_type[idx] = X264_TYPE_B;
            frames[i]->i_coded_fields_lookahead = h->i_coded_fields_lookahead;
            frames[i]->i_cpb_delay_lookahead = h->i_cpb_delay_lookahead;
            x264_calculate_durations( h, frames[i], prev_frame, &h->i_cpb_delay_lookahead, &h->i_coded_fields_lookahead );
            if( prev_frame )
            {
                frames[next_nonb]->f_planned_cpb_duration[prev_frame_idx] = (double)prev_frame->i_cpb_duration *
                                                                            h->sps->vui.i_num_units_in_tick / h->sps->vui.i_time_scale;
            }
            frames[next_nonb]->f_planned_cpb_duration[idx] = (double)frames[i]->i_cpb_duration *
                                                             h->sps->vui.i_num_units_in_tick / h->sps->vui.i_time_scale;
            prev_frame = frames[i];
            prev_frame_idx = idx;
        }
        last_nonb = cur_nonb;
        cur_nonb++;
        while( cur_nonb <= num_frames && frames[cur_nonb]->i_type == X264_TYPE_B )
            cur_nonb++;
    }
    frames[next_nonb]->i_planned_type[idx] = X264_TYPE_AUTO;
}

static int x264_slicetype_path_cost( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, char *path, int threshold )
{
    int loc = 1;
    int cost = 0;
    int cur_p = 0;
    path--; /* Since the 1st path element is really the second frame */
    while( path[loc] )
    {
        int next_p = loc;
        /* Find the location of the next P-frame. */
        while( path[next_p] != 'P' )
            next_p++;

        /* Add the cost of the P-frame found above */
        cost += x264_slicetype_frame_cost( h, a, frames, cur_p, next_p, next_p, 0 );
        /* Early terminate if the cost we have found is larger than the best path cost so far */
        if( cost > threshold )
            break;

        if( h->param.i_bframe_pyramid && next_p - cur_p > 2 )
        {
            int middle = cur_p + (next_p - cur_p)/2;
            cost += x264_slicetype_frame_cost( h, a, frames, cur_p, next_p, middle, 0 );
            for( int next_b = loc; next_b < middle && cost < threshold; next_b++ )
                cost += x264_slicetype_frame_cost( h, a, frames, cur_p, middle, next_b, 0 );
            for( int next_b = middle+1; next_b < next_p && cost < threshold; next_b++ )
                cost += x264_slicetype_frame_cost( h, a, frames, middle, next_p, next_b, 0 );
        }
        else
            for( int next_b = loc; next_b < next_p && cost < threshold; next_b++ )
                cost += x264_slicetype_frame_cost( h, a, frames, cur_p, next_p, next_b, 0 );

        loc = next_p + 1;
        cur_p = next_p;
    }
    return cost;
}

/* Viterbi/trellis slicetype decision algorithm. */
/* Uses strings due to the fact that the speed of the control functions is
   negligible compared to the cost of running slicetype_frame_cost, and because
   it makes debugging easier. */
static void x264_slicetype_path( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int length, char (*best_paths)[X264_LOOKAHEAD_MAX] )
{
    char paths[2][X264_LOOKAHEAD_MAX];
    int num_paths = X264_MIN( h->param.i_bframe+1, length );
    int best_cost = COST_MAX;
    int idx = 0;

    /* Iterate over all currently possible paths */
    for( int path = 0; path < num_paths; path++ )
    {
        /* Add suffixes to the current path */
        int len = length - (path + 1);
        memcpy( paths[idx], best_paths[len % (X264_BFRAME_MAX+1)], len );
        memset( paths[idx]+len, 'B', path );
        strcpy( paths[idx]+len+path, "P" );

        /* Calculate the actual cost of the current path */
        int cost = x264_slicetype_path_cost( h, a, frames, paths[idx], best_cost );
        if( cost < best_cost )
        {
            best_cost = cost;
            idx ^= 1;
        }
    }

    /* Store the best path. */
    memcpy( best_paths[length % (X264_BFRAME_MAX+1)], paths[idx^1], length );
}

static int scenecut_internal( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int p0, int p1, int print )
{
    x264_frame_t *frame = frames[p1];
    x264_slicetype_frame_cost( h, a, frames, p0, p1, p1, 0 );

    int icost = frame->i_cost_est[0][0];
    int pcost = frame->i_cost_est[p1-p0][0];
    float f_bias;
    int i_gop_size = frame->i_frame - h->lookahead->i_last_keyframe;
    float f_thresh_max = h->param.i_scenecut_threshold / 100.0;
    /* magic numbers pulled out of thin air */
    float f_thresh_min = f_thresh_max * 0.25;
    int res;

    if( h->param.i_keyint_min == h->param.i_keyint_max )
        f_thresh_min = f_thresh_max;
    if( i_gop_size <= h->param.i_keyint_min / 4 || h->param.b_intra_refresh )
        f_bias = f_thresh_min / 4;
    else if( i_gop_size <= h->param.i_keyint_min )
        f_bias = f_thresh_min * i_gop_size / h->param.i_keyint_min;
    else
    {
        f_bias = f_thresh_min
                 + ( f_thresh_max - f_thresh_min )
                 * ( i_gop_size - h->param.i_keyint_min )
                 / ( h->param.i_keyint_max - h->param.i_keyint_min );
    }

    res = pcost >= (1.0 - f_bias) * icost;
    if( res && print )
    {
        int imb = frame->i_intra_mbs[p1-p0];
        int pmb = NUM_MBS - imb;
        x264_log( h, X264_LOG_DEBUG, "scene cut at %d Icost:%d Pcost:%d ratio:%.4f bias:%.4f gop:%d (imb:%d pmb:%d)\n",
                  frame->i_frame,
                  icost, pcost, 1. - (double)pcost / icost,
                  f_bias, i_gop_size, imb, pmb );
    }
    return res;
}

static int scenecut( x264_t *h, x264_mb_analysis_t *a, x264_frame_t **frames, int p0, int p1, int real_scenecut, int num_frames )
{
    /* Only do analysis during a normal scenecut check. */
    if( real_scenecut && h->param.i_bframe )
    {
        int maxp1 = p0 + 1;
        /* Look ahead to avoid coding short flashes as scenecuts. */
        if( h->param.i_bframe_adaptive == X264_B_ADAPT_TRELLIS )
            /* Don't analyse any more frames than the trellis would have covered. */
            maxp1 += h->param.i_bframe;
        else
            maxp1++;
        maxp1 = X264_MIN( maxp1, num_frames );

        /* Where A and B are scenes: AAAAAABBBAAAAAA
         * If BBB is shorter than (maxp1-p0), it is detected as a flash
         * and not considered a scenecut. */
        for( int curp1 = p1; curp1 <= maxp1; curp1++ )
            if( !scenecut_internal( h, a, frames, p0, curp1, 0 ) )
                /* Any frame in between p0 and cur_p1 cannot be a real scenecut. */
                for( int i = curp1; i > p0; i-- )
                    frames[i]->b_scenecut = 0;

        /* Where A-F are scenes: AAAAABBCCDDEEFFFFFF
         * If each of BB ... EE are shorter than (maxp1-p0), they are
         * detected as flashes and not considered scenecuts.
         * Instead, the first F frame becomes a scenecut. */
        for( int curp0 = p0; curp0 < maxp1; curp0++ )
            if( scenecut_internal( h, a, frames, curp0, maxp1, 0 ) )
                /* If cur_p0 is the p0 of a scenecut, it cannot be the p1 of a scenecut. */
                    frames[curp0]->b_scenecut = 0;
    }

    /* Ignore frames that are part of a flash, i.e. cannot be real scenecuts. */
    if( !frames[p1]->b_scenecut )
        return 0;
    return scenecut_internal( h, a, frames, p0, p1, real_scenecut );
}

void x264_slicetype_analyse( x264_t *h, int keyframe )
{
    x264_mb_analysis_t a;
    x264_frame_t *frames[X264_LOOKAHEAD_MAX+3] = { NULL, };
    int num_frames, orig_num_frames, keyint_limit, framecnt;
    int i_mb_count = NUM_MBS;
    int cost1p0, cost2p0, cost1b1, cost2p1;
    int i_max_search = X264_MIN( h->lookahead->next.i_size, X264_LOOKAHEAD_MAX );
    int vbv_lookahead = h->param.rc.i_vbv_buffer_size && h->param.rc.i_lookahead;
    if( h->param.b_deterministic )
        i_max_search = X264_MIN( i_max_search, h->lookahead->i_slicetype_length + !keyframe );

    assert( h->frames.b_have_lowres );

    if( !h->lookahead->last_nonb )
        return;
    frames[0] = h->lookahead->last_nonb;
    for( framecnt = 0; framecnt < i_max_search && h->lookahead->next.list[framecnt]->i_type == X264_TYPE_AUTO; framecnt++ )
        frames[framecnt+1] = h->lookahead->next.list[framecnt];

    if( !framecnt )
    {
        if( h->param.rc.b_mb_tree )
            x264_macroblock_tree( h, &a, frames, 0, keyframe );
        return;
    }

    keyint_limit = h->param.i_keyint_max - frames[0]->i_frame + h->lookahead->i_last_keyframe - 1;
    orig_num_frames = num_frames = h->param.b_intra_refresh ? framecnt : X264_MIN( framecnt, keyint_limit );

    x264_lowres_context_init( h, &a );

    /* This is important psy-wise: if we have a non-scenecut keyframe,
     * there will be significant visual artifacts if the frames just before
     * go down in quality due to being referenced less, despite it being
     * more RD-optimal. */
    if( (h->param.analyse.b_psy && h->param.rc.b_mb_tree) || vbv_lookahead )
        num_frames = framecnt;
    else if( num_frames == 0 )
    {
        frames[1]->i_type = X264_TYPE_I;
        return;
    }

    int num_bframes = 0;
    int num_analysed_frames = num_frames;
    int reset_start;
    if( h->param.i_scenecut_threshold && scenecut( h, &a, frames, 0, 1, 1, orig_num_frames ) )
    {
        frames[1]->i_type = X264_TYPE_I;
        return;
    }

    if( h->param.i_bframe )
    {
        if( h->param.i_bframe_adaptive == X264_B_ADAPT_TRELLIS )
        {
            if( num_frames > 1 )
            {
                char best_paths[X264_BFRAME_MAX+1][X264_LOOKAHEAD_MAX] = {"","P"};
                int best_path_index = (num_frames-1) % (X264_BFRAME_MAX+1);

                /* Perform the frametype analysis. */
                for( int j = 2; j < num_frames; j++ )
                    x264_slicetype_path( h, &a, frames, j, best_paths );

                num_bframes = strspn( best_paths[best_path_index], "B" );
                /* Load the results of the analysis into the frame types. */
                for( int j = 1; j < num_frames; j++ )
                    frames[j]->i_type = best_paths[best_path_index][j-1] == 'B' ? X264_TYPE_B : X264_TYPE_P;
            }
            frames[num_frames]->i_type = X264_TYPE_P;
        }
        else if( h->param.i_bframe_adaptive == X264_B_ADAPT_FAST )
        {
            for( int i = 0; i <= num_frames-2; )
            {
                cost2p1 = x264_slicetype_frame_cost( h, &a, frames, i+0, i+2, i+2, 1 );
                if( frames[i+2]->i_intra_mbs[2] > i_mb_count / 2 )
                {
                    frames[i+1]->i_type = X264_TYPE_P;
                    frames[i+2]->i_type = X264_TYPE_P;
                    i += 2;
                    continue;
                }

                cost1b1 = x264_slicetype_frame_cost( h, &a, frames, i+0, i+2, i+1, 0 );
                cost1p0 = x264_slicetype_frame_cost( h, &a, frames, i+0, i+1, i+1, 0 );
                cost2p0 = x264_slicetype_frame_cost( h, &a, frames, i+1, i+2, i+2, 0 );

                if( cost1p0 + cost2p0 < cost1b1 + cost2p1 )
                {
                    frames[i+1]->i_type = X264_TYPE_P;
                    i += 1;
                    continue;
                }

                // arbitrary and untuned
                #define INTER_THRESH 300
                #define P_SENS_BIAS (50 - h->param.i_bframe_bias)
                frames[i+1]->i_type = X264_TYPE_B;

                int j;
                for( j = i+2; j <= X264_MIN( i+h->param.i_bframe, num_frames-1 ); j++ )
                {
                    int pthresh = X264_MAX(INTER_THRESH - P_SENS_BIAS * (j-i-1), INTER_THRESH/10);
                    int pcost = x264_slicetype_frame_cost( h, &a, frames, i+0, j+1, j+1, 1 );
                    if( pcost > pthresh*i_mb_count || frames[j+1]->i_intra_mbs[j-i+1] > i_mb_count/3 )
                        break;
                    frames[j]->i_type = X264_TYPE_B;
                }
                frames[j]->i_type = X264_TYPE_P;
                i = j;
            }
            frames[num_frames]->i_type = X264_TYPE_P;
            num_bframes = 0;
            while( num_bframes < num_frames && frames[num_bframes+1]->i_type == X264_TYPE_B )
                num_bframes++;
        }
        else
        {
            num_bframes = X264_MIN(num_frames-1, h->param.i_bframe);
            for( int j = 1; j < num_frames; j++ )
                frames[j]->i_type = (j%(num_bframes+1)) ? X264_TYPE_B : X264_TYPE_P;
            frames[num_frames]->i_type = X264_TYPE_P;
        }

        /* Check scenecut on the first minigop. */
        for( int j = 1; j < num_bframes+1; j++ )
            if( h->param.i_scenecut_threshold && scenecut( h, &a, frames, j, j+1, 0, orig_num_frames ) )
            {
                frames[j]->i_type = X264_TYPE_P;
                num_analysed_frames = j;
                break;
            }

        reset_start = keyframe ? 1 : X264_MIN( num_bframes+2, num_analysed_frames+1 );
    }
    else
    {
        for( int j = 1; j <= num_frames; j++ )
            frames[j]->i_type = X264_TYPE_P;
        reset_start = !keyframe + 1;
        num_bframes = 0;
    }

    /* Perform the actual macroblock tree analysis.
     * Don't go farther than the maximum keyframe interval; this helps in short GOPs. */
    if( h->param.rc.b_mb_tree )
        x264_macroblock_tree( h, &a, frames, X264_MIN(num_frames, h->param.i_keyint_max), keyframe );

    /* Enforce keyframe limit. */
    if( !h->param.b_intra_refresh )
        for( int i = keyint_limit+1; i <= num_frames; i += h->param.i_keyint_max )
        {
            frames[i]->i_type = X264_TYPE_I;
            reset_start = X264_MIN( reset_start, i+1 );
            if( h->param.i_open_gop == X264_OPEN_GOP_BLURAY )
                while( IS_X264_TYPE_B( frames[i-1]->i_type ) )
                    i--;
        }

    if( vbv_lookahead )
        x264_vbv_lookahead( h, &a, frames, num_frames, keyframe );

    /* Restore frametypes for all frames that haven't actually been decided yet. */
    for( int j = reset_start; j <= num_frames; j++ )
        frames[j]->i_type = X264_TYPE_AUTO;
}

void x264_slicetype_decide( x264_t *h )
{
    x264_frame_t *frames[X264_BFRAME_MAX+2];
    x264_frame_t *frm;
    int bframes;
    int brefs;

    if( !h->lookahead->next.i_size )
        return;

    int lookahead_size = h->lookahead->next.i_size;

    if( h->param.rc.i_rc_method == X264_RC_ABR || h->param.rc.b_stat_write || h->param.rc.i_vbv_buffer_size )
    {
        for( int i = 0; i < h->lookahead->next.i_size; i++ )
        {
            if( h->param.b_vfr_input )
            {
                if( lookahead_size-- > 1 )
                    h->lookahead->next.list[i]->i_duration = 2 * (h->lookahead->next.list[i+1]->i_pts - h->lookahead->next.list[i]->i_pts);
                else
                    h->lookahead->next.list[i]->i_duration = h->i_prev_duration;
            }
            else
                h->lookahead->next.list[i]->i_duration = delta_tfi_divisor[h->lookahead->next.list[i]->i_pic_struct];
            h->i_prev_duration = h->lookahead->next.list[i]->i_duration;

            if( h->lookahead->next.list[i]->i_frame > h->i_disp_fields_last_frame && lookahead_size > 0 )
            {
                h->lookahead->next.list[i]->i_field_cnt = h->i_disp_fields;
                h->i_disp_fields += h->lookahead->next.list[i]->i_duration;
                h->i_disp_fields_last_frame = h->lookahead->next.list[i]->i_frame;
            }
            else if( lookahead_size == 0 )
            {
                h->lookahead->next.list[i]->i_field_cnt = h->i_disp_fields;
                h->lookahead->next.list[i]->i_duration = h->i_prev_duration;
            }
        }
    }

    if( h->param.rc.b_stat_read )
    {
        /* Use the frame types from the first pass */
        for( int i = 0; i < h->lookahead->next.i_size; i++ )
            h->lookahead->next.list[i]->i_type =
                x264_ratecontrol_slice_type( h, h->lookahead->next.list[i]->i_frame );
    }
    else if( (h->param.i_bframe && h->param.i_bframe_adaptive)
             || h->param.i_scenecut_threshold
             || h->param.rc.b_mb_tree
             || (h->param.rc.i_vbv_buffer_size && h->param.rc.i_lookahead) )
        x264_slicetype_analyse( h, 0 );

    for( bframes = 0, brefs = 0;; bframes++ )
    {
        frm = h->lookahead->next.list[bframes];
        if( frm->i_type == X264_TYPE_BREF && h->param.i_bframe_pyramid < X264_B_PYRAMID_NORMAL &&
            brefs == h->param.i_bframe_pyramid )
        {
            frm->i_type = X264_TYPE_B;
            x264_log( h, X264_LOG_WARNING, "B-ref at frame %d incompatible with B-pyramid %s \n",
                      frm->i_frame, x264_b_pyramid_names[h->param.i_bframe_pyramid] );
        }
        /* pyramid with multiple B-refs needs a big enough dpb that the preceding P-frame stays available.
           smaller dpb could be supported by smart enough use of mmco, but it's easier just to forbid it. */
        else if( frm->i_type == X264_TYPE_BREF && h->param.i_bframe_pyramid == X264_B_PYRAMID_NORMAL &&
            brefs && h->param.i_frame_reference <= (brefs+3) )
        {
            frm->i_type = X264_TYPE_B;
            x264_log( h, X264_LOG_WARNING, "B-ref at frame %d incompatible with B-pyramid %s and %d reference frames\n",
                      frm->i_frame, x264_b_pyramid_names[h->param.i_bframe_pyramid], h->param.i_frame_reference );
        }

        if( frm->i_type == X264_TYPE_KEYFRAME )
            frm->i_type = h->param.i_open_gop ? X264_TYPE_I : X264_TYPE_IDR;

        /* Limit GOP size */
        if( (!h->param.b_intra_refresh || frm->i_frame == 0) && frm->i_frame - h->lookahead->i_last_keyframe >= h->param.i_keyint_max )
        {
            if( frm->i_type == X264_TYPE_AUTO || frm->i_type == X264_TYPE_I )
                frm->i_type = h->param.i_open_gop && h->lookahead->i_last_keyframe >= 0 ? X264_TYPE_I : X264_TYPE_IDR;
            int warn = frm->i_type != X264_TYPE_IDR;
            if( warn && h->param.i_open_gop )
                warn &= frm->i_type != X264_TYPE_I;
            if( warn )
                x264_log( h, X264_LOG_WARNING, "specified frame type (%d) at %d is not compatible with keyframe interval\n", frm->i_type, frm->i_frame );
        }
        if( frm->i_type == X264_TYPE_I && frm->i_frame - h->lookahead->i_last_keyframe >= h->param.i_keyint_min )
        {
            if( h->param.i_open_gop )
            {
                h->lookahead->i_last_keyframe = frm->i_frame; // Use display order
                if( h->param.i_open_gop == X264_OPEN_GOP_BLURAY )
                    h->lookahead->i_last_keyframe -= bframes; // Use bluray order
                frm->b_keyframe = 1;
            }
            else
                frm->i_type = X264_TYPE_IDR;
        }
        if( frm->i_type == X264_TYPE_IDR )
        {
            /* Close GOP */
            h->lookahead->i_last_keyframe = frm->i_frame;
            frm->b_keyframe = 1;
            if( bframes > 0 )
            {
                bframes--;
                h->lookahead->next.list[bframes]->i_type = X264_TYPE_P;
            }
        }

        if( bframes == h->param.i_bframe ||
            !h->lookahead->next.list[bframes+1] )
        {
            if( IS_X264_TYPE_B( frm->i_type ) )
                x264_log( h, X264_LOG_WARNING, "specified frame type is not compatible with max B-frames\n" );
            if( frm->i_type == X264_TYPE_AUTO
                || IS_X264_TYPE_B( frm->i_type ) )
                frm->i_type = X264_TYPE_P;
        }

        if( frm->i_type == X264_TYPE_BREF )
            brefs++;

        if( frm->i_type == X264_TYPE_AUTO )
            frm->i_type = X264_TYPE_B;

        else if( !IS_X264_TYPE_B( frm->i_type ) ) break;
    }

    if( bframes )
        h->lookahead->next.list[bframes-1]->b_last_minigop_bframe = 1;
    h->lookahead->next.list[bframes]->i_bframes = bframes;

    /* insert a bref into the sequence */
    if( h->param.i_bframe_pyramid && bframes > 1 && !brefs )
    {
        h->lookahead->next.list[bframes/2]->i_type = X264_TYPE_BREF;
        brefs++;
    }

    /* calculate the frame costs ahead of time for x264_rc_analyse_slice while we still have lowres */
    if( h->param.rc.i_rc_method != X264_RC_CQP )
    {
        x264_mb_analysis_t a;
        int p0, p1, b;
        p1 = b = bframes + 1;

        x264_lowres_context_init( h, &a );

        frames[0] = h->lookahead->last_nonb;
        memcpy( &frames[1], h->lookahead->next.list, (bframes+1) * sizeof(x264_frame_t*) );
        if( IS_X264_TYPE_I( h->lookahead->next.list[bframes]->i_type ) )
            p0 = bframes + 1;
        else // P
            p0 = 0;

        x264_slicetype_frame_cost( h, &a, frames, p0, p1, b, 0 );

        if( (p0 != p1 || bframes) && h->param.rc.i_vbv_buffer_size )
        {
            /* We need the intra costs for row SATDs. */
            x264_slicetype_frame_cost( h, &a, frames, b, b, b, 0 );

            /* We need B-frame costs for row SATDs. */
            p0 = 0;
            for( b = 1; b <= bframes; b++ )
            {
                if( frames[b]->i_type == X264_TYPE_B )
                    for( p1 = b; frames[p1]->i_type == X264_TYPE_B; )
                        p1++;
                else
                    p1 = bframes + 1;
                x264_slicetype_frame_cost( h, &a, frames, p0, p1, b, 0 );
                if( frames[b]->i_type == X264_TYPE_BREF )
                    p0 = b;
            }
        }
    }

    /* Analyse for weighted P frames */
    if( !h->param.rc.b_stat_read && h->lookahead->next.list[bframes]->i_type == X264_TYPE_P
        && h->param.analyse.i_weighted_pred == X264_WEIGHTP_SMART )
    {
        x264_emms();
        x264_weights_analyse( h, h->lookahead->next.list[bframes], h->lookahead->last_nonb, 0 );
    }

    /* shift sequence to coded order.
       use a small temporary list to avoid shifting the entire next buffer around */
    int i_coded = h->lookahead->next.list[0]->i_frame;
    if( bframes )
    {
        int idx_list[] = { brefs+1, 1 };
        for( int i = 0; i < bframes; i++ )
        {
            int idx = idx_list[h->lookahead->next.list[i]->i_type == X264_TYPE_BREF]++;
            frames[idx] = h->lookahead->next.list[i];
            frames[idx]->i_reordered_pts = h->lookahead->next.list[idx]->i_pts;
        }
        frames[0] = h->lookahead->next.list[bframes];
        frames[0]->i_reordered_pts = h->lookahead->next.list[0]->i_pts;
        memcpy( h->lookahead->next.list, frames, (bframes+1) * sizeof(x264_frame_t*) );
    }

    for( int i = 0; i <= bframes; i++ )
    {
        h->lookahead->next.list[i]->i_coded = i_coded++;
        if( h->param.rc.i_rc_method == X264_RC_ABR || h->param.rc.b_stat_write || h->param.rc.i_vbv_buffer_size )
        {
            if( i )
            {
                x264_calculate_durations( h, h->lookahead->next.list[i], h->lookahead->next.list[i-1], &h->i_cpb_delay, &h->i_coded_fields );
                h->lookahead->next.list[0]->f_planned_cpb_duration[i-1] = (double)h->lookahead->next.list[i-1]->i_cpb_duration *
                                                                          h->sps->vui.i_num_units_in_tick / h->sps->vui.i_time_scale;
            }
            else
                x264_calculate_durations( h, h->lookahead->next.list[i], NULL, &h->i_cpb_delay, &h->i_coded_fields );

            h->lookahead->next.list[0]->f_planned_cpb_duration[i] = (double)h->lookahead->next.list[i]->i_cpb_duration *
                                                                    h->sps->vui.i_num_units_in_tick / h->sps->vui.i_time_scale;
        }
    }
}

int x264_rc_analyse_slice( x264_t *h )
{
    int p0 = 0, p1, b;
    int cost;
    x264_emms();

    if( IS_X264_TYPE_I(h->fenc->i_type) )
        p1 = b = 0;
    else if( h->fenc->i_type == X264_TYPE_P )
        p1 = b = h->fenc->i_bframes + 1;
    else //B
    {
        p1 = (h->fref1[0]->i_poc - h->fref0[0]->i_poc)/2;
        b  = (h->fenc->i_poc - h->fref0[0]->i_poc)/2;
    }
    /* We don't need to assign p0/p1 since we are not performing any real analysis here. */
    x264_frame_t **frames = &h->fenc - b;

    /* cost should have been already calculated by x264_slicetype_decide */
    cost = frames[b]->i_cost_est[b-p0][p1-b];
    assert( cost >= 0 );

    if( h->param.rc.b_mb_tree && !h->param.rc.b_stat_read )
    {
        cost = x264_slicetype_frame_cost_recalculate( h, frames, p0, p1, b );
        if( b && h->param.rc.i_vbv_buffer_size )
            x264_slicetype_frame_cost_recalculate( h, frames, b, b, b );
    }
    /* In AQ, use the weighted score instead. */
    else if( h->param.rc.i_aq_mode )
        cost = frames[b]->i_cost_est_aq[b-p0][p1-b];

    h->fenc->i_row_satd = h->fenc->i_row_satds[b-p0][p1-b];
    h->fdec->i_row_satd = h->fdec->i_row_satds[b-p0][p1-b];
    h->fdec->i_satd = cost;
    memcpy( h->fdec->i_row_satd, h->fenc->i_row_satd, h->mb.i_mb_height * sizeof(int) );
    if( !IS_X264_TYPE_I(h->fenc->i_type) )
        memcpy( h->fdec->i_row_satds[0][0], h->fenc->i_row_satds[0][0], h->mb.i_mb_height * sizeof(int) );

    if( h->param.b_intra_refresh && h->param.rc.i_vbv_buffer_size && h->fenc->i_type == X264_TYPE_P )
    {
        int ip_factor = 256 * h->param.rc.f_ip_factor; /* fix8 */
        for( int y = 0; y < h->mb.i_mb_height; y++ )
        {
            int mb_xy = y * h->mb.i_mb_stride;
            for( int x = h->fdec->i_pir_start_col; x <= h->fdec->i_pir_end_col; x++, mb_xy++ )
            {
                int intra_cost = (h->fenc->i_intra_cost[mb_xy] * ip_factor + 128) >> 8;
                int inter_cost = h->fenc->lowres_costs[b-p0][p1-b][mb_xy] & LOWRES_COST_MASK;
                int diff = intra_cost - inter_cost;
                if( h->param.rc.i_aq_mode )
                    h->fdec->i_row_satd[y] += (diff * frames[b]->i_inv_qscale_factor[mb_xy] + 128) >> 8;
                else
                    h->fdec->i_row_satd[y] += diff;
                cost += diff;
            }
        }
    }

    return cost;
}
