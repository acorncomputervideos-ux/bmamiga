/*
 * romcache.cpp - Amiberry's ROM scan, only when the ROMs have changed.
 *
 * Amiberry's libretro core scans its ROM folders on every start
 * (scan_roms() in libretro/libretro_stubs.cpp): it reads every file in
 * them in full to know which ROM it is, which on the Pi's SD card takes
 * seconds with a set such as Amiga Forever's. What it finds goes in
 * amiberry.ini (DetectedROMs), which is kept on the card.
 *
 * This wraps scan_roms() (--wrap=_Z9scan_romsi in the Makefile): it looks
 * at the same folders without reading the files (their names, sizes and
 * times), and if nothing has changed since the last scan, and the ROM list
 * is still in amiberry.ini, Amiberry uses that list instead of scanning.
 * Otherwise Amiberry scans as before, and the folders' fingerprint is kept
 * in amiberry.ini (BMAmigaROMScan) for the next start. Amiberry's own ROM
 * table is part of the fingerprint, so a new Amiberry scans again.
 *
 * Compiled with Amiberry's flags (src/port/amiberry.mk, BMA_EXTRA).
 *
 * This file is part of BMAmiga, a bare metal Amiga emulator for the
 * Raspberry Pi. See LICENSE for licensing details.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "registry.h"
#include "rommgr.h"
#include "uae.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

/* int scan_roms(int show), Amiberry's, and this in its place. */
extern "C" int __real__Z9scan_romsi(int show);
extern "C" int __wrap__Z9scan_romsi(int show);

#define FINGERPRINT_KEY _T("BMAmigaROMScan")

static uint64_t hash_bytes(uint64_t h, const void *data, size_t size)
{
	const unsigned char *p = static_cast<const unsigned char *>(data);
	for (size_t i = 0; i < size; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;   /* FNV-1a */
	}
	return h;
}

static uint64_t hash_text(uint64_t h, const std::string &text)
{
	return hash_bytes(h, text.c_str(), text.size() + 1);
}

static std::string join(const std::string &dir, const std::string &name)
{
	if (dir.empty() || dir.back() == '/')
		return dir + name;
	return dir + "/" + name;
}

/* The files of DIR that scan_roms() would look at (and its folders, to two
   levels, if DEEP), by name, size and time. A folder that isn't there
   counts too, so that making one is a change. */
static uint64_t hash_dir(uint64_t h, const std::string &dir, bool deep, int level)
{
	h = hash_text(h, dir);
	DIR *dp = opendir(dir.c_str());
	if (dp == nullptr)
		return hash_text(h, "(none)");
	std::vector<std::string> names;
	struct dirent *entry;
	while ((entry = readdir(dp)) != nullptr) {
		if (entry->d_name[0] == '.' && (entry->d_name[1] == 0
		    || (entry->d_name[1] == '.' && entry->d_name[2] == 0)))
			continue;
		names.emplace_back(entry->d_name);
	}
	closedir(dp);
	std::sort(names.begin(), names.end());

	for (const auto &name : names) {
		const std::string path = join(dir, name);
		struct stat st {};
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISREG(st.st_mode) && st.st_size < 10000000) {
			const int64_t size = st.st_size, time = st.st_mtime;
			h = hash_text(h, name);
			h = hash_bytes(h, &size, sizeof size);
			h = hash_bytes(h, &time, sizeof time);
		} else if (deep && S_ISDIR(st.st_mode) && name[0] != '.' && level < 2) {
			h = hash_dir(h, path, deep, level + 1);
		}
	}
	return h;
}

/* A folder scan_roms() looks in as a "root": its own files, and the
   folders below it that Amiberry keeps ROMs in. */
static uint64_t hash_root(uint64_t h, const char *root)
{
	static const char *const subdirs[] = {
		"roms", "Kickstarts", "save-data/Kickstarts", "whdboot/save-data/Kickstarts",
		"amiberry/whdboot/save-data/Kickstarts",
	};
	if (root == nullptr || *root == '\0')
		return h;
	h = hash_dir(h, root, false, 0);
	for (const char *subdir : subdirs)
		h = hash_dir(h, join(root, subdir), true, 0);
	return h;
}

/* The fingerprint of everything scan_roms() would read, and of Amiberry's
   table of known ROMs. */
static std::string fingerprint(void)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (int id = 1; ; id++) {
		const struct romdata *rd = getromdatabyid(id);
		if (rd == nullptr)
			break;
		h = hash_bytes(h, &rd->id, sizeof rd->id);
		h = hash_bytes(h, &rd->crc32, sizeof rd->crc32);
		h = hash_bytes(h, &rd->size, sizeof rd->size);
	}
	h = hash_dir(h, get_rom_path(), false, 0);
	h = hash_root(h, getenv("AMIBERRY_LIBRETRO_SYSTEM_DIR"));
	h = hash_root(h, getenv("AMIBERRY_LIBRETRO_SAVE_DIR"));
	h = hash_root(h, getenv("AMIBERRY_WHDBOOT_ASSETS_DIR"));
	h = hash_root(h, getenv("AMIBERRY_WHDBOOT_PATH"));
	h = hash_root(h, getenv("WHDBOOT_SAVE_DATA"));
	if (changed_prefs.path_rom.path[0][0] != '\0')
		h = hash_dir(h, changed_prefs.path_rom.path[0], false, 0);

	char text[24];
	snprintf(text, sizeof text, "%016" PRIx64, h);
	return text;
}

int __wrap__Z9scan_romsi(int show)
{
	static int busy;
	if (busy)
		return 0;
	busy++;

	const std::string now = fingerprint();
	TCHAR saved[64] = {};
	int size = sizeof saved / sizeof saved[0];
	int ret;

	/* show is 0 when a rescan was asked for (forceroms). */
	if (show != 0 && regexiststree(nullptr, _T("DetectedROMs"))
	    && regquerystr(nullptr, FINGERPRINT_KEY, saved, &size) && now == saved) {
		printf("BMAmiga: the ROMs are as before: Amiberry's list of them kept\n");
		read_rom_list(false);
		ret = 1;
	} else {
		printf("BMAmiga: the ROMs have changed: Amiberry reads them\n");
		ret = __real__Z9scan_romsi(show);
		regsetstr(nullptr, FINGERPRINT_KEY, now.c_str());
		regclosetree(nullptr);   /* writes amiberry.ini */
	}
	busy--;
	return ret;
}
