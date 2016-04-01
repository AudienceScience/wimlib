/*
 * decompress_common.c
 *
 * Code for decompression shared among multiple compression formats.
 *
 * Author:  Eric Biggers
 * Year:    2012 - 2014
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib/decompress_common.h"

#include <string.h>

#define USE_WORD_FILL

#ifdef __GNUC__
#  ifdef __SSE2__
#    undef USE_WORD_FILL
#    define USE_SSE2_FILL
#    include <emmintrin.h>
#  endif
#endif

/* Construct a direct mapping entry in the lookup table.  */
#define MAKE_DIRECT_ENTRY(symbol, length) ((symbol) | ((length) << 11))

/*
 * make_huffman_decode_table() -
 *
 * Build a decoding table for a canonical prefix code, or "Huffman code".
 *
 * This takes as input the length of the codeword for each symbol in the
 * alphabet and produces as output a table that can be used for fast
 * decoding of prefix-encoded symbols using read_huffsym().
 *
 * Strictly speaking, a canonical prefix code might not be a Huffman
 * code.  But this algorithm will work either way; and in fact, since
 * Huffman codes are defined in terms of symbol frequencies, there is no
 * way for the decompressor to know whether the code is a true Huffman
 * code or not until all symbols have been decoded.
 *
 * Because the prefix code is assumed to be "canonical", it can be
 * reconstructed directly from the codeword lengths.  A prefix code is
 * canonical if and only if a longer codeword never lexicographically
 * precedes a shorter codeword, and the lexicographic ordering of
 * codewords of the same length is the same as the lexicographic ordering
 * of the corresponding symbols.  Consequently, we can sort the symbols
 * primarily by codeword length and secondarily by symbol value, then
 * reconstruct the prefix code by generating codewords lexicographically
 * in that order.
 *
 * This function does not, however, generate the prefix code explicitly.
 * Instead, it directly builds a table for decoding symbols using the
 * code.  The basic idea is this: given the next 'max_codeword_len' bits
 * in the input, we can look up the decoded symbol by indexing a table
 * containing 2**max_codeword_len entries.  A codeword with length
 * 'max_codeword_len' will have exactly one entry in this table, whereas
 * a codeword shorter than 'max_codeword_len' will have multiple entries
 * in this table.  Precisely, a codeword of length n will be represented
 * by 2**(max_codeword_len - n) entries in this table.  The 0-based index
 * of each such entry will contain the corresponding codeword as a prefix
 * when zero-padded on the left to 'max_codeword_len' binary digits.
 *
 * That's the basic idea, but we implement two optimizations regarding
 * the format of the decode table itself:
 *
 * - For many compression formats, the maximum codeword length is too
 *   long for it to be efficient to build the full decoding table
 *   whenever a new prefix code is used.  Instead, we can build the table
 *   using only 2**table_bits entries, where 'table_bits' is some number
 *   less than or equal to 'max_codeword_len'.  Then, only codewords of
 *   length 'table_bits' and shorter can be directly looked up.  For
 *   longer codewords, the direct lookup instead produces the root of a
 *   binary tree.  Using this tree, the decoder can do traditional
 *   bit-by-bit decoding of the remainder of the codeword.  Child nodes
 *   are allocated in extra entries at the end of the table; leaf nodes
 *   contain symbols.  Note that the long-codeword case is, in general,
 *   not performance critical, since in Huffman codes the most frequently
 *   used symbols are assigned the shortest codeword lengths.
 *
 * - When we decode a symbol using a direct lookup of the table, we still
 *   need to know its length so that the bitstream can be advanced by the
 *   appropriate number of bits.  The simple solution is to simply retain
 *   the 'lens' array and use the decoded symbol as an index into it.
 *   However, this requires two separate array accesses in the fast path.
 *   The optimization is to store the length directly in the decode
 *   table.  We use the bottom 11 bits for the symbol and the top 5 bits
 *   for the length.  In addition, to combine this optimization with the
 *   previous one, we introduce a special case where the top 2 bits of
 *   the length are both set if the entry is actually the root of a
 *   binary tree.
 *
 * @decode_table:
 *	The array in which to create the decoding table.  This must be
 *	16-byte aligned and must have a length of at least
 *	((2**table_bits) + 2 * num_syms) entries.  This is permitted to
 *	alias @lens, since all information from @lens is consumed before
*	anything is written to @decode_table.
 *
 * @num_syms:
 *	The number of symbols in the alphabet; also, the length of the
 *	'lens' array.  Must be less than or equal to
 *	DECODE_TABLE_MAX_SYMBOLS.
 *
 * @table_bits:
 *	The order of the decode table size, as explained above.  Must be
 *	less than or equal to DECODE_TABLE_MAX_TABLE_BITS.
 *
 * @lens:
 *	An array of length @num_syms, indexable by symbol, that gives the
 *	length of the codeword, in bits, for that symbol.  The length can
 *	be 0, which means that the symbol does not have a codeword
 *	assigned.  This is permitted to alias @decode_table, since all
 *	information from @lens is consumed before anything is written to
 *	@decode_table.
 *
 * @max_codeword_len:
 *	The longest codeword length allowed in the compression format.
 *	All entries in 'lens' must be less than or equal to this value.
 *	This must be less than or equal to DECODE_TABLE_MAX_CODEWORD_LEN.
 *
 * Returns 0 on success, or -1 if the lengths do not form a valid prefix
 * code.
 */
int
make_huffman_decode_table(u16 decode_table[const],
			  const unsigned num_syms,
			  const unsigned table_bits,
			  const u8 lens[const],
			  const unsigned max_codeword_len)
{
	const unsigned table_num_entries = 1 << table_bits;
	unsigned len_counts[max_codeword_len + 1];
	u16 sorted_syms[num_syms];
	int left;
	void *decode_table_ptr;
	unsigned sym_idx;
	unsigned codeword_len;
	unsigned stores_per_loop;
	unsigned decode_table_pos;

#ifdef USE_WORD_FILL
	const unsigned entries_per_word = WORDSIZE / sizeof(decode_table[0]);
#endif

#ifdef USE_SSE2_FILL
	const unsigned entries_per_xmm = sizeof(__m128i) / sizeof(decode_table[0]);
#endif

	/* Count how many symbols have each possible codeword length.
	 * Note that a length of 0 indicates the corresponding symbol is not
	 * used in the code and therefore does not have a codeword.  */
	for (unsigned len = 0; len <= max_codeword_len; len++)
		len_counts[len] = 0;
	for (unsigned sym = 0; sym < num_syms; sym++)
		len_counts[lens[sym]]++;

	/* We can assume all lengths are <= max_codeword_len, but we
	 * cannot assume they form a valid prefix code.  A codeword of
	 * length n should require a proportion of the codespace equaling
	 * (1/2)^n.  The code is valid if and only if the codespace is
	 * exactly filled by the lengths, by this measure.  */
	left = 1;
	for (unsigned len = 1; len <= max_codeword_len; len++) {
		left <<= 1;
		left -= len_counts[len];
		if (unlikely(left < 0)) {
			/* The lengths overflow the codespace; that is, the code
			 * is over-subscribed.  */
			return -1;
		}
	}

	if (unlikely(left != 0)) {
		/* The lengths do not fill the codespace; that is, they form an
		 * incomplete set.  */
		if (left == (1 << max_codeword_len)) {
			/* The code is completely empty.  This is arguably
			 * invalid, but in fact it is valid in LZX and XPRESS,
			 * so we must allow it.  By definition, no symbols can
			 * be decoded with an empty code.  Consequently, we
			 * technically don't even need to fill in the decode
			 * table.  However, to avoid accessing uninitialized
			 * memory if the algorithm nevertheless attempts to
			 * decode symbols using such a code, we zero out the
			 * decode table.  */
			memset(decode_table, 0,
			       table_num_entries * sizeof(decode_table[0]));
			return 0;
		}
		return -1;
	}

	/* Sort the symbols primarily by length and secondarily by symbol order.
	 */
	{
		unsigned offsets[max_codeword_len + 1];

		/* Initialize 'offsets' so that offsets[len] for 1 <= len <=
		 * max_codeword_len is the number of codewords shorter than
		 * 'len' bits.  */
		offsets[1] = 0;
		for (unsigned len = 1; len < max_codeword_len; len++)
			offsets[len + 1] = offsets[len] + len_counts[len];

		/* Use the 'offsets' array to sort the symbols.
		 * Note that we do not include symbols that are not used in the
		 * code.  Consequently, fewer than 'num_syms' entries in
		 * 'sorted_syms' may be filled.  */
		for (unsigned sym = 0; sym < num_syms; sym++)
			if (lens[sym] != 0)
				sorted_syms[offsets[lens[sym]]++] = sym;
	}

	/* Fill entries for codewords with length <= table_bits
	 * --- that is, those short enough for a direct mapping.
	 *
	 * The table will start with entries for the shortest codeword(s), which
	 * have the most entries.  From there, the number of entries per
	 * codeword will decrease.  As an optimization, we may begin filling
	 * entries with SSE2 vector accesses (8 entries/store), then change to
	 * 'machine_word_t' accesses (2 or 4 entries/store), then change to
	 * 16-bit accesses (1 entry/store).  */
	decode_table_ptr = decode_table;
	sym_idx = 0;
	codeword_len = 1;
#ifdef USE_SSE2_FILL
	/* Fill the entries one 128-bit vector at a time.
	 * This is 8 entries per store.  */
	stores_per_loop = (1 << (table_bits - codeword_len)) / entries_per_xmm;
	for (; stores_per_loop != 0; codeword_len++, stores_per_loop >>= 1) {
		unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
		for (; sym_idx < end_sym_idx; sym_idx++) {
			/* Note: unlike in the machine_word_t version below, the
			 * __m128i type already has __attribute__((may_alias)),
			 * so using it to access the decode table, which is an
			 * array of unsigned shorts, will not violate strict
			 * aliasing.  */
			u16 entry;
			__m128i v;
			__m128i *p;
			unsigned n;

			entry = MAKE_DIRECT_ENTRY(sorted_syms[sym_idx], codeword_len);

			v = _mm_set1_epi16(entry);
			p = (__m128i*)decode_table_ptr;
			n = stores_per_loop;
			do {
				*p++ = v;
			} while (--n);
			decode_table_ptr = p;
		}
	}
#endif /* USE_SSE2_FILL */

#ifdef USE_WORD_FILL
	/* Fill the entries one machine word at a time.
	 * On 32-bit systems this is 2 entries per store, while on 64-bit
	 * systems this is 4 entries per store.  */
	stores_per_loop = (1 << (table_bits - codeword_len)) / entries_per_word;
	for (; stores_per_loop != 0; codeword_len++, stores_per_loop >>= 1) {
		unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
		for (; sym_idx < end_sym_idx; sym_idx++) {

			/* Accessing the array of u16 as u32 or u64 would
			 * violate strict aliasing and would require compiling
			 * the code with -fno-strict-aliasing to guarantee
			 * correctness.  To work around this problem, use the
			 * gcc 'may_alias' extension.  */
			typedef machine_word_t _may_alias_attribute aliased_word_t;

			machine_word_t v;
			aliased_word_t *p;
			unsigned n;

			STATIC_ASSERT(WORDSIZE == 4 || WORDSIZE == 8);

			v = MAKE_DIRECT_ENTRY(sorted_syms[sym_idx], codeword_len);
			v |= v << 16;
			v |= v << (WORDSIZE == 8 ? 32 : 0);

			p = (aliased_word_t *)decode_table_ptr;
			n = stores_per_loop;

			do {
				*p++ = v;
			} while (--n);
			decode_table_ptr = p;
		}
	}
#endif /* USE_WORD_FILL */

	/* Fill the entries one 16-bit integer at a time.  */
	stores_per_loop = (1 << (table_bits - codeword_len));
	for (; stores_per_loop != 0; codeword_len++, stores_per_loop >>= 1) {
		unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
		for (; sym_idx < end_sym_idx; sym_idx++) {
			u16 entry;
			u16 *p;
			unsigned n;

			entry = MAKE_DIRECT_ENTRY(sorted_syms[sym_idx], codeword_len);

			p = (u16*)decode_table_ptr;
			n = stores_per_loop;

			do {
				*p++ = entry;
			} while (--n);

			decode_table_ptr = p;
		}
	}

	/* If we've filled in the entire table, we are done.  Otherwise,
	 * there are codewords longer than table_bits for which we must
	 * generate binary trees.  */

	decode_table_pos = (u16*)decode_table_ptr - decode_table;
	if (decode_table_pos != table_num_entries) {
		unsigned j;
		unsigned next_free_tree_slot;
		unsigned cur_codeword;

		/* First, zero out the remaining entries.  This is
		 * necessary so that these entries appear as
		 * "unallocated" in the next part.  Each of these entries
		 * will eventually be filled with the representation of
		 * the root node of a binary tree.  */
		j = decode_table_pos;
		do {
			decode_table[j] = 0;
		} while (++j != table_num_entries);

		/* We allocate child nodes starting at the end of the
		 * direct lookup table.  Note that there should be
		 * 2*num_syms extra entries for this purpose, although
		 * fewer than this may actually be needed.  */
		next_free_tree_slot = table_num_entries;

		/* Iterate through each codeword with length greater than
		 * 'table_bits', primarily in order of codeword length
		 * and secondarily in order of symbol.  */
		for (cur_codeword = decode_table_pos << 1;
		     codeword_len <= max_codeword_len;
		     codeword_len++, cur_codeword <<= 1)
		{
			unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
			for (; sym_idx < end_sym_idx; sym_idx++, cur_codeword++)
			{
				/* 'sym' is the symbol represented by the
				 * codeword.  */
				unsigned sym = sorted_syms[sym_idx];

				unsigned extra_bits = codeword_len - table_bits;

				unsigned node_idx = cur_codeword >> extra_bits;

				/* Go through each bit of the current codeword
				 * beyond the prefix of length @table_bits and
				 * walk the appropriate binary tree, allocating
				 * any slots that have not yet been allocated.
				 *
				 * Note that the 'pointer' entry to the binary
				 * tree, which is stored in the direct lookup
				 * portion of the table, is represented
				 * identically to other internal (non-leaf)
				 * nodes of the binary tree; it can be thought
				 * of as simply the root of the tree.  The
				 * representation of these internal nodes is
				 * simply the index of the left child combined
				 * with the special bits 0xC000 to distinguish
				 * the entry from direct mapping and leaf node
				 * entries.  */
				do {

					/* At least one bit remains in the
					 * codeword, but the current node is an
					 * unallocated leaf.  Change it to an
					 * internal node.  */
					if (decode_table[node_idx] == 0) {
						decode_table[node_idx] =
							next_free_tree_slot | 0xC000;
						decode_table[next_free_tree_slot++] = 0;
						decode_table[next_free_tree_slot++] = 0;
					}

					/* Go to the left child if the next bit
					 * in the codeword is 0; otherwise go to
					 * the right child.  */
					node_idx = decode_table[node_idx] & 0x3FFF;
					--extra_bits;
					node_idx += (cur_codeword >> extra_bits) & 1;
				} while (extra_bits != 0);

				/* We've traversed the tree using the entire
				 * codeword, and we're now at the entry where
				 * the actual symbol will be stored.  This is
				 * distinguished from internal nodes by not
				 * having its high two bits set.  */
				decode_table[node_idx] = sym;
			}
		}
	}
	return 0;
}