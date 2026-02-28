/*
 * Copyright 2006 James Bursa <bursa@users.sourceforge.net>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * Interface to browser history operations
 *
 * The are operations on a browsing contexts history. These interfaces
 * allow navigation forward and backwards in the history as well as
 * enumerating the entries.
 *
 * The local history viewing is distinct via corewindow defined in
 * desktop/local_history.h
 */

#ifndef NETSURF_DESKTOP_BROWSER_HISTORY_H
#define NETSURF_DESKTOP_BROWSER_HISTORY_H

#include <stdbool.h>

#include "utils/errors.h"

struct browser_window;
struct history_entry;
struct bitmap;

/**
 * Go back in the history.
 *
 * \param bw A browser window to navigate the history in.
 * \param new_window whether to open in new window.
 * \return NSERROR_OK or error code on faliure.
 */
nserror browser_window_history_back(struct browser_window *bw, bool new_window);


/**
 * Go forward in the history.
 *
 * \param bw A browser window to navigate the history in.
 * \param new_window whether to open in new window.
 * \return NSERROR_OK or error code on faliure.
 */
nserror browser_window_history_forward(struct browser_window *bw, bool new_window);


/**
 * Check whether it is pssible to go back in the history.
 *
 * \param bw A browser window to check the history navigation in.
 * \return true if the history can go back, false otherwise
 */
bool browser_window_history_back_available(struct browser_window *bw);


/**
 * Check whether it is pssible to go forwards in the history.
 *
 * \param bw A browser window to check the history navigation in.
 * \return true if the history can go forwards, false otherwise
 */
bool browser_window_history_forward_available(struct browser_window *bw);

/**
 * Get the thumbnail bitmap for the current history entry
 *
 * \param bw The browser window
 * \param bitmap The bitmat for the current history entry.
 * \return NSERROR_OK or error code on faliure.
 */
nserror browser_window_history_get_thumbnail(struct browser_window *bw, struct bitmap **bitmap_out);

/**
 * Callback function type for history enumeration
 *
 * \param	bw		The browser window with history being enumerated
 * \param	x0, y0, x1, y1	Coordinates of entry in history tree view
 * \param	entry		Current history entry
 * \return	true to continue enumeration, false to cancel enumeration
 */
typedef bool (*browser_window_history_enumerate_cb)(
		const struct browser_window *bw,
		int x0, int y0, int x1, int y1, 
		const struct history_entry *entry, void *user_data);


/**
 * Enumerate all entries in the history.
 * Do not change the history while it is being enumerated.
 *
 * \param	bw		The browser window to enumerate history of
 * \param	cb		callback function
 * \param	user_data	context pointer passed to cb
 */
void browser_window_history_enumerate(const struct browser_window *bw,
		browser_window_history_enumerate_cb cb, void *user_data);


/**
 * Enumerate all entries that will be reached by the 'forward' button
 *
 * \param	bw		The browser window to enumerate history of
 * \param	cb		The callback function
 * \param	user_data	Data passed to the callback
 */
void browser_window_history_enumerate_forward(const struct browser_window *bw, 
		browser_window_history_enumerate_cb cb, void *user_data);


/**
 * Enumerate all entries that will be reached by the 'back' button
 *
 * \param	bw		The browser window to enumerate history of
 * \param	cb		The callback function
 * \param	user_data	Data passed to the callback
 */
void browser_window_history_enumerate_back(const struct browser_window *bw, 
		browser_window_history_enumerate_cb cb, void *user_data);


/**
 * Returns the URL to a history entry
 *
 * \param entry the history entry to retrieve the URL from
 * \return A referenced nsurl URL
 */
struct nsurl *browser_window_history_entry_get_url(const struct history_entry *entry);


/**
 * Returns the URL to a history entry
 *
 * \param entry the history entry to retrieve the fragment id from
 * \return the fragment id
 */
const char *browser_window_history_entry_get_fragment_id(const struct history_entry *entry);


/**
 * Returns the title of a history entry
 *
 * \param entry The history entry to retrieve the title from
 * \return the title
 */
const char *browser_window_history_entry_get_title(const struct history_entry *entry);


/**
 * Navigate to specified history entry, optionally in new window
 *
 * \param  bw          browser window
 * \param  entry       entry to open
 * \param  new_window  open entry in new window
 * \return NSERROR_OK or error code on faliure.
 */
nserror browser_window_history_go(struct browser_window *bw, struct history_entry *entry, bool new_window);


/**
 * Return the number of entries in the linear history chain.
 *
 * WHY: HTML5 History API exposes window.history.length. NetSurf's history is
 *      a tree; we count the back-chain length (entries behind current) plus
 *      the forward-pref chain length plus the current entry itself.
 *
 * \param bw  browser window
 * \return    number of history entries visible in the linear chain, or 0
 */
int browser_window_history_length(struct browser_window *bw);


/**
 * Update the current history entry URL and refresh the URL bar without
 * performing a new fetch (HTML5 History API replaceState semantics).
 *
 * WHY: replaceState modifies the current session-history entry in place;
 *      no navigation occurs and no new entry is created.
 *
 * \param bw    browser window
 * \param url   new absolute URL to associate with current entry
 * \return NSERROR_OK or error code on failure
 */
nserror browser_window_history_replace_state(struct browser_window *bw,
					     struct nsurl *url);


/**
 * Insert a new history entry for the given URL and refresh the URL bar without
 * performing a new fetch (HTML5 History API pushState semantics).
 *
 * WHY: pushState appends a new session-history entry after the current one,
 *      truncating any forward entries, without triggering a navigation.
 *
 * \param bw    browser window
 * \param url   new absolute URL for the inserted entry
 * \return NSERROR_OK or error code on failure
 */
nserror browser_window_history_push_state(struct browser_window *bw,
					  struct nsurl *url);

#endif
