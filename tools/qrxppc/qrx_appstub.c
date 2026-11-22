#include "qrx_appstub.h"

#include <string.h>

static const qrx_appimport_t qrx_appimports[] = {
    { "InterfaceLib", "AddResource", 0xA9AB, QRX_STUB_DIRECT },
    { "InterfaceLib", "Alert", 0xA985, QRX_STUB_DIRECT },
    { "InterfaceLib", "BeginUpdate", 0xA922, QRX_STUB_DIRECT },
    { "InterfaceLib", "CallUniversalProc", QRX_APP_TRAP_CALL_UNIVERSAL_PROC,
      QRX_STUB_DIRECT },
    { "InterfaceLib", "CautionAlert", 0xA988, QRX_STUB_DIRECT },
    { "InterfaceLib", "Count1Resources", 0xA99C, QRX_STUB_INDIRECT },
    { "InterfaceLib", "CountResources", 0xA99C, QRX_STUB_DIRECT },
    { "InterfaceLib", "CountTypes", 0xA99E, QRX_STUB_DIRECT },
    { "InterfaceLib", "Create", 0xA008, QRX_STUB_INDIRECT },
    { "InterfaceLib", "DetachResource", 0xA992, QRX_STUB_DIRECT },
    { "InterfaceLib", "DisposeHandle", 0xA023, QRX_STUB_DIRECT },
    { "InterfaceLib", "DisposeRoutineDescriptor",
      QRX_APP_TRAP_DISPOSE_ROUTINE_DESCRIPTOR, QRX_STUB_DIRECT },
    { "InterfaceLib", "DisposePtr", 0xA01F, QRX_STUB_DIRECT },
    { "InterfaceLib", "DrawString", 0xA884, QRX_STUB_DIRECT },
    { "InterfaceLib", "EndUpdate", 0xA923, QRX_STUB_DIRECT },
    { "InterfaceLib", "EraseRect", 0xA8A3, QRX_STUB_DIRECT },
    { "InterfaceLib", "EventAvail", 0xA971, QRX_STUB_DIRECT },
    { "InterfaceLib", "FSClose", 0xA001, QRX_STUB_INDIRECT },
    { "InterfaceLib", "FSDelete", 0xA009, QRX_STUB_INDIRECT },
    { "InterfaceLib", "FSOpen", 0xA000, QRX_STUB_INDIRECT },
    { "InterfaceLib", "FSRead", 0xA002, QRX_STUB_INDIRECT },
    { "InterfaceLib", "FSWrite", 0xA003, QRX_STUB_INDIRECT },
    { "InterfaceLib", "FindWindow", 0xA92C, QRX_STUB_DIRECT },
    { "InterfaceLib", "FlushEvents", 0xA032, QRX_STUB_DIRECT },
    { "InterfaceLib", "FrameRect", 0xA8A1, QRX_STUB_DIRECT },
    { "InterfaceLib", "FreeMem", 0xA01C, QRX_STUB_DIRECT },
    { "InterfaceLib", "FrontWindow", 0xA924, QRX_STUB_DIRECT },
    { "InterfaceLib", "Get1IndResource", 0xA99D, QRX_STUB_INDIRECT },
    { "InterfaceLib", "Get1Resource", 0xA9A0, QRX_STUB_INDIRECT },
    { "InterfaceLib", "GetEOF", 0xA011, QRX_STUB_INDIRECT },
    { "InterfaceLib", "GetHandleSize", 0xA025, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetIndResource", 0xA99D, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetIndType", 0xA99F, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetKeys", 0xA976, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetPort", 0xA874, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetResource", 0xA9A0, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetWMgrPort", 0xA910, QRX_STUB_DIRECT },
    { "InterfaceLib", "Gestalt", 0xA1AD, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetZone", 0xA11A, QRX_STUB_DIRECT },
    { "InterfaceLib", "GetWRefCon", 0xA917, QRX_STUB_DIRECT },
    { "InterfaceLib", "HiliteWindow", 0xA91C, QRX_STUB_DIRECT },
    { "InterfaceLib", "InsetRect", 0xA8A9, QRX_STUB_DIRECT },
    { "InterfaceLib", "InvalRect", 0xA928, QRX_STUB_DIRECT },
    { "InterfaceLib", "ModalDialog", 0xA991, QRX_STUB_DIRECT },
    { "InterfaceLib", "MoveTo", 0xA893, QRX_STUB_DIRECT },
    { "InterfaceLib", "MoveWindow", 0xA91B, QRX_STUB_DIRECT },
    { "InterfaceLib", "NewFatRoutineDescriptor",
      QRX_APP_TRAP_NEW_FAT_ROUTINE_DESCRIPTOR, QRX_STUB_DIRECT },
    { "InterfaceLib", "NewHandle", 0xA122, QRX_STUB_DIRECT },
    { "InterfaceLib", "NewRoutineDescriptor",
      QRX_APP_TRAP_NEW_ROUTINE_DESCRIPTOR, QRX_STUB_DIRECT },
    { "InterfaceLib", "NewPtr", 0xA11E, QRX_STUB_DIRECT },
    { "InterfaceLib", "NoteAlert", 0xA987, QRX_STUB_DIRECT },
    { "InterfaceLib", "PaintRect", 0xA8A2, QRX_STUB_DIRECT },
    { "InterfaceLib", "PenNormal", 0xA89E, QRX_STUB_DIRECT },
    { "InterfaceLib", "PenPat", 0xA89D, QRX_STUB_DIRECT },
    { "InterfaceLib", "PenSize", 0xA89B, QRX_STUB_DIRECT },
    { "InterfaceLib", "PtInRect", 0xA8AD, QRX_STUB_DIRECT },
    { "InterfaceLib", "ReleaseResource", 0xA9A3, QRX_STUB_DIRECT },
    { "InterfaceLib", "RemoveResource", 0xA9AD, QRX_STUB_DIRECT },
    { "InterfaceLib", "ResError", 0xA9AF, QRX_STUB_DIRECT },
    { "InterfaceLib", "SelectWindow", 0xA91F, QRX_STUB_DIRECT },
    { "InterfaceLib", "SetClip", 0xA879, QRX_STUB_INDIRECT },
    { "InterfaceLib", "SetEOF", 0xA012, QRX_STUB_INDIRECT },
    { "InterfaceLib", "SetHandleSize", 0xA024, QRX_STUB_DIRECT },
    { "InterfaceLib", "SetPort", 0xA873, QRX_STUB_DIRECT },
    { "InterfaceLib", "SetWTitle", 0xA91A, QRX_STUB_DIRECT },
    { "InterfaceLib", "ShowWindow", 0xA915, QRX_STUB_DIRECT },
    { "InterfaceLib", "StopAlert", 0xA986, QRX_STUB_DIRECT },
    { "InterfaceLib", "StringWidth", 0xA88C, QRX_STUB_DIRECT },
    { "InterfaceLib", "TickCount", 0xA975, QRX_STUB_DIRECT },
    { "InterfaceLib", "WaitNextEvent", 0xA860, QRX_STUB_DIRECT },
};

unsigned qrx_appimport_count(void)
{
    return (unsigned)(sizeof(qrx_appimports) / sizeof(qrx_appimports[0]));
}

const qrx_appimport_t *qrx_appimport_find(const char *library,
                                          const char *name)
{
    unsigned i;

    if (!library || !name)
        return 0;
    for (i = 0; i < qrx_appimport_count(); i++) {
        const qrx_appimport_t *e = &qrx_appimports[i];

        if (strcmp(e->library, library) == 0 && strcmp(e->name, name) == 0)
            return e;
    }
    return 0;
}
