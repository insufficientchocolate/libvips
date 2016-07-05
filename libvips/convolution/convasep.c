/* convasep ... separable approximate convolution
 *
 * This operation does an approximate, seperable convolution. 
 *
 * Author: John Cupitt & Nicolas Robidoux
 * Written on: 31/5/11
 * Modified on: 
 * 31/5/11
 *      - from im_conv()
 * 5/7/16
 * 	- redone as a class
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*

  See:

	http://incubator.quasimondo.com/processing/stackblur.pde

  This thing is a little like stackblur, but generalised to any separable 
  mask.

 */

/*

  TODO

  	- are we handling mask offset correctly?

 */

/* Show sample pixels as they are transformed.
 */
#define DEBUG_PIXELS

/*
 */
#define DEBUG
#define VIPS_DEBUG

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/vector.h>
#include <vips/debug.h>

/* Maximum number of lines we can break the mask into.
 */
#define MAX_LINES (1000)

/* Euclid's algorithm. Use this to common up mults.
 */
static int
gcd( int a, int b )
{
	if( b == 0 )
		return( abs( a ) );
	else
		return( gcd( b, a % b ) );
}

typedef struct {
	VipsConvolution parent_instance;

	VipsImage *in;
	VipsImage *out;
	VipsImage *mask;

	int layers;

	int area;
	int rounding;

	/* The mask broken into a set of lines.
	 *
	 * Start is the left-most pixel in the line, end is one beyond the
	 * right-most pixel.
	 */
	int n_lines;
	int start[MAX_LINES];
	int end[MAX_LINES];
	int factor[MAX_LINES];
} VipsConvasep;

typedef VipsConvolutionClass VipsConvasepClass;

G_DEFINE_TYPE( VipsConvasep, vips_convasep, VIPS_TYPE_CONVOLUTION );

static void
vips_convasep_lines_start( Lines *lines, int x, int factor )
{
	lines->start[lines->n_lines] = x;
	lines->factor[lines->n_lines] = factor;
}

static int
vips_convasep_lines_end( Lines *lines, int x )
{
	lines->end[lines->n_lines] = x;

	if( lines->n_lines >= MAX_LINES - 1 ) {
		vips_error( "VipsConvasep", "%s", _( "mask too complex" ) );
		return( -1 );
	}
	lines->n_lines += 1;

	return( 0 );
}

/* Break a mask into lines.
 */
static int
vips_convasep_decompose( VipsConvasep *convasep )
{
	const int width = mask->xsize * mask->ysize;

	Lines *lines;
	double max;
	double min;
	double depth;
	double sum;
	int layers_above;
	int layers_below;
	int z, n, x;

	/* Check parameters.
	 */
	if( im_piocheck( in, out ) ||
		im_check_uncoded( "im_convasep", in ) ||
		vips_check_dmask_1d( "im_convasep", mask ) ) 
		return( NULL );

	lines = VIPS_NEW( out, Lines );
	lines->in = in;
	lines->out = out;
	if( !(lines->mask = (DOUBLEMASK *) im_local( out, 
		(im_construct_fn) im_dup_dmask,
		(im_callback_fn) im_free_dmask, mask, mask->filename, NULL )) )
		return( NULL );
	lines->n_layers = n_layers;
	lines->n_lines = 0;

	VIPS_DEBUG_MSG( "lines_new: breaking into %d layers ...\n", n_layers );

	/* Find mask range. We must always include the zero axis in the mask.
	 */
	max = 0;
	min = 0;
	for( x = 0; x < width; x++ ) {
		if( mask->coeff[x] > max )
			max = mask->coeff[x];
		if( mask->coeff[x] < min )
			min = mask->coeff[x];
	}

	/* The zero axis must fall on a layer boundary. Estimate the
	 * depth, find n-lines-above-zero, get exact depth, then calculate a
	 * fixed n-lines which includes any negative parts.
	 */
	depth = (max - min) / n_layers;
	layers_above = ceil( max / depth );
	depth = max / layers_above;
	layers_below = floor( min / depth );
	n_layers = layers_above - layers_below;

	VIPS_DEBUG_MSG( "depth = %g, n_layers = %d\n", depth, n_layers );

	/* For each layer, generate a set of lines which are inside the
	 * perimeter. Work down from the top.
	 */
	for( z = 0; z < n_layers; z++ ) {
		double y = max - (1 + z) * depth;

		/* y plus half depth ... ie. the layer midpoint.
		 */
		double y_ph = y + depth / 2;

		/* Odd, but we must avoid rounding errors that make us miss 0
		 * in the line above.
		 */
		int y_positive = z < layers_above;

		int inside;

		/* Start outside the perimeter.
		 */
		inside = 0;

		for( x = 0; x < width; x++ ) {
			/* The vertical line from mask[z] to 0 is inside. Is
			 * our current square (x, y) part of that line?
			 */
			if( (y_positive && mask->coeff[x] >= y_ph) ||
				(!y_positive && mask->coeff[x] <= y_ph) ) {
				if( !inside ) {
					vips_convasep_lines_start( lines, x, 
						y_positive ? 1 : -1 );
					inside = 1;
				}
			}
			else {
				if( inside ) {
					if( vips_convasep_lines_end( lines, x ) )
						return( NULL );
					inside = 0;
				}
			}
		}

		if( inside && 
			vips_convasep_lines_end( lines, width ) )
			return( NULL );
	}

	/* Can we common up any lines? Search for lines with identical
	 * start/end.
	 */
	for( z = 0; z < lines->n_lines; z++ ) {
		for( n = z + 1; n < lines->n_lines; n++ ) {
			if( lines->start[z] == lines->start[n] &&
				lines->end[z] == lines->end[n] ) {
				lines->factor[z] += lines->factor[n];

				/* n can be deleted. Do this in a separate
				 * pass below.
				 */
				lines->factor[n] = 0;
			}
		}
	}

	/* Now we can remove all factor 0 lines.
	 */
	for( z = 0; z < lines->n_lines; z++ ) {
		if( lines->factor[z] == 0 ) {
			for( x = z; x < lines->n_lines; x++ ) {
				lines->start[x] = lines->start[x + 1];
				lines->end[x] = lines->end[x + 1];
				lines->factor[x] = lines->factor[x + 1];
			}
			lines->n_lines -= 1;
		}
	}

	/* Find the area of the lines.
	 */
	lines->area = 0;
	for( z = 0; z < lines->n_lines; z++ ) 
		lines->area += lines->factor[z] * 
			(lines->end[z] - lines->start[z]);

	/* Strength reduction: if all lines are divisible by n, we can move
	 * that n out into the ->area factor. The aim is to produce as many
	 * factor 1 lines as we can and to reduce the chance of overflow.
	 */
	x = lines->factor[0];
	for( z = 1; z < lines->n_lines; z++ ) 
		x = gcd( x, lines->factor[z] );
	for( z = 0; z < lines->n_lines; z++ ) 
		lines->factor[z] /= x;
	lines->area *= x;

	/* Find the area of the original mask.
	 */
	sum = 0;
	for( z = 0; z < width; z++ ) 
		sum += mask->coeff[z];

	lines->area = rint( sum * lines->area / mask->scale );
	lines->rounding = (lines->area + 1) / 2 + mask->offset * lines->area;

	/* ASCII-art layer drawing.
	printf( "lines:\n" );
	for( z = 0; z < lines->n_lines; z++ ) {
		printf( "%3d - %2d x ", z, lines->factor[z] );
		for( x = 0; x < 55; x++ ) {
			int rx = x * (width + 1) / 55;

			if( rx >= lines->start[z] && rx < lines->end[z] )
				printf( "#" );
			else
				printf( " " );
		}
		printf( " %3d .. %3d\n", lines->start[z], lines->end[z] );
	}
	printf( "area = %d\n", lines->area );
	printf( "rounding = %d\n", lines->rounding );
	 */

	return( lines );
}

/* Our sequence value.
 */
typedef struct {
	Lines *lines;
	REGION *ir;		/* Input region */

	int *start;		/* Offsets for start and stop */
	int *end;

	/* The sums for each line. int for integer types, double for floating
	 * point types.
	 */
	void *sum;		

	int last_stride;	/* Avoid recalcing offsets, if we can */
} AConvSep;

/* Free a sequence value.
 */
static int
vips_convasep_stop( void *vseq, void *a, void *b )
{
	AConvSep *seq = (AConvSep *) vseq;

	IM_FREEF( im_region_free, seq->ir );

	return( 0 );
}

/* Convolution start function.
 */
static void *
vips_convasep_start( IMAGE *out, void *a, void *b )
{
	IMAGE *in = (IMAGE *) a;
	Lines *lines = (Lines *) b;

	AConvSep *seq;

	if( !(seq = IM_NEW( out, AConvSep )) )
		return( NULL );

	/* Init!
	 */
	seq->lines = lines;
	seq->ir = im_region_create( in );
	seq->start = IM_ARRAY( out, lines->n_lines, int );
	seq->end = IM_ARRAY( out, lines->n_lines, int );
	if( vips_band_format_isint( out->BandFmt ) )
		seq->sum = IM_ARRAY( out, lines->n_lines, int );
	else
		seq->sum = IM_ARRAY( out, lines->n_lines, double );
	seq->last_stride = -1;

	if( !seq->ir || 
		!seq->start || 
		!seq->end || 
		!seq->sum ) {
		vips_convasep_stop( seq, in, lines );
		return( NULL );
	}

	return( seq );
}

#define CLIP_UCHAR( V ) \
G_STMT_START { \
	if( (V) < 0 ) \
		(V) = 0; \
	else if( (V) > UCHAR_MAX ) \
		(V) = UCHAR_MAX; \
} G_STMT_END

#define CLIP_CHAR( V ) \
G_STMT_START { \
	if( (V) < SCHAR_MIN ) \
		(V) = SCHAR_MIN; \
	else if( (V) > SCHAR_MAX ) \
		(V) = SCHAR_MAX; \
} G_STMT_END

#define CLIP_USHORT( V ) \
G_STMT_START { \
	if( (V) < 0 ) \
		(V) = 0; \
	else if( (V) > USHRT_MAX ) \
		(V) = USHRT_MAX; \
} G_STMT_END

#define CLIP_SHORT( V ) \
G_STMT_START { \
	if( (V) < SHRT_MIN ) \
		(V) = SHRT_MIN; \
	else if( (V) > SHRT_MAX ) \
		(V) = SHRT_MAX; \
} G_STMT_END

#define CLIP_NONE( V ) {}

/* The h and v loops are very similar, but also annoyingly different. Keep
 * them separate for easy debugging.
 */

#define HCONV_INT( TYPE, CLIP ) { \
	for( i = 0; i < bands; i++ ) { \
		int *seq_sum = (int *) seq->sum; \
		\
		TYPE *q; \
		TYPE *p; \
		int sum; \
		\
		p = i + (TYPE *) IM_REGION_ADDR( ir, r->left, r->top + y ); \
		q = i + (TYPE *) IM_REGION_ADDR( or, r->left, r->top + y ); \
		\
		sum = 0; \
		for( z = 0; z < n_lines; z++ ) { \
			seq_sum[z] = 0; \
			for( x = lines->start[z]; x < lines->end[z]; x++ ) \
				seq_sum[z] += p[x * istride]; \
			sum += lines->factor[z] * seq_sum[z]; \
		} \
		sum = (sum + lines->rounding) / lines->area; \
		CLIP( sum ); \
		*q = sum; \
		q += ostride; \
		\
		for( x = 1; x < r->width; x++ ) {  \
			sum = 0; \
			for( z = 0; z < n_lines; z++ ) { \
				seq_sum[z] += p[seq->end[z]]; \
				seq_sum[z] -= p[seq->start[z]]; \
				sum += lines->factor[z] * seq_sum[z]; \
			} \
			p += istride; \
			sum = (sum + lines->rounding) / lines->area; \
			CLIP( sum ); \
			*q = sum; \
			q += ostride; \
		} \
	} \
}

#define HCONV_FLOAT( TYPE ) { \
	for( i = 0; i < bands; i++ ) { \
		double *seq_sum = (double *) seq->sum; \
		\
		TYPE *q; \
		TYPE *p; \
		double sum; \
		\
		p = i + (TYPE *) IM_REGION_ADDR( ir, r->left, r->top + y ); \
		q = i + (TYPE *) IM_REGION_ADDR( or, r->left, r->top + y ); \
		\
		sum = 0; \
		for( z = 0; z < lines->n_lines; z++ ) { \
			seq_sum[z] = 0; \
			for( x = lines->start[z]; x < lines->end[z]; x++ ) \
				seq_sum[z] += p[x * istride]; \
			sum += lines->factor[z] * seq_sum[z]; \
		} \
		sum = sum / lines->area + mask->offset; \
		*q = sum; \
		q += ostride; \
		\
		for( x = 1; x < r->width; x++ ) {  \
			sum = 0; \
			for( z = 0; z < lines->n_lines; z++ ) { \
				seq_sum[z] += p[seq->end[z]]; \
				seq_sum[z] -= p[seq->start[z]]; \
				sum += lines->factor[z] * seq_sum[z]; \
			} \
			p += istride; \
			sum = sum / lines->area + mask->offset; \
			*q = sum; \
			q += ostride; \
		} \
	} \
}

/* Do horizontal masks ... we scan the mask along scanlines.
 */
static int
vips_convasep_generate_horizontal( REGION *or, void *vseq, void *a, void *b )
{
	AConvSep *seq = (AConvSep *) vseq;
	IMAGE *in = (IMAGE *) a;
	Lines *lines = (Lines *) b;

	REGION *ir = seq->ir;
	const int n_lines = lines->n_lines;
	DOUBLEMASK *mask = lines->mask;
	Rect *r = &or->valid;

	/* Double the bands (notionally) for complex.
	 */
	int bands = vips_band_format_iscomplex( in->BandFmt ) ? 
		2 * in->Bands : in->Bands;

	Rect s;
	int x, y, z, i;
	int istride;
	int ostride;

	/* Prepare the section of the input image we need. A little larger
	 * than the section of the output image we are producing.
	 */
	s = *r;
	s.width += mask->xsize - 1;
	s.height += mask->ysize - 1;
	if( im_prepare( ir, &s ) )
		return( -1 );

	/* Stride can be different for the vertical case, keep this here for
	 * ease of direction change.
	 */
	istride = IM_IMAGE_SIZEOF_PEL( in ) / 
		IM_IMAGE_SIZEOF_ELEMENT( in );
	ostride = IM_IMAGE_SIZEOF_PEL( lines->out ) / 
		IM_IMAGE_SIZEOF_ELEMENT( lines->out );

        /* Init offset array. 
	 */
	if( seq->last_stride != istride ) {
		seq->last_stride = istride;

		for( z = 0; z < n_lines; z++ ) {
			seq->start[z] = lines->start[z] * istride;
			seq->end[z] = lines->end[z] * istride;
		}
	}

	for( y = 0; y < r->height; y++ ) { 
		switch( in->BandFmt ) {
		case IM_BANDFMT_UCHAR: 	
			HCONV_INT( unsigned char, CLIP_UCHAR );
			break;

		case IM_BANDFMT_CHAR: 	
			HCONV_INT( signed char, CLIP_UCHAR );
			break;

		case IM_BANDFMT_USHORT: 	
			HCONV_INT( unsigned short, CLIP_USHORT );
			break;

		case IM_BANDFMT_SHORT: 	
			HCONV_INT( signed short, CLIP_SHORT );
			break;

		case IM_BANDFMT_UINT: 	
			HCONV_INT( unsigned int, CLIP_NONE );
			break;

		case IM_BANDFMT_INT: 	
			HCONV_INT( signed int, CLIP_NONE );
			break;

		case IM_BANDFMT_FLOAT: 	
			HCONV_FLOAT( float );
			break;

		case IM_BANDFMT_DOUBLE: 	
			HCONV_FLOAT( double );
			break;

		case IM_BANDFMT_COMPLEX: 	
			HCONV_FLOAT( float );
			break;

		case IM_BANDFMT_DPCOMPLEX: 	
			HCONV_FLOAT( double );
			break;

		default:
			g_assert_not_reached();
		}
	}

	return( 0 );
}

#define VCONV_INT( TYPE, CLIP ) { \
	for( x = 0; x < sz; x++ ) { \
		int *seq_sum = (int *) seq->sum; \
		\
		TYPE *q; \
		TYPE *p; \
		int sum; \
		\
		p = x + (TYPE *) IM_REGION_ADDR( ir, r->left, r->top ); \
		q = x + (TYPE *) IM_REGION_ADDR( or, r->left, r->top ); \
		\
		sum = 0; \
		for( z = 0; z < lines->n_lines; z++ ) { \
			seq_sum[z] = 0; \
			for( y = lines->start[z]; y < lines->end[z]; y++ ) \
				seq_sum[z] += p[y * istride]; \
			sum += lines->factor[z] * seq_sum[z]; \
		} \
		sum = (sum + lines->rounding) / lines->area; \
		CLIP( sum ); \
		*q = sum; \
		q += ostride; \
		\
		for( y = 1; y < r->height; y++ ) { \
			sum = 0; \
			for( z = 0; z < lines->n_lines; z++ ) { \
				seq_sum[z] += p[seq->end[z]]; \
				seq_sum[z] -= p[seq->start[z]]; \
				sum += lines->factor[z] * seq_sum[z]; \
			} \
			p += istride; \
			sum = (sum + lines->rounding) / lines->area; \
			CLIP( sum ); \
			*q = sum; \
			q += ostride; \
		}   \
	} \
}

#define VCONV_FLOAT( TYPE ) { \
	for( x = 0; x < sz; x++ ) { \
		double *seq_sum = (double *) seq->sum; \
		\
		TYPE *q; \
		TYPE *p; \
		double sum; \
		\
		p = x + (TYPE *) IM_REGION_ADDR( ir, r->left, r->top ); \
		q = x + (TYPE *) IM_REGION_ADDR( or, r->left, r->top ); \
		\
		sum = 0; \
		for( z = 0; z < lines->n_lines; z++ ) { \
			seq_sum[z] = 0; \
			for( y = lines->start[z]; y < lines->end[z]; y++ ) \
				seq_sum[z] += p[y * istride]; \
			sum += lines->factor[z] * seq_sum[z]; \
		} \
		sum = sum / lines->area + mask->offset; \
		*q = sum; \
		q += ostride; \
		\
		for( y = 1; y < r->height; y++ ) { \
			sum = 0; \
			for( z = 0; z < lines->n_lines; z++ ) { \
				seq_sum[z] += p[seq->end[z]]; \
				seq_sum[z] -= p[seq->start[z]]; \
				sum += lines->factor[z] * seq_sum[z]; \
			} \
			p += istride; \
			sum = sum / lines->area + mask->offset; \
			*q = sum; \
			q += ostride; \
		}   \
	} \
}

/* Do vertical masks ... we scan the mask down columns of pixels. Copy-paste
 * from above with small changes.
 */
static int
vips_convasep_generate_vertical( REGION *or, void *vseq, void *a, void *b )
{
	AConvSep *seq = (AConvSep *) vseq;
	IMAGE *in = (IMAGE *) a;
	Lines *lines = (Lines *) b;

	REGION *ir = seq->ir;
	const int n_lines = lines->n_lines;
	DOUBLEMASK *mask = lines->mask;
	Rect *r = &or->valid;

	/* Double the width (notionally) for complex.
	 */
	int sz = vips_band_format_iscomplex( in->BandFmt ) ? 
		2 * IM_REGION_N_ELEMENTS( or ) : IM_REGION_N_ELEMENTS( or );

	Rect s;
	int x, y, z;
	int istride;
	int ostride;

	/* Prepare the section of the input image we need. A little larger
	 * than the section of the output image we are producing.
	 */
	s = *r;
	s.width += mask->xsize - 1;
	s.height += mask->ysize - 1;
	if( im_prepare( ir, &s ) )
		return( -1 );

	/* Stride can be different for the vertical case, keep this here for
	 * ease of direction change.
	 */
	istride = IM_REGION_LSKIP( ir ) / 
		IM_IMAGE_SIZEOF_ELEMENT( lines->in );
	ostride = IM_REGION_LSKIP( or ) / 
		IM_IMAGE_SIZEOF_ELEMENT( lines->out );

        /* Init offset array. 
	 */
	if( seq->last_stride != istride ) {
		seq->last_stride = istride;

		for( z = 0; z < n_lines; z++ ) {
			seq->start[z] = lines->start[z] * istride;
			seq->end[z] = lines->end[z] * istride;
		}
	}

	switch( in->BandFmt ) {
	case IM_BANDFMT_UCHAR: 	
		VCONV_INT( unsigned char, CLIP_UCHAR );
		break;

	case IM_BANDFMT_CHAR: 	
		VCONV_INT( signed char, CLIP_UCHAR );
		break;

	case IM_BANDFMT_USHORT: 	
		VCONV_INT( unsigned short, CLIP_USHORT );
		break;

	case IM_BANDFMT_SHORT: 	
		VCONV_INT( signed short, CLIP_SHORT );
		break;

	case IM_BANDFMT_UINT: 	
		VCONV_INT( unsigned int, CLIP_NONE );
		break;

	case IM_BANDFMT_INT: 	
		VCONV_INT( signed int, CLIP_NONE );
		break;

	case IM_BANDFMT_FLOAT: 	
		VCONV_FLOAT( float );
		break;

	case IM_BANDFMT_DOUBLE: 	
		VCONV_FLOAT( double );
		break;

	case IM_BANDFMT_COMPLEX: 	
		VCONV_FLOAT( float );
		break;

	case IM_BANDFMT_DPCOMPLEX: 	
		VCONV_FLOAT( double );
		break;

	default:
		g_assert_not_reached();
	}

	return( 0 );
}

static int
vips_convasep_pass( VipsConvasep *convasep, 
	VipsImage *in, VipsImage **out, VipsDirection direction )
{
	VipsGenerateFn gen;

	*out = vips_image_new(); 
	if( vips_image_pipelinev( *out, 
		VIPS_DEMAND_STYLE_SMALLTILE, in, NULL ) )
		return( -1 );

	if( direction == VIPS_DIRECtION_HORIZONTAL ) { 
		(*out)->Xsize -= M->Xsize - 1;
		(*out)->Ysize -= M->Ysize - 1;
		gen = vips_convasep_generate_horizontal;
	}
	else {
		(*out)->Xsize -= M->Ysize - 1;
		(*out)->Ysize -= M->Xsize - 1;
		gen = vips_convasep_generate_vertical;
	}

	if( (*out)->Xsize <= 0 || 
		(*out)->Ysize <= 0 ) {
		vips_error( class->nickname, 
			"%s", _( "image too small for mask" ) );
		return( -1 );
	}

	if( vips_image_generate( *out, 
		vips_convasep_start, gen, vips_convasep_stop, in, convasep ) )
		return( -1 );

	if( direction == VIPS_DIRECTION_HORIZONTAL ) { 
		(*out)->Xoffset = -M->Xsize / 2;
		(*out)->Yoffset = -M->Ysize / 2;
	}
	else {
		(*out)->Xoffset = -M->Ysize / 2;
		(*out)->Yoffset = -M->Xsize / 2;
	}

	return( 0 );
}

static int 
vips_convasep_build( VipsObject *object )
{
	VipsConvolution *convolution = (VipsConvolution *) object;
	VipsConvasep *convasep = (VipsConvasep *) object;
	VipsImage **t = (VipsImage **) vips_object_local_array( object, 4 );

	VipsImage *in;
	VipsImage *M;
	double *coeff;
	int ne;
        int i;

	if( VIPS_OBJECT_CLASS( vips_convasep_parent_class )->build( object ) )
		return( -1 );

	if( vips_convasep_decompose( convasep ) )
		return( -1 ); 

	M = convolution->M;
	coeff = (double *) VIPS_IMAGE_ADDR( M, 0, 0 );
	ne = M->Xsize * M->Ysize;

	g_object_set( convf, "out", vips_image_new(), NULL ); 
	if( 
		vips_embed( convolution->in, &t[0], 
			M->Xsize / 2, M->Ysize / 2, 
			in->Xsize + M->Xsize - 1, in->Ysize + M->Ysize - 1,
			"extend", VIPS_EXTEND_COPY,
			NULL ) ||
		vips_convasep_pass( convasep, 
			t[0], &t[1], VIPS_DIRECTION_HORIZONTAL ) ||
		vips_convasep_pass( convasep, 
			t[1], &t[2], VIPS_DIRECTION_VERTICAL ) ||
		vips_image_write( t[2], convolution->out ) )
		return( -1 );

	out->Xoffset = 0;
	out->Yoffset = 0;

	return( 0 );
}

static void
vips_convasep_class_init( VipsConvasepClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );
	VipsObjectClass *object_class = (VipsObjectClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "convasep";
	object_class->description = _( "approximate separable convolution" );
	object_class->build = vips_convasep_build;

	VIPS_ARG_INT( class, "layers", 104, 
		_( "Layers" ), 
		_( "Use this many layers in approximation" ),
		VIPS_ARGUMENT_OPTIONAL_INPUT, 
		G_STRUCT_OFFSET( VipsConvasep, layers ), 
		1, 1000, 5 ); 

}

static void
vips_convasep_init( VipsConvf *convasep )
{
        convasep->layers = 5;
}

/**
 * vips_convasep:
 * @in: input image
 * @out: output image
 * @mask: convolve with this mask
 * @...: %NULL-terminated list of optional named arguments
 *
 * Optional arguments:
 *
 * * @layers: %gint, number of layers for approximation
 *
 * Approximate separable convolution. This is a low-level operation, see 
 * vips_conv() for something more convenient. 
 *
 * The mask must be 1xn or nx1 elements. 
 * The output image 
 * always has the same #VipsBandFormat as the input image. 
 *
 * The image is convolved twice: once with @mask and then again with @mask 
 * rotated by 90 degrees. 
 *
 * Larger values for @layers give more accurate
 * results, but are slower. As @layers approaches the mask radius, the
 * accuracy will become close to exact convolution and the speed will drop to 
 * match. For many large masks, such as Gaussian, @layers need be only 10% of
 * this value and accuracy will still be good.
 *
 * See also: vips_conv().
 *
 * Returns: 0 on success, -1 on error
 */
int 
vips_convasep( VipsImage *in, VipsImage **out, VipsImage *mask, ... )
{
	va_list ap;
	int result;

	va_start( ap, mask );
	result = vips_call_split( "convasep", ap, in, out, mask );
	va_end( ap );

	return( result );
}

