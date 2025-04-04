#include "sysdeps.h"

#include <emscripten.h>

#include "clip.h"
#include "cpu_emulation.h"
#include "emul_op.h"
#include "macos_util.h"
#include "mac_encodings.h"
#include "main.h"

#define DEBUG 0
#include "debug.h"

// Flag for PutScrap(): the data was put by GetScrap(), don't bounce it back to
// the JS side
static bool we_put_this_data = false;

// Script Manager constants
#define smMacSysScript		18
#define smMacRegionCode		40
#define verJapan          14
#define smJapanese		    1

/*
 *	Get current system script encoding on Mac
 */

static int GetMacScriptManagerVariable(uint16_t varID) {
   int ret = -1;
   M68kRegisters r;
   static uint8_t proc[] = {
     0x59, 0x4f,							// subq.w	 #4,sp
     0x3f, 0x3c, 0x00, 0x00,				// move.w	 #varID,-(sp)
     0x2f, 0x3c, 0x84, 0x02, 0x00, 0x08, // move.l	 #-2080243704,-(sp)
     0xa8, 0xb5,							// ScriptUtil()
     0x20, 0x1f,							// move.l	 (a7)+,d0
     M68K_RTS >> 8, M68K_RTS & 0xff
   };
   r.d[0] = sizeof(proc);
   Execute68kTrap(0xa71e, &r);		// NewPtrSysClear()
   uint32_t proc_area = r.a[0];
   if (proc_area) {
     Host2Mac_memcpy(proc_area, proc, sizeof(proc));
     WriteMacInt16(proc_area + 4, varID);
     Execute68k(proc_area, &r);
     ret = r.d[0];
     r.a[0] = proc_area;
     Execute68kTrap(0xa01f, &r); // DisposePtr
   }
   return ret;
}

typedef struct {
  const char *(*utf8_to_mac)(const char *, size_t);
  const char *(*mac_to_utf8)(const char *, size_t);
} EncodingFunctions;

static EncodingFunctions getEncodingFunctions() {
	int script = GetMacScriptManagerVariable(smMacSysScript);
	int region = GetMacScriptManagerVariable(smMacRegionCode);

  if (script == smJapanese && region == verJapan) {
    return {utf8_to_macjapanese, macjapanese_to_utf8};
  }

  // Default to MacRoman
  return {utf8_to_macroman, macroman_to_utf8};
}

void ClipInit(void) {
  D(bug("ClipInit\n"));
}

void ClipExit(void) {
  D(bug("ClipExit\n"));
}

EM_JS(char*, getClipboardText, (), {
  const clipboardText = workerApi.getClipboardText();
  if (!clipboardText || !clipboardText.length) {
    return 0;
  }
  const clipboardTextLength = lengthBytesUTF8(clipboardText) + 1;
  const clipboardTextCstr = _malloc(clipboardTextLength);
  stringToUTF8(clipboardText, clipboardTextCstr, clipboardTextLength);
  return clipboardTextCstr;
});

// Mac application reads clipboard
void GetScrap(void** handle, uint32 type, int32 offset) {
  D(bug("GetScrap type '%s', handle %p, offset %d\n", FOURCCstr(type), handle, offset));
  switch (type) {
    case FOURCC('T', 'E', 'X', 'T'): {
      char* clipboardTextCstr = getClipboardText();
      if (!clipboardTextCstr) {
        break;
      }

      char* clipboardTextMac =
          const_cast<char*>(getEncodingFunctions().utf8_to_mac(clipboardTextCstr, strlen(clipboardTextCstr)));
      free(clipboardTextCstr);
      size_t clipboardTextMacLength = strlen(clipboardTextMac);
      for (int i = 0; i < clipboardTextMacLength; i++) {
        // LF -> CR
        if (clipboardTextMac[i] == 10) {
          clipboardTextMac[i] = 13;
        }
      }

      // Allocate space for new scrap in MacOS side
      M68kRegisters r;
      r.d[0] = clipboardTextMacLength;
      Execute68kTrap(0xa71e, &r);  // NewPtrSysClear()
      uint32 scrap_area = r.a[0];

      if (!scrap_area) {
        break;
      }
      uint8* const data = Mac2HostAddr(scrap_area);
      memcpy(data, clipboardTextMac, clipboardTextMacLength);
      free(clipboardTextMac);

      // Add new data to clipboard
      static uint8 proc[] = {0x59,
                             0x8f,  // subq.l	#4,sp
                             0xa9,
                             0xfc,  // ZeroScrap()
                             0x2f,
                             0x3c,
                             0,
                             0,
                             0,
                             0,  // move.l	#length,-(sp)
                             0x2f,
                             0x3c,
                             0,
                             0,
                             0,
                             0,  // move.l	#type,-(sp)
                             0x2f,
                             0x3c,
                             0,
                             0,
                             0,
                             0,  // move.l	#outbuf,-(sp)
                             0xa9,
                             0xfe,  // PutScrap()
                             0x58,
                             0x8f,  // addq.l	#4,sp
                             M68K_RTS >> 8,
                             M68K_RTS & 0xff};
      r.d[0] = sizeof(proc);
      Execute68kTrap(0xa71e, &r);  // NewPtrSysClear()
      uint32 proc_area = r.a[0];

      // The procedure is run-time generated because it must lays in
      // Mac address space. This is mandatory for "33-bit" address
      // space optimization on 64-bit platforms because the static
      // proc[] array is not remapped
      Host2Mac_memcpy(proc_area, proc, sizeof(proc));
      WriteMacInt32(proc_area + 6, clipboardTextMacLength);
      WriteMacInt32(proc_area + 12, type);
      WriteMacInt32(proc_area + 18, scrap_area);
      we_put_this_data = true;
      Execute68k(proc_area, &r);

      // We are done with scratch memory
      r.a[0] = proc_area;
      Execute68kTrap(0xa01f, &r);  // DisposePtr
      r.a[0] = scrap_area;
      Execute68kTrap(0xa01f, &r);  // DisposePtr
      break;
    }
    default:
      D(bug("GetScrap: unknown type '%s', ignoring\n", FOURCCstr(type)));
      break;
  }
}

// ZeroScrap() is called before a Mac application writes to the clipboard;
// clears out the previous contents.
void ZeroScrap() {
  D(bug("ZeroScrap\n"));
}

// Mac application wrote to clipboard
void PutScrap(uint32 type, void* scrap, int32 length) {
  D(bug("PutScrap type '%s', data %p, length %d\n", FOURCCstr(type), scrap, length));
  if (we_put_this_data) {
    we_put_this_data = false;
    return;
  }
  if (length <= 0) {
    return;
  }

  switch (type) {
    case FOURCC('T', 'E', 'X', 'T'):
      EM_ASM_({ workerApi.setClipboardText(UTF8ToString($0)); },
              getEncodingFunctions().mac_to_utf8((char*)scrap, length));
      break;
    default:
      D(bug("PutScrap: unknown type '%s', ignoring\n", FOURCCstr(type)));
      break;
  }
}
