/* Where Blender keeps its files on QRX OS One: next to the program. */
#ifndef __GHOST_SYSTEMPATHSAROS_H__
#define __GHOST_SYSTEMPATHSAROS_H__

#include "GHOST_SystemPaths.h"

class GHOST_SystemPathsAROS : public GHOST_SystemPaths {
public:
	const GHOST_TUns8 *getSystemDir(int, const char *) const { return (const GHOST_TUns8 *)"PROGDIR:"; }
	const GHOST_TUns8 *getUserDir(int, const char *) const { return (const GHOST_TUns8 *)"PROGDIR:user"; }
	const GHOST_TUns8 *getBinaryDir() const { return (const GHOST_TUns8 *)"PROGDIR:"; }
	void addToSystemRecentFiles(const char *) const {}
};

#endif
