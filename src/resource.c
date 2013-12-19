/*
 * resource.c
 *
 * Read uncompressed and compressed metadata and file resources from a WIM file.
 */

/*
 * Copyright (C) 2012, 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "wimlib.h"
#include "wimlib/endianness.h"
#include "wimlib/error.h"
#include "wimlib/file_io.h"
#include "wimlib/lookup_table.h"
#include "wimlib/resource.h"
#include "wimlib/sha1.h"

#ifdef __WIN32__
/* for read_win32_file_prefix(), read_win32_encrypted_file_prefix() */
#  include "wimlib/win32.h"
#endif

#ifdef WITH_NTFS_3G
/* for read_ntfs_file_prefix() */
#  include "wimlib/ntfs_3g.h"
#endif

#ifdef HAVE_ALLOCA_H
#  include <alloca.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/*
 *                         Compressed WIM resources
 *
 * A compressed resource in a WIM consists of a number of compressed chunks,
 * each of which decompresses to a fixed chunk size (given in the WIM header;
 * usually 32768) except possibly the last, which always decompresses to any
 * remaining bytes.  In addition, immediately before the chunks, a table (the
 * "chunk table") provides the offset, in bytes relative to the end of the chunk
 * table, of the start of each compressed chunk, except for the first chunk
 * which is omitted as it always has an offset of 0.  Therefore, a compressed
 * resource with N chunks will have a chunk table with N - 1 entries.
 *
 * Additional information:
 *
 * - Entries in the chunk table are 4 bytes each, except if the uncompressed
 *   size of the resource is greater than 4 GiB, in which case the entries in
 *   the chunk table are 8 bytes each.  In either case, the entries are unsigned
 *   little-endian integers.
 *
 * - The chunk table is included in the compressed size of the resource provided
 *   in the corresponding entry in the WIM's stream lookup table.
 *
 * - The compressed size of a chunk is never greater than the uncompressed size.
 *   From the compressor's point of view, chunks that would have compressed to a
 *   size greater than or equal to their original size are in fact stored
 *   uncompressed.  From the decompresser's point of view, chunks with
 *   compressed size equal to their uncompressed size are in fact uncompressed.
 *
 * Furthermore, wimlib supports its own "pipable" WIM format, and for this the
 * structure of compressed resources was modified to allow piped reading and
 * writing.  To make sequential writing possible, the chunk table is placed
 * after the chunks rather than before the chunks, and to make sequential
 * reading possible, each chunk is prefixed with a 4-byte header giving its
 * compressed size as a 32-bit, unsigned, little-endian integer.  Otherwise the
 * details are the same.
 */


/* Decompress the specified chunk that uses the specified compression type
 * @ctype, part of a WIM with default chunk size @wim_chunk_size.  For LZX the
 * separate @wim_chunk_size is needed because it determines the window size used
 * for LZX compression.  */
static int
decompress(const void *cchunk, unsigned clen, void *uchunk, unsigned ulen,
	   int ctype, u32 wim_chunk_size)
{
	switch (ctype) {
	case WIMLIB_COMPRESSION_TYPE_LZX:
		return wimlib_lzx_decompress2(cchunk, clen,
					      uchunk, ulen, wim_chunk_size);
	case WIMLIB_COMPRESSION_TYPE_XPRESS:
		return wimlib_xpress_decompress(cchunk, clen,
						uchunk, ulen);
	case WIMLIB_COMPRESSION_TYPE_LZMS:
		return wimlib_lzms_decompress(cchunk, clen, uchunk, ulen);
	default:
		wimlib_assert(0);
		return -1;
	}
}

/* Read data from a compressed WIM resource.  Assumes parameters were already
 * verified by read_partial_wim_resource().  */
static int
read_compressed_wim_resource(const struct wim_resource_spec * const rspec,
			     const u64 size, const consume_data_callback_t cb,
			     const u32 cb_chunk_size, void * const ctx_or_buf,
			     const int flags, const u64 offset)
{
	int ret;
	int errno_save;

	const u32 orig_chunk_size = rspec->cchunk_size;
	const u32 orig_chunk_order = bsr32(orig_chunk_size);

	wimlib_assert(is_power_of_2(orig_chunk_size));

	/* Handle the trivial case.  */
	if (size == 0)
		return 0;

	if (rspec->ctype == WIMLIB_COMPRESSION_TYPE_LZMS) {
		/* TODO */

		unsigned clen = rspec->size_in_wim;
		unsigned ulen = rspec->uncompressed_size;
		unsigned lzms_offset;

		fprintf(stderr, "clen=%u, ulen=%u, offset=%lu\n", clen, ulen,
			rspec->offset_in_wim);

		u8 *cbuf = MALLOC(clen);
		u8 *ubuf = MALLOC(ulen);


		ret = full_pread(&rspec->wim->in_fd,
				 cbuf, clen, rspec->offset_in_wim);
		if (ret) {
			ERROR_WITH_ERRNO("Can't read compressed data");
			goto out_free_bufs;
		}

		if (clen <= rspec->cchunk_size)
			lzms_offset = 0;
		else
			lzms_offset = 20;

		ret = wimlib_lzms_decompress(cbuf + lzms_offset,
					     clen - lzms_offset,
					     ubuf, ulen);
		if (ret) {
			ERROR("LZMS decompression error.");
			errno = EINVAL;
			ret = WIMLIB_ERR_DECOMPRESSION;
			goto out_free_bufs;
		}
		if (cb) {
			u32 chunk_size;
			for (u64 i = offset; i < offset + size; i += chunk_size) {
				chunk_size = min(offset + size - i, cb_chunk_size);
				ret = cb(&ubuf[i], chunk_size, ctx_or_buf);
				if (ret)
					goto out_free_bufs;
			}
		} else {
			memcpy(ctx_or_buf, &ubuf[offset], size);
		}
		ret = 0;
	out_free_bufs:
		FREE(ubuf);
		FREE(cbuf);
		return ret;
	}

	u64 *chunk_offsets = NULL;
	u8 *out_buf = NULL;
	u8 *tmp_buf = NULL;
	void *compressed_buf = NULL;
	bool chunk_offsets_malloced = false;
	bool out_buf_malloced = false;
	bool tmp_buf_malloced = false;
	bool compressed_buf_malloced = false;

	/* Get the file descriptor for the WIM.  */
	struct filedes * const in_fd = &rspec->wim->in_fd;

	/* Determine if we're reading a pipable resource from a pipe or not.  */
	const bool is_pipe_read = !filedes_is_seekable(in_fd);

	/* Calculate the number of chunks the resource is divided into.  */
	const u64 num_chunks = (rspec->uncompressed_size + orig_chunk_size - 1) >> orig_chunk_order;

	/* Calculate the 0-based index of the chunk at which the read starts.
	 */
	const u64 start_chunk = offset >> orig_chunk_order;

	/* For pipe reads, we always must start from the 0th chunk.  */
	const u64 actual_start_chunk = (is_pipe_read ? 0 : start_chunk);

	/* Calculate the offset, within the start chunk, of the first byte of
	 * the read.  */
	const u32 start_offset_in_chunk = offset & (orig_chunk_size - 1);

	/* Calculate the index of the chunk that contains the last byte of the
	 * read.  */
	const u64 end_chunk = (offset + size - 1) >> orig_chunk_order;

	/* Calculate the offset, within the end chunk, of the last byte of the
	 * read.  */
	const u32 end_offset_in_chunk = (offset + size - 1) & (orig_chunk_size - 1);

	/* Calculate the number of entries in the chunk table; it's one less
	 * than the number of chunks, since the first chunk has no entry.  */
	const u64 num_chunk_entries = num_chunks - 1;

	/* Set the size of each chunk table entry based on the resource's
	 * uncompressed size.  */
	const u64 chunk_entry_size = (rspec->uncompressed_size > (1ULL << 32)) ? 8 : 4;

	/* Calculate the size, in bytes, of the full chunk table.  */
	const u64 chunk_table_size = num_chunk_entries * chunk_entry_size;

	/* Current offset to read from.  */
	u64 cur_read_offset = rspec->offset_in_wim;
	if (!is_pipe_read) {
		/* Read the chunk table into memory.  */

		/* Calculate the number of chunk entries are actually needed to
		 * read the requested part of the resource.  Include an entry
		 * for the first chunk even though that doesn't exist in the
		 * on-disk table, but take into account that if the last chunk
		 * required for the read is not the last chunk of the resource,
		 * an extra chunk entry is needed so that the compressed size of
		 * the last chunk of the read can be determined.  */
		const u64 num_alloc_chunk_entries = end_chunk - start_chunk +
						    1 + (end_chunk != num_chunks - 1);

		/* Allocate a buffer to hold a subset of the chunk table.  It
		 * will only contain offsets for the chunks that are actually
		 * needed for this read.  For speed, allocate the buffer on the
		 * stack unless it's too large.  */
		if ((size_t)(num_alloc_chunk_entries * sizeof(u64)) !=
		            (num_alloc_chunk_entries * sizeof(u64)))
			goto oom;

		if (num_alloc_chunk_entries <= STACK_MAX / sizeof(u64)) {
			chunk_offsets = alloca(num_alloc_chunk_entries * sizeof(u64));
		} else {
			chunk_offsets = MALLOC(num_alloc_chunk_entries * sizeof(u64));
			if (chunk_offsets == NULL)
				goto oom;
			chunk_offsets_malloced = true;
		}

		/* Set the implicit offset of the first chunk if it's included
		 * in the needed chunks.  */
		if (start_chunk == 0)
			chunk_offsets[0] = 0;

		/* Calculate the index of the first needed entry in the chunk
		 * table.  */
		const u64 start_table_idx = (start_chunk == 0) ?
				0 : start_chunk - 1;

		/* Calculate the number of entries that need to be read from the
		 * chunk table.  */
		const u64 num_needed_chunk_entries = (start_chunk == 0) ?
				num_alloc_chunk_entries - 1 : num_alloc_chunk_entries;

		/* Calculate the number of bytes of data that need to be read
		 * from the chunk table.  */
		const size_t chunk_table_needed_size =
				num_needed_chunk_entries * chunk_entry_size;

		/* Calculate the byte offset, in the WIM file, of the first
		 * chunk table entry to read.  Take into account that if the WIM
		 * file is in the special "pipable" format, then the chunk table
		 * is at the end of the resource, not the beginning.  */
		const u64 file_offset_of_needed_chunk_entries =
			rspec->offset_in_wim
			+ (start_table_idx * chunk_entry_size)
			+ (rspec->is_pipable ? (rspec->size_in_wim - chunk_table_size) : 0);

		/* Read the needed chunk table entries into the end of the
		 * chunk_offsets buffer.  */
		void * const chunk_tab_data = (u8*)&chunk_offsets[num_alloc_chunk_entries] -
					      chunk_table_needed_size;
		ret = full_pread(in_fd, chunk_tab_data, chunk_table_needed_size,
				 file_offset_of_needed_chunk_entries);
		if (ret)
			goto read_error;

		/* Now fill in chunk_offsets from the entries we have read in
		 * chunk_tab_data.  Careful: chunk_offsets aliases
		 * chunk_tab_data, which breaks C's aliasing rules when we read
		 * 32-bit integers and store 64-bit integers.  But since the
		 * operations are safe as long as the compiler doesn't mess with
		 * their order, we use the gcc may_alias extension to tell the
		 * compiler that loads from the 32-bit integers may alias stores
		 * to the 64-bit integers.  */
		{
			typedef le64 __attribute__((may_alias)) aliased_le64_t;
			typedef le32 __attribute__((may_alias)) aliased_le32_t;
			u64 * const chunk_offsets_p = chunk_offsets + (start_chunk == 0);
			u64 i;

			if (chunk_entry_size == 4) {
				aliased_le32_t *raw_entries = (aliased_le32_t*)chunk_tab_data;
				for (i = 0; i < num_needed_chunk_entries; i++)
					chunk_offsets_p[i] = le32_to_cpu(raw_entries[i]);
			} else {
				aliased_le64_t *raw_entries = (aliased_le64_t*)chunk_tab_data;
				for (i = 0; i < num_needed_chunk_entries; i++)
					chunk_offsets_p[i] = le64_to_cpu(raw_entries[i]);
			}
		}

		/* Set offset to beginning of first chunk to read.  */
		cur_read_offset += chunk_offsets[0];
		if (rspec->is_pipable)
			cur_read_offset += start_chunk * sizeof(struct pwm_chunk_hdr);
		else
			cur_read_offset += chunk_table_size;
	}

	/* If using a callback function, allocate a temporary buffer that will
	 * hold data being passed to it.  If writing directly to a buffer
	 * instead, arrange to write data directly into it.  */
	size_t out_buf_size;
	u8 *out_buf_end, *out_p;
	if (cb) {
		out_buf_size = max(cb_chunk_size, orig_chunk_size);
		if (out_buf_size <= STACK_MAX) {
			out_buf = alloca(out_buf_size);
		} else {
			out_buf = MALLOC(out_buf_size);
			if (out_buf == NULL)
				goto oom;
			out_buf_malloced = true;
		}
	} else {
		out_buf_size = size;
		out_buf = ctx_or_buf;
	}
	out_buf_end = out_buf + out_buf_size;
	out_p = out_buf;

	/* Unless the raw compressed data was requested, allocate a temporary
	 * buffer for reading compressed chunks, each of which can be at most
	 * @orig_chunk_size - 1 bytes.  This excludes compressed chunks that are
	 * a full @orig_chunk_size bytes, which are actually stored
	 * uncompressed.  */
	if (!(flags & WIMLIB_READ_RESOURCE_FLAG_RAW_CHUNKS)) {
		if (orig_chunk_size - 1 <= STACK_MAX) {
			compressed_buf = alloca(orig_chunk_size - 1);
		} else {
			compressed_buf = MALLOC(orig_chunk_size - 1);
			if (compressed_buf == NULL)
				goto oom;
			compressed_buf_malloced = true;
		}
	}

	/* Allocate yet another temporary buffer, this one for decompressing
	 * chunks for which only part of the data is needed.  */
	if (start_offset_in_chunk != 0 ||
	    (end_offset_in_chunk != orig_chunk_size - 1 &&
	     offset + size != rspec->uncompressed_size))
	{
		if (orig_chunk_size <= STACK_MAX) {
			tmp_buf = alloca(orig_chunk_size);
		} else {
			tmp_buf = MALLOC(orig_chunk_size);
			if (tmp_buf == NULL)
				goto oom;
			tmp_buf_malloced = true;
		}
	}

	/* Read, and possibly decompress, each needed chunk, either writing the
	 * data directly into the @ctx_or_buf buffer or passing it to the @cb
	 * callback function.  */
	for (u64 i = actual_start_chunk; i <= end_chunk; i++) {

		/* Calculate uncompressed size of next chunk.  */
		u32 chunk_usize;
		if ((i == num_chunks - 1) && (rspec->uncompressed_size & (orig_chunk_size - 1)))
			chunk_usize = (rspec->uncompressed_size & (orig_chunk_size - 1));
		else
			chunk_usize = orig_chunk_size;

		/* Calculate compressed size of next chunk.  */
		u32 chunk_csize;
		if (is_pipe_read) {
			struct pwm_chunk_hdr chunk_hdr;

			ret = full_pread(in_fd, &chunk_hdr,
					 sizeof(chunk_hdr), cur_read_offset);
			if (ret)
				goto read_error;
			chunk_csize = le32_to_cpu(chunk_hdr.compressed_size);
		} else {
			if (i == num_chunks - 1) {
				chunk_csize = rspec->size_in_wim -
					      chunk_table_size -
					      chunk_offsets[i - start_chunk];
				if (rspec->is_pipable)
					chunk_csize -= num_chunks * sizeof(struct pwm_chunk_hdr);
			} else {
				chunk_csize = chunk_offsets[i + 1 - start_chunk] -
					      chunk_offsets[i - start_chunk];
			}
		}
		if (chunk_csize == 0 || chunk_csize > chunk_usize) {
			ERROR("Invalid chunk size in compressed resource!");
			errno = EINVAL;
			ret = WIMLIB_ERR_DECOMPRESSION;
			goto out_free_memory;
		}
		if (rspec->is_pipable)
			cur_read_offset += sizeof(struct pwm_chunk_hdr);

		if (i >= start_chunk) {
			/* Calculate how much of this chunk needs to be read.  */
			u32 chunk_needed_size;
			u32 start_offset = 0;
			u32 end_offset = orig_chunk_size - 1;

			if (flags & WIMLIB_READ_RESOURCE_FLAG_RAW_CHUNKS) {
				chunk_needed_size = chunk_csize;
			} else {
				if (i == start_chunk)
					start_offset = start_offset_in_chunk;

				if (i == end_chunk)
					end_offset = end_offset_in_chunk;

				chunk_needed_size = end_offset + 1 - start_offset;
			}

			if (chunk_csize == chunk_usize ||
			    (flags & WIMLIB_READ_RESOURCE_FLAG_RAW_CHUNKS))
			{
				/* Read the raw chunk data.  */

				ret = full_pread(in_fd,
						 out_p,
						 chunk_needed_size,
						 cur_read_offset + start_offset);
				if (ret)
					goto read_error;
			} else {
				/* Read and decompress the chunk.  */

				u8 *target;

				ret = full_pread(in_fd,
						 compressed_buf,
						 chunk_csize,
						 cur_read_offset);
				if (ret)
					goto read_error;

				if (chunk_needed_size == chunk_usize)
					target = out_p;
				else
					target = tmp_buf;

				ret = decompress(compressed_buf,
						 chunk_csize,
						 target,
						 chunk_usize,
						 rspec->ctype,
						 orig_chunk_size);
				if (ret) {
					ERROR("Failed to decompress data!");
					ret = WIMLIB_ERR_DECOMPRESSION;
					errno = EINVAL;
					goto out_free_memory;
				}
				if (chunk_needed_size != chunk_usize)
					memcpy(out_p, tmp_buf + start_offset,
					       chunk_needed_size);
			}

			out_p += chunk_needed_size;

			if (cb) {
				/* Feed the data to the callback function.  */

				if (flags & WIMLIB_READ_RESOURCE_FLAG_RAW_CHUNKS) {
					ret = cb(out_buf, out_p - out_buf, ctx_or_buf);
					if (ret)
						goto out_free_memory;
					out_p = out_buf;
				} else if (i == end_chunk || out_p == out_buf_end) {
					size_t bytes_sent;
					const u8 *p;

					for (p = out_buf; p != out_p; p += bytes_sent) {
						bytes_sent = min(cb_chunk_size, out_p - p);
						ret = cb(p, bytes_sent, ctx_or_buf);
						if (ret)
							goto out_free_memory;
					}
					out_p = out_buf;
				}
			}
			cur_read_offset += chunk_csize;
		} else {
			u8 dummy;

			/* Skip data only.  */
			cur_read_offset += chunk_csize;
			ret = full_pread(in_fd, &dummy, 1, cur_read_offset - 1);
			if (ret)
				goto read_error;
		}
	}

	if (is_pipe_read
	    && size == rspec->uncompressed_size
	    && chunk_table_size)
	{
		u8 dummy;
		/* Skip chunk table at end of pipable resource.  */

		cur_read_offset += chunk_table_size;
		ret = full_pread(in_fd, &dummy, 1, cur_read_offset - 1);
		if (ret)
			goto read_error;
	}
	ret = 0;
out_free_memory:
	errno_save = errno;
	if (chunk_offsets_malloced)
		FREE(chunk_offsets);
	if (out_buf_malloced)
		FREE(out_buf);
	if (compressed_buf_malloced)
		FREE(compressed_buf);
	if (tmp_buf_malloced)
		FREE(tmp_buf);
	errno = errno_save;
	return ret;

oom:
	ERROR("Not enough memory available to read size=%"PRIu64" bytes "
	      "from compressed resource!", size);
	errno = ENOMEM;
	ret = WIMLIB_ERR_NOMEM;
	goto out_free_memory;

read_error:
	ERROR_WITH_ERRNO("Error reading compressed file resource!");
	goto out_free_memory;
}

/* Read raw data from a file descriptor at the specified offset.  */
static int
read_raw_file_data(struct filedes *in_fd, u64 size, consume_data_callback_t cb,
		   u32 cb_chunk_size, void *ctx_or_buf, u64 offset)
{
	int ret;
	u8 *tmp_buf;
	bool tmp_buf_malloced = false;

	if (cb) {
		/* Send data to callback function in chunks.  */
		if (cb_chunk_size <= STACK_MAX) {
			tmp_buf = alloca(cb_chunk_size);
		} else {
			tmp_buf = MALLOC(cb_chunk_size);
			if (tmp_buf == NULL) {
				ret = WIMLIB_ERR_NOMEM;
				goto out;
			}
			tmp_buf_malloced = true;
		}

		while (size) {
			size_t bytes_to_read = min(cb_chunk_size, size);
			ret = full_pread(in_fd, tmp_buf, bytes_to_read,
					 offset);
			if (ret)
				goto read_error;
			ret = cb(tmp_buf, bytes_to_read, ctx_or_buf);
			if (ret)
				goto out;
			size -= bytes_to_read;
			offset += bytes_to_read;
		}
	} else {
		/* Read data directly into buffer.  */
		ret = full_pread(in_fd, ctx_or_buf, size, offset);
		if (ret)
			goto read_error;
	}
	ret = 0;
	goto out;

read_error:
	ERROR_WITH_ERRNO("Read error");
out:
	if (tmp_buf_malloced)
		FREE(tmp_buf);
	return ret;
}

/*
 * read_partial_wim_resource()-
 *
 * Read a range of data from an uncompressed or compressed resource in a WIM
 * file.  Data is written into a buffer or fed into a callback function, as
 * documented in read_stream_prefix().
 *
 * By default, this function provides the uncompressed data of the resource, and
 * @size and @offset and interpreted relative to the uncompressed contents of
 * the resource.  This behavior can be modified by either of the following
 * flags:
 *
 * WIMLIB_READ_RESOURCE_FLAG_RAW_FULL:
 *	Read @size bytes at @offset of the raw contents of the compressed
 *	resource.  In the case of pipable resources, this excludes the stream
 *	header.  Exclusive with WIMLIB_READ_RESOURCE_FLAG_RAW_CHUNKS.
 *
 * WIMLIB_READ_RESOURCE_FLAG_RAW_CHUNKS:
 *	Read the raw compressed chunks of the compressed resource.  @size must
 *	be the full uncompressed size, @offset must be 0, and @cb_chunk_size
 *	must be the resource chunk size.
 *
 * Return values:
 *	WIMLIB_ERR_SUCCESS (0)
 *	WIMLIB_ERR_READ			  (errno set)
 *	WIMLIB_ERR_UNEXPECTED_END_OF_FILE (errno set to 0)
 *	WIMLIB_ERR_NOMEM		  (errno set to ENOMEM)
 *	WIMLIB_ERR_DECOMPRESSION	  (errno set to EINVAL)
 *
 *	or other error code returned by the @cb function.
 */
int
read_partial_wim_resource(const struct wim_lookup_table_entry *lte,
			  u64 size, consume_data_callback_t cb,
			  u32 cb_chunk_size, void *ctx_or_buf,
			  int flags, u64 offset)
{
	const struct wim_resource_spec *rspec;
	struct filedes *in_fd;

	/* Verify parameters.  */
	wimlib_assert(lte->resource_location == RESOURCE_IN_WIM);
	rspec = lte->rspec;
	in_fd = &rspec->wim->in_fd;
	if (cb)
		wimlib_assert(is_power_of_2(cb_chunk_size));
	if (flags & WIMLIB_READ_RESOURCE_FLAG_RAW_CHUNKS) {
		/* Raw chunks mode is subject to the restrictions noted.  */
		wimlib_assert(!lte_is_partial(lte));
		wimlib_assert(!(flags & WIMLIB_READ_RESOURCE_FLAG_RAW_FULL));
		wimlib_assert(cb_chunk_size == rspec->cchunk_size);
		wimlib_assert(size == rspec->uncompressed_size);
		wimlib_assert(offset == 0);
	} else if (flags & WIMLIB_READ_RESOURCE_FLAG_RAW_FULL) {
		/* Raw full mode:  read must not overrun end of store size.  */
		wimlib_assert(!lte_is_partial(lte));
		wimlib_assert(offset + size >= size &&
			      offset + size <= rspec->size_in_wim);
	} else {
		/* Normal mode:  read must not overrun end of original size.  */
		wimlib_assert(offset + size >= size &&
			      lte->offset_in_res + offset + size <= rspec->uncompressed_size);
	}

	DEBUG("Reading WIM resource: %"PRIu64" @ +%"PRIu64"[+%"PRIu64"] "
	      "from %"PRIu64"(%"PRIu64") @ +%"PRIu64" "
	      "(readflags 0x%08x, resflags 0x%02x%s)",
	      size, offset, lte->offset_in_res,
	      rspec->size_in_wim,
	      rspec->uncompressed_size,
	      rspec->offset_in_wim,
	      flags, lte->flags,
	      (rspec->is_pipable ? ", pipable" : ""));

	if ((flags & WIMLIB_READ_RESOURCE_FLAG_RAW_FULL) ||
	    rspec->ctype == WIMLIB_COMPRESSION_TYPE_NONE)
	{
		return read_raw_file_data(in_fd,
					  size,
					  cb,
					  cb_chunk_size,
					  ctx_or_buf,
					  offset + rspec->offset_in_wim);
	} else {
		return read_compressed_wim_resource(rspec, size, cb,
						    cb_chunk_size,
						    ctx_or_buf, flags, offset + lte->offset_in_res);
	}
}

int
read_partial_wim_stream_into_buf(const struct wim_lookup_table_entry *lte,
				 size_t size, u64 offset, void *buf)
{
	return read_partial_wim_resource(lte, size, NULL, 0, buf, 0, offset);
}

static int
read_wim_stream_prefix(const struct wim_lookup_table_entry *lte, u64 size,
		       consume_data_callback_t cb, u32 cb_chunk_size,
		       void *ctx_or_buf, int flags)
{
	return read_partial_wim_resource(lte, size, cb, cb_chunk_size,
					 ctx_or_buf, flags, 0);
}

#ifndef __WIN32__
/* This function handles reading stream data that is located in an external
 * file,  such as a file that has been added to the WIM image through execution
 * of a wimlib_add_command.
 *
 * This assumes the file can be accessed using the standard POSIX open(),
 * read(), and close().  On Windows this will not necessarily be the case (since
 * the file may need FILE_FLAG_BACKUP_SEMANTICS to be opened, or the file may be
 * encrypted), so Windows uses its own code for its equivalent case.
 */
static int
read_file_on_disk_prefix(const struct wim_lookup_table_entry *lte, u64 size,
			 consume_data_callback_t cb, u32 cb_chunk_size,
			 void *ctx_or_buf, int _ignored_flags)
{
	int ret;
	int raw_fd;
	struct filedes fd;

	wimlib_assert(size <= lte->size);
	DEBUG("Reading %"PRIu64" bytes from \"%"TS"\"", size, lte->file_on_disk);

	raw_fd = open(lte->file_on_disk, O_BINARY | O_RDONLY);
	if (raw_fd < 0) {
		ERROR_WITH_ERRNO("Can't open \"%"TS"\"", lte->file_on_disk);
		return WIMLIB_ERR_OPEN;
	}
	filedes_init(&fd, raw_fd);
	ret = read_raw_file_data(&fd, size, cb, cb_chunk_size, ctx_or_buf, 0);
	filedes_close(&fd);
	return ret;
}
#endif /* !__WIN32__ */

/* This function handles the trivial case of reading stream data that is, in
 * fact, already located in an in-memory buffer.  */
static int
read_buffer_prefix(const struct wim_lookup_table_entry *lte,
		   u64 size, consume_data_callback_t cb,
		   u32 cb_chunk_size, void *ctx_or_buf, int _ignored_flags)
{
	wimlib_assert(size <= lte->size);

	if (cb) {
		/* Feed the data into the callback function in
		 * appropriately-sized chunks.  */
		int ret;
		u32 chunk_size;

		for (u64 offset = 0; offset < size; offset += chunk_size) {
			chunk_size = min(cb_chunk_size, size - offset);
			ret = cb((const u8*)lte->attached_buffer + offset,
				 chunk_size, ctx_or_buf);
			if (ret)
				return ret;
		}
	} else {
		/* Copy the data directly into the specified buffer.  */
		memcpy(ctx_or_buf, lte->attached_buffer, size);
	}
	return 0;
}

typedef int (*read_stream_prefix_handler_t)(const struct wim_lookup_table_entry *lte,
					    u64 size, consume_data_callback_t cb,
					    u32 cb_chunk_size, void *ctx_or_buf,
					    int flags);

/*
 * read_stream_prefix()-
 *
 * Reads the first @size bytes from a generic "stream", which may be located in
 * any one of several locations, such as in a WIM file (compressed or
 * uncompressed), in an external file, or directly in an in-memory buffer.
 *
 * This function feeds the data either to a callback function (@cb != NULL,
 * passing it @ctx_or_buf), or write it directly into a buffer (@cb == NULL,
 * @ctx_or_buf specifies the buffer, which must have room for at least @size
 * bytes).
 *
 * When (@cb != NULL), @cb_chunk_size specifies the maximum size of data chunks
 * to feed the callback function.  @cb_chunk_size must be positive, and if the
 * stream is in a WIM file, must be a power of 2.  All chunks, except possibly
 * the last one, will be this size.  If (@cb == NULL), @cb_chunk_size is
 * ignored.
 *
 * If the stream is located in a WIM file, @flags can be set as documented in
 * read_partial_wim_resource().  Otherwise @flags are ignored.
 *
 * Returns 0 on success; nonzero on error.  A nonzero value will be returned if
 * the stream data cannot be successfully read (for a number of different
 * reasons, depending on the stream location), or if a callback function was
 * specified and it returned nonzero.
 */
int
read_stream_prefix(const struct wim_lookup_table_entry *lte, u64 size,
		   consume_data_callback_t cb, u32 cb_chunk_size,
		   void *ctx_or_buf, int flags)
{
	/* This function merely verifies several preconditions, then passes
	 * control to an appropriate function for understanding each possible
	 * stream location.  */
	static const read_stream_prefix_handler_t handlers[] = {
		[RESOURCE_IN_WIM]             = read_wim_stream_prefix,
	#ifdef __WIN32__
		[RESOURCE_IN_FILE_ON_DISK]    = read_win32_file_prefix,
	#else
		[RESOURCE_IN_FILE_ON_DISK]    = read_file_on_disk_prefix,
	#endif
		[RESOURCE_IN_ATTACHED_BUFFER] = read_buffer_prefix,
	#ifdef WITH_FUSE
		[RESOURCE_IN_STAGING_FILE]    = read_file_on_disk_prefix,
	#endif
	#ifdef WITH_NTFS_3G
		[RESOURCE_IN_NTFS_VOLUME]     = read_ntfs_file_prefix,
	#endif
	#ifdef __WIN32__
		[RESOURCE_WIN32_ENCRYPTED]    = read_win32_encrypted_file_prefix,
	#endif
	};
	wimlib_assert(lte->resource_location < ARRAY_LEN(handlers)
		      && handlers[lte->resource_location] != NULL);
	wimlib_assert(cb == NULL || cb_chunk_size > 0);
	return handlers[lte->resource_location](lte, size, cb, cb_chunk_size,
						ctx_or_buf, flags);
}

/* Read the full uncompressed data of the specified stream into the specified
 * buffer, which must have space for at least lte->size bytes.  */
int
read_full_stream_into_buf(const struct wim_lookup_table_entry *lte, void *buf)
{
	return read_stream_prefix(lte, lte->size, NULL, 0, buf, 0);
}

/* Read the full uncompressed data of the specified stream.  A buffer sufficient
 * to hold the data is allocated and returned in @buf_ret.  */
int
read_full_stream_into_alloc_buf(const struct wim_lookup_table_entry *lte,
				void **buf_ret)
{
	int ret;
	void *buf;

	if ((size_t)lte->size != lte->size) {
		ERROR("Can't read %"PRIu64" byte stream into "
		      "memory", lte->size);
		return WIMLIB_ERR_NOMEM;
	}

	buf = MALLOC(lte->size);
	if (buf == NULL)
		return WIMLIB_ERR_NOMEM;

	ret = read_full_stream_into_buf(lte, buf);
	if (ret) {
		FREE(buf);
		return ret;
	}

	*buf_ret = buf;
	return 0;
}

/* Retrieve the full uncompressed data of the specified WIM resource.  */
static int
wim_resource_spec_to_data(struct wim_resource_spec *rspec, void **buf_ret)
{
	int ret;
	struct wim_lookup_table_entry *lte;

	lte = new_lookup_table_entry();
	if (lte == NULL)
		return WIMLIB_ERR_NOMEM;

	lte->unhashed = 1;
	lte_bind_wim_resource_spec(lte, rspec);
	lte->flags = rspec->flags;
	lte->size = rspec->uncompressed_size;
	lte->offset_in_res = 0;

	ret = read_full_stream_into_alloc_buf(lte, buf_ret);

	lte_unbind_wim_resource_spec(lte);
	free_lookup_table_entry(lte);
	return ret;
}

/* Retrieve the full uncompressed data of the specified WIM resource.  */
int
wim_reshdr_to_data(const struct wim_reshdr *reshdr, WIMStruct *wim, void **buf_ret)
{
	DEBUG("offset_in_wim=%"PRIu64", size_in_wim=%"PRIu64", "
	      "uncompressed_size=%"PRIu64,
	      reshdr->offset_in_wim, reshdr->size_in_wim, reshdr->uncompressed_size);

	struct wim_resource_spec rspec;
	wim_res_hdr_to_spec(reshdr, wim, &rspec);
	return wim_resource_spec_to_data(&rspec, buf_ret);
}

struct extract_ctx {
	SHA_CTX sha_ctx;
	consume_data_callback_t extract_chunk;
	void *extract_chunk_arg;
};

static int
extract_chunk_sha1_wrapper(const void *chunk, size_t chunk_size, void *_ctx)
{
	struct extract_ctx *ctx = _ctx;

	sha1_update(&ctx->sha_ctx, chunk, chunk_size);
	return ctx->extract_chunk(chunk, chunk_size, ctx->extract_chunk_arg);
}

/* Extracts the first @size bytes of a stream to somewhere.  In the process, the
 * SHA1 message digest of the uncompressed stream is checked if the full stream
 * is being extracted.
 *
 * @extract_chunk is a function that will be called to extract each chunk of the
 * stream.  */
int
extract_stream(const struct wim_lookup_table_entry *lte, u64 size,
	       consume_data_callback_t extract_chunk, void *extract_chunk_arg)
{
	int ret;
	if (size == lte->size) {
		/* Do SHA1 */
		struct extract_ctx ctx;
		ctx.extract_chunk = extract_chunk;
		ctx.extract_chunk_arg = extract_chunk_arg;
		sha1_init(&ctx.sha_ctx);
		ret = read_stream_prefix(lte, size,
					 extract_chunk_sha1_wrapper,
					 lte_cchunk_size(lte),
					 &ctx, 0);
		if (ret == 0) {
			u8 hash[SHA1_HASH_SIZE];
			sha1_final(hash, &ctx.sha_ctx);
			if (!hashes_equal(hash, lte->hash)) {
				if (wimlib_print_errors) {
					ERROR("Invalid SHA1 message digest "
					      "on the following WIM stream:");
					print_lookup_table_entry(lte, stderr);
					if (lte->resource_location == RESOURCE_IN_WIM)
						ERROR("The WIM file appears to be corrupt!");
				}
				ret = WIMLIB_ERR_INVALID_RESOURCE_HASH;
			}
		}
	} else {
		/* Don't do SHA1 */
		ret = read_stream_prefix(lte, size, extract_chunk,
					 lte_cchunk_size(lte),
					 extract_chunk_arg, 0);
	}
	return ret;
}

static int
extract_wim_chunk_to_fd(const void *buf, size_t len, void *_fd_p)
{
	struct filedes *fd = _fd_p;
	int ret = full_write(fd, buf, len);
	if (ret)
		ERROR_WITH_ERRNO("Error writing to file descriptor");
	return ret;
}

/* Extract the first @size bytes of the specified stream to the specified file
 * descriptor.  If @size is the full size of the stream, its SHA1 message digest
 * is also checked.  */
int
extract_stream_to_fd(const struct wim_lookup_table_entry *lte,
		     struct filedes *fd, u64 size)
{
	return extract_stream(lte, size, extract_wim_chunk_to_fd, fd);
}


static int
sha1_chunk(const void *buf, size_t len, void *ctx)
{
	sha1_update(ctx, buf, len);
	return 0;
}

/* Calculate the SHA1 message digest of a stream, storing it in @lte->hash.  */
int
sha1_stream(struct wim_lookup_table_entry *lte)
{
	int ret;
	SHA_CTX sha_ctx;

	sha1_init(&sha_ctx);
	ret = read_stream_prefix(lte, lte->size,
				 sha1_chunk, lte_cchunk_size(lte),
				 &sha_ctx, 0);
	if (ret == 0)
		sha1_final(lte->hash, &sha_ctx);

	return ret;
}

/* Convert a WIM resource header to a stand-alone resource specification.  */
void
wim_res_hdr_to_spec(const struct wim_reshdr *reshdr, WIMStruct *wim,
		    struct wim_resource_spec *spec)
{
	spec->wim = wim;
	spec->offset_in_wim = reshdr->offset_in_wim;
	spec->size_in_wim = reshdr->size_in_wim;
	spec->uncompressed_size = reshdr->uncompressed_size;
	INIT_LIST_HEAD(&spec->lte_list);
	spec->flags = reshdr->flags;
	spec->is_pipable = wim_is_pipable(wim);
	if (spec->flags & (WIM_RESHDR_FLAG_COMPRESSED | WIM_RESHDR_FLAG_CONCAT)) {
		spec->ctype = wim->compression_type;
		spec->cchunk_size = wim->chunk_size;
	} else {
		spec->ctype = WIMLIB_COMPRESSION_TYPE_NONE;
		spec->cchunk_size = 0;
	}
}

/* Convert a stand-alone resource specification to a WIM resource header.  */
void
wim_res_spec_to_hdr(const struct wim_resource_spec *rspec,
		    struct wim_reshdr *reshdr)
{
	reshdr->offset_in_wim     = rspec->offset_in_wim;
	reshdr->size_in_wim       = rspec->size_in_wim;
	reshdr->flags             = rspec->flags;
	reshdr->uncompressed_size = rspec->uncompressed_size;
}

/* Translates a WIM resource header from the on-disk format into an in-memory
 * format.  */
int
get_wim_reshdr(const struct wim_reshdr_disk *disk_reshdr,
	       struct wim_reshdr *reshdr)
{
	reshdr->offset_in_wim = le64_to_cpu(disk_reshdr->offset_in_wim);
	reshdr->size_in_wim = (((u64)disk_reshdr->size_in_wim[0] <<  0) |
			      ((u64)disk_reshdr->size_in_wim[1] <<  8) |
			      ((u64)disk_reshdr->size_in_wim[2] << 16) |
			      ((u64)disk_reshdr->size_in_wim[3] << 24) |
			      ((u64)disk_reshdr->size_in_wim[4] << 32) |
			      ((u64)disk_reshdr->size_in_wim[5] << 40) |
			      ((u64)disk_reshdr->size_in_wim[6] << 48));
	reshdr->uncompressed_size = le64_to_cpu(disk_reshdr->uncompressed_size);
	reshdr->flags = disk_reshdr->flags;

	/* Truncate numbers to 62 bits to avoid possible overflows.  */
	if (reshdr->offset_in_wim & 0xc000000000000000ULL)
		return WIMLIB_ERR_INVALID_LOOKUP_TABLE_ENTRY;

	if (reshdr->uncompressed_size & 0xc000000000000000ULL)
		return WIMLIB_ERR_INVALID_LOOKUP_TABLE_ENTRY;

	return 0;
}

/* Translates a WIM resource header from an in-memory format into the on-disk
 * format.  */
void
put_wim_reshdr(const struct wim_reshdr *reshdr,
	       struct wim_reshdr_disk *disk_reshdr)
{
	disk_reshdr->size_in_wim[0] = reshdr->size_in_wim  >>  0;
	disk_reshdr->size_in_wim[1] = reshdr->size_in_wim  >>  8;
	disk_reshdr->size_in_wim[2] = reshdr->size_in_wim  >> 16;
	disk_reshdr->size_in_wim[3] = reshdr->size_in_wim  >> 24;
	disk_reshdr->size_in_wim[4] = reshdr->size_in_wim  >> 32;
	disk_reshdr->size_in_wim[5] = reshdr->size_in_wim  >> 40;
	disk_reshdr->size_in_wim[6] = reshdr->size_in_wim  >> 48;
	disk_reshdr->flags = reshdr->flags;
	disk_reshdr->offset_in_wim = cpu_to_le64(reshdr->offset_in_wim);
	disk_reshdr->uncompressed_size = cpu_to_le64(reshdr->uncompressed_size);
}
