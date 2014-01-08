/*
 * wildcard.c
 *
 * Wildcard matching functions.
 */

/*
 * Copyright (C) 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <ctype.h>
#include "wimlib/dentry.h"
#include "wimlib/encoding.h"
#include "wimlib/error.h"
#include "wimlib/metadata.h"
#include "wimlib/wildcard.h"

struct match_dentry_ctx {
	int (*consume_dentry)(struct wim_dentry *, void *);
	void *consume_dentry_ctx;
	size_t consume_dentry_count;
	tchar *wildcard_path;
	size_t cur_component_offset;
	size_t cur_component_len;
	bool case_insensitive;
};

#define PLATFORM_SUPPORTS_FNMATCH

#ifdef __WIN32__
/* PathMatchSpec() could provide a fnmatch() alternative, but it isn't
 * documented properly, nor does it work properly.  For example, it returns that
 * any name matches *.* even if that name doesn't actually contain a period.  */
#  undef PLATFORM_SUPPORTS_FNMATCH
#endif

#ifndef PLATFORM_SUPPORTS_FNMATCH
static bool
do_match_wildcard(const tchar *string, size_t string_len,
		  const tchar *wildcard, size_t wildcard_len,
		  bool ignore_case)
{
	for (;;) {
		if (string_len == 0) {
			while (wildcard_len != 0 && *wildcard == T('*')) {
				wildcard++;
				wildcard_len--;
			}
			return (wildcard_len == 0);
		} else if (wildcard_len == 0) {
			return false;
		} else if (*string == *wildcard || *wildcard == T('?') ||
			   (ignore_case && totlower(*string) == totlower(*wildcard)))
		{
			string++;
			string_len--;
			wildcard_len--;
			wildcard++;
			continue;
		} else if (*wildcard == T('*')) {
			return do_match_wildcard(string, string_len,
						 wildcard + 1, wildcard_len - 1,
						 ignore_case) ||
			       do_match_wildcard(string + 1, string_len - 1,
						 wildcard, wildcard_len,
						 ignore_case);
		} else {
			return false;
		}
	}
}
#endif /* ! PLATFORM_SUPPORTS_FNMATCH  */

static bool
match_wildcard(const tchar *string, tchar *wildcard,
	       size_t wildcard_len, bool ignore_case)
{
#ifdef PLATFORM_SUPPORTS_FNMATCH
	char orig;
	int ret;
	int flags = FNM_NOESCAPE;
	if (ignore_case)
		flags |= FNM_CASEFOLD;

	orig = wildcard[wildcard_len];
	wildcard[wildcard_len] = T('\0');

	ret = fnmatch(wildcard, string, flags);

	wildcard[wildcard_len] = orig;
	return (ret == 0);
#else
	return do_match_wildcard(string, tstrlen(string),
				 wildcard, wildcard_len, ignore_case);
#endif
}

static int
expand_wildcard_recursive(struct wim_dentry *cur_dentry,
			  struct match_dentry_ctx *ctx);

enum {
	WILDCARD_STATUS_DONE_FULLY,
	WILDCARD_STATUS_DONE_TRAILING_SLASHES,
	WILDCARD_STATUS_NOT_DONE,
};

static int
wildcard_status(const tchar *wildcard)
{
	if (*wildcard == T('\0'))
		return WILDCARD_STATUS_DONE_FULLY;
	while (*wildcard == WIM_PATH_SEPARATOR)
		wildcard++;
	if (*wildcard == T('\0'))
		return WILDCARD_STATUS_DONE_TRAILING_SLASHES;

	return WILDCARD_STATUS_NOT_DONE;
}

static int
match_dentry(struct wim_dentry *cur_dentry, void *_ctx)
{
	struct match_dentry_ctx *ctx = _ctx;
	tchar *name;
	size_t name_len;
	int ret;

	if (cur_dentry->file_name_nbytes == 0)
		return 0;

#if TCHAR_IS_UTF16LE
	name = cur_dentry->file_name;
	name_len = cur_dentry->file_name_nbytes;
#else
	ret = utf16le_to_tstr(cur_dentry->file_name,
			      cur_dentry->file_name_nbytes,
			      &name, &name_len);
	if (ret)
		return ret;
#endif
	name_len /= sizeof(tchar);

	if (match_wildcard(name,
			   &ctx->wildcard_path[ctx->cur_component_offset],
			   ctx->cur_component_len,
			   ctx->case_insensitive))
	{
		switch (wildcard_status(&ctx->wildcard_path[
				ctx->cur_component_offset +
				ctx->cur_component_len]))
		{
		case WILDCARD_STATUS_DONE_TRAILING_SLASHES:
			if (!dentry_is_directory(cur_dentry)) {
				ret = 0;
				break;
			}
			/* Fall through  */
		case WILDCARD_STATUS_DONE_FULLY:
			ret = (*ctx->consume_dentry)(cur_dentry,
						     ctx->consume_dentry_ctx);
			ctx->consume_dentry_count++;
			break;
		case WILDCARD_STATUS_NOT_DONE:
			ret = expand_wildcard_recursive(cur_dentry, ctx);
			break;
		}
	} else {
		ret = 0;
	}

#if !TCHAR_IS_UTF16LE
	FREE(name);
#endif
	return ret;
}

static int
expand_wildcard_recursive(struct wim_dentry *cur_dentry,
			  struct match_dentry_ctx *ctx)
{
	tchar *w;
	size_t begin;
	size_t end;
	size_t len;
	size_t offset_save;
	size_t len_save;
	int ret;

	w = ctx->wildcard_path;

	begin = ctx->cur_component_offset + ctx->cur_component_len;
	while (w[begin] == WIM_PATH_SEPARATOR)
		begin++;

	end = begin;

	while (w[end] != T('\0') && w[end] != WIM_PATH_SEPARATOR)
		end++;

	len = end - begin;

	if (len == 0)
		return 0;

	offset_save = ctx->cur_component_offset;
	len_save = ctx->cur_component_len;

	ctx->cur_component_offset = begin;
	ctx->cur_component_len = len;

	ret = for_dentry_child(cur_dentry, match_dentry, ctx);

	ctx->cur_component_len = len_save;
	ctx->cur_component_offset = offset_save;

	return ret;
}

/* Expand a wildcard relative to the current WIM image.
 *
 * @wim
 *	WIMStruct whose currently selected image is searched to expand the
 *	wildcard.
 * @wildcard_path
 *	Wildcard path to expand, which may contain the '?' and '*' characters.
 *	Path separators must be WIM_PATH_SEPARATOR.  Leading path separators are
 *	ignored, whereas one or more trailing path separators indicate that the
 *	wildcard path can only match directories (and not reparse points).
 * @consume_dentry
 *	Callback function which will receive each directory entry matched by the
 *	wildcard.
 * @consume_dentry_ctx
 *	Argument to pass to @consume_dentry.
 * @flags
 *	Zero or more of the following flags:
 *
 *	WILDCARD_FLAG_WARN_IF_NO_MATCH:
 *		Issue a warning if the wildcard does not match any dentries.
 *
 *	WILDCARD_FLAG_ERROR_IF_NO_MATCH:
 *		Issue an error and return WIMLIB_ERR_PATH_DOES_NOT_EXIST if the
 *		wildcard does not match any dentries.
 *
 *	WILDCARD_FLAG_CASE_INSENSITIVE:
 *		Perform the matching case insensitively.  Note that this may
 *		cause @wildcard to match multiple dentries, even if it does not
 *		contain wildcard characters.
 *
 * @return 0 on success; a positive error code on error; or the first nonzero
 * value returned by @consume_dentry.
 *
 * Note: this function uses the @tmp_list field of dentries it attempts to
 * match.
 */
int
expand_wildcard(WIMStruct *wim,
		const tchar *wildcard_path,
		int (*consume_dentry)(struct wim_dentry *, void *),
		void *consume_dentry_ctx,
		u32 flags)
{
	struct wim_dentry *root;
	int ret;

	root = wim_root_dentry(wim);
	if (root == NULL)
		goto no_match;

	struct match_dentry_ctx ctx = {
		.consume_dentry = consume_dentry,
		.consume_dentry_ctx = consume_dentry_ctx,
		.consume_dentry_count = 0,
		.wildcard_path = TSTRDUP(wildcard_path),
		.cur_component_offset = 0,
		.cur_component_len = 0,
		.case_insensitive = ((flags & WILDCARD_FLAG_CASE_INSENSITIVE) != 0),
	};

	if (ctx.wildcard_path == NULL)
		return WIMLIB_ERR_NOMEM;

	ret = expand_wildcard_recursive(root, &ctx);
	FREE(ctx.wildcard_path);
	if (ret == 0 && ctx.consume_dentry_count == 0)
		goto no_match;
	return ret;

no_match:
	ret = 0;
	if (flags & WILDCARD_FLAG_WARN_IF_NO_MATCH)
		WARNING("No matches for wildcard path \"%"TS"\"", wildcard_path);

	if (flags & WILDCARD_FLAG_ERROR_IF_NO_MATCH) {
		ERROR("No matches for wildcard path \"%"TS"\"", wildcard_path);
		ret = WIMLIB_ERR_PATH_DOES_NOT_EXIST;
	}
	return ret;
}
