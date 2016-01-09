/*
 * Portions of this file are copyright Rebirth contributors and licensed as
 * described in COPYING.txt.
 * Portions of this file are copyright Parallax Software and licensed
 * according to the Parallax license below.
 * See COPYING.txt for license details.

THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.
COPYRIGHT 1993-1999 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/

/*
 *
 * Routines for reading and writing IFF files
 *
 */

#define COMPRESS		1	//do the RLE or not? (for debugging mostly)
#define WRITE_TINY	0	//should we write a TINY chunk?

#define MIN_COMPRESS_WIDTH	65	//don't compress if less than this wide

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_mem.h"
#include "iff.h"
#include "dxxerror.h"
#include "makesig.h"
#include "physfsx.h"
#include "gr.h"

#include "dxxsconf.h"
#include "compiler-range_for.h"
#include "compiler-make_unique.h"
#include "partial_range.h"

//Internal constants and structures for this library

//Type values for bitmaps
#define TYPE_PBM		0
#define TYPE_ILBM		1

//Compression types
#define cmpNone	0
#define cmpByteRun1	1

//Masking types
#define mskNone	0
#define mskHasMask	1
#define mskHasTransparentColor 2

//structure of the header in the file
struct iff_bitmap_header : prohibit_void_ptr<iff_bitmap_header>
{
	short w,h;						//width and height of this bitmap
	short x,y;						//generally unused
	short type;						//see types above
	short transparentcolor;		//which color is transparent (if any)
	short pagewidth,pageheight; //width & height of source screen
	sbyte nplanes;              //number of planes (8 for 256 color image)
	sbyte masking,compression;  //see constants above
	sbyte xaspect,yaspect;      //aspect ratio (usually 5/6)
	palette_array_t palette;		//the palette for this bitmap
	short row_size;				//offset to next row
	RAIIdmem<uint8_t[]> raw_data;				//ptr to array of data
};

ubyte iff_transparent_color;
ubyte iff_has_transparency;	// 0=no transparency, 1=iff_transparent_color is valid

#define form_sig MAKE_SIG('F','O','R','M')
#define ilbm_sig MAKE_SIG('I','L','B','M')
#define body_sig MAKE_SIG('B','O','D','Y')
#define pbm_sig  MAKE_SIG('P','B','M',' ')
#define bmhd_sig MAKE_SIG('B','M','H','D')
#define anhd_sig MAKE_SIG('A','N','H','D')
#define cmap_sig MAKE_SIG('C','M','A','P')
#define tiny_sig MAKE_SIG('T','I','N','Y')
#define anim_sig MAKE_SIG('A','N','I','M')
#define dlta_sig MAKE_SIG('D','L','T','A')

static int32_t get_sig(PHYSFS_File *f)
{
	int s;

	PHYSFS_readSBE32(f, &s);
	return s;
}

#define put_sig(sig, f) PHYSFS_writeSBE32(f, sig)

static int parse_bmhd(PHYSFS_File *ifile,long len,iff_bitmap_header *bmheader)
{
	len++;  /* so no "parm not used" warning */

//  debug("parsing bmhd len=%ld\n",len);

	PHYSFS_readSBE16(ifile, &bmheader->w);
	PHYSFS_readSBE16(ifile, &bmheader->h);
	PHYSFS_readSBE16(ifile, &bmheader->x);
	PHYSFS_readSBE16(ifile, &bmheader->y);
	
	bmheader->nplanes = PHYSFSX_readByte(ifile);
	bmheader->masking = PHYSFSX_readByte(ifile);
	bmheader->compression = PHYSFSX_readByte(ifile);
	PHYSFSX_readByte(ifile);        /* skip pad */
	
	PHYSFS_readSBE16(ifile, &bmheader->transparentcolor);
	bmheader->xaspect = PHYSFSX_readByte(ifile);
	bmheader->yaspect = PHYSFSX_readByte(ifile);
	
	PHYSFS_readSBE16(ifile, &bmheader->pagewidth);
	PHYSFS_readSBE16(ifile, &bmheader->pageheight);

	iff_transparent_color = bmheader->transparentcolor;

	iff_has_transparency = 0;

	if (bmheader->masking == mskHasTransparentColor)
		iff_has_transparency = 1;

	else if (bmheader->masking != mskNone && bmheader->masking != mskHasMask)
		return IFF_UNKNOWN_MASK;

//  debug("w,h=%d,%d x,y=%d,%d\n",w,h,x,y);
//  debug("nplanes=%d, masking=%d ,compression=%d, transcolor=%d\n",nplanes,masking,compression,transparentcolor);

	return IFF_NO_ERROR;
}


//  the buffer pointed to by raw_data is stuffed with a pointer to decompressed pixel data
static int parse_body(PHYSFS_File *ifile,long len,iff_bitmap_header *bmheader)
{
	auto p = bmheader->raw_data.get();
	int width,depth;
	signed char n;
	int nn,wid_cnt,end_cnt,plane;
	unsigned char *data_end;
	int end_pos;
        #ifndef NDEBUG
	int row_count=0;
        #endif

        width=0;
        depth=0;

	end_pos = PHYSFS_tell(ifile) + len;
	if (len&1)
		end_pos++;

	if (bmheader->type == TYPE_PBM) {
		width=bmheader->w;
		depth=1;
	} else if (bmheader->type == TYPE_ILBM) {
		width = (bmheader->w+7)/8;
		depth=bmheader->nplanes;
	}

	end_cnt = (width&1)?-1:0;

	data_end = p + width*bmheader->h*depth;

	if (bmheader->compression == cmpNone) {        /* no compression */
		int y;

		for (y=bmheader->h;y;y--) {
			PHYSFS_read(ifile, p, width, depth);
			p += bmheader->w;

			if (bmheader->masking == mskHasMask)
				PHYSFSX_fseek(ifile, width, SEEK_CUR);				//skip mask!

			if (bmheader->w & 1) PHYSFSX_fgetc(ifile);
		}

		//cnt = len - bmheader->h * ((bmheader->w+1)&~1);

	}
	else if (bmheader->compression == cmpByteRun1)
		for (wid_cnt=width,plane=0; PHYSFS_tell(ifile) < end_pos && p<data_end;) {
			unsigned char c;

			if (wid_cnt == end_cnt) {
				wid_cnt = width;
				plane++;
				if ((bmheader->masking == mskHasMask && plane==depth+1) ||
				    (bmheader->masking != mskHasMask && plane==depth))
					plane=0;
			}

			Assert(wid_cnt > end_cnt);

			n=PHYSFSX_fgetc(ifile);

			if (n >= 0) {                       // copy next n+1 bytes from source, they are not compressed
				nn = (int) n+1;
				wid_cnt -= nn;
				if (wid_cnt==-1) {--nn; Assert(width&1);}
				if (plane==depth)	//masking row
					PHYSFSX_fseek(ifile, nn, SEEK_CUR);
				else
				{
					PHYSFS_read(ifile, p, nn, 1);
					p += nn;
				}
				if (wid_cnt==-1) PHYSFSX_fseek(ifile, 1, SEEK_CUR);
			}
			else if (n>=-127) {             // next -n + 1 bytes are following byte
				c=PHYSFSX_fgetc(ifile);
				nn = (int) -n+1;
				wid_cnt -= nn;
				if (wid_cnt==-1) {--nn; Assert(width&1);}
				if (plane!=depth)	//not masking row
					{memset(p,c,nn); p+=nn;}
			}

			#ifndef NDEBUG
			if ((p - bmheader->raw_data.get()) % width == 0)
					row_count++;

			Assert((p - bmheader->raw_data.get()) - (width*row_count) < width);
			#endif

		}

#if defined(DXX_BUILD_DESCENT_I)
	if (bmheader->masking==mskHasMask && p==data_end && PHYSFS_tell(ifile)==end_pos-2)		//I don't know why...
		PHYSFSX_fseek(ifile, 1, SEEK_CUR);		//...but if I do this it works

	if (p==data_end && PHYSFS_tell(ifile)==end_pos-1)		//must be a pad byte
		//ignore = PHYSFSX_fgetc(ifile);		//get pad byte
		PHYSFSX_fseek(ifile, 1, SEEK_CUR);
	else
		if (PHYSFS_tell(ifile)!=end_pos || p!=data_end) {
//			debug("IFF Error: p=%x, data_end=%x, cnt=%d\n",p,data_end,cnt);
			return IFF_CORRUPT;
		}
#elif defined(DXX_BUILD_DESCENT_II)
	if (p!=data_end)				//if we don't have the whole bitmap...
		return IFF_CORRUPT;		//...the give an error

	//Pretend we read the whole chuck, because if we didn't, it's because
	//we didn't read the last mask like or the last rle record for padding
	//or whatever and it's not important, because we check to make sure
	//we got the while bitmap, and that's what really counts.
#endif

	return IFF_NO_ERROR;
}

//modify passed bitmap
static int parse_delta(PHYSFS_File *ifile,long len,iff_bitmap_header *bmheader)
{
	auto p = bmheader->raw_data.get();
	long chunk_end = PHYSFS_tell(ifile) + len;

	PHYSFSX_fseek(ifile, 4, SEEK_CUR);		//longword, seems to be equal to 4.  Don't know what it is

	for (int y=0;y<bmheader->h;y++) {
		ubyte n_items;
		int cnt = bmheader->w;
		ubyte code;

		n_items = PHYSFSX_readByte(ifile);

		while (n_items--) {

			code = PHYSFSX_readByte(ifile);

			if (code==0) {				//repeat
				ubyte rep,val;

				rep = PHYSFSX_readByte(ifile);
				val = PHYSFSX_readByte(ifile);

				cnt -= rep;
				if (cnt==-1)
					rep--;
				while (rep--)
					*p++ = val;
			}
			else if (code > 0x80) {	//skip
				cnt -= (code-0x80);
				p += (code-0x80);
				if (cnt==-1)
					p--;
			}
			else {						//literal
				cnt -= code;
				if (cnt==-1)
					code--;

				while (code--)
					*p++ = PHYSFSX_readByte(ifile);

				if (cnt==-1)
					PHYSFSX_readByte(ifile);
			}

		}

		if (cnt == -1) {
			if (!bmheader->w&1)
				return IFF_CORRUPT;
		}
		else if (cnt)
			return IFF_CORRUPT;
	}

	if (PHYSFS_tell(ifile) == chunk_end-1)		//pad
		PHYSFSX_fseek(ifile, 1, SEEK_CUR);

	if (PHYSFS_tell(ifile) != chunk_end)
		return IFF_CORRUPT;
	else
		return IFF_NO_ERROR;
}

//  the buffer pointed to by raw_data is stuffed with a pointer to bitplane pixel data
static void skip_chunk(PHYSFS_File *ifile,long len)
{
	int ilen;
	ilen = (len+1) & ~1;

	PHYSFSX_fseek(ifile,ilen,SEEK_CUR);
}

//read an ILBM or PBM file
// Pass pointer to opened file, and to empty bitmap_header structure, and form length
static int iff_parse_ilbm_pbm(PHYSFS_File *ifile,long form_type,iff_bitmap_header *bmheader,int form_len,grs_bitmap *prev_bm)
{
	int sig,len;
	long start_pos,end_pos;

	start_pos = PHYSFS_tell(ifile);
	end_pos = start_pos-4+form_len;

			if (form_type == pbm_sig)
				bmheader->type = TYPE_PBM;
			else
				bmheader->type = TYPE_ILBM;

			while ((PHYSFS_tell(ifile) < end_pos) && (sig=get_sig(ifile)) != EOF) {

				PHYSFS_readSBE32(ifile, &len);

				switch (sig) {

					case bmhd_sig: {
						int ret;
						int save_w=bmheader->w,save_h=bmheader->h;

						ret = parse_bmhd(ifile,len,bmheader);

						if (ret != IFF_NO_ERROR)
							return ret;

						if (bmheader->raw_data) {

							if (save_w != bmheader->w  ||  save_h != bmheader->h)
								return IFF_BM_MISMATCH;

						}
						else {
							MALLOC(bmheader->raw_data, uint8_t[], bmheader->w * bmheader->h);
							if (!bmheader->raw_data)
								return IFF_NO_MEM;
						}

						break;

					}

					case anhd_sig:

						if (!prev_bm) return IFF_CORRUPT;

						bmheader->w = prev_bm->bm_w;
						bmheader->h = prev_bm->bm_h;
						bmheader->type = prev_bm->get_type();

						MALLOC(bmheader->raw_data, uint8_t[], bmheader->w * bmheader->h);

						memcpy(bmheader->raw_data.get(), prev_bm->bm_data, bmheader->w * bmheader->h);
						skip_chunk(ifile,len);

						break;

					case cmap_sig:
					{
						unsigned ncolors=(len/3);

						range_for (auto &p, partial_range(bmheader->palette, ncolors))
						{
							p.r = static_cast<unsigned char>(PHYSFSX_fgetc(ifile)) >> 2;
							p.g = static_cast<unsigned char>(PHYSFSX_fgetc(ifile)) >> 2;
							p.b = static_cast<unsigned char>(PHYSFSX_fgetc(ifile)) >> 2;
						}
						if (len & 1) PHYSFSX_fgetc(ifile);

						break;
					}

					case body_sig:
					{
						int r;
						if ((r=parse_body(ifile,len,bmheader))!=IFF_NO_ERROR)
							return r;
						break;
					}
					case dlta_sig:
					{
						int r;
						if ((r=parse_delta(ifile,len,bmheader))!=IFF_NO_ERROR)
							return r;
						break;
					}
					default:
						skip_chunk(ifile,len);
						break;
				}
			}

	if (PHYSFS_tell(ifile) != start_pos-4+form_len)
		return IFF_CORRUPT;

	return IFF_NO_ERROR;    /* ok! */
}

//convert an ILBM file to a PBM file
static int convert_ilbm_to_pbm(iff_bitmap_header *bmheader)
{
	int x,p;
	sbyte *rowptr;
	int bytes_per_row,byteofs;
	ubyte checkmask,newbyte,setbit;

	RAIIdmem<uint8_t[]> new_data;
	MALLOC(new_data, uint8_t[], bmheader->w * bmheader->h);
	if (new_data == NULL) return IFF_NO_MEM;

	auto destptr = new_data.get();

	bytes_per_row = 2*((bmheader->w+15)/16);

	for (int y=0;y<bmheader->h;y++) {

		rowptr = (signed char *) &bmheader->raw_data[y * bytes_per_row * bmheader->nplanes];

		for (x=0,checkmask=0x80;x<bmheader->w;x++) {

			byteofs = x >> 3;

			for (p=newbyte=0,setbit=1;p<bmheader->nplanes;p++) {

				if (rowptr[bytes_per_row * p + byteofs] & checkmask)
					newbyte |= setbit;

				setbit <<= 1;
			}

			*destptr++ = newbyte;

			if ((checkmask >>= 1) == 0) checkmask=0x80;
		}

	}

	bmheader->raw_data = std::move(new_data);

	bmheader->type = TYPE_PBM;

	return IFF_NO_ERROR;
}

#define INDEX_TO_15BPP(i) ((short)((((palptr[(i)].r/2)&31)<<10)+(((palptr[(i)].g/2)&31)<<5)+((palptr[(i)].b/2 )&31)))

static int convert_rgb15(grs_bitmap &bm,iff_bitmap_header &bmheader)
{
	palette_array_t::iterator palptr = begin(bmheader.palette);

#if defined(DXX_BUILD_DESCENT_I)
	for (int y=0; y < bm.bm_h; y++) {
		for (int x=0; x < bmheader.w; x++)
			gr_bm_pixel(bm, x, y, INDEX_TO_15BPP(bm.get_bitmap_data()[y * bmheader.w + x]));
	}
#elif defined(DXX_BUILD_DESCENT_II)
	ushort *new_data;
	MALLOC(new_data, ushort, bm.bm_w * bm.bm_h * 2);
	if (new_data == NULL)
		return IFF_NO_MEM;

	unsigned newptr = 0;
	for (int y=0; y < bm.bm_h; y++) {

		for (int x=0; x < bmheader.w; x++)
			new_data[newptr++] = INDEX_TO_15BPP(bm.get_bitmap_data()[y * bmheader.w + x]);

	}

	d_free(bm.bm_mdata);				//get rid of old-style data
	bm.bm_mdata = (ubyte *) new_data;			//..and point to new data

	bm.bm_rowsize *= 2;				//two bytes per row
#endif
	return IFF_NO_ERROR;

}

//copy an iff header structure to a grs_bitmap structure
static void copy_iff_to_grs(grs_bitmap &bm,iff_bitmap_header &bmheader)
{
	gr_init_bitmap(bm, bmheader.type, 0, 0, bmheader.w, bmheader.h, bmheader.w, bmheader.raw_data.release());
}

//if bm->bm_data is set, use it (making sure w & h are correct), else
//allocate the memory
static int iff_parse_bitmap(PHYSFS_File *ifile, grs_bitmap &bm, int bitmap_type, palette_array_t *palette, grs_bitmap *prev_bm)
{
	int ret;			//return code
	iff_bitmap_header bmheader;
	int sig,form_len;
	long form_type;

	bmheader.raw_data.reset(bm.get_bitmap_data());

	if (bmheader.raw_data) {
		bmheader.w = bm.bm_w;
		bmheader.h = bm.bm_h;
	}//added 05/17/99 Matt Mueller - don't just leave them unitialized
	else{
		bmheader.w=bmheader.h=0;
	}

	sig=get_sig(ifile);

	if (sig != form_sig) {
		return IFF_NOT_IFF;
	}

	PHYSFS_readSBE32(ifile, &form_len);

	form_type = get_sig(ifile);

	if (form_type == anim_sig)
		ret = IFF_FORM_ANIM;
	else if ((form_type == pbm_sig) || (form_type == ilbm_sig))
		ret = iff_parse_ilbm_pbm(ifile,form_type,&bmheader,form_len,prev_bm);
	else
		ret = IFF_UNKNOWN_FORM;

	if (ret != IFF_NO_ERROR) {		//got an error parsing
		return ret;
	}

	//If IFF file is ILBM, convert to PPB
	if (bmheader.type == TYPE_ILBM) {

		ret = convert_ilbm_to_pbm(&bmheader);

		if (ret != IFF_NO_ERROR)
			return ret;
	}

	//Copy data from iff_bitmap_header structure into grs_bitmap structure

	copy_iff_to_grs(bm,bmheader);

	if (palette)
		*palette = bmheader.palette;

	//Now do post-process if required

	if (bitmap_type == BM_RGB15) {
		ret = convert_rgb15(bm, bmheader);
		if (ret != IFF_NO_ERROR)
			return ret;
	}

	return ret;

}

//returns error codes - see IFF.H.  see GR.H for bitmap_type
int iff_read_bitmap(const char *ifilename,grs_bitmap &bm,int bitmap_type,palette_array_t *palette)
{
	int ret;			//return code
	auto ifile = PHYSFSX_openReadBuffered(ifilename);
	if (!ifile)
		return IFF_NO_FILE;

	bm.bm_data = nullptr;
	ret = iff_parse_bitmap(ifile, bm, bitmap_type, palette, nullptr);
	return ret;
}

#define BMHD_SIZE 20

#if 0
static int write_bmhd(PHYSFS_File *ofile,iff_bitmap_header *bitmap_header)
{
	put_sig(bmhd_sig,ofile);
	PHYSFS_writeSBE32(ofile, BMHD_SIZE);

	PHYSFS_writeSBE16(ofile, bitmap_header->w);
	PHYSFS_writeSBE16(ofile, bitmap_header->h);
	PHYSFS_writeSBE16(ofile, bitmap_header->x);
	PHYSFS_writeSBE16(ofile, bitmap_header->y);

	PHYSFSX_writeU8(ofile, bitmap_header->nplanes);
	PHYSFSX_writeU8(ofile, bitmap_header->masking);
	PHYSFSX_writeU8(ofile, bitmap_header->compression);
	PHYSFSX_writeU8(ofile, 0);	/* pad */

	PHYSFS_writeSBE16(ofile, bitmap_header->transparentcolor);
	PHYSFSX_writeU8(ofile, bitmap_header->xaspect);
	PHYSFSX_writeU8(ofile, bitmap_header->yaspect);

	PHYSFS_writeSBE16(ofile, bitmap_header->pagewidth);
	PHYSFS_writeSBE16(ofile, bitmap_header->pageheight);

	return IFF_NO_ERROR;

}

static int write_pal(PHYSFS_File *ofile,iff_bitmap_header *bitmap_header)
{
	int n_colors = 1<<bitmap_header->nplanes;

	put_sig(cmap_sig,ofile);
//	PHYSFS_writeSBE32(sizeof(pal_entry) * n_colors,ofile);
	PHYSFS_writeSBE32(ofile, 3 * n_colors);

	range_for (auto &c, bitmap_header->palette)
	{
		unsigned char r,g,b;
		r = c.r * 4 + (c.r?3:0);
		g = c.g * 4 + (c.g?3:0);
		b = c.b * 4 + (c.b?3:0);
		PHYSFSX_writeU8(ofile, r);
		PHYSFSX_writeU8(ofile, g);
		PHYSFSX_writeU8(ofile, b);
	}

	return IFF_NO_ERROR;
}

static int rle_span(ubyte *dest,ubyte *src,int len)
{
	int lit_cnt,rep_cnt;
	ubyte last,*cnt_ptr,*dptr;

        cnt_ptr=0;

	dptr = dest;

	last=src[0]; lit_cnt=1;

	for (int n=1;n<len;n++) {

		if (src[n] == last) {

			rep_cnt = 2;

			n++;
			while (n<len && rep_cnt<128 && src[n]==last) {n++; rep_cnt++;}

			if (rep_cnt > 2 || lit_cnt < 2) {

				if (lit_cnt > 1) {*cnt_ptr = lit_cnt-2; --dptr;}		//save old lit count
				*dptr++ = -(rep_cnt-1);
				*dptr++ = last;
				last = src[n];
				lit_cnt = (n<len)?1:0;

				continue;		//go to next char
			} else n--;

		}

		{

			if (lit_cnt==1) {
				cnt_ptr = dptr++;		//save place for count
				*dptr++=last;			//store first char
			}

			*dptr++ = last = src[n];

			if (lit_cnt == 127) {
				*cnt_ptr = lit_cnt;
				//cnt_ptr = dptr++;
				lit_cnt = 1;
				last = src[++n];
			}
			else lit_cnt++;
		}


	}

	if (lit_cnt==1) {
		*dptr++ = 0;
		*dptr++=last;			//store first char
	}
	else if (lit_cnt > 1)
		*cnt_ptr = lit_cnt-1;

	return dptr-dest;
}

#define EVEN(a) ((a+1)&0xfffffffel)

//returns length of chunk
static int write_body(PHYSFS_File *ofile,iff_bitmap_header *bitmap_header,int compression_on)
{
	int w=bitmap_header->w,h=bitmap_header->h;
	int y,odd=w&1;
	long len = EVEN(w) * h,newlen,total_len=0;
	ubyte *p=bitmap_header->raw_data;
	long save_pos;

	put_sig(body_sig,ofile);
	save_pos = PHYSFS_tell(ofile);
	PHYSFS_writeSBE32(ofile, len);

	RAIIdmem<uint8_t[]> new_span;
	MALLOC( new_span, uint8_t[], bitmap_header->w + (bitmap_header->w/128+2)*2);
	if (!new_span) return IFF_NO_MEM;

	for (y=bitmap_header->h;y--;) {

		if (compression_on) {
			total_len += newlen = rle_span(new_span,p,bitmap_header->w+odd);
			PHYSFS_write(ofile,new_span,newlen,1);
		}
		else
			PHYSFS_write(ofile,p,bitmap_header->w+odd,1);

		p+=bitmap_header->row_size;	//bitmap_header->w;
	}

	if (compression_on) {		//write actual data length
		Assert(PHYSFSX_fseek(ofile,save_pos,SEEK_SET)==0);
		(void)save_pos;
		PHYSFS_writeSBE32(ofile, total_len);
		Assert(PHYSFSX_fseek(ofile,total_len,SEEK_CUR)==0);
		if (total_len&1) PHYSFSX_writeU8(ofile, 0);		//pad to even
	}
	return ((compression_on) ? (EVEN(total_len)+8) : (len+8));

}

#if WRITE_TINY
//write a small representation of a bitmap. returns size
int write_tiny(PHYSFS_File *ofile,iff_bitmap_header *bitmap_header,int compression_on)
{
	int skip;
	int new_w,new_h;
	int len,total_len=0,newlen;
	int x,xofs,odd;
	ubyte *p = bitmap_header->raw_data;
	ubyte tspan[80],new_span[80*2];
	long save_pos;

	skip = max((bitmap_header->w+79)/80,(bitmap_header->h+63)/64);

	new_w = bitmap_header->w / skip;
	new_h = bitmap_header->h / skip;

	odd = new_w & 1;

	len = new_w * new_h + 4;

	put_sig(tiny_sig,ofile);
	save_pos = PHYSFS_tell(ofile);
	PHYSFS_writeSBE32(ofile, EVEN(len));

	PHYSFS_writeSBE16(ofile, new_w);
	PHYSFS_writeSBE16(ofile, new_h);

	for (int y=0;y<new_h;y++) {
		for (x=xofs=0;x<new_w;x++,xofs+=skip)
			tspan[x] = p[xofs];

		if (compression_on) {
			total_len += newlen = rle_span(new_span,tspan,new_w+odd);
			PHYSFS_write(ofile,new_span,newlen,1);
		}
		else
			PHYSFS_write(ofile,p,new_w+odd,1);

		p += skip * bitmap_header->row_size;		//bitmap_header->w;

	}

	if (compression_on) {
		Assert(PHYSFSX_fseek(ofile,save_pos,SEEK_SET)==0);
		(void)save_pos;
		PHYSFS_writeSBE32(ofile, 4+total_len);
		Assert(PHYSFSX_fseek(ofile,4+total_len,SEEK_CUR)==0);
		if (total_len&1) PHYSFSX_writeU8(ofile, 0);		//pad to even
	}

	return ((compression_on) ? (EVEN(total_len)+8+4) : (len+8));
}
#endif

static int write_pbm(PHYSFS_File *ofile,iff_bitmap_header *bitmap_header,int compression_on)			/* writes a pbm iff file */
{
	int ret;
	long raw_size = EVEN(bitmap_header->w) * bitmap_header->h;
	long body_size,tiny_size,pbm_size = 4 + BMHD_SIZE + 8 + EVEN(raw_size) + sizeof(rgb_t)*(1<<bitmap_header->nplanes)+8;
	long save_pos;

	put_sig(form_sig,ofile);
	save_pos = PHYSFS_tell(ofile);
	PHYSFS_writeSBE32(ofile, pbm_size+8);
	put_sig(pbm_sig,ofile);

	ret = write_bmhd(ofile,bitmap_header);
	if (ret != IFF_NO_ERROR) return ret;

	ret = write_pal(ofile,bitmap_header);
	if (ret != IFF_NO_ERROR) return ret;

#if WRITE_TINY
	tiny_size = write_tiny(ofile,bitmap_header,compression_on);
#else
	tiny_size = 0;
#endif

	body_size = write_body(ofile,bitmap_header,compression_on);

	pbm_size = 4 + BMHD_SIZE + body_size + tiny_size + sizeof(rgb_t)*(1<<bitmap_header->nplanes)+8;

	Assert(PHYSFSX_fseek(ofile,save_pos,SEEK_SET)==0);
	(void)save_pos;
	PHYSFS_writeSBE32(ofile, pbm_size+8);
	Assert(PHYSFSX_fseek(ofile,pbm_size+8,SEEK_CUR)==0);

	return ret;

}

//writes an IFF file from a grs_bitmap structure. writes palette if not null
//returns error codes - see IFF.H.
int iff_write_bitmap(const char *ofilename,grs_bitmap *bm,palette_array_t *palette)
{
	iff_bitmap_header bmheader;
	int ret;
	int compression_on;

	if (bm->bm_type == BM_RGB15) return IFF_BAD_BM_TYPE;

#if COMPRESS
	compression_on = (bm->bm_w>=MIN_COMPRESS_WIDTH);
#else
	compression_on = 0;
#endif

	//fill in values in bmheader

	bmheader.x = bmheader.y = 0;
	bmheader.w = bm->bm_w;
	bmheader.h = bm->bm_h;
	bmheader.type = TYPE_PBM;
	bmheader.transparentcolor = iff_transparent_color;
	bmheader.pagewidth = bm->bm_w;	//I don't think it matters what I write
	bmheader.pageheight = bm->bm_h;
	bmheader.nplanes = 8;
	bmheader.masking = mskNone;
	if (iff_has_transparency)	{
		 bmheader.masking |= mskHasTransparentColor;
	}
	bmheader.compression = (compression_on?cmpByteRun1:cmpNone);

	bmheader.xaspect = bmheader.yaspect = 1;	//I don't think it matters what I write
	bmheader.raw_data = bm->get_bitmap_data();
	bmheader.row_size = bm->bm_rowsize;

	if (palette)
		bmheader.palette = *palette;

	//open file and write

	RAIIPHYSFS_File ofile{PHYSFS_openWrite(ofilename)};
	if (!ofile)
		return IFF_NO_FILE;

	ret = write_pbm(ofile,&bmheader,compression_on);
	return ret;
}
#endif

//read in many brushes.  fills in array of pointers, and n_bitmaps.
//returns iff error codes
int iff_read_animbrush(const char *ifilename,array<std::unique_ptr<grs_main_bitmap>, MAX_BITMAPS_PER_BRUSH> &bm_list,unsigned *n_bitmaps,palette_array_t &palette)
{
	int ret = IFF_NO_ERROR;			//return code
	int sig,form_len;
	long form_type;

	*n_bitmaps=0;

	auto ifile = PHYSFSX_openReadBuffered(ifilename);
	if (!ifile)
		return IFF_NO_FILE;

	sig=get_sig(ifile);
	PHYSFS_readSBE32(ifile, &form_len);

	if (sig != form_sig) {
		ret = IFF_NOT_IFF;
		goto done;
	}

	form_type = get_sig(ifile);

	if ((form_type == pbm_sig) || (form_type == ilbm_sig))
		ret = IFF_FORM_BITMAP;
	else if (form_type == anim_sig) {
		int anim_end = PHYSFS_tell(ifile) + form_len - 4;

		while (PHYSFS_tell(ifile) < anim_end && *n_bitmaps < bm_list.size()) {

			grs_bitmap *prev_bm;

			prev_bm = *n_bitmaps>0?bm_list[*n_bitmaps-1].get() : nullptr;

			auto &n = bm_list[*n_bitmaps];
			n = make_unique<grs_main_bitmap>();
			gr_init_bitmap_data(*n.get());

			ret = iff_parse_bitmap(ifile, *n.get(), form_type, *n_bitmaps > 0 ? nullptr : &palette, prev_bm);

			if (ret != IFF_NO_ERROR)
				goto done;

			(*n_bitmaps)++;
		}

		if (PHYSFS_tell(ifile) < anim_end)	//ran out of room
			ret = IFF_TOO_MANY_BMS;

	}
	else
		ret = IFF_UNKNOWN_FORM;

done:
	return ret;
}

//text for error messges
static const char error_messages[] = {
	"No error.\0"
	"Not enough mem for loading or processing bitmap.\0"
	"IFF file has unknown FORM type.\0"
	"Not an IFF file.\0"
	"Cannot open file.\0"
	"Tried to save invalid type, like BM_RGB15.\0"
	"Bad data in file.\0"
	"ANIM file cannot be loaded with normal bitmap loader.\0"
	"Normal bitmap file cannot be loaded with anim loader.\0"
	"Array not big enough on anim brush read.\0"
	"Unknown mask type in bitmap header.\0"
	"Error reading file.\0"
};


//function to return pointer to error message
const char *iff_errormsg(int error_number)
{
	const char *p = error_messages;

	while (error_number--) {

		if (!p) return NULL;

		p += strlen(p)+1;

	}

	return p;

}


