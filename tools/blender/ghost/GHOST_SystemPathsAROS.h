/* Where Blender keeps its files on QRX OS One: next to the program. */
#ifndef __GHOST_SYSTEMPATHSAROS_H__
#define __GHOST_SYSTEMPATHSAROS_H__

#include "GHOST_SystemPaths.h"

/* The Mac reference (tools/blender/hostref) builds this too: there Blender finds its files next to the program. */
#ifdef __AROS__
#  define QRX_DIR(d) ((const GHOST_TUns8 *)(d))
#else
#  define QRX_DIR(d) ((const GHOST_TUns8 *)0)
#endif

class GHOST_SystemPathsAROS : public GHOST_SystemPaths {
public:
	const GHOST_TUns8 *getSystemDir(int, const char *) const { return QRX_DIR("PROGDIR:"); }
	const GHOST_TUns8 *getUserDir(int, const char *) const { return QRX_DIR("PROGDIR:user"); }
	const GHOST_TUns8 *getBinaryDir() const { return QRX_DIR("PROGDIR:"); }
	void addToSystemRecentFiles(const char *) const {}
};

#endif
