#include "xspman.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#define PCG_PATTERN_BYTES ((16*16)/2)

// Endianness macros

static inline uint16_t endian_swap_w(uint16_t in)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return ((in & 0xFF00) >> 8) | ((in & 0xFF) << 8);
#else
	return in;
#endif
}

static inline uint32_t endian_swap_l(uint32_t in)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	const uint16_t hiword = (in >> 16) & 0xFFFF;
	const uint16_t loword = in & 0xFFFF;
	return (endian_swap_w(loword)<<16) |
	       (endian_swap_w(hiword));
#else
	return in;
#endif
}

// =============================================================================
// Data structures and static data
// =============================================================================

// Frame data.
typedef struct
{
	short vx;  // 相対座標データ Relative X coordinate data
	short vy;  // 相対座標データ Relative Y coordinate data
	short pt;  // スプライトパターン Sprite pattern no.
	short rv;  // 反転コード Reflection code
} __attribute__((packed)) XspFrmDat;
_Static_assert(sizeof(XspFrmDat) == 8);

// Ref as loaded from the file. Data is big endian.
typedef struct
{
	short num;          // 合成スプライト数 Composite sprite FRM data count
	uint32_t ptr_offs;  // Offset within FRM data block.
	short unused;       //（未使用）(Unused)
} __attribute__((packed))  XspRefDatFile;
_Static_assert(sizeof(XspRefDatFile) == 8);

// Ref as kept in memory.
typedef struct
{
	short num;             // 合成スプライト数 Composite sprite FRM data count
	const XspFrmDat *ptr;  // Absolute pointer to start of FRM data
	short unused;          //（未使用）(Unused)
} __attribute__((packed)) XspRefDat;

enum
{
	XSB_TYPE_XSP,
	XSB_TYPE_SP,
};

// Definition for "XSB" bundle format from png2xsp.
typedef struct XSBHeader
{
	uint16_t type;       // XSP(0) or SP(1)
	uint16_t ref_count;  // ref bytes / 8sizeof(XspRefDatFile);  0 for sp
	uint16_t frm_count;  // frm count; 0 for sp.
	uint16_t pcg_count;  // valid for both xsp and sp
	uint16_t pal[16];
	uint32_t ref_offs;
	uint32_t frm_offs;
	uint32_t pcg_offs;
} __attribute__((packed))  XSBHeader;

enum
{
	XSPMAN_FILE_XSP,
	XSPMAN_FILE_SP,
	XSPMAN_FILE_BUNDLE,
};

typedef struct XSPManFileNode XSPManFileNode;
struct XSPManFileNode
{
	char fname_base[256];
	XSPManFileNode *next;
	uint16_t ref_count;
	uint16_t frm_count;
	uint16_t pcg_count;
	int16_t filetype;
};

static struct
{
	bool loaded;  // If true, registration is complete.

	XspFrmDat *frmdat;  // referenced by ref/objdat.
	XspRefDat *refdat;  // REF data.
	uint8_t *pcgdat;    // PCG pattern data.
	char *pcg_alt;      // Scratchpad for PCG data.

	// File lists.
	XSPManFileNode *file_list;

	// Buffer sizes.
	uint16_t ref_count;
	uint16_t frm_count;
	uint16_t pcg_count;

	// Load indices.
	int ref_load_idx;
	int frm_load_idx;
	int pcg_load_idx;
} s_xspman;

// =============================================================================
// Init
// =============================================================================

void xspman_init(void)
{
	memset(&s_xspman, 0, sizeof(s_xspman));
}

// =============================================================================
// File loaders
// =============================================================================

FILE *file_open_sub(const char *fname_base, const char *ext, int filetype)
{
	if (filetype == XSPMAN_FILE_BUNDLE) return fopen(fname_base, "rb");
	char fname_buffer[256];
	snprintf(fname_buffer, sizeof(fname_buffer), "%s.%s", fname_base, ext);
	return fopen(fname_buffer, "rb");
}

int load_ref_data(const char *fname_base, int count, int filetype)
{
	// Load the REF data into the REF buffer.
	FILE *f = file_open_sub(fname_base, "ref", filetype);
	if (!f) return 0;

	// Read in file ref data.
	if (filetype == XSPMAN_FILE_BUNDLE)
	{
		fseek(f, offsetof(XSBHeader, ref_offs), SEEK_SET);
		uint32_t offs;
		fread(&offs, sizeof(uint32_t), 1, f);
		offs = endian_swap_l(offs);
		fseek(f, offs, SEEK_SET);
	}

	const XspFrmDat *frm_base = &s_xspman.frmdat[s_xspman.frm_load_idx];
	// Each file ref data with relative frm offsets is converted to the
	// ref data in memory that contains a native absolute pointer.
	const uint8_t *frm_raw = (const uint8_t *)(frm_base);
	for (int i = 0; i < count; i++)
	{
		XspRefDatFile ref_file;
		fread(&ref_file, sizeof(ref_file), 1, f);
		ref_file.num = endian_swap_w(ref_file.num);
		ref_file.ptr_offs = endian_swap_l(ref_file.ptr_offs);

		XspRefDat *ref = &s_xspman.refdat[s_xspman.ref_load_idx+i];
//		printf("ref %d @ %p : %d frames, offs %X\n", i, ref, ref_file.num, ref_file.ptr_offs);
		ref->num = ref_file.num;
		ref->ptr = (const XspFrmDat *)(frm_raw+ref_file.ptr_offs);
	}

	fclose(f);

	return count;
}

int load_frm_data(const char *fname_base, int count, int filetype)
{
	// Load the FRM data into the FRM buffer.
	FILE *f = file_open_sub(fname_base, "frm", filetype);
	if (!f) return 0;

	XspFrmDat *frm_base = &s_xspman.frmdat[s_xspman.frm_load_idx];
	if (filetype == XSPMAN_FILE_BUNDLE)
	{
		fseek(f, offsetof(XSBHeader, frm_offs), SEEK_SET);
		uint32_t offs;
		fread(&offs, sizeof(uint32_t), 1, f);
		offs = endian_swap_l(offs);
//		printf("FRM offs = $%X\n", (int)offs);
		fseek(f, offs, SEEK_SET);
	}
	fread(frm_base, sizeof(XspFrmDat), count, f);
	fclose(f);

	// Rebase pattern data and fix endianness.
	const short pt_base = s_xspman.pcg_load_idx;
	for (int i = 0; i < count; i++)
	{
		XspFrmDat *frm = &frm_base[i];
//		printf("frm %d @ %p\n", i, frm);
		frm->vx = endian_swap_w(frm->vx);
		frm->vy = endian_swap_w(frm->vy);
		frm->pt = endian_swap_w(frm->pt) + pt_base;
		frm->rv = endian_swap_w(frm->rv);
//		printf("  pt %02d : +%d, +%d\n", frm->pt, frm->vx, frm->vy);
	}
	return count;
}

int load_pcg_data(const char *fname_base, int count, int filetype)
{
	// Load the PCG data into the PCG buffer.
	FILE *f = file_open_sub(fname_base,
	                        filetype == XSPMAN_FILE_XSP ? "xsp" : "sp",
	                        filetype);
	if (!f) return 0;
	
	if (filetype == XSPMAN_FILE_BUNDLE)
	{
		fseek(f, offsetof(XSBHeader, pcg_offs), SEEK_SET);
		uint32_t offs;
		fread(&offs, sizeof(uint32_t), 1, f);
		offs = endian_swap_l(offs);
		fseek(f, offs, SEEK_SET);
	}
	fread(&s_xspman.pcgdat[s_xspman.pcg_load_idx*PCG_PATTERN_BYTES], PCG_PATTERN_BYTES, count, f);
	fclose(f);

	return count;
}

// =============================================================================
// File list creation
// =============================================================================

static void free_file_list(XSPManFileNode *current)
{
	XSPManFileNode *next = NULL;
	while (current != NULL)
	{
		next = current->next;
		free(current);
		current = next;
	}
}

static void add_file(const char *fname_base, int filetype,
                     int ref_count, int frm_count, int pcg_count)
{
	// Add a new node or create the list
	XSPManFileNode *newnode = NULL;
	if (s_xspman.file_list == NULL)
	{
		s_xspman.file_list = malloc(sizeof(XSPManFileNode));
		newnode = s_xspman.file_list;
	}
	else
	{
		XSPManFileNode *current = s_xspman.file_list;
		while (current->next != NULL)
		{
			current = current->next;
		}
		current->next = malloc(sizeof(XSPManFileNode));
		newnode = current->next;
	}

	// Register information
	strncpy(newnode->fname_base, fname_base, sizeof(newnode->fname_base)-1);
	newnode->next = NULL;
	newnode->filetype = filetype;
	newnode->ref_count = ref_count;
	newnode->frm_count = frm_count;
	newnode->pcg_count = pcg_count;
	printf("REF %d FRM %d PCG %d\n",
	       newnode->ref_count,
	       newnode->frm_count,
	       newnode->pcg_count);
}

// =============================================================================
// Registration
// =============================================================================

static int get_file_bytes(const char *fname_base, const char *ext)
{
	char fname_buffer[256];
	snprintf(fname_buffer, sizeof(fname_buffer), "%s.%s", fname_base, ext);
	FILE *f_ref = fopen(fname_buffer, "rb");
	if (!f_ref)
	{
		printf("Couldn't open %s\n", fname_buffer);
		return -1;
	}

	fseek(f_ref, 0L, SEEK_END);
	const int fsize = (int)ftell(f_ref);
	fclose(f_ref);
	return fsize;
}

int16_t xspman_reg_bundle(const char *fname, uint16_t *pal)
{
	int16_t ret = -1;
	if (s_xspman.loaded) return ret;

	printf("[xspman] Spec %s: ", fname);
	FILE *f = fopen(fname, "rb");
	if (!f)
	{
		printf("Read Error\n");
		return ret;
	}
	XSBHeader header;
	fread(&header, 1, sizeof(header), f);
	fclose(f);

	// Endian fixes
	header.type = endian_swap_w(header.type);
	header.ref_count = endian_swap_w(header.ref_count);
	header.frm_count = endian_swap_w(header.frm_count);
	header.pcg_count = endian_swap_w(header.pcg_count);
	for (uint16_t i = 0; i < 16; i++)
	{
		header.pal[i] = endian_swap_w(header.pal[i]);
	}
	header.ref_offs = endian_swap_l(header.ref_offs);
	header.frm_offs = endian_swap_l(header.frm_offs);
	header.pcg_offs = endian_swap_l(header.pcg_offs);

	if (pal) memcpy(pal, header.pal, sizeof(header.pal));

	switch (header.type)
	{
		case XSB_TYPE_XSP:
			ret = s_xspman.ref_count;
			break;

		case XSB_TYPE_SP:
			ret = s_xspman.pcg_count;
			break;

		default:
			printf("Error: bundle type %d\n", header.type);
			break;
	}

	s_xspman.ref_count += header.ref_count;
	s_xspman.frm_count += header.frm_count;
	s_xspman.pcg_count += header.pcg_count;
	add_file(fname, XSPMAN_FILE_BUNDLE,
	         header.ref_count, header.frm_count, header.pcg_count);
	return ret;
}

int16_t xspman_reg_xsp(const char *fname_base)
{
	if (s_xspman.loaded) return -1;

	const int ret = s_xspman.ref_count;  // Xobj no.

	// Read as many refs as we can, and deduce the xobj count from that.
	const int ref_bytes = get_file_bytes(fname_base, "ref");
	if (ref_bytes < 0) return -1;
	if (ref_bytes % sizeof(XspRefDatFile) != 0)
	{
		printf("[xspman] Unusual REF file size for %s\n", fname_base);
		return -1;
	}
	const int ref_count = ref_bytes / sizeof(XspRefDatFile);
	s_xspman.ref_count += ref_count;

	// Read associated FRM data.
	const int frm_bytes = get_file_bytes(fname_base, "frm");
	if (frm_bytes < 0) return -1;
	if (frm_bytes % sizeof(XspFrmDat) != 0)
	{
		printf("[xspman] Unusual FRM file size for %s\n", fname_base);
		return -1;
	}
	const int frm_count = frm_bytes / sizeof(XspFrmDat);
	s_xspman.frm_count += frm_count;

	// and finally PCG data.
	const int pcg_bytes = get_file_bytes(fname_base, "xsp");
	if (pcg_bytes < 0) return -1;
	if (pcg_bytes % PCG_PATTERN_BYTES != 0)
	{
		printf("[xspman] Unusual PCG file size for %s\n", fname_base);
		return -1;
	}
	const int pcg_count = pcg_bytes / PCG_PATTERN_BYTES;
	s_xspman.pcg_count += pcg_count;

	add_file(fname_base, XSPMAN_FILE_XSP, ref_count, frm_count, pcg_count);

	return ret;
}

int16_t xspman_reg_sp(const char *fname_base)
{
	if (s_xspman.loaded) return -1;
	const int16_t ret = s_xspman.pcg_count;

	const int pcg_bytes = get_file_bytes(fname_base, "sp");
	if (pcg_bytes < 0) return -1;
	if (pcg_bytes % PCG_PATTERN_BYTES != 0)
	{
		printf("[xspman] Unusual PCG file size for %s\n", fname_base);
		return -1;
	}
	const int pcg_count = pcg_bytes / PCG_PATTERN_BYTES;
	s_xspman.pcg_count += pcg_count;

	add_file(fname_base, XSPMAN_FILE_SP, 0, 0, pcg_count);
	return ret;
}



bool xspman_load(void)
{
	if (s_xspman.loaded) return false;

	printf("[xspman] Data Summary: FRM %d REF %d PCG %d\n",
	       s_xspman.frm_count, s_xspman.ref_count, s_xspman.pcg_count);

	// Create buffers based on surveyed data from registration.
	s_xspman.frmdat = malloc(s_xspman.frm_count * sizeof(XspFrmDat));
	if (!s_xspman.frmdat) return false;
	s_xspman.refdat = malloc(s_xspman.ref_count * sizeof(XspRefDat));
	if (!s_xspman.refdat)
	{
		free(s_xspman.frmdat);
		return false;
	}
	s_xspman.pcgdat = malloc(s_xspman.pcg_count * PCG_PATTERN_BYTES);
	if (!s_xspman.pcgdat)
	{
		free(s_xspman.frmdat);
		free(s_xspman.refdat);
		return false;
	}
	s_xspman.pcg_alt = malloc(s_xspman.pcg_count+1);
	if (!s_xspman.pcgdat)
	{
		free(s_xspman.frmdat);
		free(s_xspman.refdat);
		free(s_xspman.pcgdat);
		return false;
	}

	// Load the data into the buffers.
	XSPManFileNode *current = s_xspman.file_list;
	while (current != NULL)
	{
		printf("[xspman] Load %s... ", current->fname_base);
		if (current->filetype != XSPMAN_FILE_SP)
		{
			// Loading an XOBJ file requires multiple steps. The complication,
			// at least when compared to a plain PCG tile, stems from the fact
			// that the data structures must be rebased.
			//
			// REF_DAT: Composite sprite reference data structure.
			//   * Dictates number of composite frame structs (FRM_DAT count)
			//   * Has a pointer to start of FRM_DAT (rebased against FRM_DAT)
			//
			// FRM_DAT: Composite sprite frame data structure (describes a tile)
			//   * Has coordinate data and draw metadata (reflection, color)
			//   * Has a PCG pattern no. (rebased against PCG number base)

			// First, load the REF data, and rebase it against frm_base, which is
			// frmdat[frm_load_idx].
			s_xspman.ref_load_idx += load_ref_data(current->fname_base,
			                                       current->ref_count,
			                                       current->filetype);
			// Next, load the FRM data into frm_base, add to frm_load_idx the
			// size of the loaded data, and add pcg_load_idx/128 to the pattern
			// field of the FRM data.
			s_xspman.frm_load_idx += load_frm_data(current->fname_base,
			                                       current->frm_count,
			                                       current->filetype);
		}
		// Finally, load the PCG data, and increment pcg_load_idx by the number
		// of bytes loaded.
		s_xspman.pcg_load_idx += load_pcg_data(current->fname_base,
		                                       current->pcg_count,
		                                       current->filetype);
		printf(" OK!\n");
		// Visit the next.
		current = current->next;
	}

	free_file_list(s_xspman.file_list);

	s_xspman.loaded = true;
	return true;
}

const void *xspman_get_pcgdat_ptr(void)
{
	return s_xspman.pcgdat;
}

char *xspman_get_pcg_alt_ptr(void)
{
	return s_xspman.pcg_alt;
}

int xspman_get_pcg_alt_bytes(void)
{
	return s_xspman.pcg_count+1;
}

const void *xspman_get_objdat_ptr(void)
{
	return s_xspman.refdat;
}

void xspman_shutdown(void)
{
	if (!s_xspman.loaded) return;

	free(s_xspman.frmdat);
	free(s_xspman.refdat);
	free(s_xspman.pcgdat);
	free(s_xspman.pcg_alt);
	xspman_init();
}

